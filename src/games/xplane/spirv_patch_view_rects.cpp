// SPIR-V edits for the views' rects within their layers (spirv_patch.h).
#include "spirv_module.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace xplane {

using namespace spirv;

namespace {

// spirv_sample_view_rects' and spirv_histogram_view_rect's edit on a parsed module: every 2D array image sample at
// float (u, v, layer) and fetch at integer (x, y, layer) takes its view's rect, (u s, v s) / (int(x s), int(y s)), s the
// share of that layer's view (focus for layers >= 2). Emits into `words` when anything was edited.
int view_rect_coords(Module& m, std::vector<std::uint32_t>& words, std::string* why) {
    auto fail = [&](const char* reason) {
        if (why)
            *why = reason;
        return 0;
    };
    constexpr std::uint32_t kDim2D = 1;
    constexpr std::uint16_t kOpTypeBool = 20, kOpTypeImage = 25, kOpTypeSampledImage = 27,
                            kOpImageSampleImplicitLod = 87, kOpImageSampleExplicitLod = 88, kOpCompositeExtract = 81,
                            kOpSelect = 169, kOpFOrdGreaterThanEqual = 190, kOpImageFetch = 95, kOpConvertFToS = 110,
                            kOpConvertSToF = 111, kOpSGreaterThanEqual = 175;
    // 2D array images, the sampled image types of them, float vec3 types and 32-bit int vec3 types (by component).
    std::set<std::uint32_t> arrayImages, arraySampled, vec3f, ints;
    std::map<std::uint32_t, std::uint32_t> vec3i;  // int vec3 type -> its component type
    const std::uint32_t fl = m.type_float();
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpTypeInt && w.size() == 4 && w[2] == 32)
            ints.insert(w[1]);
    for (const auto& w : m.insts) {
        const std::uint16_t c = Module::code(w);
        if (c == kOpTypeImage && w.size() >= 9 && w[3] == kDim2D && w[5] == 1)
            arrayImages.insert(w[1]);
        if (c == Module::kTypeVector && w.size() == 4 && w[2] == fl && w[3] == 3)
            vec3f.insert(w[1]);
        if (c == Module::kTypeVector && w.size() == 4 && ints.count(w[2]) && w[3] == 3)
            vec3i[w[1]] = w[2];
    }
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpTypeSampledImage && w.size() == 3 && arrayImages.count(w[2]))
            arraySampled.insert(w[1]);
    auto type_of = [&](std::uint32_t id) -> std::uint32_t {
        const std::size_t at = m.defined_at(id);
        return at < m.insts.size() && m.insts[at].size() >= 3 ? m.insts[at][1] : 0;
    };
    // Samples at float (u, v, layer), and fetches at integer (x, y, layer) (the MSAA variants' stencil mask).
    std::vector<std::uint32_t> samples, fetches;
    for (const auto& w : m.insts) {
        const std::uint16_t c = Module::code(w);
        if ((c == kOpImageSampleImplicitLod || c == kOpImageSampleExplicitLod) && w.size() >= 5 &&
            arraySampled.count(type_of(w[3])) && vec3f.count(type_of(w[4])))
            samples.push_back(w[2]);
        if (c == kOpImageFetch && w.size() >= 5 && arrayImages.count(type_of(w[3])) && vec3i.count(type_of(w[4])))
            fetches.push_back(w[2]);
    }
    if (samples.empty() && fetches.empty())
        return fail("no array image sampled at (u, v, layer)");
    // Types and constants first (they go before the first function and move the instructions).
    std::uint32_t intType = m.find(kOpTypeInt, {32, 1}, 1);
    if (!intType)
        intType = m.add_global({op(kOpTypeInt, 4), 0, 32, 1}, 1);
    std::uint32_t boolType = m.find(kOpTypeBool, {}, 1);
    if (!boolType)
        boolType = m.add_global({op(kOpTypeBool, 2), 0}, 1);
    const FocusBlock block = focus_block(m, intType);
    const std::uint32_t rectsEntry = m.constant(intType, kViewRectsEntry), v3 = m.type_vector(fl, 3);
    std::uint32_t oneHalfBits;
    const float oneHalf = 1.5f;
    std::memcpy(&oneHalfBits, &oneHalf, 4);
    const std::uint32_t cFocusLayer = m.constant(fl, oneHalfBits);
    int edited = 0;
    for (const std::uint32_t sample : samples) {
        const std::size_t at = m.defined_at(sample);
        if (at >= m.insts.size())
            continue;
        const std::uint32_t coord = m.insts[at][4];
        std::vector<std::vector<std::uint32_t>> body;
        auto emit = [&](std::vector<std::uint32_t> w) {
            const std::uint32_t id = m.fresh();
            w[2] = id;
            w[0] = op(static_cast<std::uint16_t>(w[0]), static_cast<std::uint16_t>(w.size()));
            body.push_back(std::move(w));
            return id;
        };
        const std::uint32_t rects = focus_entry_load(m, block, rectsEntry, body);
        auto part = [&](std::uint32_t composite, std::uint32_t k) { return emit({kOpCompositeExtract, fl, 0, composite, k}); };
        const std::uint32_t layer = part(coord, 2);
        const std::uint32_t focus = emit({kOpFOrdGreaterThanEqual, boolType, 0, layer, cFocusLayer});
        const std::uint32_t sx = emit({kOpSelect, fl, 0, focus, part(rects, 2), part(rects, 0)});
        const std::uint32_t sy = emit({kOpSelect, fl, 0, focus, part(rects, 3), part(rects, 1)});
        const std::uint32_t u = emit({kOpFMul, fl, 0, part(coord, 0), sx}), v = emit({kOpFMul, fl, 0, part(coord, 1), sy});
        const std::uint32_t scaled = emit({kOpCompositeConstruct, v3, 0, u, v, layer});
        m.insts.insert(m.insts.begin() + static_cast<std::ptrdiff_t>(at), body.begin(), body.end());
        m.insts[at + body.size()][4] = scaled;
        ++edited;
    }
    for (const std::uint32_t fetch : fetches) {
        const std::size_t at = m.defined_at(fetch);
        if (at >= m.insts.size())
            continue;
        const std::uint32_t coord = m.insts[at][4], coordType = type_of(coord), component = vec3i[coordType];
        const std::uint32_t two = m.constant(component, 2);
        const std::size_t here = m.defined_at(fetch);  // the constant may have moved it
        std::vector<std::vector<std::uint32_t>> body;
        auto emit = [&](std::vector<std::uint32_t> w) {
            const std::uint32_t id = m.fresh();
            w[2] = id;
            w[0] = op(static_cast<std::uint16_t>(w[0]), static_cast<std::uint16_t>(w.size()));
            body.push_back(std::move(w));
            return id;
        };
        const std::uint32_t rects = focus_entry_load(m, block, rectsEntry, body);
        auto part = [&](std::uint32_t type, std::uint32_t composite, std::uint32_t k) {
            return emit({kOpCompositeExtract, type, 0, composite, k});
        };
        const std::uint32_t layer = part(component, coord, 2);
        const std::uint32_t focus = emit({kOpSGreaterThanEqual, boolType, 0, layer, two});
        const std::uint32_t sx = emit({kOpSelect, fl, 0, focus, part(fl, rects, 2), part(fl, rects, 0)});
        const std::uint32_t sy = emit({kOpSelect, fl, 0, focus, part(fl, rects, 3), part(fl, rects, 1)});
        auto scale = [&](std::uint32_t k, std::uint32_t share) {
            const std::uint32_t f = emit({kOpConvertSToF, fl, 0, part(component, coord, k)});
            return emit({kOpConvertFToS, component, 0, emit({kOpFMul, fl, 0, f, share})});
        };
        const std::uint32_t x = scale(0, sx), y = scale(1, sy);
        const std::uint32_t scaled = emit({kOpCompositeConstruct, coordType, 0, x, y, layer});
        m.insts.insert(m.insts.begin() + static_cast<std::ptrdiff_t>(here), body.begin(), body.end());
        m.insts[here + body.size()][4] = scaled;
        ++edited;
    }
    words = m.emit();
    return edited;
}

