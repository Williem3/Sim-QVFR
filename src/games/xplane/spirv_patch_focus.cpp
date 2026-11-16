// SPIR-V edits that give the focus views their box's projection, mip and rates (spirv_patch.h).
#include "spirv_module.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace xplane {

using namespace spirv;

namespace {

// Where screen positions come from (per-view rects; docs/recon-xplane.md "Measured: stage 2's rects"): the pixel the
// shader runs on (gl_FragCoord, a compute invocation's IDs) over the whole layer, or a varying spanning the viewport (the
// sky's v_ratio). For each load in `loads` (a vec4 of view-ray coefficients): the operands multiplied with its xy (the
// load, its shuffles of components 0-1, or extracts of 0 / 1) traced back through arithmetic, conversions, composites,
// selects, phis and function-local variables. True for the loads whose trace reaches such a pixel builtin.
std::set<std::uint32_t> screen_from_pixels(const Module& m, const std::vector<std::uint32_t>& loads) {
    constexpr std::uint16_t kOpExtInst = 12, kOpFunctionParameter = 55, kOpLoadCode = 61,
                            kOpAccessChainCode = 65, kOpInBoundsAccessChainCode = 66, kOpDecorate = 71,
                            kOpShuffle = 79, kOpConstruct = 80, kOpExtract = 81, kOpInsert = 82, kOpFMulCode = 133,
                            kOpVectorTimesScalar = 142, kOpSelect = 169, kOpPhi = 245;
    constexpr std::uint32_t kBuiltIn = 11, kFragCoord = 15, kWorkgroupId = 26, kLocalInvocationId = 27,
                            kGlobalInvocationId = 28, kFunctionStorage = 7, kPrivateStorage = 6, kGlslFma = 50;
    // The value operands' word positions of a value instruction (none for others).
    auto operands = [](const std::vector<std::uint32_t>& w) {
        std::vector<std::size_t> at;
        const std::uint16_t c = Module::code(w);
        if (c == kOpExtInst)
            for (std::size_t k = 5; k < w.size(); ++k) at.push_back(k);
        else if (c == kOpShuffle || c == kOpInsert)
            at = {3, 4};
        else if (c == kOpExtract || c == kOpLoadCode || c == kOpAccessChainCode || c == kOpInBoundsAccessChainCode)
            at = {3};
        else if (c == kOpPhi)
            for (std::size_t k = 3; k < w.size(); k += 2) at.push_back(k);
        else if (c == kOpConstruct || c == kOpSelect || (c >= 109 && c <= 200))
            for (std::size_t k = 3; k < w.size(); ++k) at.push_back(k);
        at.erase(std::remove_if(at.begin(), at.end(), [&](std::size_t k) { return k >= w.size(); }), at.end());
        return at;
    };
    std::map<std::uint32_t, std::size_t> def;
    std::map<std::uint32_t, std::vector<std::size_t>> users;
    std::map<std::uint32_t, std::uint32_t> builtin, storage, chainBase;
    std::map<std::uint32_t, std::vector<std::uint32_t>> stored;
    for (std::size_t i = 0; i < m.insts.size(); ++i) {
        const auto& w = m.insts[i];
        const std::uint16_t c = Module::code(w);
        if (c == kOpDecorate && w.size() >= 4 && w[2] == kBuiltIn)
            builtin[w[1]] = w[3];
        if (c == kOpVariable && w.size() >= 4)
            storage[w[2]] = w[3];
        if ((c == kOpAccessChainCode || c == kOpInBoundsAccessChainCode) && w.size() >= 4)
            chainBase[w[2]] = w[3];
        const std::vector<std::size_t> at = operands(w);
        if (!at.empty() || c == kOpVariable || c == kOpFunctionParameter)
            def[w[2]] = i;
        for (const std::size_t k : at)
            users[w[k]].push_back(i);
    }
    auto base = [&](std::uint32_t ptr) {
        for (int guard = 0; guard < 16 && chainBase.count(ptr); ++guard)
            ptr = chainBase[ptr];
        return ptr;
    };
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpStore && w.size() >= 3)
            stored[base(w[1])].push_back(w[2]);
    // Function parameters (the cloud render passes its screen position by pointer): each one's function and place, and
    // the calls of each function, whose arguments stand for the parameters.
    constexpr std::uint16_t kOpFunctionCall = 57;
    std::map<std::uint32_t, std::pair<std::uint32_t, std::size_t>> parameter;
    std::map<std::uint32_t, std::vector<std::size_t>> calls;
    {
        std::uint32_t function = 0;
        std::size_t index = 0;
        for (std::size_t i = 0; i < m.insts.size(); ++i) {
            const auto& w = m.insts[i];
            const std::uint16_t c = Module::code(w);
            if (c == Module::kFunction && w.size() >= 3) {
                function = w[2];
                index = 0;
            } else if (c == kOpFunctionParameter && w.size() >= 3) {
                parameter[w[2]] = {function, index++};
            } else if (c == kOpFunctionCall && w.size() >= 4) {
                calls[w[3]].push_back(i);
            }
        }
    }
    auto arguments = [&](std::uint32_t param) {
        std::vector<std::uint32_t> args;
        const auto p = parameter.find(param);
        if (p != parameter.end())
            for (const std::size_t i : calls[p->second.first])
                if (4 + p->second.second < m.insts[i].size())
                    args.push_back(m.insts[i][4 + p->second.second]);
        return args;
    };
    auto reaches_pixels = [&](std::uint32_t from) {
        std::vector<std::uint32_t> stack = {from}, pointers;
        std::set<std::uint32_t> seen, seenPointers;
        while ((!stack.empty() || !pointers.empty()) && seen.size() + seenPointers.size() < 4000) {
            if (!pointers.empty()) {  // what a pointer's loads see: a builtin, the values stored, or a call's argument
                const std::uint32_t ptr = pointers.back();
                pointers.pop_back();
                if (!seenPointers.insert(ptr).second)
                    continue;
                const std::uint32_t var = base(ptr);
                const auto b = builtin.find(var);
                if (b != builtin.end() && (b->second == kFragCoord || b->second == kGlobalInvocationId ||
                                           b->second == kLocalInvocationId || b->second == kWorkgroupId))
                    return true;
                const auto s = storage.find(var);
                if (s != storage.end() && (s->second == kFunctionStorage || s->second == kPrivateStorage))
                    for (const std::uint32_t v : stored[var])
                        stack.push_back(v);
                for (const std::uint32_t a : arguments(var))
                    pointers.push_back(a);
                continue;
            }
            const std::uint32_t id = stack.back();
            stack.pop_back();
            if (!seen.insert(id).second || !def.count(id))
                continue;
            const auto& w = m.insts[def[id]];
            const std::uint16_t c = Module::code(w);
            if (c == kOpLoadCode) {
                pointers.push_back(w[3]);
                continue;
            }
            if (c == kOpFunctionParameter) {
                for (const std::uint32_t a : arguments(id))
                    stack.push_back(a);
                continue;
            }
            for (const std::size_t k : operands(w))
                stack.push_back(w[k]);
        }
        return false;
    };
    std::set<std::uint32_t> pixels;
    for (const std::uint32_t load : loads) {
        // The load's xy: itself, shuffles of components 0-1 only, extracts of 0 or 1.
        std::set<std::uint32_t> xy = {load};
        for (const std::size_t i : users[load]) {
            const auto& w = m.insts[i];
            const std::uint16_t c = Module::code(w);
            if (c == kOpShuffle && w[3] == load && w[4] == load &&
                std::all_of(w.begin() + 5, w.end(), [](std::uint32_t k) { return k <= 1; }))
                xy.insert(w[2]);
            if (c == kOpExtract && w.size() == 5 && w[4] <= 1)
                xy.insert(w[2]);
        }
        bool found = false;
        for (const std::uint32_t x : xy)
            for (const std::size_t i : users[x]) {
                const auto& w = m.insts[i];
                const std::uint16_t c = Module::code(w);
                const bool fma = c == kOpExtInst && w.size() >= 5 && w[4] == kGlslFma;
                if (c != kOpFMulCode && c != kOpVectorTimesScalar && !fma)
                    continue;
                for (const std::size_t k : operands(w))
                    if (!xy.count(w[k]) && !(fma && k == 7) && reaches_pixels(w[k]))
                        found = true;
            }
        if (found)
            pixels.insert(load);
    }
    return pixels;
}

