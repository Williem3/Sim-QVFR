// SPIR-V edits to the screen-space rays (SSR) of the focus views (spirv_patch.h).
#include "spirv_module.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace xplane {

using namespace spirv;

int spirv_ray_reads_from_eye(std::vector<std::uint32_t>& words, const std::vector<std::string>& images) {
    constexpr std::uint16_t kOpSampledImage = 86, kOpImage = 100, kOpConvertSToF = 111, kOpConvertUToF = 112,
                            kOpBitcast = 124, kOpImageSampleImplicitLod = 87, kOpImageSampleExplicitLod = 88,
                            kOpImageFetch = 95, kOpImageGather = 96, kOpImageRead = 98, kOpCompositeConstructCode = 80;
    constexpr std::uint32_t kBuiltIn = 11, kGlobalInvocationId = 28;
    Module m;
    if (!m.parse(words))
        return 0;
    // The image variables by name, and gl_GlobalInvocationID.
    std::set<std::uint32_t> targets;
    for (const std::string& name : images)
        if (const std::uint32_t id = m.named(name.c_str()))
            targets.insert(id);
    std::uint32_t gid = 0;
    for (const auto& w : m.insts)
        if (Module::code(w) == Module::kDecorate && w.size() >= 4 && w[2] == kBuiltIn && w[3] == kGlobalInvocationId)
            gid = w[1];
    if (targets.empty() || !gid)
        return 0;
    std::map<std::uint32_t, std::uint32_t> constValue, typeOf;
    std::map<std::uint32_t, std::size_t> def;
    for (std::size_t i = 0; i < m.insts.size(); ++i) {
        const auto& w = m.insts[i];
        const std::uint16_t c = Module::code(w);
        if (c == kOpConstant && w.size() == 4)
            constValue[w[2]] = w[3];
        if (w.size() >= 3 && (typed_result(c) || c == kOpAccessChain || c == kOpInBoundsAccessChain ||
                              c == kOpSampledImage || c == kOpImage || c == kOpCompositeConstructCode ||
                              (c >= 87 && c <= 98))) {
            def[w[2]] = i;
            typeOf[w[2]] = w[1];
        }
    }
    // z: loads of gl_GlobalInvocationID[2] (uint) and their bitcasts (int); zf: their float conversions.
    std::set<std::uint32_t> zChains, z, zf;
    for (const auto& w : m.insts)
        if ((Module::code(w) == kOpAccessChain || Module::code(w) == kOpInBoundsAccessChain) && w.size() == 5 &&
            w[3] == gid && constValue.count(w[4]) && constValue[w[4]] == 2)
            zChains.insert(w[2]);
    for (const auto& w : m.insts) {
        const std::uint16_t c = Module::code(w);
        if (c == kOpLoad && w.size() >= 4 && zChains.count(w[3]))
            z.insert(w[2]);
        else if (c == kOpBitcast && w.size() == 4 && z.count(w[3]))
            z.insert(w[2]);
        else if ((c == kOpConvertSToF || c == kOpConvertUToF) && w.size() == 4 && z.count(w[3]))
            zf.insert(w[2]);
    }
    if (z.empty())
        return 0;
    // The image variable a read's first operand comes from (through OpSampledImage / OpImage / OpLoad).
    auto image_var = [&](std::uint32_t id) -> std::uint32_t {
        for (int depth = 0; depth < 8 && def.count(id); ++depth) {
            const auto& w = m.insts[def[id]];
            const std::uint16_t c = Module::code(w);
            if (c == kOpSampledImage || c == kOpImage || c == kOpLoad)
                id = w[3];
            else
                break;
        }
        return id;
    };
    struct Edit {
        std::uint32_t read, construct;
    };
    std::vector<Edit> edits;
    for (const auto& w : m.insts) {
        const std::uint16_t c = Module::code(w);
        if ((c != kOpImageSampleImplicitLod && c != kOpImageSampleExplicitLod && c != kOpImageFetch && c != kOpImageGather &&
             c != kOpImageRead) || w.size() < 5 || !targets.count(image_var(w[3])) || !def.count(w[4]))
            continue;
        const auto& coord = m.insts[def[w[4]]];
        if (Module::code(coord) != kOpCompositeConstructCode || coord.size() < 5)
            continue;
        const std::uint32_t last = coord.back();
        if (z.count(last) || zf.count(last))
            edits.push_back({w[2], w[4]});
    }
    if (edits.empty())
        return 0;
    for (const Edit& e : edits) {
        const std::vector<std::uint32_t> coord = m.insts[m.defined_at(e.construct)];
        const std::uint32_t last = coord.back();
        // The layer's integer source and its type (z itself, or the value a float conversion converted).
        std::uint32_t zi = last;
        if (zf.count(last))
            zi = m.insts[m.defined_at(last)][3];
        const std::uint32_t zType = typeOf.count(zi) ? typeOf[zi] : 0;
        if (!zType || m.defined_at(e.read) >= m.insts.size())
            continue;
        const std::uint32_t one = m.constant(zType, 1);
        const std::size_t at = m.defined_at(e.read);  // constants may have moved it
        const std::uint32_t masked = m.fresh(), coordNew = m.fresh();
        std::vector<std::vector<std::uint32_t>> body = {{op(kOpBitwiseAnd, 5), zType, masked, zi, one}};
        std::uint32_t layer = masked;
        if (zf.count(last)) {  // the same conversion (signed or unsigned) to the same float type, of z & 1
            const std::vector<std::uint32_t>& conversion = m.insts[m.defined_at(last)];
            const std::uint32_t asFloat = m.fresh();
            body.push_back({op(Module::code(conversion), 4), conversion[1], asFloat, masked});
            layer = asFloat;
        }
        std::vector<std::uint32_t> construct = coord;
        construct[2] = coordNew;
        construct.back() = layer;
        body.push_back(construct);
        m.insts.insert(m.insts.begin() + static_cast<std::ptrdiff_t>(at), body.begin(), body.end());
        m.insts[at + body.size()][4] = coordNew;  // the read takes the eye's layer
    }
    words = m.emit();
    return static_cast<int>(edits.size());
}

