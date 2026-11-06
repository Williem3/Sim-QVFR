// Offline check of the SPIR-V edit applied to X-Plane's final resolve shader (src/games/xplane/spirv_patch.h) on a
// small hand-built module. With SIM_QVFR_SPV_IN set to a .spv file (X-Plane's shader, from a RenderDoc capture; not
// in the repo), it also edits that and writes SIM_QVFR_SPV_OUT, for the Vulkan SDK's spirv-val.
#include <algorithm>
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

    {
        // %1 int; %2 = 2; %3 = int[2]; %4 = struct { int, int[2] }; %5 Uniform ptr to %4; %6 Uniform var "u_t_var";
        // %7 = 1 (member index); %8 Uniform ptr to int; %11 Input ptr to int; %12 Input var; %13 = OpLoad %12;
        // %14 = OpAccessChain %8 %6 %7 %13 (an indexed access into the two-entry member).
        std::vector<std::uint32_t> m = {0x07230203, 0x00010300, 0, 15, 0};
        m.insert(m.end(), {op(5, 4), 6, 0x745F5F75 /* "u__t" */, 0x00726176 /* "var" */});
        m.insert(m.end(), {op(21, 4), 1, 32, 1});
        m.insert(m.end(), {op(43, 4), 1, 2, 2});
        m.insert(m.end(), {op(28, 4), 3, 1, 2});
        m.insert(m.end(), {op(30, 4), 4, 1, 3});
        m.insert(m.end(), {op(32, 4), 5, 2, 4});
        m.insert(m.end(), {op(59, 4), 5, 6, 2});
        m.insert(m.end(), {op(43, 4), 1, 7, 1});
        m.insert(m.end(), {op(32, 4), 8, 2, 1});
        m.insert(m.end(), {op(32, 4), 11, 1, 1});
        m.insert(m.end(), {op(59, 4), 11, 12, 1});
        m.insert(m.end(), {op(61, 4), 1, 13, 12});
        m.insert(m.end(), {op(65, 6), 8, 14, 6, 7, 13});
        m.insert(m.end(), {op(65, 6), 8, 10, 6, 7, 7});  // a fixed index 1: left alone
        std::vector<std::uint32_t> folded = m;
        int skipped = -1;
        const int n = xplane::spirv_fold_eye_arrays(folded, {"u__tvar"}, &skipped);
        check(n == 1 && skipped == 0, "one indexed access into the block's two-entry member edited, the fixed one left");
        // Expected: constant %15 = 1 right after the int type; before the access chain %16 = OpBitwiseAnd %1 %13 %15,
        // and the chain indexes with %16; bound 17.
        std::vector<std::uint32_t> want2 = {0x07230203, 0x00010300, 0, 17, 0};
        want2.insert(want2.end(), {op(5, 4), 6, 0x745F5F75, 0x00726176});
        want2.insert(want2.end(), {op(21, 4), 1, 32, 1});
        want2.insert(want2.end(), {op(43, 4), 1, 15, 1});
        want2.insert(want2.end(), {op(43, 4), 1, 2, 2});
        want2.insert(want2.end(), {op(28, 4), 3, 1, 2});
        want2.insert(want2.end(), {op(30, 4), 4, 1, 3});
        want2.insert(want2.end(), {op(32, 4), 5, 2, 4});
        want2.insert(want2.end(), {op(59, 4), 5, 6, 2});
        want2.insert(want2.end(), {op(43, 4), 1, 7, 1});
        want2.insert(want2.end(), {op(32, 4), 8, 2, 1});
        want2.insert(want2.end(), {op(32, 4), 11, 1, 1});
        want2.insert(want2.end(), {op(59, 4), 11, 12, 1});
        want2.insert(want2.end(), {op(61, 4), 1, 13, 12});
        want2.insert(want2.end(), {op(199, 5), 1, 16, 13, 15});
        want2.insert(want2.end(), {op(65, 6), 8, 14, 6, 7, 16});
        want2.insert(want2.end(), {op(65, 6), 8, 10, 6, 7, 7});
        check(folded == want2, "index & 1 before the access chain, a new constant 1 after its type, bound + 2");
        std::vector<std::uint32_t> untouched = m;
        check(xplane::spirv_fold_eye_arrays(untouched, {"u_other_var"}, nullptr) == 0 && untouched == m,
              "a block not named: left alone");
    }

    {
        // The cloud render's idiom: %1 uint, %2 int, %3 bool, %4 = uint 2, %5 = uint 1, %6 = int 1, %7 = int 0,
        // OpDecorate %10 BuiltIn GlobalInvocationId; %11 = OpAccessChain %ptr %10 %4; %12 = OpLoad %1 %11;
        // %13 = OpIEqual %3 %12 %5; %14 = OpSelect %2 %13 %6 %7; %15 = OpCompositeConstruct %int3 %20 %21 %14.
        std::vector<std::uint32_t> m = {0x07230203, 0x00010300, 0, 30, 0};
        m.insert(m.end(), {op(71, 4), 10, 11, 28});
        m.insert(m.end(), {op(21, 4), 1, 32, 0});
        m.insert(m.end(), {op(21, 4), 2, 32, 1});
        m.insert(m.end(), {op(43, 4), 1, 4, 2});
        m.insert(m.end(), {op(43, 4), 1, 5, 1});
        m.insert(m.end(), {op(43, 4), 2, 6, 1});
        m.insert(m.end(), {op(43, 4), 2, 7, 0});
        m.insert(m.end(), {op(65, 5), 9, 11, 10, 4});
        m.insert(m.end(), {op(61, 4), 1, 12, 11});
        m.insert(m.end(), {op(170, 5), 3, 13, 12, 5});
        m.insert(m.end(), {op(169, 6), 2, 14, 13, 6, 7});
        m.insert(m.end(), {op(80, 6), 8, 15, 20, 21, 14});
        std::vector<std::uint32_t> e = m;
        check(xplane::spirv_layer_from_dispatch_z(e), "cloud idiom found");
        // Expected: after the load, %30 = OpBitwiseAnd %1 %12 %5 and %31 = OpBitcast %2 %12; the IEqual tests %30; the
        // construct's last component is %31; bound 32.
        std::vector<std::uint32_t> want3 = {0x07230203, 0x00010300, 0, 32, 0};
        want3.insert(want3.end(), {op(71, 4), 10, 11, 28});
        want3.insert(want3.end(), {op(21, 4), 1, 32, 0});
        want3.insert(want3.end(), {op(21, 4), 2, 32, 1});
        want3.insert(want3.end(), {op(43, 4), 1, 4, 2});
        want3.insert(want3.end(), {op(43, 4), 1, 5, 1});
        want3.insert(want3.end(), {op(43, 4), 2, 6, 1});
        want3.insert(want3.end(), {op(43, 4), 2, 7, 0});
        want3.insert(want3.end(), {op(65, 5), 9, 11, 10, 4});
        want3.insert(want3.end(), {op(61, 4), 1, 12, 11});
        want3.insert(want3.end(), {op(199, 5), 1, 30, 12, 5});
        want3.insert(want3.end(), {op(124, 4), 2, 31, 12});
        want3.insert(want3.end(), {op(170, 5), 3, 13, 30, 5});
        want3.insert(want3.end(), {op(169, 6), 2, 14, 13, 6, 7});
        want3.insert(want3.end(), {op(80, 6), 8, 15, 20, 21, 31});
        check(e == want3, "eye test on z & 1, written layer = z");
        std::vector<std::uint32_t> none(m.begin(), m.begin() + 9);  // no GlobalInvocationId use
        none[3] = 30;
        check(!xplane::spirv_layer_from_dispatch_z(none), "no idiom: left alone");
    }

    // The light billboards' size: in the block that reads blk.inv[i], the xf.proj[i] before it takes i & 1; a later
    // block's xf.proj[i] (the placement) is left as it is.
    {
        auto str = [](std::vector<std::uint32_t>& v, const char* s) {
            const std::size_t n = std::strlen(s) / 4 + 1;
            std::vector<std::uint32_t> words(n, 0);
            std::memcpy(words.data(), s, std::strlen(s));
            v.insert(v.end(), words.begin(), words.end());
        };
        std::vector<std::uint32_t> m = {0x07230203, 0x00010300, 0, 30, 0};
        m.push_back(op(5, 2 + 1)); m.push_back(10); str(m, "blk");
        m.push_back(op(5, 2 + 1)); m.push_back(20); str(m, "xf");
        m.push_back(op(6, 3 + 1)); m.insert(m.end(), {10, 0}); str(m, "inv");
        m.push_back(op(6, 3 + 2)); m.insert(m.end(), {20, 0}); str(m, "proj");
        m.insert(m.end(), {op(21, 4), 2, 32, 1, op(22, 3), 3, 32, op(23, 4), 4, 3, 4, op(24, 4), 5, 4, 4});
        m.insert(m.end(), {op(43, 4), 2, 6, 2, op(43, 4), 2, 7, 0, op(43, 4), 2, 9, 4});
        m.insert(m.end(), {op(28, 4), 8, 5, 6, op(28, 4), 11, 5, 9, op(30, 3), 10, 8, op(30, 3), 20, 11});
        m.insert(m.end(), {op(32, 4), 12, 2, 10, op(32, 4), 21, 2, 20, op(32, 4), 13, 2, 5, op(32, 4), 15, 1, 2});
        m.insert(m.end(), {op(59, 4), 12, 14, 2, op(59, 4), 21, 22, 2, op(59, 4), 15, 16, 1});
        m.insert(m.end(), {op(248, 2), 40, op(61, 4), 2, 17, 16});
        m.insert(m.end(), {op(65, 6), 13, 23, 22, 7, 17, op(61, 4), 5, 24, 23});  // the size's projection
        m.insert(m.end(), {op(65, 6), 13, 25, 14, 7, 17, op(61, 4), 5, 26, 25});  // the inverse
        m.insert(m.end(), {op(248, 2), 41, op(65, 6), 13, 27, 22, 7, 17, op(61, 4), 5, 28, 27});  // the placement
        const std::size_t sizeBefore = m.size();
        const int n = xplane::spirv_eye_projection_beside(m, "blk", "inv", "xf", "proj");
        // A constant 1 (%30) after the int type, %31 = %17 & 1 before the size's access, which takes %31; bound 32.
        auto has = [&](const std::vector<std::uint32_t>& seq) {
            return std::search(m.begin(), m.end(), seq.begin(), seq.end()) != m.end();
        };
        check(n == 1 && m[3] == 32 && m.size() == sizeBefore + 9 && has({op(21, 4), 2, 32, 1, op(43, 4), 2, 30, 1}) &&
                  has({op(199, 5), 2, 31, 17, 30, op(65, 6), 13, 23, 22, 7, 31}) &&
                  has({op(65, 6), 13, 25, 14, 7, 17}) && has({op(65, 6), 13, 27, 22, 7, 17}),
              "light size: the eye's projection beside the inverse, the placement left");
        std::vector<std::uint32_t> none(m.begin(), m.begin() + 5);
        none[3] = 2;
        check(xplane::spirv_eye_projection_beside(none, "blk", "inv", "xf", "proj") == 0, "no blocks: left alone");
    }

    // The final resolve in eye columns: v <= 0 (the lower row) samples v + 1 in layer v_eye + 2.
    {
        auto str = [](std::vector<std::uint32_t>& v, const char* s) {
            const std::size_t n = std::strlen(s) / 4 + 1;
            std::vector<std::uint32_t> words(n, 0);
            std::memcpy(words.data(), s, std::strlen(s));
            v.insert(v.end(), words.begin(), words.end());
        };
        auto build = [&](bool eyeFirst) {
            std::vector<std::uint32_t> m = {0x07230203, 0x00010300, 0, 21, 0};
            m.push_back(op(5, 2 + 2)); m.push_back(5); str(m, "v_eye");
            m.push_back(op(6, 3 + 3)); m.insert(m.end(), {10, 0}); str(m, "u_offset");
            m.push_back(op(6, 3 + 3)); m.insert(m.end(), {10, 1}); str(m, "u_y_offset");
            m.insert(m.end(), {op(21, 4), 2, 32, 1, op(22, 3), 3, 32, op(20, 2), 4});
            m.insert(m.end(), {op(32, 4), 6, 1, 2, op(59, 4), 6, 5, 1, op(43, 4), 2, 7, 1});
            m.insert(m.end(), {op(30, 4), 10, 3, 3, op(32, 4), 11, 2, 10, op(59, 4), 11, 12, 2, op(32, 4), 13, 2, 3});
            if (eyeFirst)
                m.insert(m.end(), {op(61, 4), 2, 18, 5});
            m.insert(m.end(), {op(65, 5), 13, 14, 12, 7, op(61, 4), 3, 15, 14, op(129, 5), 3, 16, 15, 15});
            m.insert(m.end(), {op(131, 5), 3, 17, 15, 16});
            if (!eyeFirst)
                m.insert(m.end(), {op(61, 4), 2, 18, 5});
            m.insert(m.end(), {op(128, 5), 2, 19, 18, 18, op(129, 5), 3, 20, 17, 17});
            return m;
        };
        std::vector<std::uint32_t> m = build(false);
        std::string reason;
        const bool ok = xplane::spirv_eye_columns_resolve(m, &reason);
        // New ids from 21: 0.0 %21, 1.0 %22, 2 %23, y raw %24, row %25, y + 1 %26, v_eye raw %27, v_eye + 2 %28.
        auto has = [&](const std::vector<std::uint32_t>& seq) {
            return std::search(m.begin(), m.end(), seq.begin(), seq.end()) != m.end();
        };
        check(ok && m[3] == 29 && has({op(131, 5), 3, 24, 15, 16, op(188, 5), 4, 25, 24, 21, op(129, 5), 3, 26, 24, 22,
                                       op(169, 6), 3, 17, 25, 26, 24}) &&
                  has({op(61, 4), 2, 27, 5, op(128, 5), 2, 28, 27, 23, op(169, 6), 2, 18, 25, 27, 28}) &&
                  has({op(22, 3), 3, 32, op(43, 4), 3, 21, 0, op(43, 4), 3, 22, 0x3F800000}) &&
                  has({op(21, 4), 2, 32, 1, op(43, 4), 2, 23, 2}),
              "eye columns resolve: lower row v + 1 in layer v_eye, upper row layer v_eye + 2");
        std::vector<std::uint32_t> early = build(true);
        const std::vector<std::uint32_t> earlyCopy = early;
        check(!xplane::spirv_eye_columns_resolve(early, &reason) && early == earlyCopy,
              "v_eye loaded before the row: left alone");
    }

    // SIM_QVFR_SPV_COLUMNS: the final resolve fragment shader (from a capture); writes <file>.columns.spv.
    if (const char* in = std::getenv("SIM_QVFR_SPV_COLUMNS")) {
        std::ifstream file(in, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::vector<std::uint32_t> real(bytes.size() / 4);
        std::memcpy(real.data(), bytes.data(), real.size() * 4);
        std::string reason;
        const bool ok = xplane::spirv_eye_columns_resolve(real, &reason);
        check(ok, "edits the final resolve for eye columns");
        if (!ok)
            std::printf("     %s\n", reason.c_str());
        std::ofstream o(std::string(in) + ".columns.spv", std::ios::binary);
        o.write(reinterpret_cast<const char*>(real.data()), static_cast<std::streamsize>(real.size() * 4));
    }

    // SIM_QVFR_SPV_INV: the light billboards' vertex shader (from a capture); writes <file>.inv.spv.
    if (const char* in = std::getenv("SIM_QVFR_SPV_INV")) {
        std::ifstream file(in, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::vector<std::uint32_t> real(bytes.size() / 4);
        std::memcpy(real.data(), bytes.data(), real.size() * 4);
        const int n = xplane::spirv_eye_projection_beside(real, "u_ogl_light_data", "u_proj_inv", "u_transform_block_fat",
                                                          "projection_matrix");
        std::printf("     %s: %d accesses\n", in, n);
        check(n > 0, "edits the light billboards' size projection");
        std::ofstream o(std::string(in) + ".inv.spv", std::ios::binary);
        o.write(reinterpret_cast<const char*>(real.data()), static_cast<std::streamsize>(real.size() * 4));
    }

    // SIM_QVFR_SPV_EYEMAP: a fullscreen vertex shader writing v_eye (from a capture); writes <file>.map.spv.
    if (const char* in = std::getenv("SIM_QVFR_SPV_EYEMAP")) {
        std::ifstream file(in, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::vector<std::uint32_t> real(bytes.size() / 4);
        std::memcpy(real.data(), bytes.data(), real.size() * 4);
        std::string reason;
        const bool ok = xplane::spirv_view_to_eye_map_out(real, 15, &reason);
        check(ok, "a vertex shader passes the view-to-eye map");
        if (!ok)
            std::printf("     %s\n", reason.c_str());
        std::vector<std::uint32_t> again = real;
        check(!xplane::spirv_view_to_eye_map_out(again, 15, &reason) && again == real, "a second time: left alone");
        std::ofstream o(std::string(in) + ".map.spv", std::ios::binary);
        o.write(reinterpret_cast<const char*>(real.data()), static_cast<std::streamsize>(real.size() * 4));
    }

    // SIM_QVFR_SPV_BLOOM: the bloom apply fragment shader (from a capture); writes <file>.bloom.spv.
    if (const char* in = std::getenv("SIM_QVFR_SPV_BLOOM")) {
        std::ifstream file(in, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::vector<std::uint32_t> real(bytes.size() / 4);
        std::memcpy(real.data(), bytes.data(), real.size() * 4);
        std::string reason;
        const bool ok = xplane::spirv_sample_eye_box(real, 15, &reason);
        check(ok, "the bloom apply samples its eye's bloom");
        if (!ok)
            std::printf("     %s\n", reason.c_str());
        std::ofstream o(std::string(in) + ".bloom.spv", std::ios::binary);
        o.write(reinterpret_cast<const char*>(real.data()), static_cast<std::streamsize>(real.size() * 4));
    }

    // The shadows' right-eye test (spirv_eye_parity_test): %3 = struct { int; int u_eye_right_to_left } named
    // u_shadow_data, read at member 1; v_eye loaded and tested == 1.
    {
        const auto named = [](std::vector<std::uint32_t>& w, std::uint16_t code, std::initializer_list<std::uint32_t> head,
                              const char* s) {
            std::vector<std::uint32_t> text((std::strlen(s) + 4) / 4, 0);
            std::memcpy(text.data(), s, std::strlen(s));
            w.push_back(op(code, static_cast<std::uint16_t>(1 + head.size() + text.size())));
            w.insert(w.end(), head);
            w.insert(w.end(), text.begin(), text.end());
        };
        const auto shadowModule = [&](std::uint32_t readMember) {
            std::vector<std::uint32_t> w = {0x07230203, 0x00010300, 0, 14, 0};
            named(w, 5, {3}, "u_shadow_data");
            named(w, 6, {3, 1}, "u_eye_right_to_left");
            named(w, 5, {7}, "v_eye");
            w.insert(w.end(), {op(21, 4), 1, 32, 1});         // %1 int
            w.insert(w.end(), {op(20, 2), 2});                // %2 bool
            w.insert(w.end(), {op(30, 4), 3, 1, 1});          // %3 struct
            w.insert(w.end(), {op(32, 4), 4, 2, 3});          // %4 Uniform pointer to it
            w.insert(w.end(), {op(59, 4), 4, 5, 2});          // %5 the block
            w.insert(w.end(), {op(32, 4), 6, 1, 1});          // %6 Input pointer to int
            w.insert(w.end(), {op(59, 4), 6, 7, 1});          // %7 v_eye
            w.insert(w.end(), {op(43, 4), 1, 8, 1});          // %8 = 1
            w.insert(w.end(), {op(43, 4), 1, 12, 0});         // %12 = 0
            w.insert(w.end(), {op(32, 4), 9, 2, 1});          // %9 Uniform pointer to int
            w.insert(w.end(), {op(65, 5), 9, 10, 5, readMember == 1 ? 8u : 12u});  // %10 = &block.member
            w.insert(w.end(), {op(61, 4), 1, 11, 7});         // %11 = v_eye
            w.insert(w.end(), {op(170, 5), 2, 13, 11, 8});   // %13 = OpIEqual %2 %11 %8
            return w;
        };
        auto m = shadowModule(1);
        const std::size_t shadowBefore = m.size();
        const int n = xplane::spirv_eye_parity_test(m, "u_shadow_data", "u_eye_right_to_left", "v_eye");
        check(n == 1 && m.size() == shadowBefore + 5, "shadow eye test: one test edited, one instruction added");
        // The new %14 = OpBitwiseAnd %1 %11 %8 sits before the test, and the test compares %14.
        check(m[3] == 15 && m[shadowBefore - 5] == op(199, 5) && m[shadowBefore - 4] == 1 && m[shadowBefore - 3] == 14 &&
                  m[shadowBefore - 2] == 11 && m[shadowBefore - 1] == 8 && m[m.size() - 2] == 14 && m[m.size() - 1] == 8,
              "shadow eye test: compares v_eye & 1");
        auto shadowOther = shadowModule(0);
        const auto shadowOtherCopy = shadowOther;
        check(xplane::spirv_eye_parity_test(shadowOther, "u_shadow_data", "u_eye_right_to_left", "v_eye") == 0 &&
                  shadowOther == shadowOtherCopy,
              "shadow eye test: a module not reading the member is left alone");
    }

    // The focus projection matrix (spirv_focus_projection_matrix): %7 = struct { mat4 proj[2] } named blk; the input
    // %13 loaded, folded (& 1) and used to read proj[]; %18 = the loaded matrix, used by %19.
    {
        const auto named = [](std::vector<std::uint32_t>& w, std::uint16_t code, std::initializer_list<std::uint32_t> head,
                              const char* s) {
            std::vector<std::uint32_t> text((std::strlen(s) + 4) / 4, 0);
            std::memcpy(text.data(), s, std::strlen(s));
            w.push_back(op(code, static_cast<std::uint16_t>(1 + head.size() + text.size())));
            w.insert(w.end(), head);
            w.insert(w.end(), text.begin(), text.end());
        };
        std::vector<std::uint32_t> pm = {0x07230203, 0x00010300, 0, 20, 0};
        named(pm, 5, {7}, "blk");
        named(pm, 6, {7, 0}, "proj");
        pm.insert(pm.end(), {op(22, 3), 1, 32});              // %1 float
        pm.insert(pm.end(), {op(23, 4), 2, 1, 4});            // %2 vec4
        pm.insert(pm.end(), {op(24, 4), 3, 2, 4});            // %3 mat4
        pm.insert(pm.end(), {op(21, 4), 4, 32, 1});           // %4 int
        pm.insert(pm.end(), {op(43, 4), 4, 5, 2});            // %5 = 2
        pm.insert(pm.end(), {op(28, 4), 6, 3, 5});            // %6 mat4[2]
        pm.insert(pm.end(), {op(30, 3), 7, 6});               // %7 struct
        pm.insert(pm.end(), {op(32, 4), 8, 2, 7});            // %8 Uniform pointer to it
        pm.insert(pm.end(), {op(59, 4), 8, 9, 2});            // %9 the block
        pm.insert(pm.end(), {op(43, 4), 4, 10, 0});           // %10 = 0
        pm.insert(pm.end(), {op(43, 4), 4, 11, 1});           // %11 = 1
        pm.insert(pm.end(), {op(32, 4), 12, 1, 4});           // %12 Input pointer to int
        pm.insert(pm.end(), {op(59, 4), 12, 13, 1});          // %13 the view index
        pm.insert(pm.end(), {op(32, 4), 14, 2, 3});           // %14 Uniform pointer to mat4
        pm.insert(pm.end(), {op(61, 4), 4, 15, 13});          // %15 = load %13
        pm.insert(pm.end(), {op(199, 5), 4, 16, 15, 11});     // %16 = %15 & 1 (the fold)
        pm.insert(pm.end(), {op(65, 6), 14, 17, 9, 10, 16});  // %17 = &blk.proj[%16]
        pm.insert(pm.end(), {op(61, 4), 3, 18, 17});          // %18 = load %17
        pm.insert(pm.end(), {op(81, 5), 2, 19, 18, 0});       // %19 = column 0 of %18
        const auto pmCopy = pm;
        auto pmOther = pm;
        const int n = xplane::spirv_focus_projection_matrix(pm, "blk", "proj", 0.2f, 0.2f);
        // %18 is now a matrix built from four selected columns; the load got a new id; %19 still reads %18.
        bool built = false, loadRenamed = false, useKept = false;
        for (std::size_t i = 5; i < pm.size();) {
            const std::uint16_t count = static_cast<std::uint16_t>(pm[i] >> 16), code = pm[i] & 0xffff;
            if (code == 80 && count == 7 && pm[i + 1] == 3 && pm[i + 2] == 18)
                built = true;
            if (code == 61 && count == 4 && pm[i + 1] == 3 && pm[i + 3] == 17)
                loadRenamed = pm[i + 2] != 18;
            if (code == 81 && count == 5 && pm[i + 2] == 19)
                useKept = pm[i + 3] == 18;
            i += count ? count : 1;
        }
        check(n == 1 && built && loadRenamed && useKept, "focus projection: the loaded matrix is rebuilt for views 2-3");
        check(xplane::spirv_focus_projection_matrix(pmOther, "pmOther", "proj", 0.2f, 0.2f) == 0 && pmOther == pmCopy,
              "focus projection: another block is left alone");
    }

    // The alpha-test mip bias and the layer output leave modules without their pieces alone (the v_eye module above has
    // neither an alpha test nor gl_Layer).
    {
        auto plain = module(false);
        const auto plainCopy = plain;
        std::string reason;
        check(xplane::spirv_alpha_test_lod_bias(plain, 20, -1.0f, &reason) == 0 && plain == plainCopy,
              "alpha mip bias: a module without an alpha test is left alone");
        check(!xplane::spirv_layer_out(plain, 20) && plain == plainCopy, "layer output: a module without gl_Layer is left alone");
        check(xplane::spirv_ray_reads_from_eye(plain, {"tex_hiZ"}) == 0 && plain == plainCopy,
              "SSR trace: a module without the images is left alone");
        check(xplane::spirv_focus_light_tiles(plain, 0.3f, 0.3f, &reason) == 0 && plain == plainCopy,
              "light tiles: a module without them is left alone");
    }

    // SIM_QVFR_SPV_TILES: deferred shading shaders with light tiles (from a capture, ';'-separated); each written to
    // <file>.tiles.spv.
    if (const char* list = std::getenv("SIM_QVFR_SPV_TILES")) {
        std::string all = list;
        for (std::size_t start = 0; start < all.size();) {
            std::size_t end = all.find(';', start);
            if (end == std::string::npos)
                end = all.size();
            const std::string in = all.substr(start, end - start);
            start = end + 1;
            std::ifstream file(in, std::ios::binary);
            std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            std::vector<std::uint32_t> real(bytes.size() / 4);
            std::memcpy(real.data(), bytes.data(), real.size() * 4);
            std::string reason;
            const int n = xplane::spirv_focus_light_tiles(real, 0.3f, 0.3f, &reason);
            std::printf("     %s: %d%s%s\n", in.c_str(), n, n ? "" : " - ", n ? "" : reason.c_str());
            check(n == 2, "light tiles: both coordinates of the tile index edited");
            std::ofstream o(in + ".tiles.spv", std::ios::binary);
            o.write(reinterpret_cast<const char*>(real.data()), static_cast<std::streamsize>(real.size() * 4));
        }
    }

    // SIM_QVFR_SPV_ALPHA: alpha-tested fragment shaders (from a capture, ';'-separated), each written to
    // <file>.alpha.spv; SIM_QVFR_SPV_LAYER: vertex shaders writing gl_Layer, each written to <file>.layer.spv.
    for (const char* var : {"SIM_QVFR_SPV_ALPHA", "SIM_QVFR_SPV_LAYER"}) {
        const char* list = std::getenv(var);
        if (!list)
            continue;
        const bool alpha = std::strcmp(var, "SIM_QVFR_SPV_ALPHA") == 0;
        std::string all = list;
        for (std::size_t start = 0; start < all.size();) {
            std::size_t end = all.find(';', start);
            if (end == std::string::npos)
                end = all.size();
            const std::string in = all.substr(start, end - start);
            start = end + 1;
            std::ifstream file(in, std::ios::binary);
            std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            std::vector<std::uint32_t> real(bytes.size() / 4);
            std::memcpy(real.data(), bytes.data(), real.size() * 4);
            std::string reason;
            const int n = alpha ? xplane::spirv_alpha_test_lod_bias(real, 20, -1.737f, &reason)
                                : (xplane::spirv_layer_out(real, 20) ? 1 : 0);
            std::printf("     %s: %d%s%s\n", in.c_str(), n, n ? "" : " - ", n ? "" : reason.c_str());
            check(n >= 1, alpha ? "alpha mip bias: edits the given shader" : "layer output: edits the given shader");
            std::ofstream o(in + (alpha ? ".alpha.spv" : ".layer.spv"), std::ios::binary);
            o.write(reinterpret_cast<const char*>(real.data()), static_cast<std::streamsize>(real.size() * 4));
        }
    }

    // SIM_QVFR_SPV_SSRTRACE: SSR compute shaders (from a capture, ';'-separated): the reads along the ray (hi-Z, the
    // lit history, velocities) take the eye's layer; each written to <file>.trace.spv.
    if (const char* list = std::getenv("SIM_QVFR_SPV_SSRTRACE")) {
        std::string all = list;
        for (std::size_t start = 0; start < all.size();) {
            std::size_t end = all.find(';', start);
            if (end == std::string::npos)
                end = all.size();
            const std::string in = all.substr(start, end - start);
            start = end + 1;
            std::ifstream file(in, std::ios::binary);
            std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            std::vector<std::uint32_t> real(bytes.size() / 4);
            std::memcpy(real.data(), bytes.data(), real.size() * 4);
            const int n = xplane::spirv_ray_reads_from_eye(real, {"tex_hiZ", "tex_ssr", "gbuffer_vel"});
            const int starts = xplane::spirv_ray_start_at_eye_pixel(real);
            std::printf("     %s: %d reads, ray start at the eye pixel %d\n", in.c_str(), n, starts);
            check(n == 3, "SSR trace: hi-Z, history and velocity reads take the eye's layer");
            check(starts == 1, "SSR trace: the focus views' rays start at their eye pixel's centre and depth");
            std::ofstream o(in + ".trace.spv", std::ios::binary);
            o.write(reinterpret_cast<const char*>(real.data()), static_cast<std::streamsize>(real.size() * 4));
        }
    }

    // SIM_QVFR_SPV_AOEDGE: deferred shading from a capture (';'-separated): the focus views' AO at their edges from the
    // eye; each written to <file>.aoedge.spv.
    if (const char* list = std::getenv("SIM_QVFR_SPV_AOEDGE")) {
        std::string all = list;
        for (std::size_t start = 0; start < all.size();) {
            std::size_t end = all.find(';', start);
            if (end == std::string::npos)
                end = all.size();
            const std::string in = all.substr(start, end - start);
            start = end + 1;
            std::ifstream file(in, std::ios::binary);
            std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            std::vector<std::uint32_t> real(bytes.size() / 4);
            std::memcpy(real.data(), bytes.data(), real.size() * 4);
            std::string aoWhy;
            const int n = xplane::spirv_focus_ao_edges_from_eye(real, 0.3f, 0.3f, 0.12f, &aoWhy);
            std::printf("     %s: %d AO samples%s%s\n", in.c_str(), n, aoWhy.empty() ? "" : ": ", aoWhy.c_str());
            check(n >= 1, "AO edges: the focus views' AO comes from the eye at their edges");
            std::ofstream o(in + ".aoedge.spv", std::ios::binary);
            o.write(reinterpret_cast<const char*>(real.data()), static_cast<std::streamsize>(real.size() * 4));
        }
    }

    // SIM_QVFR_SPV_SSR: shaders reading u_ssr_data.u_proj (from a capture, ';'-separated); each writes <file>.ssr.spv.
    if (const char* list = std::getenv("SIM_QVFR_SPV_SSR")) {
        std::string all = list;
        for (std::size_t start = 0; start < all.size();) {
            std::size_t end = all.find(';', start);
            if (end == std::string::npos)
                end = all.size();
            const std::string in = all.substr(start, end - start);
            start = end + 1;
            std::ifstream file(in, std::ios::binary);
            std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            std::vector<std::uint32_t> real(bytes.size() / 4);
            std::memcpy(real.data(), bytes.data(), real.size() * 4);
            const int n = xplane::spirv_focus_projection_matrix(real, "u_ssr_data", "u_proj", 0.2f, 0.2f);
            std::printf("     %s: %d loads\n", in.c_str(), n);
            check(n >= 1, "focus projection: edits the given SSR shader");
            std::ofstream o(in + ".ssr.spv", std::ios::binary);
            o.write(reinterpret_cast<const char*>(real.data()), static_cast<std::streamsize>(real.size() * 4));
        }
    }

    // SIM_QVFR_SPV_SHADOW: shaders reading u_shadow_data.u_eye_right_to_left (from a capture, ';'-separated); each
    // writes <file>.shadow.spv.
    if (const char* list = std::getenv("SIM_QVFR_SPV_SHADOW")) {
        std::string all = list;
        for (std::size_t start = 0; start < all.size();) {
            std::size_t end = all.find(';', start);
            if (end == std::string::npos)
                end = all.size();
            const std::string in = all.substr(start, end - start);
            start = end + 1;
            std::ifstream file(in, std::ios::binary);
            std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            std::vector<std::uint32_t> real(bytes.size() / 4);
            std::memcpy(real.data(), bytes.data(), real.size() * 4);
            const int n = xplane::spirv_eye_parity_test(real, "u_shadow_data", "u_eye_right_to_left", "v_eye");
            std::printf("     %s: %d tests\n", in.c_str(), n);
            check(n >= 1, "shadow eye test: edits the given shader");
            std::ofstream o(in + ".shadow.spv", std::ios::binary);
            o.write(reinterpret_cast<const char*>(real.data()), static_cast<std::streamsize>(real.size() * 4));
        }
    }

    // SIM_QVFR_SPV_VRS: the shading-rate compute shader (from a capture); writes <file>.vrs.spv.
    if (const char* in = std::getenv("SIM_QVFR_SPV_VRS")) {
        std::ifstream file(in, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::vector<std::uint32_t> real(bytes.size() / 4);
        std::memcpy(real.data(), bytes.data(), real.size() * 4);
        std::string reason;
        const int n = xplane::spirv_full_rate_from_layer(real, 2, &reason);
        check(n == 1, "the shading-rate shader writes the full rate in layers 2-3");
        if (n != 1)
            std::printf("     %d: %s\n", n, reason.c_str());
        std::ofstream o(std::string(in) + ".vrs.spv", std::ios::binary);
        o.write(reinterpret_cast<const char*>(real.data()), static_cast<std::streamsize>(real.size() * 4));
    }

    // SIM_QVFR_SPV_COEF: shaders reading the folded u_proj_coefficients (from a capture, ';'-separated); each
    // writes <file>.coef.spv.
    if (const char* list = std::getenv("SIM_QVFR_SPV_COEF")) {
        std::string all = list;
        for (std::size_t start = 0; start < all.size();) {
            std::size_t end = all.find(';', start);
            if (end == std::string::npos)
                end = all.size();
            const std::string in = all.substr(start, end - start);
            start = end + 1;
            std::ifstream file(in, std::ios::binary);
            std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            std::vector<std::uint32_t> real(bytes.size() / 4);
            std::memcpy(real.data(), bytes.data(), real.size() * 4);
            int skipped = 0;
            const int n = xplane::spirv_focus_proj_coefficients(real, "u_proj_coefficients", 0.2f, 0.2f, &skipped);
            std::printf("     %s: %d loads, %d component reads\n", in.c_str(), n, skipped);
            check(n >= 0, "focus coefficients: coverage reported");
            std::ofstream o(in + ".coef.spv", std::ios::binary);
            o.write(reinterpret_cast<const char*>(real.data()), static_cast<std::streamsize>(real.size() * 4));
        }
    }

    // SIM_QVFR_SPV_CLOUD: the cloud render compute shader (from a capture); writes <file>.edited.spv.
    if (const char* in = std::getenv("SIM_QVFR_SPV_CLOUD")) {
        std::ifstream file(in, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::vector<std::uint32_t> real(bytes.size() / 4);
        std::memcpy(real.data(), bytes.data(), real.size() * 4);
        check(xplane::spirv_layer_from_dispatch_z(real), "edits the cloud render shader");
        std::ofstream o(std::string(in) + ".edited.spv", std::ios::binary);
        o.write(reinterpret_cast<const char*>(real.data()), static_cast<std::streamsize>(real.size() * 4));
    }

    // SIM_QVFR_SPV_FOLD: a .spv using the per-eye blocks (from a capture); writes <file>.folded.spv.
    if (const char* in = std::getenv("SIM_QVFR_SPV_FOLD")) {
        std::ifstream file(in, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::vector<std::uint32_t> real(bytes.size() / 4);
        std::memcpy(real.data(), bytes.data(), real.size() * 4);
        int skipped = 0;
        const int n = xplane::spirv_fold_eye_arrays(
            real,
            {"u_misc_data_var", "u_new_sky_data_var", "u_gbuffer_data_var", "u_dome_data_var", "u_particle_data_var",
             "u_ocean_shading_data_var", "u_line3d_data_var", "u_ocean_meta_data_var", "u_ssr_data_var",
             "u_ogl_light_data_var", "u_cloud_upscale_data_var", "u_volumetric_apply_data_var", "u_rain_draw_info_var"},
            &skipped);
        std::printf("     %s: %d accesses folded, %d skipped\n", in, n, skipped);
        check(n > 0 && skipped == 0, "folds the given shader's per-eye accesses");
        std::ofstream o(std::string(in) + ".folded.spv", std::ios::binary);
        o.write(reinterpret_cast<const char*>(real.data()), static_cast<std::streamsize>(real.size() * 4));
    }

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
