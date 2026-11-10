#include "cpu_profile.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <intrin.h>
#include <windows.h>
#include <dbghelp.h>

#include "../log.h"

namespace simqvfr {
namespace {

constexpr size_t kStackCopy = 64 * 1024;  // the top of a stack copied per sample: enough for the frames walked
constexpr int kMaxFrames = 48;
constexpr size_t kTop = 25;

// Games' functions mapped so far (static analysis, docs/recon-ace.md and games/ace/abi.h), by RVA of their start, per
// game module. Other games' functions show as module+offset: their names stay in the local recon notes.
struct Label {
    uint32_t rva;
    const char* name;
};
constexpr Label kAceLabels[] = {
    {0x1d43090, "renderer init"},
    {0x1d6bfd0, "instance visibility batch merge"},
    {0x1d84a80, "frame builder"},
    {0x1d922b0, "view viewport"},
    {0x1d92f50, "view record"},
    {0x1d96c80, "tile culling setup"},
    {0x1da1e20, "volumetrics setup"},
    {0x1db19c0, "pass executor"},
    {0x1dc4b70, "per-view pass setup"},
    {0x1dc68e0, "per-view pass setup (2)"},
    {0x1dc7f50, "viewCount"},
    {0x1f7a6e0, "final pass"},
    {0x1f8fa40, "DLSS parameters"},
    {0x1f90430, "DLSS feature create"},
    {0x1f920f0, "DLSS evaluate"},
};
struct GameLabels {
    const wchar_t* module;
    const Label* labels;
    size_t count;
};
constexpr GameLabels kGameLabels[] = {
    {L"AssettoCorsaEVO.exe", kAceLabels, std::size(kAceLabels)},
};

// Frame ends (cpu_profile_mark_frame), QPC ticks, while a capture runs: a quarter million is over 45 minutes at 90 fps.
constexpr size_t kMaxMarks = size_t{1} << 18;
// Plain words, read and written through std::atomic_ref (mark): an array of std::atomic this size cost MSVC 4.6 s of
// every compile of this file, initializing each element.
alignas(std::atomic_ref<uint64_t>::required_alignment) uint64_t g_marks[kMaxMarks];
std::atomic_ref<uint64_t> mark(size_t i) { return std::atomic_ref<uint64_t>(g_marks[i]); }
std::atomic<size_t> g_markCount{0};
std::atomic<bool> g_marking{false};

// A sample kept for the slow-frame comparison: its time and its stack's distinct functions (in `Samples::starts`,
// innermost first, from `first`).
struct Sample {
    uint64_t qpc;
    uint32_t target;
    uint32_t first;
    uint16_t count;
    bool blocked;
};
constexpr size_t kMaxStarts = size_t{8} << 20;  // 64 MB of function starts at most; later samples are not kept
struct Samples {
    std::vector<Sample> list;
    std::vector<uint64_t> starts;
    bool full = false;
};

struct Target {
    uint32_t id = 0;
    HANDLE handle = nullptr;
    uint64_t lastCycles = 0;
    uint64_t cycles = 0;          // used while profiled: its load
    uint64_t samples = 0, idle = 0, failed = 0;
    uint64_t running = 0, blocked = 0;  // read samples: in its own code, or blocked in a wait (not broken down)
    std::unordered_map<uint64_t, uint64_t> self, total;  // function start -> running samples
};

struct State {
    std::mutex mutex;
    std::thread worker;
    std::atomic<bool> running{false};
    std::wstring lastReport;
    // A worker never joined (a capture ran, none started after it) would end the process in std::terminate when
    // this is destroyed at exit, even after it finished. At a process exit it has ended, so the join returns at once.
    ~State() {
        if (worker.joinable())
            worker.join();
    }
};
State state;

// The function containing `address`: its start from the unwind data. A leaf function (no stack use) has no
// unwind entry: it sits in the gap after the entry before it, at the next 16-byte boundary (MSVC aligns
// functions to 16), found in the module's own table (.pdata). Code outside any module: the address itself.
uint64_t function_start(uint64_t address) {
    DWORD64 imageBase = 0;
    if (const PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(address, &imageBase, nullptr))
        return imageBase + f->BeginAddress;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module))
        return address;
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    const auto* table = reinterpret_cast<const RUNTIME_FUNCTION*>(base + dir.VirtualAddress);
    const size_t count = dir.Size / sizeof(RUNTIME_FUNCTION);
    const uint32_t rva = static_cast<uint32_t>(address - reinterpret_cast<uint64_t>(module));
    // The last entry ending at or before the address (the table is sorted by start).
    const RUNTIME_FUNCTION* before = std::upper_bound(table, table + count, rva, [](uint32_t r, const RUNTIME_FUNCTION& e) {
        return r < e.EndAddress;
    });
    const uint32_t gap = before == table ? 0 : ((before - 1)->EndAddress + 15) & ~15u;
    return reinterpret_cast<uint64_t>(module) + (gap <= rva ? gap : rva);
}

