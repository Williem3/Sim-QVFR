// One object's calls routed through our functions without changing any code: the object's vtable pointer (its first
// 8 bytes, in the object's own memory) pointed at a copy of its vtable in which some entries are ours. The module's
// code and its vtable stay as they are, and no executable memory is allocated: the original is called through the
// entry taken from the object's own vtable. Only calls through this object's pointer are seen (and through interfaces
// that share it, as each newer D3D11 device and context interface does), not other objects of its class.
// Only for a vtable that never changes (a module's, like a D3D11 device's): D3D11's immediate context keeps its vtable
// inside itself and rewrites entries as it runs, which a copy would freeze (and slow every call: measured 2026-11-09).
#pragma once

#include <cstddef>
#include <initializer_list>

namespace simqvfr {

struct VtableEntry {
    std::size_t slot;
    void* replacement;
    void** original;  // receives the object's own entry
};

class ObjectVtable {
public:
    // Points `object` at a copy of its vtable with `entries` replaced. The copy holds every entry up to the first that
    // isn't a function (the vtable's end), and the one before the vtable (MSVC's RTTI locator). False and nothing
    // changed: no readable vtable, fewer than `minSlots` entries (callers may use that many), an entry beyond them,
    // or already installed.
    bool install(void* object, std::size_t minSlots, std::initializer_list<VtableEntry> entries);
    // True while the object still points at the copy (something else may have replaced its vtable pointer since).
    bool installed() const;
    // The object pointed back at its own vtable if it still points at the copy. The copy stays allocated: a call may
    // be on its way through it.
    void remove();
    std::size_t slots() const { return slots_; }

private:
    void** volatile* object_ = nullptr;  // the object's vtable pointer
    void** own_ = nullptr;               // its own vtable
    void** copy_ = nullptr;              // our copy (its entry 0; the RTTI locator before it)
    std::size_t slots_ = 0;
};

// The entries of the newest D3D11 device and context interfaces in this SDK (ID3D11Device5, ID3D11DeviceContext4):
// what a copy of the game's device or immediate context vtable must hold at least. From the SDK's C declarations
// (d3d11_vtables.cpp), so a newer SDK raises them.
extern const std::size_t kD3D11DeviceSlots;
extern const std::size_t kD3D11ContextSlots;

}  // namespace simqvfr
