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

    // The light billboards' inverse projection: blk.inv[i] -> inverse(xf.proj[i]).
    {
        auto str = [](std::vector<std::uint32_t>& v, const char* s) {
            const std::size_t n = std::strlen(s) / 4 + 1;
            std::vector<std::uint32_t> words(n, 0);
            std::memcpy(words.data(), s, std::strlen(s));
            v.insert(v.end(), words.begin(), words.end());
        };
        auto build = [&](bool sharedIndex, bool folded) {
            std::vector<std::uint32_t> m = {0x07230203, 0x00010300, 0, 30, 0};
            m.push_back(op(11, 2 + 4)); m.push_back(1); str(m, "GLSL.std.450");
            m.push_back(op(5, 2 + 1)); m.push_back(10); str(m, "blk");
            m.push_back(op(5, 2 + 1)); m.push_back(20); str(m, "xf");
            m.push_back(op(6, 3 + 1)); m.insert(m.end(), {10, 0}); str(m, "inv");
            m.push_back(op(6, 3 + 2)); m.insert(m.end(), {20, 0}); str(m, "proj");
            m.insert(m.end(), {op(72, 4), 10, 0, 5, op(72, 4), 20, 0, 5});  // ColMajor
            m.insert(m.end(), {op(21, 4), 2, 32, 1, op(22, 3), 3, 32, op(23, 4), 4, 3, 4, op(24, 4), 5, 4, 4});
            m.insert(m.end(), {op(43, 4), 2, 6, 2, op(43, 4), 2, 7, 0, op(43, 4), 2, 9, 4, op(43, 4), 2, 18, 1});
            m.insert(m.end(), {op(28, 4), 8, 5, 6, op(28, 4), 11, 5, 9, op(30, 3), 10, 8, op(30, 3), 20, 11});
            m.insert(m.end(), {op(32, 4), 12, 2, 10, op(32, 4), 21, 2, 20, op(32, 4), 13, 2, 5, op(32, 4), 15, 1, 2});
            m.insert(m.end(), {op(59, 4), 12, 14, 2, op(59, 4), 21, 22, 2, op(59, 4), 15, 16, 1});
            m.insert(m.end(), {op(61, 4), 2, 17, 16, op(61, 4), 2, 28, 16});
            m.insert(m.end(), {op(199, 5), 2, 19, 17, 18});  // %19 = %17 & 1 (as folded)
            m.insert(m.end(), {op(65, 6), 13, 23, 22, 7, 17, op(61, 4), 5, 24, 23});
            const std::uint32_t idx = folded ? 19 : (sharedIndex ? 17 : 28);
            m.insert(m.end(), {op(65, 6), 13, 25, 14, 7, idx, op(61, 4), 5, 26, 25});
            return m;
        };
        for (int folded = 0; folded < 2; ++folded) {
            std::vector<std::uint32_t> m = build(true, folded == 1);
            const std::size_t sizeBefore = m.size();
            const int n = xplane::spirv_inverse_from_view_projection(m, "blk", "inv", "xf", "proj");
            // The access now xf.proj[%17], its load %30, then %26 = OpExtInst %5 %1 MatrixInverse %30; bound 31.
            const std::vector<std::uint32_t> tail = {op(65, 6), 13, 25, 22, 7, 17, op(61, 4), 5, 30, 25,
                                                     op(12, 6), 5, 26, 1, 34, 30};
            check(n == 1 && m.size() == sizeBefore + 6 && m[3] == 31 &&
                      std::vector<std::uint32_t>(m.end() - 16, m.end()) == tail,
                  folded ? "inverse from the view projection (index already folded to i & 1)"
                         : "inverse from the view projection");
        }
        std::vector<std::uint32_t> unshared = build(false, false);
        const std::vector<std::uint32_t> copy = unshared;
        check(xplane::spirv_inverse_from_view_projection(unshared, "blk", "inv", "xf", "proj") == 0 && unshared == copy,
              "an index the projection doesn't use: left alone");
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
                  has({op(61, 4), 2, 27, 5, op(128, 5), 2, 28, 27, 23, op(169, 6), 2, 18, 25, 28, 27}) &&
                  has({op(22, 3), 3, 32, op(43, 4), 3, 21, 0, op(43, 4), 3, 22, 0x3F800000}) &&
                  has({op(21, 4), 2, 32, 1, op(43, 4), 2, 23, 2}),
              "eye columns resolve: lower row v + 1, layer v_eye + 2");
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
        const int n = xplane::spirv_inverse_from_view_projection(real, "u_ogl_light_data", "u_proj_inv",
                                                                 "u_transform_block_fat", "projection_matrix");
        std::printf("     %s: %d loads\n", in, n);
        check(n > 0, "edits the light billboards' inverse projection");
        std::ofstream o(std::string(in) + ".inv.spv", std::ios::binary);
        o.write(reinterpret_cast<const char*>(real.data()), static_cast<std::streamsize>(real.size() * 4));
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