// The module's entry point is of this execution model (4 fragment, 5 compute).
bool entry_model(const Module& m, std::uint32_t model) {
    for (const auto& w : m.insts)
        if (Module::code(w) == Module::kEntryPoint && w.size() >= 2 && w[1] == model)
            return true;
    return false;
}

}  // namespace

int spirv_viewport_split_off(std::vector<std::uint32_t>& words, std::string* why, bool push) {
    auto fail = [&](const char* reason) {
        if (why)
            *why = reason;
        return 0;
    };
    constexpr std::uint32_t kExecutionVertex = 0, kBuiltInViewportIndex = 10, kBuiltInInstanceIndex = 43,
                            kBuiltInBaseInstance = 4425;
    constexpr std::uint16_t kOpTypeBool = 20, kOpConvertSToF = 111, kOpSMod = 139, kOpIEqual = 170,
                            kOpLogicalAnd = 167, kOpSelect = 169, kOpReturnValue = 254, kOpReturn = 253,
                            kOpISub = 130, kOpSGreaterThanEqual = 175, kOpCompositeExtract = 81,
                            kOpCompositeInsert = 82, kOpFunctionEnd = 56;
    Module m;
    if (!m.parse(words))
        return fail("not SPIR-V");
    bool vertex = false;
    for (const auto& w : m.insts)
        if (Module::code(w) == Module::kEntryPoint && w.size() >= 2 && w[1] == kExecutionVertex)
            vertex = true;
    if (!vertex)
        return fail("not a vertex shader");
    // The transform block, its two counts' members and its variable.
    const std::uint32_t block = m.named("u_transform_block_fat");
    std::int64_t viewportMember = -1, layerMember = -1;
    for (const auto& w : m.insts)
        if (block && Module::code(w) == Module::kMemberName && w.size() >= 4 && w[1] == block) {
            const std::string name = literal(w, 3, w.size());
            if (name == "u_viewport_count")
                viewportMember = w[2];
            else if (name == "u_layer_count")
                layerMember = w[2];
        }
    if (viewportMember < 0 || layerMember < 0)
        return fail("no transform block counts");
    std::uint32_t blockVar = 0, intType = 0;
    for (const auto& w : m.insts) {
        if (Module::code(w) == Module::kVariable && w.size() >= 4 && w[3] == kUniform)
            for (const auto& t : m.insts)
                if (Module::code(t) == Module::kTypePointer && t[1] == w[1] && t[3] == block)
                    blockVar = w[2];
        if (Module::code(w) == kOpTypeStruct && w[1] == block && static_cast<std::size_t>(viewportMember) + 2 < w.size())
            intType = w[2 + viewportMember];
    }
    if (!blockVar || !intType)
        return fail("transform block not bound");
    // The loads of u_viewport_count (through chains [block][member]), and whether one is converted to float.
    std::map<std::uint32_t, std::uint32_t> constValue;
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpConstant && w.size() == 4)
            constValue[w[2]] = w[3];
    std::set<std::uint32_t> chains, loads;
    for (const auto& w : m.insts) {
        const std::uint16_t c = Module::code(w);
        if ((c == kOpAccessChain || c == kOpInBoundsAccessChain) && w.size() == 5 && w[3] == blockVar &&
            constValue.count(w[4]) && constValue[w[4]] == static_cast<std::uint32_t>(viewportMember))
            chains.insert(w[2]);
    }
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpLoad && w.size() >= 4 && chains.count(w[3]))
            loads.insert(w[2]);
    bool split = false;
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpConvertSToF && w.size() == 4 && loads.count(w[3]))
            split = true;
    if (!split)
        return fail("no side-by-side split");
    // The `i % n` stored to gl_ViewportIndex keeps n.
    const std::uint32_t viewportVar = builtin_variable(m, kBuiltInViewportIndex, kOutput, nullptr);
    std::set<std::uint32_t> kept;
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpStore && w.size() >= 3 && w[1] == viewportVar)
            kept.insert(w[2]);
    // Types and constants first (they go before the first function and move the instructions).
    std::uint32_t boolType = m.find(kOpTypeBool, {}, 1);
    if (!boolType)
        boolType = m.add_global({op(kOpTypeBool, 2), 0}, 1);
    const std::uint32_t c1 = m.constant(intType, 1), c4 = m.constant(intType, 4),
                        cLayer = m.constant(intType, static_cast<std::uint32_t>(layerMember)),
                        ptrInt = m.type_pointer(kUniform, intType);
    int edited = 0;
    for (const std::uint32_t load : loads) {
        const std::size_t at = m.defined_at(load);
        if (at >= m.insts.size())
            continue;
        const std::uint32_t layerPtr = m.fresh(), layers = m.fresh(), fourViewports = m.fresh(), fourLayers = m.fresh(),
                            fourViews = m.fresh(), count = m.fresh();
        const std::vector<std::vector<std::uint32_t>> body = {
            {op(kOpAccessChain, 5), ptrInt, layerPtr, blockVar, cLayer},
            {op(kOpLoad, 4), intType, layers, layerPtr},
            {op(kOpIEqual, 5), boolType, fourViewports, load, c4},
            {op(kOpIEqual, 5), boolType, fourLayers, layers, c4},
            {op(kOpLogicalAnd, 5), boolType, fourViews, fourViewports, fourLayers},
            {op(kOpSelect, 6), intType, count, fourViews, c1, load},
        };
        m.insts.insert(m.insts.begin() + static_cast<std::ptrdiff_t>(at + 1), body.begin(), body.end());
        // Every later use but the viewport index's `i % n` takes the count.
        for (std::size_t i = at + 1 + body.size(); i < m.insts.size(); ++i) {
            auto& u = m.insts[i];
            const std::uint16_t c = Module::code(u);
            if (c == kOpSMod && u.size() >= 3 && kept.count(u[2]))
                continue;
            const std::size_t first = (c == kOpStore || c == kOpReturnValue) ? 1 : 3;
            for (std::size_t k = first; k < u.size(); ++k)
                if (u[k] == load)
                    u[k] = count;
        }
        ++edited;
    }
    if (!edited)
        return fail("loads moved");
    // Per-view rects: before each return of the entry point, a four-view pass's texture coordinates go into its view's
    // rect, from its top row (v = 0: the rects start at the layer's top-left in memory) and left column, the share s
    // from the push block (kViewRectsEntry).
    if (push) {
        std::uint32_t instType = 0, texType = 0;
        const std::uint32_t instVar = builtin_variable(m, kBuiltInInstanceIndex, kInput, &instType),
                            baseVar = builtin_variable(m, kBuiltInBaseInstance, kInput, nullptr),
                            texVar = m.variable("v_texcoord0", kOutput, &texType);
        const std::uint32_t fl = m.type_float(), v4 = m.type_vector(fl, 4);
        if (!instVar || !baseVar || !texVar || instType != intType || texType != v4)
            return fail("no instance, base instance or vec4 v_texcoord0");
        const FocusBlock focusBlock = focus_block(m, intType);
        const std::uint32_t c2 = m.constant(intType, 2), cRects = m.constant(intType, kViewRectsEntry),
                            cViewport = m.constant(intType, static_cast<std::uint32_t>(viewportMember));
        std::uint32_t entry = 0;
        for (const auto& w : m.insts)
            if (Module::code(w) == Module::kEntryPoint && w.size() >= 3)
                entry = w[2];
        std::vector<std::size_t> returns;
        bool inEntry = false;
        for (std::size_t i = 0; i < m.insts.size(); ++i) {
            const std::uint16_t c = Module::code(m.insts[i]);
            if (c == Module::kFunction)
                inEntry = m.insts[i].size() >= 3 && m.insts[i][2] == entry;
            else if (c == kOpFunctionEnd)
                inEntry = false;
            else if (c == kOpReturn && inEntry)
                returns.push_back(i);
        }
        if (returns.empty())
            return fail("no return in the entry point");
        for (std::size_t r = returns.size(); r-- > 0;) {  // from the last, so earlier positions hold
            std::vector<std::vector<std::uint32_t>> body;
            auto emit = [&](std::vector<std::uint32_t> w) {
                const std::uint32_t id = m.fresh();
                w[2] = id;
                w[0] = op(static_cast<std::uint16_t>(w[0]), static_cast<std::uint16_t>(w.size()));
                body.push_back(std::move(w));
                return id;
            };
            const std::uint32_t inst = emit({kOpLoad, intType, 0, instVar}), base = emit({kOpLoad, intType, 0, baseVar});
            const std::uint32_t view0 = emit({kOpISub, intType, 0, inst, base});
            const std::uint32_t viewports = emit({kOpLoad, intType, 0, emit({kOpAccessChain, ptrInt, 0, blockVar, cViewport})});
            const std::uint32_t layers = emit({kOpLoad, intType, 0, emit({kOpAccessChain, ptrInt, 0, blockVar, cLayer})});
            const std::uint32_t four = emit({kOpLogicalAnd, boolType, 0, emit({kOpIEqual, boolType, 0, viewports, c4}),
                                             emit({kOpIEqual, boolType, 0, layers, c4})});
            const std::uint32_t view = emit({kOpSMod, intType, 0, view0, layers});
            const std::uint32_t focus = emit({kOpSGreaterThanEqual, boolType, 0, view, c2});
            const std::uint32_t rects = focus_entry_load(m, focusBlock, cRects, body);
            auto part = [&](std::uint32_t composite, std::uint32_t k) { return emit({kOpCompositeExtract, fl, 0, composite, k}); };
            const std::uint32_t sx = emit({kOpSelect, fl, 0, focus, part(rects, 2), part(rects, 0)});
            const std::uint32_t sy = emit({kOpSelect, fl, 0, focus, part(rects, 3), part(rects, 1)});
            const std::uint32_t tc = emit({kOpLoad, v4, 0, texVar});
            const std::uint32_t u = part(tc, 0), v = part(tc, 1);
            const std::uint32_t u2 = emit({kOpFMul, fl, 0, u, sx});
            const std::uint32_t v2 = emit({kOpFMul, fl, 0, v, sy});
            const std::uint32_t uOut = emit({kOpSelect, fl, 0, four, u2, u}), vOut = emit({kOpSelect, fl, 0, four, v2, v});
            const std::uint32_t tc2 = emit({kOpCompositeInsert, v4, 0, uOut, tc, 0});
            const std::uint32_t tc3 = emit({kOpCompositeInsert, v4, 0, vOut, tc2, 1});
            body.push_back({op(kOpStore, 3), texVar, tc3});
            m.insts.insert(m.insts.begin() + static_cast<std::ptrdiff_t>(returns[r]), body.begin(), body.end());
        }
    }
    words = m.emit();
    return edited;
}