// A Location decoration with this value on any variable.
bool location_taken(const Module& m, std::uint32_t location) {
    for (const auto& w : m.insts)
        if (Module::code(w) == Module::kDecorate && w.size() == 4 && w[2] == 30 && w[3] == location)
            return true;
    return false;
}

}  // namespace

int spirv_full_rate_from_layer(std::vector<std::uint32_t>& words, std::uint32_t firstLayer, std::string* why) {
    auto fail = [&](const char* reason) {
        if (why)
            *why = reason;
        return 0;
    };
    Module m;
    if (!m.parse(words))
        return fail("not SPIR-V");
    // X-Plane's shading-rate compute: a block named shading_rate_data with a member u_fill_rate.
    const std::uint32_t block = m.named("shading_rate_data");
    bool fill = false;
    for (const auto& w : m.insts)
        if (block && Module::code(w) == Module::kMemberName && w.size() >= 4 && w[1] == block &&
            literal(w, 3, w.size()) == "u_fill_rate")
            fill = true;
    if (!fill)
        return fail("not the shading-rate shader");
    const std::uint32_t gid = m.variable("gl_GlobalInvocationID", kInput, nullptr);
    if (!gid)
        return fail("no gl_GlobalInvocationID");
    constexpr std::uint16_t kOpImageWrite = 99, kOpTypeBool = 20, kOpUGreaterThanEqual = 174, kOpSelect = 169;
    // Each image write's texel: an OpCompositeConstruct whose first component is the rate.
    std::vector<std::size_t> constructs;
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpImageWrite && w.size() >= 4) {
            const std::size_t at = m.defined_at(w[3]);
            if (at < m.insts.size() && Module::code(m.insts[at]) == kOpCompositeConstruct && m.insts[at].size() >= 4)
                constructs.push_back(at);
        }
    if (constructs.empty())
        return fail("no rate write");
    const std::uint32_t uintType = m.find(kOpTypeInt, {32, 0}, 1);
    if (!uintType)
        return fail("no uint type");
    std::uint32_t boolType = m.find(kOpTypeBool, {}, 1);
    if (!boolType)
        boolType = m.add_global({op(kOpTypeBool, 2), 0}, 1);
    const std::uint32_t ptrU = m.type_pointer(kInput, uintType);
    const std::uint32_t c0 = m.constant(uintType, 0), c2 = m.constant(uintType, 2),
                        cLayer = m.constant(uintType, firstLayer);
    // Later ones first, so earlier indexes hold (constructs were found before the globals moved them: find again).
    constructs.clear();
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpImageWrite && w.size() >= 4)
            constructs.push_back(m.defined_at(w[3]));
    std::sort(constructs.rbegin(), constructs.rend());
    for (const std::size_t at : constructs) {
        const std::uint32_t ptr = m.fresh(), z = m.fresh(), focus = m.fresh(), rate = m.fresh();
        const std::uint32_t original = m.insts[at][3];
        const std::vector<std::vector<std::uint32_t>> body = {
            {op(kOpAccessChain, 5), ptrU, ptr, gid, c2},
            {op(kOpLoad, 4), uintType, z, ptr},
            {op(kOpUGreaterThanEqual, 5), boolType, focus, z, cLayer},
            {op(kOpSelect, 6), uintType, rate, focus, c0, original},
        };
        m.insts[at][3] = rate;
        m.insts.insert(m.insts.begin() + static_cast<std::ptrdiff_t>(at), body.begin(), body.end());
    }
    words = m.emit();
    return static_cast<int>(constructs.size());
}

int spirv_focus_proj_coefficients(std::vector<std::uint32_t>& words, const char* member, float fx, float fy,
                                  int* skipped, bool push, int* byPixels) {
    if (skipped)
        *skipped = 0;
    if (byPixels)
        *byPixels = 0;
    Module m;
    if (!m.parse(words))
        return 0;
    constexpr std::uint16_t kOpSelect = 169, kOpSGreaterThanEqual = 175, kOpUGreaterThanEqual = 174, kOpTypeBool = 20;
    // Structs with a member named `member`, and the Uniform variables of them.
    std::map<std::uint32_t, std::uint32_t> memberOf;  // struct -> member index
    for (const auto& w : m.insts)
        if (Module::code(w) == Module::kMemberName && w.size() >= 4 && literal(w, 3, w.size()) == member)
            memberOf[w[1]] = w[2];
    if (memberOf.empty())
        return 0;
    std::map<std::uint32_t, std::uint32_t> varMember;  // variable -> member index
    for (const auto& w : m.insts)
        if (Module::code(w) == Module::kVariable && w.size() >= 4 && w[3] == kUniform)
            for (const auto& t : m.insts)
                if (Module::code(t) == Module::kTypePointer && t[1] == w[1] && memberOf.count(t[3]))
                    varMember[w[2]] = memberOf[t[3]];
    if (varMember.empty())
        return 0;
    // Integer constants by id (value), the fold's `index & 1` (result -> raw index), and int types' signedness.
    std::map<std::uint32_t, std::uint32_t> constValue, typeOf;
    std::map<std::uint32_t, bool> intSigned;
    for (const auto& w : m.insts) {
        const std::uint16_t c = Module::code(w);
        if (c == kOpConstant && w.size() == 4)
            constValue[w[2]] = w[3];
        if (c == kOpTypeInt && w.size() == 4)
            intSigned[w[1]] = w[3] != 0;
        if (w.size() >= 3 && typed_result(c))
            typeOf[w[2]] = w[1];
    }
    std::map<std::uint32_t, std::uint32_t> foldedRaw;
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpBitwiseAnd && w.size() == 5 && constValue.count(w[4]) && constValue[w[4]] == 1)
            foldedRaw[w[2]] = w[3];
    // Chains var[member][index & 1] to a vec4, and the loads through them.
    std::map<std::uint32_t, std::uint32_t> chainRaw, chainFolded;  // chain result -> raw index, its `& 1`
    int partial = 0;
    for (const auto& w : m.insts) {
        const std::uint16_t c = Module::code(w);
        if ((c != kOpAccessChain && c != kOpInBoundsAccessChain) || w.size() < 6 || !varMember.count(w[3]))
            continue;
        if (!constValue.count(w[4]) || constValue[w[4]] != varMember[w[3]] || !foldedRaw.count(w[5]))
            continue;
        if (w.size() == 6) {
            chainRaw[w[2]] = foldedRaw[w[5]];
            chainFolded[w[2]] = w[5];
        }
        else
            ++partial;  // a component of the entry: not handled (counted)
    }
    if (skipped)
        *skipped = partial;
    std::vector<std::uint32_t> loads;
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpLoad && w.size() >= 4 && chainRaw.count(w[3]))
            loads.push_back(w[2]);
    if (loads.empty())
        return 0;
    // Per-view rects: the loads whose screen position counts pixels over the whole layer take each view's share.
    const std::set<std::uint32_t> pixels = push ? screen_from_pixels(m, loads) : std::set<std::uint32_t>{};
    const std::uint32_t fl = m.type_float(), v2 = m.type_vector(fl, 2);
    std::uint32_t boolType = m.find(kOpTypeBool, {}, 1);
    if (!boolType)
        boolType = m.add_global({op(kOpTypeBool, 2), 0}, 1);
    const std::uint32_t bv4 = m.type_vector(boolType, 4);
    std::uint32_t fxBits, fyBits, halfBits;
    const float negHalf = -0.5f;
    std::memcpy(&fxBits, &fx, 4);
    std::memcpy(&fyBits, &fy, 4);
    std::memcpy(&halfBits, &negHalf, 4);
    const std::uint32_t cHalf = m.constant(fl, halfBits);
    std::uint32_t scale = 0;  // the baked size (without the push block)
    if (!push)
        scale = m.add_global({op(44 /* OpConstantComposite */, 5), v2, 0, m.constant(fl, fxBits), m.constant(fl, fyBits)}, 2);
    int edited = 0;
    for (const std::uint32_t load : loads) {
        const std::size_t at = m.defined_at(load);
        if (at >= m.insts.size())
            continue;
        const std::uint32_t v4 = m.insts[at][1], ptr = m.insts[at][3];
        const std::uint32_t raw = chainRaw[ptr];
        const auto rawType = typeOf.find(raw);
        if (rawType == typeOf.end() || !intSigned.count(rawType->second))
            continue;
        const std::uint32_t two = m.constant(rawType->second, 2), rectsEntry = push ? m.constant(rawType->second, kViewRectsEntry) : 0;
        const FocusBlock block = push ? focus_block(m, rawType->second) : FocusBlock{};
        const bool byRect = pixels.count(load) > 0;
        const std::size_t loadAt = m.defined_at(load);  // constants may have moved it
        const std::uint32_t original = m.fresh();
        m.insts[loadAt][2] = original;
        const std::uint32_t xy = m.fresh(), fxy = m.fresh(), zw = m.fresh(), focusC = m.fresh(), isFocus = m.fresh(),
                            mask = m.fresh();
        // The box's coefficients: its size (f c.xy), and its offset, the tangent at its top-left corner: the centre
        // minus half the size. Size and centre from the push constants (this frame's); else the baked size, centred on
        // the eye's forward axis.
        std::vector<std::vector<std::uint32_t>> body = {
            {op(kOpVectorShuffle, 7), v2, xy, original, original, 0, 1},
        };
        if (push) {
            const std::uint32_t box = focus_entry_load(m, block, chainFolded[ptr], body);
            const std::uint32_t size = focus_pair(m, block, box, 2, body), centre = focus_pair(m, block, box, 0, body);
            const std::uint32_t half = m.fresh();
            body.push_back({op(kOpFMul, 5), v2, fxy, xy, size});
            body.push_back({op(142 /* OpVectorTimesScalar */, 5), v2, half, fxy, cHalf});
            body.push_back({op(kOpFAdd, 5), v2, zw, centre, half});
        } else {
            body.push_back({op(kOpFMul, 5), v2, fxy, xy, scale});
            body.push_back({op(142 /* OpVectorTimesScalar */, 5), v2, zw, fxy, cHalf});
        }
        // Each view's rect (push constants): where the screen position counts pixels over the whole layer, a view in
        // share s of it spans the ray range at positions 0..s, so its scale is c.xy / s (the rect starts at the
        // layer's top-left, where position 0 is). Positions from a varying span the viewport, the rect itself.
        std::uint32_t focusXY = fxy, outerC = original;
        if (byRect) {
            const std::uint32_t rects = focus_entry_load(m, block, rectsEntry, body);
            const std::uint32_t outerShare = focus_pair(m, block, rects, 0, body), focusShare = focus_pair(m, block, rects, 2, body);
            const std::uint32_t outerXY = m.fresh();
            focusXY = m.fresh();
            outerC = m.fresh();
            body.push_back({op(kOpFDiv, 5), v2, focusXY, fxy, focusShare});
            body.push_back({op(kOpFDiv, 5), v2, outerXY, xy, outerShare});
            body.push_back({op(kOpVectorShuffle, 9), v4, outerC, outerXY, original, 0, 1, 4, 5});
            if (byPixels)
                ++*byPixels;
        }
        body.insert(body.end(), {
            {op(kOpVectorShuffle, 9), v4, focusC, focusXY, zw, 0, 1, 2, 3},
            {op(intSigned[rawType->second] ? kOpSGreaterThanEqual : kOpUGreaterThanEqual, 5), boolType, isFocus, raw,
             two},
            {op(kOpCompositeConstruct, 7), bv4, mask, isFocus, isFocus, isFocus, isFocus},
            {op(kOpSelect, 6), v4, load, mask, focusC, outerC},
        });
        m.insts.insert(m.insts.begin() + static_cast<std::ptrdiff_t>(loadAt + 1), body.begin(), body.end());
        ++edited;
    }
    if (edited > 0)
        words = m.emit();
    return edited;
}