// Walks the copied stack (`copy`, `size` bytes, taken at `rsp`) from `context`. Plain data only, so it can
// run under SEH: a corrupt stack ends the walk instead of the process. Returns the frames found.
int walk(const CONTEXT& context, const uint8_t* copy, size_t size, uint64_t rsp, uint64_t (&frames)[kMaxFrames]) {
    int n = 0;
    __try {
        CONTEXT c = context;
        const uint64_t copyStart = reinterpret_cast<uint64_t>(copy), copyEnd = copyStart + size;
        const uint64_t shift = copyStart - rsp;
        // Stack addresses read through the copy: the stack pointer, and the frame pointer while it points
        // into the copied part of the stack.
        c.Rsp += shift;
        if (c.Rbp >= rsp && c.Rbp < rsp + size)
            c.Rbp += shift;
        frames[n++] = c.Rip;
        while (n < kMaxFrames) {
            DWORD64 imageBase = 0;
            const PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
            if (f) {
                void* handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, f, &c, &handlerData, &establisher, nullptr);
            } else {
                if (c.Rsp < copyStart || c.Rsp + 8 > copyEnd)
                    break;
                c.Rip = *reinterpret_cast<const uint64_t*>(c.Rsp);  // a leaf: its return address on top
                c.Rsp += 8;
            }
            if (!c.Rip || c.Rsp < copyStart || c.Rsp >= copyEnd)
                break;
            if (c.Rbp >= rsp && c.Rbp < rsp + size)
                c.Rbp += shift;
            frames[n++] = c.Rip - 1;  // inside the call instruction: the caller's function
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

// One sample of a thread: paused only to copy its registers and stack top. False if it could not be read.
bool sample(Target& t, std::vector<uint8_t>& copy, CONTEXT& context, size_t& size, uint64_t& rsp) {
    if (SuspendThread(t.handle) == static_cast<DWORD>(-1))
        return false;
    context = {};
    context.ContextFlags = CONTEXT_FULL;
    bool ok = GetThreadContext(t.handle, &context) != 0;
    size = 0;
    rsp = context.Rsp;
    if (ok) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<void*>(rsp), &region, sizeof(region)) && region.State == MEM_COMMIT) {
            const uint64_t end = reinterpret_cast<uint64_t>(region.BaseAddress) + region.RegionSize;
            size = static_cast<size_t>(std::min<uint64_t>(kStackCopy, end - rsp));
            SIZE_T read = 0;
            ok = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(rsp), copy.data(), size, &read) &&
                 read == size;
        } else {
            ok = false;
        }
    }
    ResumeThread(t.handle);
    return ok;
}

// Names: DbgHelp's symbols where it has them (System32's dbghelp, loaded for the report only).
struct Symbols {
    HMODULE dll = nullptr;
    decltype(&SymInitializeW) init = nullptr;
    decltype(&SymFromAddrW) fromAddr = nullptr;
    decltype(&SymCleanup) cleanup = nullptr;
    decltype(&SymSetOptions) options = nullptr;
    bool ready = false;
    Symbols() {
        dll = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll)
            return;
        init = reinterpret_cast<decltype(init)>(reinterpret_cast<void*>(GetProcAddress(dll, "SymInitializeW")));
        fromAddr = reinterpret_cast<decltype(fromAddr)>(reinterpret_cast<void*>(GetProcAddress(dll, "SymFromAddrW")));
        cleanup = reinterpret_cast<decltype(cleanup)>(reinterpret_cast<void*>(GetProcAddress(dll, "SymCleanup")));
        options = reinterpret_cast<decltype(options)>(reinterpret_cast<void*>(GetProcAddress(dll, "SymSetOptions")));
        if (init && fromAddr && cleanup && options) {
            options(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
            ready = init(GetCurrentProcess(), nullptr, TRUE) != 0;
        }
    }
    ~Symbols() {
        if (ready)
            cleanup(GetCurrentProcess());
        if (dll)
            FreeLibrary(dll);
    }
};