int spirv_sample_view_rects(std::vector<std::uint32_t>& words, std::string* why) {
    Module m;
    if (!m.parse(words)) {
        if (why)
            *why = "not SPIR-V";
        return 0;
    }
    if (!entry_model(m, 4)) {
        if (why)
            *why = "not a fragment shader";
        return 0;
    }
    return view_rect_coords(m, words, why);
}

int spirv_histogram_view_rect(std::vector<std::uint32_t>& words, std::string* why) {
    Module m;
    if (!m.parse(words)) {
        if (why)
            *why = "not SPIR-V";
        return 0;
    }
    if (!entry_model(m, 5)) {
        if (why)
            *why = "not a compute shader";
        return 0;
    }
    // Its uniform block's type is named u_histo_data (the variable itself has no name), its input image_input_hdr.
    if (!m.named("u_histo_data") || !m.named("image_input_hdr")) {
        if (why)
            *why = "not the histogram (no u_histo_data, image_input_hdr)";
        return 0;
    }
    return view_rect_coords(m, words, why);
}

int spirv_ssr_view_rects(std::vector<std::uint32_t>& words, bool eyeLayers, std::string* why) {
    auto fail = [&](const char* reason) {
        if (why)
            *why = reason;
        return 0;
    };
    constexpr std::uint16_t kOpMemberName = 6, kOpCompositeExtract = 81, kOpTypeMatrix = 24;
    constexpr std::uint32_t kExecutionGLCompute = 5;
    if (!eyeLayers)
        return fail("SSR in the focus views' own screens (xplane_ssr_from_eye=0): not edited");
    Module m;
    if (!m.parse(words))
        return fail("not SPIR-V");
    bool compute = false;
    for (const auto& w : m.insts)
        if (Module::code(w) == Module::kEntryPoint && w.size() >= 2 && w[1] == kExecutionGLCompute)
            compute = true;
    if (!compute)
        return fail("not a compute shader");
    // The SSR block (its type u_ssr_data) and its projections u_proj (mat4[2], one per eye).
    const std::uint32_t ssrType = m.named("u_ssr_data");
    if (!ssrType)
        return fail("no u_ssr_data");
    std::uint32_t projMember = UINT32_MAX;
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpMemberName && w.size() >= 4 && w[1] == ssrType && literal(w, 3, w.size()) == "u_proj")
            projMember = w[2];
    if (projMember == UINT32_MAX)
        return fail("no u_proj");
    std::set<std::uint32_t> pointers, vars;
    for (const auto& w : m.insts)
        if (Module::code(w) == Module::kTypePointer && w.size() == 4 && w[3] == ssrType)
            pointers.insert(w[1]);
    for (const auto& w : m.insts)
        if (Module::code(w) == Module::kVariable && w.size() >= 4 && pointers.count(w[1]))
            vars.insert(w[2]);
    std::map<std::uint32_t, std::uint32_t> constants;  // id -> value
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpConstant && w.size() == 4)
            constants[w[2]] = w[3];
    // Chains to one eye's projection: (var, member u_proj, eye); their loads are the matrices.
    std::set<std::uint32_t> chains;
    for (const auto& w : m.insts)
        if ((Module::code(w) == kOpAccessChain || Module::code(w) == kOpInBoundsAccessChain) && w.size() == 6 &&
            vars.count(w[3]) && constants.count(w[4]) && constants[w[4]] == projMember)
            chains.insert(w[2]);
    const std::uint32_t fl = m.type_float(), v4 = m.type_vector(fl, 4);
    std::vector<std::uint32_t> loads;
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpLoad && w.size() >= 4 && chains.count(w[3]))
            loads.push_back(w[2]);
    if (loads.empty())
        return fail("u_proj never loaded");
    // Types and constants first (they go before the first function and move the instructions).
    std::uint32_t intType = m.find(kOpTypeInt, {32, 1}, 1);
    if (!intType)
        intType = m.add_global({op(kOpTypeInt, 4), 0, 32, 1}, 1);
    const FocusBlock block = focus_block(m, intType);
    const std::uint32_t rectsEntry = m.constant(intType, kViewRectsEntry), cOne = m.constant(fl, kFloatOne);
    int edited = 0;
    for (const std::uint32_t load : loads) {
        // The load gets a new id; right after it the old id = S x M, S taking ndc to ndc' = s ndc + (s - 1) on x and y
        // (uv' = uv s: the view's place at its layer's top-left), s the outer share: the eye's layer is read.
        const std::size_t at = m.defined_at(load);
        if (at >= m.insts.size())
            continue;
        const std::uint32_t matType = m.insts[at][1];
        bool mat4 = false;
        for (const auto& t : m.insts)
            if (Module::code(t) == kOpTypeMatrix && t.size() == 4 && t[1] == matType && t[2] == v4 && t[3] == 4)
                mat4 = true;
        if (!mat4)
            continue;
        const std::uint32_t raw = m.fresh();
        m.insts[at][2] = raw;
        std::vector<std::vector<std::uint32_t>> body;
        auto emit = [&](std::vector<std::uint32_t> w) {
            if (w[2] == 0)
                w[2] = m.fresh();
            w[0] = op(static_cast<std::uint16_t>(w[0]), static_cast<std::uint16_t>(w.size()));
            body.push_back(w);
            return w[2];
        };
        const std::uint32_t rects = focus_entry_load(m, block, rectsEntry, body);
        const std::uint32_t s[2] = {emit({kOpCompositeExtract, fl, 0, rects, 0}),
                                    emit({kOpCompositeExtract, fl, 0, rects, 1})};
        const std::uint32_t t[2] = {emit({kOpFSub, fl, 0, s[0], cOne}), emit({kOpFSub, fl, 0, s[1], cOne})};
        std::uint32_t columns[4];
        for (std::uint32_t c = 0; c < 4; ++c) {
            const std::uint32_t col = emit({kOpCompositeExtract, v4, 0, raw, c});
            std::uint32_t parts[4];
            for (std::uint32_t k = 0; k < 4; ++k)
                parts[k] = emit({kOpCompositeExtract, fl, 0, col, k});
            for (int a = 0; a < 2; ++a)  // row a' = s row a + (s - 1) row w
                parts[a] = emit({kOpFAdd, fl, 0, emit({kOpFMul, fl, 0, parts[a], s[a]}),
                                 emit({kOpFMul, fl, 0, parts[3], t[a]})});
            columns[c] = emit({kOpCompositeConstruct, v4, 0, parts[0], parts[1], parts[2], parts[3]});
        }
        emit({kOpCompositeConstruct, matType, load, columns[0], columns[1], columns[2], columns[3]});
        m.insts.insert(m.insts.begin() + static_cast<std::ptrdiff_t>(at + 1), body.begin(), body.end());
        ++edited;
    }
    if (!edited)
        return fail("u_proj not a mat4");
    words = m.emit();
    return edited;
}