int spirv_focus_projection_matrix(std::vector<std::uint32_t>& words, const char* block, const char* member, float fx,
                                  float fy, bool push) {
    Module m;
    if (!m.parse(words) || (!push && (!(fx > 0.0f) || !(fy > 0.0f))))
        return 0;
    constexpr std::uint16_t kOpSelect = 169, kOpSGreaterThanEqual = 175, kOpUGreaterThanEqual = 174, kOpTypeBool = 20,
                            kOpTypeMatrix = 24, kOpCompositeExtract = 81, kOpCompositeInsert = 82;
    // The struct named `block`, its member named `member`, and the Uniform variables of it.
    const std::uint32_t blockType = m.named(block);
    std::int64_t memberIndex = -1;
    for (const auto& w : m.insts)
        if (Module::code(w) == Module::kMemberName && w.size() >= 4 && w[1] == blockType && literal(w, 3, w.size()) == member)
            memberIndex = w[2];
    if (!blockType || memberIndex < 0)
        return 0;
    std::set<std::uint32_t> vars;
    for (const auto& w : m.insts)
        if (Module::code(w) == Module::kVariable && w.size() >= 4 && w[3] == kUniform)
            for (const auto& t : m.insts)
                if (Module::code(t) == Module::kTypePointer && t[1] == w[1] && t[3] == blockType)
                    vars.insert(w[2]);
    if (vars.empty())
        return 0;
    std::map<std::uint32_t, std::uint32_t> constValue, typeOf, matrixColumn;  // matrixColumn: mat4 type -> vec4 type
    std::map<std::uint32_t, bool> intSigned;
    for (const auto& w : m.insts) {
        const std::uint16_t c = Module::code(w);
        if (c == kOpConstant && w.size() == 4)
            constValue[w[2]] = w[3];
        if (c == kOpTypeInt && w.size() == 4)
            intSigned[w[1]] = w[3] != 0;
        if (c == kOpTypeMatrix && w.size() == 4 && w[3] == 4)
            matrixColumn[w[1]] = w[2];
        if (w.size() >= 3 && typed_result(c))
            typeOf[w[2]] = w[1];
    }
    std::map<std::uint32_t, std::uint32_t> foldedRaw;  // the fold's `index & 1` -> the raw index
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpBitwiseAnd && w.size() == 5 && constValue.count(w[4]) && constValue[w[4]] == 1)
            foldedRaw[w[2]] = w[3];
    std::map<std::uint32_t, std::uint32_t> chainRaw, chainFolded;  // var[member][index & 1] -> raw index, index & 1
    for (const auto& w : m.insts) {
        const std::uint16_t c = Module::code(w);
        if ((c == kOpAccessChain || c == kOpInBoundsAccessChain) && w.size() == 6 && vars.count(w[3]) &&
            constValue.count(w[4]) && constValue[w[4]] == static_cast<std::uint32_t>(memberIndex) &&
            foldedRaw.count(w[5])) {
            chainRaw[w[2]] = foldedRaw[w[5]];
            chainFolded[w[2]] = w[5];
        }
    }
    std::vector<std::uint32_t> loads;
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpLoad && w.size() >= 4 && chainRaw.count(w[3]) && matrixColumn.count(w[1]))
            loads.push_back(w[2]);
    if (loads.empty())
        return 0;
    const std::uint32_t fl = m.type_float();
    std::uint32_t boolType = m.find(kOpTypeBool, {}, 1);
    if (!boolType)
        boolType = m.add_global({op(kOpTypeBool, 2), 0}, 1);
    const std::uint32_t bv4 = m.type_vector(boolType, 4);
    const float zero = 0.0f;
    std::uint32_t zeroBits;
    std::memcpy(&zeroBits, &zero, 4);
    const std::uint32_t cZero = m.constant(fl, zeroBits);
    std::uint32_t cInvFx = 0, cInvFy = 0;  // the baked size's (without the push block)
    if (!push) {
        const float invFx = 1.0f / fx, invFy = 1.0f / fy;
        std::uint32_t invFxBits, invFyBits;
        std::memcpy(&invFxBits, &invFx, 4);
        std::memcpy(&invFyBits, &invFy, 4);
        cInvFx = m.constant(fl, invFxBits);
        cInvFy = m.constant(fl, invFyBits);
    }
    int edited = 0;
    for (const std::uint32_t load : loads) {
        const std::size_t loadAt = m.defined_at(load);
        if (loadAt >= m.insts.size())
            continue;
        const std::uint32_t mat = m.insts[loadAt][1], v4 = matrixColumn[mat], ptr = m.insts[loadAt][3];
        const std::uint32_t raw = chainRaw[ptr];
        const auto rawType = typeOf.find(raw);
        if (rawType == typeOf.end() || !intSigned.count(rawType->second))
            continue;
        const std::uint32_t two = m.constant(rawType->second, 2);
        const FocusBlock focusBlock = push ? focus_block(m, rawType->second) : FocusBlock{};
        const std::size_t at = m.defined_at(load);  // constants may have moved it
        const std::uint32_t original = m.fresh();
        m.insts[at][2] = original;
        std::uint32_t col[4], sel[4];
        for (std::uint32_t& c : col)
            c = m.fresh();
        const std::uint32_t c0x = m.fresh(), c0xf = m.fresh(), c0f = m.fresh(), c1y = m.fresh(), c1yf = m.fresh(),
                            c1f = m.fresh(), c2a = m.fresh(), c2f = m.fresh(), isFocus = m.fresh(), mask = m.fresh();
        for (std::uint32_t& s : sel)
            s = m.fresh();
        const std::uint32_t focusCol[4] = {c0f, c1f, c2f, col[3]};
        std::vector<std::vector<std::uint32_t>> body;
        for (std::uint32_t k = 0; k < 4; ++k)
            body.push_back({op(kOpCompositeExtract, 5), v4, col[k], original, k});
        // The box's scale: col0.x / fx, col1.y / fy, the size this frame's (push constants) or the baked one.
        std::uint32_t box = 0;
        body.push_back({op(kOpCompositeExtract, 5), fl, c0x, col[0], 0});
        body.push_back({op(kOpCompositeExtract, 5), fl, c1y, col[1], 1});
        if (push) {
            box = focus_entry_load(m, focusBlock, chainFolded[ptr], body);
            const std::uint32_t sx = m.fresh(), sy = m.fresh();
            body.push_back({op(kOpCompositeExtract, 5), fl, sx, box, 2});
            body.push_back({op(kOpCompositeExtract, 5), fl, sy, box, 3});
            body.push_back({op(kOpFDiv, 5), fl, c0xf, c0x, sx});
            body.push_back({op(kOpFDiv, 5), fl, c1yf, c1y, sy});
        } else {
            body.push_back({op(kOpFMul, 5), fl, c0xf, c0x, cInvFx});
            body.push_back({op(kOpFMul, 5), fl, c1yf, c1y, cInvFy});
        }
        body.push_back({op(kOpCompositeInsert, 6), v4, c0f, c0xf, col[0], 0});
        body.push_back({op(kOpCompositeInsert, 6), v4, c1f, c1yf, col[1], 1});
        // Column 2's x and y: the centre (cx, cy) times the box's scale (focus_projection), 0 for the eye's axis.
        std::uint32_t c2x = cZero, c2y = cZero;
        if (push) {
            const std::uint32_t cx = m.fresh(), cy = m.fresh();
            c2x = m.fresh();
            c2y = m.fresh();
            body.push_back({op(kOpCompositeExtract, 5), fl, cx, box, 0});
            body.push_back({op(kOpCompositeExtract, 5), fl, cy, box, 1});
            body.push_back({op(kOpFMul, 5), fl, c2x, c0xf, cx});
            body.push_back({op(kOpFMul, 5), fl, c2y, c1yf, cy});
        }
        body.push_back({op(kOpCompositeInsert, 6), v4, c2a, c2x, col[2], 0});
        body.push_back({op(kOpCompositeInsert, 6), v4, c2f, c2y, c2a, 1});
        body.push_back({op(intSigned[rawType->second] ? kOpSGreaterThanEqual : kOpUGreaterThanEqual, 5), boolType, isFocus,
                        raw, two});
        body.push_back({op(kOpCompositeConstruct, 7), bv4, mask, isFocus, isFocus, isFocus, isFocus});
        for (int k = 0; k < 4; ++k)
            body.push_back({op(kOpSelect, 6), v4, sel[k], mask, focusCol[k], col[k]});
        body.push_back({op(kOpCompositeConstruct, 7), mat, load, sel[0], sel[1], sel[2], sel[3]});
        m.insts.insert(m.insts.begin() + static_cast<std::ptrdiff_t>(at + 1), body.begin(), body.end());
        ++edited;
    }
    if (edited > 0)
        words = m.emit();
    return edited;
}