int spirv_ray_start_at_eye_pixel(std::vector<std::uint32_t>& words) {
    constexpr std::uint16_t kOpMemberName = 6, kOpExtInstImport = 11, kOpExtInst = 12, kOpTypeBool = 20,
                            kOpCompositeExtract = 81, kOpConvertSToF = 111, kOpConvertUToF = 112, kOpBitcast = 124,
                            kOpMatrixTimesVector = 145, kOpImageSampleExplicitLod = 88, kOpSelect = 169,
                            kOpUGreaterThanEqual = 174, kOpSGreaterThanEqual = 175, kOpDecorate = 71,
                            kOpReturnValue = 254;
    constexpr std::uint32_t kBuiltIn = 11, kGlobalInvocationId = 28, kUniformConstant = 0, kGlslFloor = 8,
                            kImageOperandLod = 2, kFloatTwo = 0x40000000;
    Module m;
    if (!m.parse(words))
        return 0;
    // GLSL.std.450, gl_GlobalInvocationID, tex_hiZ (and its sampled image type), u_ssr_data's members and variable.
    std::uint32_t glsl = 0, gid = 0;
    for (const auto& w : m.insts) {
        if (Module::code(w) == kOpExtInstImport && w.size() >= 3 && literal(w, 2, w.size()) == "GLSL.std.450")
            glsl = w[1];
        if (Module::code(w) == kOpDecorate && w.size() >= 4 && w[2] == kBuiltIn && w[3] == kGlobalInvocationId)
            gid = w[1];
    }
    std::uint32_t hizType = 0;
    const std::uint32_t hiz = m.variable("tex_hiZ", kUniformConstant, &hizType);
    const std::uint32_t block = m.named("u_ssr_data");
    if (!glsl || !gid || !hiz || !hizType || !block)
        return 0;
    std::map<std::string, std::uint32_t> member;
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpMemberName && w.size() >= 4 && w[1] == block)
            member[literal(w, 3, w.size())] = w[2];
    if (!member.count("u_proj") || !member.count("u_hiz_width") || !member.count("u_hiz_height") ||
        !member.count("u_viewport_to_buffer_ratio"))
        return 0;
    std::uint32_t blockPtr = 0, ssr = 0, intType = 0, ratioType = 0;
    std::map<std::uint32_t, std::uint32_t> constValue;
    for (const auto& w : m.insts) {
        const std::uint16_t c = Module::code(w);
        if (c == kOpTypePointer && w.size() == 4 && w[2] == kUniform && w[3] == block)
            blockPtr = w[1];
        if (c == kOpTypeStruct && w[1] == block && w.size() > 2 + member["u_viewport_to_buffer_ratio"]) {
            intType = w[2 + member["u_hiz_width"]];
            ratioType = w[2 + member["u_viewport_to_buffer_ratio"]];
        }
        if (c == kOpConstant && w.size() == 4)
            constValue[w[2]] = w[3];
    }
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpVariable && w.size() >= 4 && w[1] == blockPtr && w[3] == kUniform)
            ssr = w[2];
    const std::uint32_t boolType = m.find(kOpTypeBool, {}, 1);
    if (!ssr || !intType || !ratioType || !boolType)
        return 0;
    // The dispatch z: a load of gl_GlobalInvocationID[2], or its bitcast to int (signed compares then).
    std::uint32_t z = 0, zType = 0;
    bool zSigned = false;
    {
        std::set<std::uint32_t> chains, loads;
        for (const auto& w : m.insts)
            if (Module::code(w) == kOpAccessChain && w.size() == 5 && w[3] == gid && constValue.count(w[4]) &&
                constValue[w[4]] == 2)
                chains.insert(w[2]);
        for (const auto& w : m.insts) {
            const std::uint16_t c = Module::code(w);
            if (c == kOpLoad && w.size() >= 4 && chains.count(w[3])) {
                loads.insert(w[2]);
                if (!z)
                    z = w[2], zType = w[1];
            } else if (c == kOpBitcast && w.size() == 4 && loads.count(w[3])) {
                z = w[2], zType = w[1], zSigned = true;
                break;
            }
        }
    }
    if (!z)
        return 0;
    // The ray's start and end: M x (P, 1) and M x (P + R, 1), M a load of u_ssr_data.u_proj[...], then each divided by
    // its w (xyz / (w, w, w)).
    std::map<std::uint32_t, std::size_t> def;
    for (std::size_t i = 0; i < m.insts.size(); ++i)
        if (m.insts[i].size() >= 3 && (typed_result(Module::code(m.insts[i])) || Module::code(m.insts[i]) == kOpAccessChain ||
                                       Module::code(m.insts[i]) == kOpMatrixTimesVector ||
                                       Module::code(m.insts[i]) == kOpVectorShuffle ||
                                       Module::code(m.insts[i]) == kOpCompositeConstruct ||
                                       Module::code(m.insts[i]) == kOpFAdd || Module::code(m.insts[i]) == kOpFDiv))
            def[m.insts[i][2]] = i;
    auto inst = [&](std::uint32_t id, std::uint16_t c) -> const std::vector<std::uint32_t>* {
        const auto it = def.find(id);
        if (it == def.end() || Module::code(m.insts[it->second]) != c)
            return nullptr;
        return &m.insts[it->second];
    };
    auto projection_matrix = [&](std::uint32_t id) {
        const auto* load = inst(id, kOpLoad);
        const auto* chain = load ? inst((*load)[3], kOpAccessChain) : nullptr;
        return chain && chain->size() >= 5 && (*chain)[3] == ssr && constValue.count((*chain)[4]) &&
               constValue[(*chain)[4]] == member["u_proj"];
    };
    auto point_of = [&](std::uint32_t vec4) -> std::uint32_t {  // (Q.x, Q.y, Q.z, 1) -> Q
        const auto* c = inst(vec4, kOpCompositeConstruct);
        if (!c || c->size() != 7)
            return 0;
        std::uint32_t q = 0;
        for (std::uint32_t k = 0; k < 3; ++k) {
            const auto* e = inst((*c)[3 + k], kOpCompositeExtract);
            if (!e || e->size() != 5 || (*e)[4] != k || (q && (*e)[3] != q))
                return 0;
            q = (*e)[3];
        }
        return q;
    };
    struct Projected {
        std::uint32_t result, matrix, point;
    };
    std::vector<Projected> projected;
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpMatrixTimesVector && w.size() == 5 && projection_matrix(w[3]))
            if (const std::uint32_t q = point_of(w[4]))
                projected.push_back({w[2], w[3], q});
    // P + R also starts the hit's reprojection (P + R x t, projected much later): the end is the one projected next to
    // the start.
    std::uint32_t start = 0, end = 0;
    std::size_t nearest = ~std::size_t{0};
    for (const Projected& a : projected)
        for (const Projected& b : projected)
            if (a.matrix == b.matrix && &a != &b)
                if (const auto* add = inst(b.point, kOpFAdd); add && ((*add)[3] == a.point || (*add)[4] == a.point)) {
                    const std::size_t ia = def[a.result], ib = def[b.result];
                    const std::size_t apart = ia > ib ? ia - ib : ib - ia;
                    if (apart < nearest)
                        start = a.result, end = b.result, nearest = apart;
                }
    if (!start)
        return 0;
    auto divided = [&](std::uint32_t x) -> std::uint32_t {  // the xyz / (w, w, w) of x
        for (const auto& w : m.insts) {
            if (Module::code(w) != kOpFDiv || w.size() != 5)
                continue;
            const auto* xyz = inst(w[3], kOpVectorShuffle);
            const auto* www = inst(w[4], kOpCompositeConstruct);
            if (!xyz || !www || xyz->size() != 8 || (*xyz)[3] != x || (*xyz)[4] != x || (*xyz)[5] != 0 ||
                (*xyz)[6] != 1 || (*xyz)[7] != 2 || www->size() != 6 || (*www)[3] != (*www)[4] || (*www)[3] != (*www)[5])
                continue;
            const auto* ew = inst((*www)[3], kOpCompositeExtract);
            if (ew && ew->size() == 5 && (*ew)[3] == x && (*ew)[4] == 3)
                return w[2];
        }
        return 0;
    };
    const std::uint32_t startDiv = divided(start), endDiv = divided(end);
    if (!startDiv || !endDiv)
        return 0;
    // Types and constants first (they go in before the function, moving the instructions).
    const std::uint32_t f = m.type_float(), v2 = m.type_vector(f, 2), v3 = m.type_vector(f, 3),
                        v4 = m.type_vector(f, 4), b3 = m.type_vector(boolType, 3);
    const std::uint32_t ptrInt = m.type_pointer(kUniform, intType), ptrRatio = m.type_pointer(kUniform, ratioType);
    const std::uint32_t f0 = m.constant(f, 0), fHalf = m.constant(f, kFloatHalf), f1 = m.constant(f, kFloatOne),
                        f2 = m.constant(f, kFloatTwo), zOne = m.constant(zType, 1), zTwo = m.constant(zType, 2),
                        width = m.constant(intType, member["u_hiz_width"]),
                        height = m.constant(intType, member["u_hiz_height"]),
                        ratioIndex = m.constant(intType, member["u_viewport_to_buffer_ratio"]);
    if (ratioType != v2)
        return 0;
    const std::size_t startAt = m.defined_at(start), endAt = m.defined_at(end), zAt = m.defined_at(z);
    const std::size_t at = std::max(startAt, endAt) + 1;
    if (startAt >= m.insts.size() || endAt >= m.insts.size() || zAt >= at || m.defined_at(startDiv) < at ||
        m.defined_at(endDiv) < at)
        return 0;
    std::vector<std::vector<std::uint32_t>> body;
    auto emit = [&](std::vector<std::uint32_t> w) {
        const std::uint32_t id = m.fresh();
        w[2] = id;
        w[0] = op(static_cast<std::uint16_t>(w[0]), static_cast<std::uint16_t>(w.size()));
        body.push_back(std::move(w));
        return id;
    };
    // The divides of our own (the originals are left unused).
    auto divide = [&](std::uint32_t x) {
        const std::uint32_t xw = emit({kOpCompositeExtract, f, 0, x, 3});
        const std::uint32_t xyz = emit({kOpVectorShuffle, v3, 0, x, x, 0, 1, 2});
        const std::uint32_t www = emit({kOpCompositeConstruct, v3, 0, xw, xw, xw});
        return emit({kOpFDiv, v3, 0, xyz, www});
    };
    const std::uint32_t s0 = divide(start), e0 = divide(end);
    // The eye pixel (hi-Z cell) the start lies in: uv = ndc.xy / 2 + 1/2, cell = floor(uv x size), its centre in uv.
    const std::uint32_t half2 = emit({kOpCompositeConstruct, v2, 0, fHalf, fHalf});
    const std::uint32_t sxy = emit({kOpVectorShuffle, v2, 0, s0, s0, 0, 1});
    const std::uint32_t uv = emit({kOpFAdd, v2, 0, emit({kOpFMul, v2, 0, sxy, half2}), half2});
    const std::uint32_t hizW =
        emit({kOpConvertSToF, f, 0, emit({kOpLoad, intType, 0, emit({kOpAccessChain, ptrInt, 0, ssr, width})})});
    const std::uint32_t hizH =
        emit({kOpConvertSToF, f, 0, emit({kOpLoad, intType, 0, emit({kOpAccessChain, ptrInt, 0, ssr, height})})});
    const std::uint32_t cells = emit({kOpCompositeConstruct, v2, 0, hizW, hizH});
    const std::uint32_t cell = emit({kOpExtInst, v2, 0, glsl, kGlslFloor, emit({kOpFMul, v2, 0, uv, cells})});
    const std::uint32_t centre = emit({kOpFDiv, v2, 0, emit({kOpFAdd, v2, 0, cell, half2}), cells});
    // Its depth: tex_hiZ level 0 at the centre, layer z & 1 (the march's own coordinate: uv x the buffer ratio). Level
    // 0 (the z min/max builder) holds per pixel x = the depth one unit off the surface (X-Plane's thickness), y = the
    // surface's own: y.
    const std::uint32_t ratio = emit({kOpLoad, v2, 0, emit({kOpAccessChain, ptrRatio, 0, ssr, ratioIndex})});
    const std::uint32_t tc = emit({kOpFMul, v2, 0, centre, ratio});
    const std::uint32_t layer = emit({zSigned ? kOpConvertSToF : kOpConvertUToF, f, 0, emit({kOpBitwiseAnd, zType, 0, z, zOne})});
    const std::uint32_t coord = emit({kOpCompositeConstruct, v3, 0, emit({kOpCompositeExtract, f, 0, tc, 0}),
                                      emit({kOpCompositeExtract, f, 0, tc, 1}), layer});
    const std::uint32_t image = emit({kOpLoad, hizType, 0, hiz});
    const std::uint32_t depth = emit({kOpCompositeExtract, f, 0,
                                      emit({kOpImageSampleExplicitLod, v4, 0, image, coord, kImageOperandLod, f0}), 1});
    // The eye's start in NDC, the offset to it, and the end moved by the same offset; views 2-3 take them.
    const std::uint32_t two2 = emit({kOpCompositeConstruct, v2, 0, f2, f2});
    const std::uint32_t one2 = emit({kOpCompositeConstruct, v2, 0, f1, f1});
    const std::uint32_t ndc = emit({kOpFSub, v2, 0, emit({kOpFMul, v2, 0, centre, two2}), one2});
    const std::uint32_t eyeStart = emit({kOpCompositeConstruct, v3, 0, emit({kOpCompositeExtract, f, 0, ndc, 0}),
                                         emit({kOpCompositeExtract, f, 0, ndc, 1}), depth});
    const std::uint32_t moved = emit({kOpFAdd, v3, 0, e0, emit({kOpFSub, v3, 0, eyeStart, s0})});
    const std::uint32_t focus = emit({zSigned ? kOpSGreaterThanEqual : kOpUGreaterThanEqual, boolType, 0, z, zTwo});
    const std::uint32_t focus3 = emit({kOpCompositeConstruct, b3, 0, focus, focus, focus});
    const std::uint32_t newStart = emit({kOpSelect, v3, 0, focus3, eyeStart, s0});
    const std::uint32_t newEnd = emit({kOpSelect, v3, 0, focus3, moved, e0});
    m.insts.insert(m.insts.begin() + static_cast<std::ptrdiff_t>(at), body.begin(), body.end());
    // Every later use of the original divides takes ours.
    for (std::size_t i = at + body.size(); i < m.insts.size(); ++i) {
        auto& u = m.insts[i];
        const std::uint16_t c = Module::code(u);
        if (c == kOpFDiv && u.size() >= 3 && (u[2] == startDiv || u[2] == endDiv))
            continue;
        const std::size_t first = (c == kOpStore || c == kOpReturnValue) ? 1 : 3;
        for (std::size_t k = first; k < u.size(); ++k) {
            if (u[k] == startDiv)
                u[k] = newStart;
            else if (u[k] == endDiv)
                u[k] = newEnd;
        }
    }
    words = m.emit();
    return 1;
}

}  // namespace xplane