std::wstring module_name(uint64_t address, uint64_t& base) {
    HMODULE module = nullptr;
    base = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module))
        return L"?";
    base = reinterpret_cast<uint64_t>(module);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* slash = std::wcsrchr(path, L'\\');
    return slash ? slash + 1 : path;
}

// The module's exports by RVA, read from its own export table: names for system and driver DLLs without
// DbgHelp (which may be unavailable: ACE can hold its own DbgHelp session for the process).
using Exports = std::vector<std::pair<uint32_t, std::string>>;
const Exports& exports_of(uint64_t base) {
    static std::map<uint64_t, Exports> cache;
    Exports& e = cache[base];
    if (!e.empty() || !base)
        return e;
    const auto* image = reinterpret_cast<const uint8_t*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + reinterpret_cast<const IMAGE_DOS_HEADER*>(image)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size) {
        e.push_back({0, std::string()});  // none: remembered as an empty name at 0
        return e;
    }
    const auto* ex = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(image + dir.VirtualAddress);
    const auto* functions = reinterpret_cast<const uint32_t*>(image + ex->AddressOfFunctions);
    const auto* names = reinterpret_cast<const uint32_t*>(image + ex->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const uint16_t*>(image + ex->AddressOfNameOrdinals);
    for (DWORD i = 0; i < ex->NumberOfNames; ++i) {
        const uint32_t rva = functions[ordinals[i]];
        if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size)
            continue;  // a forwarder, not code here
        e.push_back({rva, reinterpret_cast<const char*>(image + names[i])});
    }
    std::sort(e.begin(), e.end());
    if (e.empty())
        e.push_back({0, std::string()});
    return e;
}

// "module!symbol", "AssettoCorsaEVO.exe+0x1d84a80 (frame builder)" or "module+0xRVA".
std::wstring function_name(Symbols& symbols, uint64_t start) {
    uint64_t base = 0;
    const std::wstring module = module_name(start, base);
    wchar_t text[512];
    for (const GameLabels& game : kGameLabels) {
        if (_wcsicmp(module.c_str(), game.module) != 0)
            continue;
        const uint64_t rva = start - base;
        for (size_t i = 0; i < game.count; ++i)
            if (game.labels[i].rva == rva) {
                _snwprintf_s(text, _TRUNCATE, L"%ls+0x%llx (%hs)", module.c_str(), static_cast<unsigned long long>(rva),
                             game.labels[i].name);
                return text;
            }
    }
    if (symbols.ready) {
        alignas(SYMBOL_INFOW) uint8_t buffer[sizeof(SYMBOL_INFOW) + 256 * sizeof(wchar_t)] = {};
        auto* info = reinterpret_cast<SYMBOL_INFOW*>(buffer);
        info->SizeOfStruct = sizeof(SYMBOL_INFOW);
        info->MaxNameLen = 255;
        DWORD64 displacement = 0;
        // Export-only modules name the nearest export, which can be far off: trust a symbol only near its start.
        if (symbols.fromAddr(GetCurrentProcess(), start, &displacement, info) && displacement < 0x1000) {
            if (displacement)
                _snwprintf_s(text, _TRUNCATE, L"%ls!%ls+0x%llx", module.c_str(), info->Name, static_cast<unsigned long long>(displacement));
            else
                _snwprintf_s(text, _TRUNCATE, L"%ls!%ls", module.c_str(), info->Name);
            return text;
        }
    }
    // The export at or before it, when it is that export or close behind it (code past an export's end is
    // another, unexported function).
    const Exports& e = exports_of(base);
    const uint32_t rva = static_cast<uint32_t>(start - base);
    const auto after = std::upper_bound(e.begin(), e.end(), rva, [](uint32_t r, const auto& x) { return r < x.first; });
    if (after != e.begin() && !(after - 1)->second.empty() && rva - (after - 1)->first < 0x100) {
        const uint32_t displacement = rva - (after - 1)->first;
        if (displacement)
            _snwprintf_s(text, _TRUNCATE, L"%ls!%hs+0x%x", module.c_str(), (after - 1)->second.c_str(), displacement);
        else
            _snwprintf_s(text, _TRUNCATE, L"%ls!%hs", module.c_str(), (after - 1)->second.c_str());
        return text;
    }
    _snwprintf_s(text, _TRUNCATE, L"%ls+0x%llx", module.c_str(), static_cast<unsigned long long>(start - base));
    return text;
}