bool spirv_layer_out(std::vector<std::uint32_t>& words, std::uint32_t location) {
    constexpr std::uint32_t kBuiltInLayer = 9, kExecutionVertex = 0;
    Module m;
    if (!m.parse(words))
        return false;
    bool vertex = false;
    for (const auto& w : m.insts)
        if (Module::code(w) == Module::kEntryPoint && w.size() >= 2 && w[1] == kExecutionVertex)
            vertex = true;
    std::uint32_t intType = 0;
    const std::uint32_t layer = builtin_variable(m, kBuiltInLayer, kOutput, &intType);
    if (!vertex || !layer || !intType || location_taken(m, location))
        return false;
    std::vector<std::size_t> stores;
    for (std::size_t i = 0; i < m.insts.size(); ++i)
        if (Module::code(m.insts[i]) == kOpStore && m.insts[i].size() >= 3 && m.insts[i][1] == layer)
            stores.push_back(i);
    if (stores.empty())
        return false;
    const std::uint32_t out = m.add_interface(m.type_pointer(kOutput, intType), kOutput, location);
    // Each store to gl_Layer is followed by the same store to ours (indexes moved by the globals just added).
    for (std::size_t i = m.insts.size(); i-- > 0;)
        if (Module::code(m.insts[i]) == kOpStore && m.insts[i].size() >= 3 && m.insts[i][1] == layer)
            m.insts.insert(m.insts.begin() + static_cast<std::ptrdiff_t>(i + 1), {op(kOpStore, 3), out, m.insts[i][2]});
    words = m.emit();
    return true;
}

