// Removed from this repository on 2026-11-12: the developer's local analysis tools. This stub keeps old commits
// building; it does nothing.
#pragma once

#include <cstddef>
#include <string>

namespace simqvfr {

void image_dump_if_packed();

struct ImageDump {
    int sections = 0;
    std::size_t relocations = 0;
    bool written = false;
    std::string why;
};
ImageDump image_dump_write(const wchar_t* exe, const void* image, const wchar_t* out, bool onlyPacked);

}  // namespace simqvfr