std::wstring thread_name(uint32_t id) {
    using Describe = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto get = reinterpret_cast<Describe>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
    std::wstring name;
    HANDLE t = get ? OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id) : nullptr;
    if (!t)
        return name;
    PWSTR text = nullptr;
    if (SUCCEEDED(get(t, &text)) && text) {
        name = text;
        LocalFree(text);
    }
    CloseHandle(t);
    return name;
}

using Ranked = std::vector<std::pair<uint64_t, uint64_t>>;  // key, cycles: most first
Ranked ranked(const std::unordered_map<uint64_t, uint64_t>& m) {
    Ranked r(m.begin(), m.end());
    std::sort(r.begin(), r.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    return r;
}

// The frames' ends the layer marked during the capture, in order (an entry a late marker had not stored yet, 0, left out).
std::vector<uint64_t> taken_marks() {
    const size_t n = std::min(g_markCount.load(), kMaxMarks);
    std::vector<uint64_t> marks;
    marks.reserve(n);
    for (size_t i = 0; i < n; ++i)
        if (const uint64_t m = mark(i).load(std::memory_order_relaxed))
            marks.push_back(m);
    std::sort(marks.begin(), marks.end());
    return marks;
}

// The slow frames against the others: each kept sample goes to the frame it fell in (between two marks), and per
// thread the functions are ranked by how much more time a slow frame spends in them than a normal one, in ms per frame
// (samples per frame x the thread's sampling period). Returns a summary line for the log (empty: no frames marked).
template <class Line>
std::string slow_frames(const Line& line, Symbols& symbols, const std::vector<Target>& targets, const Samples& samples,
                        const std::vector<uint64_t>& marks, double slowMs, double qpcPerMs, double elapsedMs) {
    if (marks.size() < 3) {
        line(L"\nSlow frames: no frames were marked during the capture (the game ended no frame, or none is marked).");
        return {};
    }
    std::vector<double> ms(marks.size() - 1);  // frame k (1..n-1) ends at marks[k]: ms[k - 1]
    for (size_t k = 1; k < marks.size(); ++k)
        ms[k - 1] = static_cast<double>(marks[k] - marks[k - 1]) / qpcPerMs;
    std::vector<double> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    const auto pct = [&](double p) { return sorted[std::min(sorted.size() - 1, static_cast<size_t>(p * static_cast<double>(sorted.size())))]; };
    const double median = pct(0.5);
    const double threshold = slowMs > 0 ? slowMs : 1.25 * median;
    size_t slow = 0;
    for (const double m : ms)
        slow += m > threshold ? 1 : 0;
    const size_t normal = ms.size() - slow;
    wchar_t text[600];
    _snwprintf_s(text, _TRUNCATE,
                 L"\nSlow frames: %zu frames, median %.2f ms, p90 %.2f, p99 %.2f, max %.2f. Slow: longer than %.2f ms (%ls): "
                 L"%zu frames (%.1f%%), %.0f ms of the %.0f ms frames.",
                 ms.size(), median, pct(0.9), pct(0.99), sorted.back(), threshold, slowMs > 0 ? L"cpu_profile_slow_ms" : L"1.25x the median",
                 slow, 100.0 * static_cast<double>(slow) / static_cast<double>(ms.size()),
                 [&] { double s = 0; for (const double m : ms) s += m > threshold ? m : 0; return s; }(),
                 [&] { double s = 0; for (const double m : ms) s += m; return s; }());
    line(text);
    if (samples.full)
        line(L"  (the sample store filled up: the capture's later samples are not in this comparison)");
    {
        std::vector<size_t> order(ms.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return ms[a] > ms[b]; });
        std::wstring list = L"  Slowest (ms at s into the capture):";
        for (size_t i = 0; i < order.size() && i < 12; ++i) {
            wchar_t item[64];
            _snwprintf_s(item, _TRUNCATE, L"%ls %.1f at %.2f", i ? L"," : L"", ms[order[i]],
                         static_cast<double>(marks[order[i] + 1] - marks.front()) / qpcPerMs / 1000.0);
            list += item;
        }
        line(list);
    }
    if (!slow || !normal) {
        line(slow ? L"  Every frame is slow: nothing to compare with." : L"  No slow frame: nothing to compare.");
        return "no slow frames";
    }
    line(L"  Per thread: ms per frame in a slow frame against a normal one, the functions slow frames spend most "
         L"extra time in first (Total: in the function or anything it called; blocked: waits, by the functions waiting).");
    struct Counts {
        uint64_t running = 0, blocked = 0;
        std::unordered_map<uint64_t, uint64_t> self, total, waits;
    };
    std::vector<Counts> slowCounts(targets.size()), normalCounts(targets.size());
    for (const Sample& s : samples.list) {
        const auto at = std::lower_bound(marks.begin(), marks.end(), s.qpc);
        if (at == marks.begin() || at == marks.end())
            continue;  // before the first frame end or after the last: no whole frame
        const size_t k = static_cast<size_t>(at - marks.begin());
        Counts& c = (ms[k - 1] > threshold ? slowCounts : normalCounts)[s.target];
        const uint64_t* starts = samples.starts.data() + s.first;
        if (s.blocked) {
            ++c.blocked;
            for (uint16_t i = 0; i < s.count; ++i)
                ++c.waits[starts[i]];
        } else {
            ++c.running;
            if (s.count)
                ++c.self[starts[0]];
            for (uint16_t i = 0; i < s.count; ++i)
                ++c.total[starts[i]];
        }
    }
    std::string summary;
    for (size_t t = 0; t < targets.size(); ++t) {
        const Target& target = targets[t];
        const double period = target.samples ? elapsedMs / static_cast<double>(target.samples) : kCpuProfileSampleMs;
        const auto perSlow = [&](uint64_t n) { return static_cast<double>(n) * period / static_cast<double>(slow); };
        const auto perNormal = [&](uint64_t n) { return static_cast<double>(n) * period / static_cast<double>(normal); };
        const Counts& a = slowCounts[t];
        const Counts& b = normalCounts[t];
        const std::wstring name = thread_name(target.id);
        _snwprintf_s(text, _TRUNCATE, L"\n  Thread %u%ls%ls%ls: running %.2f ms per slow frame (normal %.2f), blocked %.2f (normal %.2f)",
                     target.id, name.empty() ? L"" : L" \"", name.c_str(), name.empty() ? L"" : L"\"", perSlow(a.running),
                     perNormal(b.running), perSlow(a.blocked), perNormal(b.blocked));
        line(text);
        if (t == 0 || perSlow(a.running) - perNormal(b.running) > 1.0) {
            char item[200];
            std::snprintf(item, sizeof(item), "%sthread %u +%.1f ms running, +%.1f blocked", summary.empty() ? "" : "; ", target.id,
                          perSlow(a.running) - perNormal(b.running), perSlow(a.blocked) - perNormal(b.blocked));
            summary += item;
        }
        const auto ranked_extra = [&](const std::unordered_map<uint64_t, uint64_t>& x, const std::unordered_map<uint64_t, uint64_t>& y,
                                      const wchar_t* title, size_t top) {
            std::vector<std::pair<uint64_t, double>> extra;
            for (const auto& [start, n] : x) {
                const auto other = y.find(start);
                extra.push_back({start, perSlow(n) - perNormal(other == y.end() ? 0 : other->second)});
            }
            std::sort(extra.begin(), extra.end(), [](const auto& p, const auto& q) { return p.second > q.second; });
            if (extra.empty() || extra.front().second < 0.05)
                return;
            line(title);
            for (size_t i = 0; i < extra.size() && i < top && extra[i].second >= 0.05; ++i) {
                const auto inY = y.find(extra[i].first);
                const auto inX = x.find(extra[i].first);
                wchar_t row[700];
                _snwprintf_s(row, _TRUNCATE, L"    %+6.2f ms  (slow %.2f, normal %.2f)  %ls", extra[i].second, perSlow(inX->second),
                             perNormal(inY == y.end() ? 0 : inY->second), function_name(symbols, extra[i].first).c_str());
                line(row);
            }
        };
        ranked_extra(a.total, b.total, L"   Running, total:", 20);
        ranked_extra(a.self, b.self, L"   Running, self:", 10);
        ranked_extra(a.waits, b.waits, L"   Blocked:", 10);
    }
    char head[160];
    std::snprintf(head, sizeof(head), "%zu of %zu frames over %.1f ms (median %.1f, max %.1f): ", slow, ms.size(), threshold, median,
                  sorted.back());
    return head + summary;
}