int spirv_alpha_test_lod_bias(std::vector<std::uint32_t>& words, std::uint32_t location, float bias, std::string* why,
                              bool push) {
    auto fail = [&](const char* reason) {
        if (why)
            *why = reason;
        return 0;
    };
    constexpr std::uint32_t kExecutionFragment = 4;
    constexpr std::uint16_t kOpTypeBool = 20, kOpSelect = 169, kOpIEqual = 170, kOpSLessThan = 177, kOpLogicalAnd = 167,
                            kOpCompositeExtract = 81, kOpCompositeInsert = 82, kOpImageSampleImplicitLod = 87,
                            kOpPhi = 245, kOpExtInst = 12, kOpFNegate = 127,
                            kOpVectorTimesScalar = 142, kOpMemberNameCode = 6;
    Module m;
    if (!m.parse(words))
        return fail("not SPIR-V");
    bool fragment = false;
    for (const auto& w : m.insts)
        if (Module::code(w) == Module::kEntryPoint && w.size() >= 2 && w[1] == kExecutionFragment)
            fragment = true;
    if (!fragment)
        return fail("not a fragment shader");
    // Structs by member name: the alpha cut's and the transform block's counts.
    std::map<std::uint32_t, std::uint32_t> alphaCutOf;  // struct -> member index of u_alpha_cut
    std::uint32_t transformType = 0, transformCount = 0;
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpMemberNameCode && w.size() >= 4) {
            const std::string name = literal(w, 3, w.size());
            if (name == "u_alpha_cut")
                alphaCutOf[w[1]] = w[2];
            if (name == "u_transform_count") {
                transformType = w[1];
                transformCount = w[2];
            }
        }
    if (alphaCutOf.empty())
        return fail("no u_alpha_cut");
    if (!transformType)
        return fail("no transform block");
    std::map<std::uint32_t, std::uint32_t> constValue, varType;  // var -> struct
    std::map<std::uint32_t, std::size_t> def;                    // result id -> instruction index
    for (std::size_t i = 0; i < m.insts.size(); ++i) {
        const auto& w = m.insts[i];
        const std::uint16_t c = Module::code(w);
        if (c == kOpConstant && w.size() == 4)
            constValue[w[2]] = w[3];
        if (c == Module::kVariable && w.size() >= 4 && w[3] == kUniform)
            for (const auto& t : m.insts)
                if (Module::code(t) == Module::kTypePointer && t[1] == w[1])
                    varType[w[2]] = t[3];
        if (w.size() >= 3 && (typed_result(c) || c == kOpAccessChain || c == kOpInBoundsAccessChain ||
                              c == kOpCompositeExtract || c == kOpCompositeInsert || c == kOpCompositeConstruct ||
                              c == kOpVectorShuffle || c == kOpImageSampleImplicitLod || (c >= 127 && c <= 142)))
            def[w[2]] = i;
    }
    std::uint32_t transformVar = 0;
    for (const auto& [var, type] : varType)
        if (type == transformType)
            transformVar = var;
    if (!transformVar)
        return fail("transform block not bound");
    // Loads of the alpha cut (any component), then the float comparisons against them: the other side is the alpha.
    std::set<std::uint32_t> cutChains, cutValues;
    for (const auto& w : m.insts) {
        const std::uint16_t c = Module::code(w);
        if ((c == kOpAccessChain || c == kOpInBoundsAccessChain) && w.size() >= 5 && varType.count(w[3]) &&
            alphaCutOf.count(varType[w[3]]) && constValue.count(w[4]) && constValue[w[4]] == alphaCutOf[varType[w[3]]])
            cutChains.insert(w[2]);
    }
    for (const auto& w : m.insts) {
        const std::uint16_t c = Module::code(w);
        if (c == kOpLoad && w.size() >= 4 && cutChains.count(w[3]))
            cutValues.insert(w[2]);
        if (c == kOpCompositeExtract && w.size() >= 5 && cutValues.count(w[3]))
            cutValues.insert(w[2]);
    }
    // Values are traced as (id, component): -1 a scalar (or the whole value), else that component of a vector.
    std::vector<std::pair<std::uint32_t, int>> work;
    for (const auto& w : m.insts) {
        const std::uint16_t c = Module::code(w);
        if (c >= 184 && c <= 191 && w.size() == 5) {  // OpFOrd/FUnord LessThan .. GreaterThanEqual
            if (cutValues.count(w[3]))
                work.push_back({w[4], -1});
            if (cutValues.count(w[4]))
                work.push_back({w[3], -1});
        }
    }
    if (work.empty())
        return fail("no comparison with u_alpha_cut");
    // Component counts of vector types, and every result's type, to place a component in a composite.
    std::map<std::uint32_t, std::uint32_t> vecComponent, vecSize;  // vector type -> component type, size
    for (const auto& w : m.insts)
        if (Module::code(w) == Module::kTypeVector && w.size() == 4) {
            vecSize[w[1]] = w[3];
            if (w[3] == 4)
                vecComponent[w[1]] = w[2];
        }
    auto size_of = [&](std::uint32_t id) -> std::uint32_t {
        if (!def.count(id))
            return 1;
        const auto t = vecSize.find(m.insts[def[id]][1]);
        return t == vecSize.end() ? 1 : t->second;
    };
    // Back from the alpha to the texture samples whose alpha it is: through phis, selects, componentwise arithmetic and
    // GLSL.std.450 calls, and composites by component.
    std::set<std::pair<std::uint32_t, int>> seen;
    std::set<std::size_t> samples;
    while (!work.empty() && seen.size() < 4000) {
        const auto [id, comp] = work.back();
        work.pop_back();
        if (!seen.insert({id, comp}).second || !def.count(id))
            continue;
        const auto& w = m.insts[def[id]];
        const std::uint16_t c = Module::code(w);
        // An operand of a componentwise operation: the same component if it is a vector, else itself.
        auto same = [&](std::uint32_t operand) { work.push_back({operand, size_of(operand) > 1 ? comp : -1}); };
        if (c == kOpImageSampleImplicitLod) {
            if (comp == 3 || comp == -1)
                samples.insert(def[id]);
        } else if (c == kOpPhi) {
            for (std::size_t k = 3; k < w.size(); k += 2)
                same(w[k]);
        } else if (c == kOpExtInst) {
            for (std::size_t k = 5; k < w.size(); ++k)
                same(w[k]);
        } else if (c == kOpSelect) {
            same(w[4]);
            same(w[5]);
        } else if (c == kOpCompositeExtract && w.size() == 5) {
            work.push_back({w[3], static_cast<int>(w[4])});
        } else if (c == kOpCompositeInsert && w.size() == 6) {  // object, composite, index
            if (comp == static_cast<int>(w[5]))
                work.push_back({w[3], -1});
            else
                work.push_back({w[4], comp});
        } else if (c == kOpVectorShuffle && comp >= 0 && static_cast<std::size_t>(comp) + 5 < w.size()) {
            const std::uint32_t pick = w[5 + comp], first = size_of(w[3]);
            if (pick != 0xffffffff)
                work.push_back(pick < first ? std::pair<std::uint32_t, int>{w[3], static_cast<int>(pick)}
                                            : std::pair<std::uint32_t, int>{w[4], static_cast<int>(pick - first)});
        } else if (c == kOpCompositeConstruct && comp >= 0) {
            std::uint32_t at = 0;  // the constituent holding component `comp`
            for (std::size_t k = 3; k < w.size(); ++k) {
                const std::uint32_t n = size_of(w[k]);
                if (static_cast<std::uint32_t>(comp) < at + n) {
                    work.push_back({w[k], n > 1 ? static_cast<int>(comp - at) : -1});
                    break;
                }
                at += n;
            }
        } else if (c == kOpVectorTimesScalar && w.size() == 5) {
            work.push_back({w[3], comp});
            work.push_back({w[4], -1});
        } else if (c == kOpFNegate || c == kOpFAdd || c == kOpFSub || c == kOpFMul || c == kOpFDiv) {
            for (std::size_t k = 3; k < w.size(); ++k)
                same(w[k]);
        }
    }
    // Only plain samples (no image operands) of a float vec4.
    std::vector<std::uint32_t> targets;  // result ids
    for (const std::size_t i : samples)
        if (m.insts[i].size() == 5 && vecComponent.count(m.insts[i][1]))
            targets.push_back(m.insts[i][2]);
    if (targets.empty())
        return fail("no plain implicit-LOD sample reaches the alpha test");
    if (location_taken(m, location))
        return fail("location taken");
    // The int type of the transform count, a bool, and the constants.
    std::uint32_t intType = 0;
    {
        std::vector<std::uint32_t> members;
        for (const auto& w : m.insts)
            if (Module::code(w) == kOpTypeStruct && w[1] == transformType)
                members.assign(w.begin() + 2, w.end());
        if (transformCount >= members.size())
            return fail("transform block layout");
        intType = members[transformCount];
    }
    const std::uint32_t fl = m.type_float();
    std::uint32_t boolType = m.find(kOpTypeBool, {}, 1);
    if (!boolType)
        boolType = m.add_global({op(kOpTypeBool, 2), 0}, 1);
    std::uint32_t biasBits, zeroBits;
    const float zero = 0.0f;
    std::memcpy(&biasBits, &bias, 4);
    std::memcpy(&zeroBits, &zero, 4);
    const std::uint32_t cZero = m.constant(fl, zeroBits), c2 = m.constant(intType, 2), c4 = m.constant(intType, 4),
                        cCount = m.constant(intType, transformCount);
    const std::uint32_t cBias = push ? 0 : m.constant(fl, biasBits);  // the baked bias (without the push block)
    const FocusBlock focusBlock = push ? focus_block(m, intType) : FocusBlock{};
    const std::uint32_t ptrInt = m.type_pointer(kUniform, intType);
    const std::uint32_t layerVar = m.add_interface(m.type_pointer(kInput, intType), kInput, location);
    int edited = 0;
    for (const std::uint32_t target : targets) {
        const std::size_t at = m.defined_at(target);
        if (at >= m.insts.size())
            continue;
        const std::vector<std::uint32_t> sample = m.insts[at];
        const std::uint32_t v4 = sample[1], original = m.fresh();
        m.insts[at][2] = original;
        const std::uint32_t layer = m.fresh(), countPtr = m.fresh(), count = m.fresh(), periphery = m.fresh(),
                            four = m.fresh(), on = m.fresh(), b = m.fresh(), biased = m.fresh(), alpha = m.fresh();
        std::vector<std::vector<std::uint32_t>> body = {
            {op(kOpLoad, 4), intType, layer, layerVar},
            {op(kOpAccessChain, 5), ptrInt, countPtr, transformVar, cCount},
            {op(kOpLoad, 4), intType, count, countPtr},
            {op(kOpSLessThan, 5), boolType, periphery, layer, c2},
            {op(kOpIEqual, 5), boolType, four, count, c4},
            {op(kOpLogicalAnd, 5), boolType, on, periphery, four},
        };
        // The bias: this frame's (the push block's entry 2 x, following the focus size), or the baked one.
        std::uint32_t biasValue = cBias;
        if (push) {
            const std::uint32_t values = focus_entry_load(m, focusBlock, focusBlock.values, body);
            biasValue = m.fresh();
            body.push_back({op(kOpCompositeExtract, 5), fl, biasValue, values, 0});
        }
        body.insert(body.end(), {
            {op(kOpSelect, 6), fl, b, on, biasValue, cZero},
            {op(kOpImageSampleImplicitLod, 7), v4, biased, sample[3], sample[4], 0x1 /* Bias */, b},
            {op(kOpCompositeExtract, 5), vecComponent[v4], alpha, biased, 3},
            {op(kOpCompositeInsert, 6), v4, target, alpha, original, 3},
        });
        m.insts.insert(m.insts.begin() + static_cast<std::ptrdiff_t>(at + 1), body.begin(), body.end());
        ++edited;
    }
    if (!edited)
        return fail("samples moved");
    words = m.emit();
    return edited;
}