int spirv_viewport_size_from_gbuffer(std::vector<std::uint32_t>& words, std::string* why) {
    auto fail = [&](const char* reason) {
        if (why)
            *why = reason;
        return 0;
    };
    constexpr std::uint16_t kOpMemberName = 6;
    Module m;
    if (!m.parse(words))
        return fail("not SPIR-V");
    const std::uint32_t blockType = m.named("u_gbuffer_data");
    if (!blockType)
        return fail("no u_gbuffer_data");
    std::uint32_t viewportMember = UINT32_MAX, gbufferMember = UINT32_MAX;
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpMemberName && w.size() >= 4 && w[1] == blockType) {
            const std::string name = literal(w, 3, w.size());
            if (name == "u_viewport_size")
                viewportMember = w[2];
            else if (name == "u_gbuffer_size")
                gbufferMember = w[2];
        }
    if (viewportMember == UINT32_MAX || gbufferMember == UINT32_MAX)
        return fail("no u_viewport_size / u_gbuffer_size");
    // The block's variables (pointers to it, any storage class).
    std::set<std::uint32_t> pointers, vars;
    for (const auto& w : m.insts)
        if (Module::code(w) == Module::kTypePointer && w.size() == 4 && w[3] == blockType)
            pointers.insert(w[1]);
    for (const auto& w : m.insts)
        if (Module::code(w) == Module::kVariable && w.size() >= 4 && pointers.count(w[1]))
            vars.insert(w[2]);
    // Integer constants by id: value and type.
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> constants;  // id -> (type, value)
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpConstant && w.size() == 4)
            constants[w[2]] = {w[1], w[3]};
    // The chains into u_viewport_size, and the index constant each would take instead.
    std::vector<std::size_t> chains;
    std::set<std::uint32_t> indexTypes;
    for (std::size_t i = 0; i < m.insts.size(); ++i) {
        const auto& w = m.insts[i];
        if ((Module::code(w) == kOpAccessChain || Module::code(w) == kOpInBoundsAccessChain) && w.size() >= 5 &&
            vars.count(w[3]) && constants.count(w[4]) && constants[w[4]].second == viewportMember) {
            chains.push_back(i);
            indexTypes.insert(constants[w[4]].first);
        }
    }
    if (chains.empty())
        return fail("u_viewport_size never read");
    std::map<std::uint32_t, std::uint32_t> replacement;  // index type -> constant gbufferMember of it
    for (const std::uint32_t type : indexTypes)
        replacement[type] = m.constant(type, gbufferMember);
    // The constants may have gone in before the first function: find the chains again by their result ids.
    std::set<std::uint32_t> results;
    for (const std::size_t i : chains)
        results.insert(m.insts[i][2]);
    int edited = 0;
    for (auto& w : m.insts)
        if ((Module::code(w) == kOpAccessChain || Module::code(w) == kOpInBoundsAccessChain) && w.size() >= 5 &&
            results.count(w[2])) {
            w[4] = replacement[constants[w[4]].first];
            ++edited;
        }
    words = m.emit();
    return edited;
}