void report(std::vector<Target>& targets, uint64_t elapsedTsc, int seconds, const std::wstring& path, const Samples& samples,
            const std::vector<uint64_t>& marks, double slowMs, double qpcPerMs, double elapsedMs) {
    Symbols symbols;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w, ccs=UTF-8") != 0)
        f = nullptr;
    const auto line = [&](const std::wstring& s) {
        if (f)
            std::fwprintf(f, L"%ls\n", s.c_str());
    };
    line(L"Sim-QVFR CPU profile: " + std::to_wstring(seconds) + L" s, a sample every " +
         std::to_wstring(kCpuProfileSampleMs) + L" ms per thread. 'Of a core' is the thread's load (its CPU cycles).");
    line(L"Samples where the thread was blocked in a wait (or had not run since the last) are counted apart. The rest: "
         L"Self, where it was running; Total, in the function or anything it called (each function once per sample), "
         L"as shares of those running samples.");
    for (Target& t : targets) {
        const std::wstring name = thread_name(t.id);
        const double load = elapsedTsc ? 100.0 * static_cast<double>(t.cycles) / static_cast<double>(elapsedTsc) : 0;
        wchar_t head[256];
        _snwprintf_s(head, _TRUNCATE, L"\nThread %u%ls%ls%ls: %.0f%% of a core (%llu samples: %llu running, %llu blocked in a wait, "
                         L"%llu idle, %llu unreadable)",
                   t.id, name.empty() ? L"" : L" \"", name.c_str(), name.empty() ? L"" : L"\"", load,
                   static_cast<unsigned long long>(t.samples), static_cast<unsigned long long>(t.running),
                   static_cast<unsigned long long>(t.blocked), static_cast<unsigned long long>(t.idle),
                   static_cast<unsigned long long>(t.failed));
        line(head);
        if (!t.running)
            continue;
        const auto percent = [&](uint64_t samples) { return 100.0 * static_cast<double>(samples) / static_cast<double>(t.running); };
        // By module, from the self time.
        std::map<std::wstring, uint64_t> modules;
        for (const auto& [start, cycles] : t.self) {
            uint64_t base = 0;
            modules[module_name(start, base)] += cycles;
        }
        Ranked byModule;
        std::vector<std::wstring> moduleNames;
        for (const auto& [m, cycles] : modules) {
            moduleNames.push_back(m);
            byModule.push_back({moduleNames.size() - 1, cycles});
        }
        std::sort(byModule.begin(), byModule.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        std::wstring mods = L"  By module (self):";
        for (size_t i = 0; i < byModule.size() && i < 8; ++i) {
            wchar_t item[160];
            _snwprintf_s(item, _TRUNCATE, L"%ls %ls %.1f%%", i ? L"," : L"", moduleNames[byModule[i].first].c_str(), percent(byModule[i].second));
            mods += item;
        }
        line(mods);
        const Ranked self = ranked(t.self), total = ranked(t.total);
        line(L"  Self time:");
        for (size_t i = 0; i < self.size() && i < kTop; ++i) {
            wchar_t item[600];
            _snwprintf_s(item, _TRUNCATE, L"    %5.1f%%  %ls", percent(self[i].second), function_name(symbols, self[i].first).c_str());
            line(item);
        }
        line(L"  Total time:");
        for (size_t i = 0; i < total.size() && i < kTop; ++i) {
            wchar_t item[600];
            _snwprintf_s(item, _TRUNCATE, L"    %5.1f%%  %ls", percent(total[i].second), function_name(symbols, total[i].first).c_str());
            line(item);
        }
        // The log: the thread and its top three by self time.
        std::wstring summary;
        for (size_t i = 0; i < self.size() && i < 3; ++i) {
            wchar_t item[600];
            _snwprintf_s(item, _TRUNCATE, L"%ls%.0f%% %ls", i ? L", " : L"", percent(self[i].second), function_name(symbols, self[i].first).c_str());
            summary += item;
        }
        logf("cpu profile: thread %u%s%ls%s %.0f%% of a core; self: %ls", t.id, name.empty() ? "" : " \"", name.c_str(),
             name.empty() ? "" : "\"", load, summary.c_str());
    }
    const std::string slow = slow_frames(line, symbols, targets, samples, marks, slowMs, qpcPerMs, elapsedMs);
    if (!slow.empty())
        logf("cpu profile: slow frames: %s", slow.c_str());
    if (f)
        std::fclose(f);
    logf("cpu profile: report %ls%s", path.c_str(), f ? "" : " (could not be written)");
}

// Whether a sample's innermost function is a blocking system call (ntdll's and win32u's waits): the thread
// is not running there. Such samples are counted apart, not broken down, or a thread that worked and then
// blocked would show its work as the wait's (each sample carries no weight of its own).
bool blocked_in_wait(uint64_t start) {
    uint64_t base = 0;
    const std::wstring module = module_name(start, base);
    if (_wcsicmp(module.c_str(), L"ntdll.dll") != 0 && _wcsicmp(module.c_str(), L"win32u.dll") != 0)
        return false;
    const Exports& e = exports_of(base);
    const uint32_t rva = static_cast<uint32_t>(start - base);
    const auto after = std::upper_bound(e.begin(), e.end(), rva, [](uint32_t r, const auto& x) { return r < x.first; });
    if (after == e.begin() || rva - (after - 1)->first >= 0x40)
        return false;
    const std::string& name = (after - 1)->second;
    const bool syscall = name.rfind("Nt", 0) == 0 || name.rfind("Zw", 0) == 0;
    return syscall && (name.find("Wait") != std::string::npos || name.find("DelayExecution") != std::string::npos ||
                       name.find("RemoveIoCompletion") != std::string::npos);
}

uint64_t qpc() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart);
}

