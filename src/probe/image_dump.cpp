// Removed from this repository on 2026-11-12 (image_dump.h): a stub that does nothing.
#include "image_dump.h"

namespace simqvfr {

void image_dump_if_packed() {}

ImageDump image_dump_write(const wchar_t*, const void*, const wchar_t*, bool) {
    ImageDump d;
    d.why = "removed from this repository";
    return d;
}

}  // namespace simqvfr
