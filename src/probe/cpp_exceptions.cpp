#include "cpp_exceptions.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>

#include <windows.h>

#include "../log.h"

namespace simqvfr {
namespace {

std::atomic<int> logged{0};
constexpr int kMostLines = 40;

// An MSVC C++ throw (0xE06D7363): parameters 1 the object, 2 its ThrowInfo, 3 the throwing module's base. The
// ThrowInfo's catchable types name the object's classes (the 32-bit fields are offsets from the base on x64); where
// one of them is std::exception, its what() is the message.
void describe(const EXCEPTION_RECORD* r, char* out, size_t size) {
    const auto base = static_cast<uintptr_t>(r->ExceptionInformation[3]);
    const auto* info = reinterpret_cast<const int32_t*>(r->ExceptionInformation[2]);
    auto* object = reinterpret_cast<char*>(r->ExceptionInformation[1]);
    if (!base || !info || !object)
        return;
    const auto* types = reinterpret_cast<const int32_t*>(base + static_cast<uint32_t>(info[3]));
    const int count = types[0];
    size_t at = 0;
    const char* what = nullptr;
    for (int i = 0; i < count && i < 8; ++i) {
        const auto* type = reinterpret_cast<const int32_t*>(base + static_cast<uint32_t>(types[1 + i]));
        const char* name = reinterpret_cast<const char*>(base + static_cast<uint32_t>(type[1])) + 16;
        if (i < 3)
            at += static_cast<size_t>(std::snprintf(out + at, at < size ? size - at : 0, "%s%s", i ? ", " : "", name));
        if (std::strcmp(name, ".?AVexception@std@@") == 0)
            what = reinterpret_cast<std::exception*>(object + type[2])->what();
    }
    if (what && at < size)
        std::snprintf(out + at, size - at, "; message: %.300s", what);
}

LONG CALLBACK handler(EXCEPTION_POINTERS* p) {
    const EXCEPTION_RECORD* r = p->ExceptionRecord;
    if (r->ExceptionCode != 0xE06D7363 || r->NumberParameters < 4 || logged.load() >= kMostLines)
        return EXCEPTION_CONTINUE_SEARCH;
    char text[512] = "(not readable)";
    __try {
        describe(r, text, sizeof(text));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    HMODULE module = nullptr;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(r->ExceptionInformation[3]), &module) &&
        module)
        GetModuleFileNameA(module, path, MAX_PATH);
    const char* file = std::strrchr(path, '\\');
    if (logged.fetch_add(1) < kMostLines)
        logf("cpp exception thrown by %s (thread %lu): %s", file ? file + 1 : path, GetCurrentThreadId(), text);
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void cpp_exceptions_log_start() {
    static std::atomic<bool> started{false};
    if (started.exchange(true))
        return;
    AddVectoredExceptionHandler(1, &handler);
    logf("cpp exceptions: the first %d C++ throws in this process are logged (their classes and message)", kMostLines);
}

}  // namespace simqvfr