void run(std::vector<Target> targets, int seconds, std::wstring path, double slowMs) {
    // A high-resolution waitable timer paces the samples (Windows 10 1803 on); else 1 ms sleeps.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    std::vector<uint8_t> copy(kStackCopy);
    uint64_t frames[kMaxFrames];
    Samples kept;
    for (Target& t : targets)
        QueryThreadCycleTime(t.handle, &t.lastCycles);
    const uint64_t tsc0 = __rdtsc(), qpc0 = qpc();
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    while (GetTickCount64() < end) {
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(kCpuProfileSampleMs) * 10000;
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(kCpuProfileSampleMs);
        }
        for (uint32_t index = 0; index < targets.size(); ++index) {
            Target& t = targets[index];
            ULONG64 cycles = 0;
            if (!QueryThreadCycleTime(t.handle, &cycles))
                continue;  // gone
            const uint64_t weight = cycles - t.lastCycles;
            t.lastCycles = cycles;
            ++t.samples;
            if (!weight) {
                ++t.idle;  // it did not run: nothing to attribute
                continue;
            }
            CONTEXT context;
            size_t size = 0;
            uint64_t rsp = 0;
            const uint64_t when = qpc();
            if (!sample(t, copy, context, size, rsp)) {
                ++t.failed;
                continue;
            }
            t.cycles += weight;
            const int n = walk(context, copy.data(), size, rsp, frames);
            // Looked up after the thread runs again: the unwind tables' locks are never taken while it is paused.
            if (n == 0)
                continue;
            uint64_t seen[kMaxFrames];
            int unique = 0;
            for (int i = 0; i < n; ++i) {
                const uint64_t start = function_start(frames[i]);
                if (std::find(seen, seen + unique, start) == seen + unique)
                    seen[unique++] = start;
            }
            const bool blocked = blocked_in_wait(seen[0]);
            // Kept for the slow frames (also the blocked ones: what a slow frame waits in matters there).
            if (!kept.full && kept.starts.size() + unique <= kMaxStarts) {
                kept.list.push_back({when, index, static_cast<uint32_t>(kept.starts.size()), static_cast<uint16_t>(unique), blocked});
                kept.starts.insert(kept.starts.end(), seen, seen + unique);
            } else {
                kept.full = true;
            }
            if (blocked) {
                ++t.blocked;
                continue;
            }
            ++t.running;
            ++t.self[seen[0]];
            for (int i = 0; i < unique; ++i)
                ++t.total[seen[i]];
        }
    }
    const uint64_t elapsed = __rdtsc() - tsc0;
    g_marking = false;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const double qpcPerMs = static_cast<double>(frequency.QuadPart) / 1000.0;
    const double elapsedMs = static_cast<double>(qpc() - qpc0) / qpcPerMs;
    if (timer)
        CloseHandle(timer);
    report(targets, elapsed, seconds, path, kept, taken_marks(), slowMs, qpcPerMs, elapsedMs);
    for (Target& t : targets)
        CloseHandle(t.handle);
    std::lock_guard guard(state.mutex);
    state.lastReport = path;
    state.running = false;
}

}  // namespace