int spirv_focus_light_tiles(std::vector<std::uint32_t>& words, float fx, float fy, std::string* why, bool push) {
    auto fail = [&](const char* reason) {
        if (why)
            *why = reason;
        return 0;
    };
    constexpr std::uint16_t kOpTypeBool = 20, kOpSelect = 169, kOpSDiv = 135, kOpConvertSToF = 111,
                            kOpConvertFToS = 110, kOpSGreaterThanEqual = 175, kOpCompositeExtract = 81,
                            kOpFNegate = 127, kOpMemberNameCode = 6;
    Module m;
    if (!m.parse(words) || (!push && (!(fx > 0.0f) || !(fy > 0.0f))))
        return fail("not SPIR-V");
    // Members by name: the tile size, the projection coefficients and the viewport size.
    std::uint32_t tileType = 0, tileSize = 0, gbufType = 0, coefIndex = 0, sizeIndex = 0;
    std::map<std::uint32_t, std::uint32_t> coefOf, sizeOf;  // struct -> member index
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpMemberNameCode && w.size() >= 4) {
            const std::string name = literal(w, 3, w.size());
            if (name == "u_light_tile_size") {
                tileType = w[1];
                tileSize = w[2];
            } else if (name == "u_proj_coefficients") {
                coefOf[w[1]] = w[2];
            } else if (name == "u_viewport_size") {
                sizeOf[w[1]] = w[2];
            }
        }
    if (!tileType)
        return fail("no light tiles");
    for (const auto& [type, index] : coefOf)  // the block with both (u_gbuffer_data)
        if (sizeOf.count(type)) {
            gbufType = type;
            coefIndex = index;
            sizeIndex = sizeOf[type];
        }
    if (!gbufType)
        return fail("no u_gbuffer_data coefficients and viewport size");
    std::uint32_t intType = 0;
    const std::uint32_t eyeVar = m.variable("v_eye", kInput, &intType);
    if (!eyeVar || !intType)
        return fail("no v_eye input");
    std::map<std::uint32_t, std::uint32_t> constValue, varType;
    for (const auto& w : m.insts) {
        const std::uint16_t c = Module::code(w);
        if (c == kOpConstant && w.size() == 4)
            constValue[w[2]] = w[3];
        if (c == Module::kVariable && w.size() >= 4 && w[3] == kUniform)
            for (const auto& t : m.insts)
                if (Module::code(t) == Module::kTypePointer && t[1] == w[1])
                    varType[w[2]] = t[3];
    }
    std::uint32_t tileVar = 0, gbufVar = 0;
    for (const auto& [var, type] : varType) {
        if (type == tileType)
            tileVar = var;
        if (type == gbufType)
            gbufVar = var;
    }
    if (!tileVar || !gbufVar)
        return fail("blocks not bound");
    // tile_size[k] loads: chain [tileVar][tileSize][k] -> k.
    std::map<std::uint32_t, std::uint32_t> sizeChain, sizeLoad;
    for (const auto& w : m.insts) {
        const std::uint16_t c = Module::code(w);
        if ((c == kOpAccessChain || c == kOpInBoundsAccessChain) && w.size() == 6 && w[3] == tileVar &&
            constValue.count(w[4]) && constValue[w[4]] == tileSize && constValue.count(w[5]) && constValue[w[5]] <= 1)
            sizeChain[w[2]] = constValue[w[5]];
    }
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpLoad && w.size() >= 4 && sizeChain.count(w[3]))
            sizeLoad[w[2]] = sizeChain[w[3]];
    std::vector<std::uint32_t> divisions;  // result ids of `coordinate / tile_size[k]`
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpSDiv && w.size() == 5 && w[1] == intType && sizeLoad.count(w[4]))
            divisions.push_back(w[2]);
    if (divisions.empty())
        return fail("no division by u_light_tile_size");
    // Types and constants.
    const std::uint32_t fl = m.type_float(), v4 = m.type_vector(fl, 4);
    std::uint32_t boolType = m.find(kOpTypeBool, {}, 1);
    if (!boolType)
        boolType = m.add_global({op(kOpTypeBool, 2), 0}, 1);
    auto fconst = [&](float v) {
        std::uint32_t bits;
        std::memcpy(&bits, &v, 4);
        return m.constant(fl, bits);
    };
    const std::uint32_t c1 = m.constant(intType, 1), c2 = m.constant(intType, 2), cCoef = m.constant(intType, coefIndex),
                        cSize = m.constant(intType, sizeIndex);
    const std::uint32_t ptrV4 = m.type_pointer(kUniform, v4), ptrF = m.type_pointer(kUniform, fl);
    int edited = 0;
    for (const std::uint32_t division : divisions) {
        const std::size_t at = m.defined_at(division);
        if (at >= m.insts.size())
            continue;
        const std::uint32_t x = m.insts[at][3], k = sizeLoad[m.insts[at][4]];
        const float f = k == 0 ? fx : fy;
        const std::uint32_t cK = m.constant(intType, k);
        // The box's size along k: this frame's (push constants) or the baked one.
        std::uint32_t cF = 0, cHalfF = 0, cHalf = 0;
        if (push)
            cHalf = fconst(0.5f);
        else {
            cF = fconst(f);
            cHalfF = fconst(0.5f * f);
        }
        const FocusBlock focusBlock = push ? focus_block(m, intType) : FocusBlock{};
        const std::uint32_t cRects = push ? m.constant(intType, kViewRectsEntry) : 0;
        const std::size_t here = m.defined_at(division);  // constants may have moved it
        const std::uint32_t eye = m.fresh(), eyeBit = m.fresh(), coefPtr = m.fresh(), coef = m.fresh(), scale = m.fresh(),
                            offset = m.fresh(), negOffset = m.fresh(), centre = m.fresh(), sizePtr = m.fresh(),
                            size = m.fresh(), xf = m.fresh(), corner = m.fresh(), origin = m.fresh(), scaled = m.fresh(),
                            mappedF = m.fresh(), mapped = m.fresh(), focus = m.fresh(), chosen = m.fresh();
        std::vector<std::vector<std::uint32_t>> body = {
            {op(kOpLoad, 4), intType, eye, eyeVar},
            {op(kOpBitwiseAnd, 5), intType, eyeBit, eye, c1},
            {op(kOpAccessChain, 6), ptrV4, coefPtr, gbufVar, cCoef, eyeBit},
            {op(kOpLoad, 4), v4, coef, coefPtr},
            {op(kOpCompositeExtract, 5), fl, scale, coef, k},
            {op(kOpCompositeExtract, 5), fl, offset, coef, k + 2},
        };
        // uv0, the box centre in the eye: (t - c.zw) / c.xy for its centre tangent t (the gaze's; else 0).
        if (push) {
            const std::uint32_t box = focus_entry_load(m, focusBlock, eyeBit, body), t = m.fresh();
            cF = m.fresh();
            cHalfF = m.fresh();
            body.push_back({op(kOpCompositeExtract, 5), fl, t, box, k});
            body.push_back({op(kOpCompositeExtract, 5), fl, cF, box, k + 2});
            body.push_back({op(kOpFMul, 5), fl, cHalfF, cF, cHalf});
            body.push_back({op(kOpFSub, 5), fl, negOffset, t, offset});
        } else {
            body.push_back({op(kOpFNegate, 4), fl, negOffset, offset});
        }
        // Per-view rects (push constants, kViewRectsEntry): the focus pixel in its whole view is x / s_f, and its eye
        // pixel lies in the eye's rect (x s_o).
        std::uint32_t inView = xf, eyeShare = 0;
        if (push) {
            const std::uint32_t rects = focus_entry_load(m, focusBlock, cRects, body), viewShare = m.fresh();
            eyeShare = m.fresh();
            inView = m.fresh();
            body.push_back({op(kOpCompositeExtract, 5), fl, viewShare, rects, k + 2});
            body.push_back({op(kOpCompositeExtract, 5), fl, eyeShare, rects, k});
            body.push_back({op(kOpConvertSToF, 4), fl, xf, x});
            body.push_back({op(kOpFDiv, 5), fl, inView, xf, viewShare});
        } else {
            body.push_back({op(kOpConvertSToF, 4), fl, xf, x});
        }
        const std::uint32_t inEye = push ? m.fresh() : mappedF, mappedView = push ? m.fresh() : mappedF;
        body.insert(body.end(), {
            {op(kOpFDiv, 5), fl, centre, negOffset, scale},
            {op(kOpAccessChain, 6), ptrF, sizePtr, gbufVar, cSize, cK},  // u_viewport_size[k]
            {op(kOpLoad, 4), fl, size, sizePtr},
            {op(kOpFSub, 5), fl, corner, centre, cHalfF},
            {op(kOpFMul, 5), fl, origin, corner, size},
            {op(kOpFMul, 5), fl, scaled, inView, cF},
            {op(kOpFAdd, 5), fl, mappedView, origin, scaled},
        });
        if (push)
            body.push_back({op(kOpFMul, 5), fl, inEye, mappedView, eyeShare});
        body.insert(body.end(), {
            {op(kOpConvertFToS, 4), intType, mapped, push ? inEye : mappedF},
            {op(kOpSGreaterThanEqual, 5), boolType, focus, eye, c2},
            {op(kOpSelect, 6), intType, chosen, focus, mapped, x},
        });
        m.insts.insert(m.insts.begin() + static_cast<std::ptrdiff_t>(here), body.begin(), body.end());
        m.insts[here + body.size()][3] = chosen;  // the division takes the chosen coordinate
        ++edited;
    }
    if (!edited)
        return fail("divisions moved");
    words = m.emit();
    return edited;
}

