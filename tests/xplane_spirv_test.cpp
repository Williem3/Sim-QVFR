// Offline check of the SPIR-V edit applied to X-Plane's final resolve shader (src/games/xplane/spirv_patch.h) on a
// small hand-built module. With SIM_QVFR_SPV_IN set to a .spv file (X-Plane's shader, from a RenderDoc capture; not
// in the repo), it also edits that and writes SIM_QVFR_SPV_OUT, for the Vulkan SDK's spirv-val.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "games/xplane/spirv_patch.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

std::uint32_t op(std::uint16_t code, std::uint16_t count) { return (static_cast<std::uint32_t>(count) << 16) | code; }

// %2 = OpTypeInt 32 1; %3 = OpTypePointer Input %2; %5 = OpVariable %3 Input ("v_eye"); %6 = OpLoad %2 %5;
// %7 = OpIAdd %2 %6 %6 (a use of the load).
std::vector<std::uint32_t> module(bool secondLoad) {
    std::vector<std::uint32_t> w = {0x07230203, 0x00010300, 0, 8, 0};
    w.insert(w.end(), {op(5, 4), 5, 0x79655F76 /* "v_ey" */, 0x00000065 /* "e" */});
    w.insert(w.end(), {op(21, 4), 2, 32, 1});
    w.insert(w.end(), {op(32, 4), 3, 1, 2});
    w.insert(w.end(), {op(59, 4), 3, 5, 1});
    w.insert(w.end(), {op(61, 4), 2, 6, 5});
    if (secondLoad)
        w.insert(w.end(), {op(61, 4), 2, 4, 5});
    w.insert(w.end(), {op(128, 5), 2, 7, 6, 6});
    return w;
}

}  // namespace

int main() {
    std::vector<std::uint32_t> w = module(false);
    std::string why;
    const bool edited = xplane::spirv_add_to_input(w, "v_eye", 2, &why);
    check(edited, "edits a module with one load of the input");
    // Expected: the constant %8 = 2 after the int type, the load now %9, then %6 = OpIAdd %2 %9 %8, bound 10.
    const std::vector<std::uint32_t> want = {
        0x07230203, 0x00010300, 0, 10, 0,
        op(5, 4), 5, 0x79655F76, 0x00000065,
        op(21, 4), 2, 32, 1,
        op(43, 4), 2, 8, 2,
        op(32, 4), 3, 1, 2,
        op(59, 4), 3, 5, 1,
        op(61, 4), 2, 9, 5,
        op(128, 5), 2, 6, 9, 8,
        op(128, 5), 2, 7, 6, 6,
    };
    check(w == want, "constant after its type, the load renamed, the old id now load + constant, bound + 2");

    std::vector<std::uint32_t> two = module(true);
    const std::vector<std::uint32_t> before = two;
    check(!xplane::spirv_add_to_input(two, "v_eye", 2, &why) && two == before && why == "not loaded exactly once",
          "two loads: left alone");
    std::vector<std::uint32_t> other = module(false);
    check(!xplane::spirv_add_to_input(other, "v_other", 2, &why) && why == "no variable of that name",
          "another name: left alone");

    if (const char* in = std::getenv("SIM_QVFR_SPV_IN")) {
        std::ifstream file(in, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::vector<std::uint32_t> real(bytes.size() / 4);
        std::memcpy(real.data(), bytes.data(), real.size() * 4);
        const bool ok = xplane::spirv_add_to_input(real, "v_eye", 2, &why);
        check(ok, "edits the given shader");
        if (!ok)
            std::printf("     %s\n", why.c_str());
        if (const char* out = std::getenv("SIM_QVFR_SPV_OUT")) {
            std::ofstream o(out, std::ios::binary);
            o.write(reinterpret_cast<const char*>(real.data()), static_cast<std::streamsize>(real.size() * 4));
        }
    }
    std::printf("%s\n", failures ? "RESULT: FAIL" : "RESULT: pass");
    return failures ? 1 : 0;
}