bool cpu_profile_start(const CpuProfileRequest& request) {
    std::lock_guard guard(state.mutex);
    if (state.running)
        return false;
    if (state.worker.joinable())
        state.worker.join();  // the last capture, finished
    // The calling thread may be profiled too (the layer starts captures from ACE's render thread): the samples
    // come from the worker made below.
    std::vector<Target> targets;
    for (const uint32_t id : request.threads) {
        if (std::any_of(targets.begin(), targets.end(), [&](const Target& t) { return t.id == id; }))
            continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
        if (!h)
            continue;
        Target t;
        t.id = id;
        t.handle = h;
        targets.push_back(std::move(t));
    }
    if (targets.empty()) {
        log_line("cpu profile: no thread to profile");
        return false;
    }
    std::wstring path = request.reportPath;
    if (path.empty()) {
        SYSTEMTIME now;
        GetLocalTime(&now);
        wchar_t name[64];
        swprintf_s(name, L"\\cpu-profile-%04u%02u%02u-%02u%02u%02u.txt", now.wYear, now.wMonth, now.wDay, now.wHour,
                   now.wMinute, now.wSecond);
        path = log_directory() + name;
    }
    std::string ids;
    for (const Target& t : targets)
        ids += (ids.empty() ? "" : ", ") + std::to_string(t.id);
    logf("cpu profile: sampling threads %s every %d ms for %d s", ids.c_str(), kCpuProfileSampleMs, request.seconds);
    // The last capture's marks cleared, so an entry not yet stored reads 0 (left out), never an old frame end.
    for (size_t i = 0, n = std::min(g_markCount.load(), kMaxMarks); i < n; ++i)
        mark(i).store(0, std::memory_order_relaxed);
    g_markCount = 0;
    g_marking = true;
    state.running = true;
    state.worker = std::thread(run, std::move(targets), std::max(1, request.seconds), std::move(path), request.slowFrameMs);
    return true;
}

void cpu_profile_mark_frame() {
    if (!g_marking.load(std::memory_order_relaxed))
        return;
    const size_t i = g_markCount.fetch_add(1, std::memory_order_relaxed);
    if (i < kMaxMarks)
        mark(i).store(qpc(), std::memory_order_relaxed);
}

bool cpu_profile_running() { return state.running; }

void cpu_profile_wait() {
    std::thread worker;
    {
        std::lock_guard guard(state.mutex);
        worker.swap(state.worker);
    }
    if (worker.joinable())
        worker.join();
}

std::wstring cpu_profile_last_report() {
    std::lock_guard guard(state.mutex);
    return state.lastReport;
}

}  // namespace simqvfr