int spirv_hiz_view_rects(std::vector<std::uint32_t>& words, std::string* why) {
    auto fail = [&](const char* reason) {
        if (why)
            *why = reason;
        return 0;
    };
    constexpr std::uint16_t kOpTypeBool = 20, kOpMemberName = 6, kOpCompositeExtract = 81, kOpSelect = 169,
                            kOpUGreaterThanEqual = 174, kOpConvertFToS = 110, kOpConvertSToF = 111;
    constexpr std::uint32_t kBuiltInGlobalInvocationId = 28, kExecutionGLCompute = 5;
    Module m;
    if (!m.parse(words))
        return fail("not SPIR-V");
    bool compute = false;
    for (const auto& w : m.insts)
        if (Module::code(w) == Module::kEntryPoint && w.size() >= 2 && w[1] == kExecutionGLCompute)
            compute = true;
    if (!compute)
        return fail("not a compute shader");
    const std::uint32_t blockType = m.named("u_z_minmax_data");
    if (!blockType)
        return fail("no u_z_minmax_data");
    std::map<std::uint32_t, int> members;  // member index -> axis (0 width, 1 height)
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpMemberName && w.size() >= 4 && w[1] == blockType) {
            const std::string name = literal(w, 3, w.size());
            if (name == "u_src_width_limit")
                members[w[2]] = 0;
            else if (name == "u_src_height_limit")
                members[w[2]] = 1;
        }
    if (members.size() != 2)
        return fail("no u_src_width_limit / u_src_height_limit");
    std::set<std::uint32_t> pointers, vars;
    for (const auto& w : m.insts)
        if (Module::code(w) == Module::kTypePointer && w.size() == 4 && w[3] == blockType)
            pointers.insert(w[1]);
    for (const auto& w : m.insts)
        if (Module::code(w) == Module::kVariable && w.size() >= 4 && pointers.count(w[1]))
            vars.insert(w[2]);
    std::map<std::uint32_t, std::uint32_t> constants;  // id -> value
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpConstant && w.size() == 4)
            constants[w[2]] = w[3];
    std::map<std::uint32_t, int> chains;  // access chain -> axis
    for (const auto& w : m.insts)
        if ((Module::code(w) == kOpAccessChain || Module::code(w) == kOpInBoundsAccessChain) && w.size() == 5 &&
            vars.count(w[3]) && constants.count(w[4]) && members.count(constants[w[4]]))
            chains[w[2]] = members[constants[w[4]]];
    std::vector<std::pair<std::uint32_t, int>> loads;  // load result -> axis
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpLoad && w.size() >= 4 && chains.count(w[3]))
            loads.push_back({w[2], chains[w[3]]});
    if (loads.empty())
        return fail("the limits are never read");
    // Types and constants first (they go before the first function and move the instructions).
    const std::uint32_t fl = m.type_float();
    std::uint32_t intType = m.find(kOpTypeInt, {32, 1}, 1);
    if (!intType)
        intType = m.add_global({op(kOpTypeInt, 4), 0, 32, 1}, 1);
    std::uint32_t boolType = m.find(kOpTypeBool, {}, 1);
    if (!boolType)
        boolType = m.add_global({op(kOpTypeBool, 2), 0}, 1);
    std::uint32_t gidType = 0, uintType = 0;
    const std::uint32_t gid = builtin_variable(m, kBuiltInGlobalInvocationId, kInput, &gidType);
    for (const auto& w : m.insts)
        if (Module::code(w) == Module::kTypeVector && w.size() == 4 && w[1] == gidType)
            uintType = w[2];
    if (!gid || !uintType)
        return fail("no gl_GlobalInvocationID");
    const FocusBlock block = focus_block(m, intType);
    const std::uint32_t rectsEntry = m.constant(intType, kViewRectsEntry), ptrUint = m.type_pointer(kInput, uintType),
                        cTwo = m.constant(uintType, 2), cZ = m.constant(intType, 2);
    std::uint32_t upBits;
    const float up = 0.999f;  // rounds up: the partly covered texel at the rect's edge stays in
    std::memcpy(&upBits, &up, 4);
    const std::uint32_t cUp = m.constant(fl, upBits);
    int edited = 0;
    for (const auto& [load, axis] : loads) {
        // The load gets a new id; right after it, the old id = int(limit x share + 0.999), the share of layer
        // gl_GlobalInvocationID.z (the dispatch depth: one layer per view; focus views from 2).
        const std::size_t at = m.defined_at(load);
        if (at >= m.insts.size())
            continue;
        const std::uint32_t type = m.insts[at][1], raw = m.fresh();
        m.insts[at][2] = raw;
        std::vector<std::vector<std::uint32_t>> body;
        auto emit = [&](std::vector<std::uint32_t> w) {
            if (w[2] == 0)
                w[2] = m.fresh();
            w[0] = op(static_cast<std::uint16_t>(w[0]), static_cast<std::uint16_t>(w.size()));
            body.push_back(w);
            return w[2];
        };
        const std::uint32_t rects = focus_entry_load(m, block, rectsEntry, body);
        const std::uint32_t ptr = m.fresh(), z = m.fresh();
        body.push_back({op(kOpAccessChain, 5), ptrUint, ptr, gid, cZ});
        body.push_back({op(kOpLoad, 4), uintType, z, ptr});
        const std::uint32_t focus = emit({kOpUGreaterThanEqual, boolType, 0, z, cTwo});
        const std::uint32_t share = emit({kOpSelect, fl, 0, focus,
                                          emit({kOpCompositeExtract, fl, 0, rects, static_cast<std::uint32_t>(2 + axis)}),
                                          emit({kOpCompositeExtract, fl, 0, rects, static_cast<std::uint32_t>(axis)})});
        const std::uint32_t f = emit({kOpConvertSToF, fl, 0, raw});
        const std::uint32_t g = emit({kOpFAdd, fl, 0, emit({kOpFMul, fl, 0, f, share}), cUp});
        emit({kOpConvertFToS, type, load, g});
        m.insts.insert(m.insts.begin() + static_cast<std::ptrdiff_t>(at + 1), body.begin(), body.end());
        ++edited;
    }
    words = m.emit();
    return edited;
}

}  // namespace xplane