int spirv_focus_ao_edges_from_eye(std::vector<std::uint32_t>& words, float fx, float fy, float band, std::string* why,
                                  bool push) {
    auto fail = [&](const char* reason) {
        if (why)
            *why = reason;
        return 0;
    };
    constexpr std::uint16_t kOpExtInstImport = 11, kOpExtInst = 12, kOpTypeBool = 20, kOpCompositeExtract = 81,
                            kOpConvertSToF = 111, kOpFNegate = 127, kOpImageSampleImplicitLod = 87,
                            kOpImageSampleExplicitLod = 88, kOpSelect = 169, kOpSGreaterThanEqual = 175,
                            kOpMemberNameCode = 6, kOpReturnValue = 254;
    constexpr std::uint32_t kUniformConstant = 0, kGlslFMin = 37, kGlslFMix = 46, kGlslSmoothStep = 49,
                            kImageOperandLod = 2;
    constexpr std::uint16_t kOpVectorTimesScalarCode = 142;
    Module m;
    if (!m.parse(words) || (!push && (!(fx > 0.0f) || !(fy > 0.0f) || !(band > 0.0f))))
        return fail("not SPIR-V");
    std::uint32_t aoType = 0;
    const std::uint32_t ao = m.variable("u_tex_ssao", kUniformConstant, &aoType);
    if (!ao)
        return fail("no u_tex_ssao");
    std::uint32_t intType = 0;
    const std::uint32_t eyeVar = m.variable("v_eye", kInput, &intType);
    if (!eyeVar || !intType)
        return fail("no v_eye input");
    std::uint32_t glsl = 0;
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpExtInstImport && w.size() >= 3 && literal(w, 2, w.size()) == "GLSL.std.450")
            glsl = w[1];
    if (!glsl)
        return fail("no GLSL.std.450");
    // u_gbuffer_data: the block with u_proj_coefficients, u_viewport_size and u_gbuffer_size, and its variable.
    std::map<std::uint32_t, std::map<std::string, std::uint32_t>> members;
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpMemberNameCode && w.size() >= 4)
            members[w[1]][literal(w, 3, w.size())] = w[2];
    std::uint32_t gbufType = 0;
    for (const auto& [type, names] : members)
        if (names.count("u_proj_coefficients") && names.count("u_viewport_size") && names.count("u_gbuffer_size"))
            gbufType = type;
    if (!gbufType)
        return fail("no u_gbuffer_data");
    std::uint32_t gbufVar = 0;
    for (const auto& w : m.insts)
        if (Module::code(w) == Module::kVariable && w.size() >= 4 && w[3] == kUniform)
            for (const auto& t : m.insts)
                if (Module::code(t) == Module::kTypePointer && t[1] == w[1] && t[3] == gbufType)
                    gbufVar = w[2];
    if (!gbufVar)
        return fail("u_gbuffer_data not bound");
    // The samples of u_tex_ssao (through its loads).
    std::set<std::uint32_t> aoLoads;
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpLoad && w.size() >= 4 && w[3] == ao)
            aoLoads.insert(w[2]);
    std::vector<std::uint32_t> samples;
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpImageSampleImplicitLod && w.size() >= 5 && aoLoads.count(w[3]))
            samples.push_back(w[2]);
    if (samples.empty())
        return fail("u_tex_ssao not sampled");
    // Types and constants first.
    const std::uint32_t fl = m.type_float(), v2 = m.type_vector(fl, 2), v3 = m.type_vector(fl, 3),
                        v4 = m.type_vector(fl, 4);
    std::uint32_t boolType = m.find(kOpTypeBool, {}, 1);
    if (!boolType)
        boolType = m.add_global({op(kOpTypeBool, 2), 0}, 1);
    const std::uint32_t b4 = m.type_vector(boolType, 4), ptrV4 = m.type_pointer(kUniform, v4);
    auto fconst = [&](float v) {
        std::uint32_t bits;
        std::memcpy(&bits, &v, 4);
        return m.constant(fl, bits);
    };
    const std::map<std::string, std::uint32_t>& names = members[gbufType];
    const std::uint32_t c1 = m.constant(intType, 1), c2 = m.constant(intType, 2),
                        cCoef = m.constant(intType, names.at("u_proj_coefficients")),
                        cViewport = m.constant(intType, names.at("u_viewport_size")),
                        cGbuffer = m.constant(intType, names.at("u_gbuffer_size")), f0 = fconst(0.0f), f1 = fconst(1.0f);
    // The box's size and the band: this frame's (push constants) or the baked ones.
    std::uint32_t cFx = 0, cFy = 0, cHalfFx = 0, cHalfFy = 0, cBand = 0, cHalf = 0;
    if (push)
        cHalf = fconst(0.5f);
    else {
        cFx = fconst(fx);
        cFy = fconst(fy);
        cHalfFx = fconst(0.5f * fx);
        cHalfFy = fconst(0.5f * fy);
        cBand = fconst(band);
    }
    const FocusBlock focusBlock = push ? focus_block(m, intType) : FocusBlock{};
    const std::uint32_t cRects = push ? m.constant(intType, kViewRectsEntry) : 0;
    int edited = 0;
    for (const std::uint32_t sample : samples) {
        const std::size_t at = m.defined_at(sample);
        if (at >= m.insts.size())
            continue;
        const std::vector<std::uint32_t> original = m.insts[at];  // {op, v4, result, sampled image, coord, ...}
        if (original[1] != v4)
            continue;
        std::vector<std::vector<std::uint32_t>> body;
        auto emit = [&](std::vector<std::uint32_t> w) {
            const std::uint32_t id = m.fresh();
            w[2] = id;
            w[0] = op(static_cast<std::uint16_t>(w[0]), static_cast<std::uint16_t>(w.size()));
            body.push_back(std::move(w));
            return id;
        };
        const std::uint32_t image = original[3], coord = original[4];
        const std::uint32_t eye = emit({kOpLoad, intType, 0, eyeVar});
        const std::uint32_t eyeBit = emit({kOpBitwiseAnd, intType, 0, eye, c1});
        const std::uint32_t coef = emit({kOpLoad, v4, 0, emit({kOpAccessChain, ptrV4, 0, gbufVar, cCoef, eyeBit})});
        const std::uint32_t viewport = emit({kOpLoad, v4, 0, emit({kOpAccessChain, ptrV4, 0, gbufVar, cViewport})});
        const std::uint32_t gbuffer = emit({kOpLoad, v4, 0, emit({kOpAccessChain, ptrV4, 0, gbufVar, cGbuffer})});
        // The pixel (uv x the G-buffer size), and its eye pixel: (uv0 - f / 2) x viewport + pixel x f, uv0 = (t - c.zw)
        // / c.xy, the box centre's tangent t (the gaze's; else 0).
        const std::uint32_t uv = emit({kOpVectorShuffle, v2, 0, coord, coord, 0, 1});
        const std::uint32_t pixel = emit({kOpFMul, v2, 0, uv, emit({kOpVectorShuffle, v2, 0, gbuffer, gbuffer, 0, 1})});
        const std::uint32_t coefOffset = emit({kOpVectorShuffle, v2, 0, coef, coef, 2, 3});
        std::uint32_t fromCentre = 0, f2 = 0, halfF2 = 0, bandValue = cBand, eyeShare = 0, viewShare = 0;
        if (push) {
            // Per-view rects (kViewRectsEntry): the focus view in share s_f of its layer, its eye in s_o of its own.
            const std::uint32_t rects = focus_entry_load(m, focusBlock, cRects, body);
            eyeShare = focus_pair(m, focusBlock, rects, 0, body);
            viewShare = focus_pair(m, focusBlock, rects, 2, body);
            const std::uint32_t box = focus_entry_load(m, focusBlock, eyeBit, body);
            const std::uint32_t t = focus_pair(m, focusBlock, box, 0, body);
            f2 = focus_pair(m, focusBlock, box, 2, body);
            halfF2 = emit({kOpVectorTimesScalarCode, v2, 0, f2, cHalf});
            fromCentre = emit({kOpFSub, v2, 0, t, coefOffset});
            const std::uint32_t values = focus_entry_load(m, focusBlock, focusBlock.values, body);
            bandValue = emit({kOpCompositeExtract, fl, 0, values, 1});
        } else {
            fromCentre = emit({kOpFNegate, v2, 0, coefOffset});
            f2 = emit({kOpCompositeConstruct, v2, 0, cFx, cFy});
            halfF2 = emit({kOpCompositeConstruct, v2, 0, cHalfFx, cHalfFy});
        }
        const std::uint32_t uv0 = emit({kOpFDiv, v2, 0, fromCentre, emit({kOpVectorShuffle, v2, 0, coef, coef, 0, 1})});
        const std::uint32_t origin =
            emit({kOpFMul, v2, 0, emit({kOpFSub, v2, 0, uv0, halfF2}), emit({kOpVectorShuffle, v2, 0, viewport, viewport, 0, 1})});
        // The pixel in its whole view (pixel / s_f with rects), its eye pixel in the eye's rect (x s_o).
        const std::uint32_t inView = push ? emit({kOpFDiv, v2, 0, pixel, viewShare}) : pixel;
        std::uint32_t eyePixel = emit({kOpFAdd, v2, 0, origin, emit({kOpFMul, v2, 0, inView, f2})});
        if (push)
            eyePixel = emit({kOpFMul, v2, 0, eyePixel, eyeShare});
        const std::uint32_t eyeUv = emit({kOpFMul, v2, 0, eyePixel, emit({kOpVectorShuffle, v2, 0, gbuffer, gbuffer, 2, 3})});
        const std::uint32_t eyeCoord = emit({kOpCompositeConstruct, v3, 0, eyeUv, emit({kOpConvertSToF, fl, 0, eyeBit})});
        const std::uint32_t eyeAo = emit({kOpImageSampleExplicitLod, v4, 0, image, eyeCoord, kImageOperandLod, f0});
        // The weight: the distance to the view's nearest edge (in viewport uv), smoothstep(0, band).
        const std::uint32_t own = emit({kOpFMul, v2, 0, inView, emit({kOpVectorShuffle, v2, 0, viewport, viewport, 2, 3})});
        const std::uint32_t one2 = emit({kOpCompositeConstruct, v2, 0, f1, f1});
        const std::uint32_t near2 = emit({kOpExtInst, v2, 0, glsl, kGlslFMin, own, emit({kOpFSub, v2, 0, one2, own})});
        const std::uint32_t edge = emit({kOpExtInst, fl, 0, glsl, kGlslFMin, emit({kOpCompositeExtract, fl, 0, near2, 0}),
                                         emit({kOpCompositeExtract, fl, 0, near2, 1})});
        const std::uint32_t weight = emit({kOpExtInst, fl, 0, glsl, kGlslSmoothStep, f0, bandValue, edge});
        const std::uint32_t weight4 = emit({kOpCompositeConstruct, v4, 0, weight, weight, weight, weight});
        const std::uint32_t mixed = emit({kOpExtInst, v4, 0, glsl, kGlslFMix, eyeAo, sample, weight4});
        const std::uint32_t focus = emit({kOpSGreaterThanEqual, boolType, 0, eye, c2});
        const std::uint32_t focus4 = emit({kOpCompositeConstruct, b4, 0, focus, focus, focus, focus});
        const std::uint32_t chosen = emit({kOpSelect, v4, 0, focus4, mixed, sample});
        const std::size_t here = m.defined_at(sample) + 1;
        m.insts.insert(m.insts.begin() + static_cast<std::ptrdiff_t>(here), body.begin(), body.end());
        // Every later use of the sample takes the chosen AO.
        for (std::size_t i = here + body.size(); i < m.insts.size(); ++i) {
            auto& u = m.insts[i];
            const std::uint16_t c = Module::code(u);
            const std::size_t first = (c == kOpStore || c == kOpReturnValue) ? 1 : 3;
            for (std::size_t k = first; k < u.size(); ++k)
                if (u[k] == sample)
                    u[k] = chosen;
        }
        ++edited;
    }
    if (!edited)
        return fail("samples not vec4");
    words = m.emit();
    return edited;
}

}  // namespace xplane
