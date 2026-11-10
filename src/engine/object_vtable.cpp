#include "object_vtable.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <new>

namespace simqvfr {
namespace {

constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
constexpr DWORD kWritable = PAGE_READWRITE | PAGE_EXECUTE_READWRITE;
constexpr DWORD kExecutable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
constexpr std::size_t kMaxSlots = 1024;

struct Region {
    std::uintptr_t begin = 0, end = 0;
    bool contains(std::uintptr_t p, std::size_t size) const { return p >= begin && p + size <= end; }
};

// The committed region holding `p` when its protection has any of `anyOf` (an empty one otherwise).
Region region_of(const void* p, DWORD anyOf) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(mbi.Protect & anyOf))
        return {};
    const auto begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    return {begin, begin + mbi.RegionSize};
}

// The entries from `vtable` on that point at code: the vtable's length (what follows it, the next vtable's RTTI
// locator or other data, isn't code), or more when another vtable follows directly (copied, never called).
std::size_t vtable_length(void* const* vtable) {
    Region table, code;
    std::size_t n = 0;
    for (; n < kMaxSlots; ++n) {
        const auto at = reinterpret_cast<std::uintptr_t>(vtable + n);
        if (!table.contains(at, sizeof(void*)) && !(table = region_of(vtable + n, kReadable)).contains(at, sizeof(void*)))
            break;
        const auto fn = reinterpret_cast<std::uintptr_t>(vtable[n]);
        if (!code.contains(fn, 1) && !(code = region_of(vtable[n], kExecutable)).contains(fn, 1))
            break;
    }
    return n;
}

}  // namespace

bool ObjectVtable::install(void* object, std::size_t minSlots, std::initializer_list<VtableEntry> entries) {
    if (copy_ || !object)
        return false;
    const auto at = reinterpret_cast<std::uintptr_t>(object);
    if (at % sizeof(void*) || !region_of(object, kWritable).contains(at, sizeof(void*)))
        return false;
    auto vptr = static_cast<void** volatile*>(object);
    void** own = *vptr;
    const auto ownAt = reinterpret_cast<std::uintptr_t>(own - 1);
    if (!own || !region_of(own - 1, kReadable).contains(ownAt, 2 * sizeof(void*)))
        return false;
    const std::size_t n = vtable_length(own);
    if (n < minSlots)
        return false;
    for (const VtableEntry& e : entries)
        if (e.slot >= n || !e.replacement || !e.original)
            return false;
    // Pages of its own, read-only once filled.
    const std::size_t bytes = (n + 1) * sizeof(void*);
    auto** block = static_cast<void**>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!block)
        return false;
    std::memcpy(block, own - 1, bytes);
    void** copy = block + 1;
    for (const VtableEntry& e : entries) {
        *e.original = own[e.slot];
        copy[e.slot] = e.replacement;
    }
    DWORD was = 0;
    VirtualProtect(block, bytes, PAGE_READONLY, &was);
    // Published whole: a call on another thread goes through the object's vtable or the finished copy.
    if (InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(vptr), copy, own) != own) {
        VirtualFree(block, 0, MEM_RELEASE);
        return false;
    }
    object_ = vptr;
    own_ = own;
    copy_ = copy;
    slots_ = n;
    return true;
}

bool ObjectVtable::installed() const { return copy_ && object_ && *object_ == copy_; }

void ObjectVtable::remove() {
    if (copy_ && object_)
        InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(object_), own_, copy_);
    object_ = nullptr;
}

}  // namespace simqvfr
