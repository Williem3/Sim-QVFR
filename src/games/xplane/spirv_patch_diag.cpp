// SPIR-V diagnostic edits: a pass's output switched off live (spirv_patch.h).
#include "spirv_module.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace xplane {

using namespace spirv;

int spirv_diag_override_write(std::vector<std::uint32_t>& words, const char* block, const char* image,
                              std::uint32_t component, float value, std::string* why) {
    auto fail = [&](const char* reason) {
        if (why)
            *why = reason;
        return 0;
    };
    constexpr std::uint16_t kOpTypeBool = 20, kOpConstantComposite = 44, kOpCompositeExtract = 81, kOpSelect = 169,
                            kOpFOrdGreaterThan = 186, kOpImageWrite = 99;
    Module m;
    if (!m.parse(words))
        return fail("not SPIR-V");
    if (!m.named(block))
        return fail("no such block");
    const std::uint32_t imageVar = m.named(image);
    if (!imageVar)
        return fail("no such image");
    std::set<std::uint32_t> imageLoads;
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpLoad && w.size() >= 4 && w[3] == imageVar)
            imageLoads.insert(w[2]);
    auto type_of = [&](std::uint32_t id) -> std::uint32_t {
        const std::size_t at = m.defined_at(id);
        return at < m.insts.size() && m.insts[at].size() >= 3 ? m.insts[at][1] : 0;
    };
    // The writes, with their texel's type: a float or a vector of floats.
    const std::uint32_t fl = m.type_float();
    std::vector<std::uint32_t> writes;  // the texels written
    std::map<std::uint32_t, std::uint32_t> lanes;  // texel type -> its component count
    lanes[fl] = 1;
    for (const auto& w : m.insts)
        if (Module::code(w) == Module::kTypeVector && w.size() == 4 && w[2] == fl)
            lanes[w[1]] = w[3];
    for (std::size_t i = 0; i < m.insts.size(); ++i) {
        const auto& w = m.insts[i];
        if (Module::code(w) == kOpImageWrite && w.size() >= 4 && imageLoads.count(w[1]) && lanes.count(type_of(w[3])))
            writes.push_back(w[3]);
    }
    if (writes.empty())
        return fail("no float write to the image");
    // Types and constants first (they go before the first function and move the instructions).
    std::uint32_t intType = m.find(kOpTypeInt, {32, 1}, 1);
    if (!intType)
        intType = m.add_global({op(kOpTypeInt, 4), 0, 32, 1}, 1);
    std::uint32_t boolType = m.find(kOpTypeBool, {}, 1);
    if (!boolType)
        boolType = m.add_global({op(kOpTypeBool, 2), 0}, 1);
    const FocusBlock f = focus_block(m, intType);
    std::uint32_t valueBits, halfBits;
    std::memcpy(&valueBits, &value, 4);
    const float half = 0.5f;
    std::memcpy(&halfBits, &half, 4);
    const std::uint32_t cValue = m.constant(fl, valueBits), cHalf = m.constant(fl, halfBits),
                        cDiag = m.constant(intType, kDiagEntry);
    std::map<std::uint32_t, std::uint32_t> replacement, boolVector;  // texel type -> constant, its bool vector type
    for (const std::uint32_t texel : writes) {
        const std::uint32_t type = type_of(texel), n = lanes[type];
        if (replacement.count(type))
            continue;
        if (n == 1) {
            replacement[type] = cValue;
            continue;
        }
        std::vector<std::uint32_t> c = {op(kOpConstantComposite, static_cast<std::uint16_t>(3 + n)), type, 0};
        c.insert(c.end(), n, cValue);
        replacement[type] = m.add_global(c, 2);
        boolVector[type] = m.type_vector(boolType, n);
    }
    int edited = 0;
    for (std::size_t i = 0; i < m.insts.size(); ++i) {
        auto& w = m.insts[i];
        if (Module::code(w) != kOpImageWrite || w.size() < 4 || !imageLoads.count(w[1]))
            continue;
        const std::uint32_t texel = w[3], type = type_of(texel);
        if (!replacement.count(type))
            continue;
        std::vector<std::vector<std::uint32_t>> body;
        auto emit = [&](std::vector<std::uint32_t> e) {
            e[2] = m.fresh();
            e[0] = op(static_cast<std::uint16_t>(e[0]), static_cast<std::uint16_t>(e.size()));
            body.push_back(e);
            return e[2];
        };
        const std::uint32_t values = focus_entry_load(m, f, cDiag, body);
        const std::uint32_t flag = emit({kOpCompositeExtract, fl, 0, values, component});
        std::uint32_t on = emit({kOpFOrdGreaterThan, boolType, 0, flag, cHalf});
        if (lanes[type] > 1) {
            std::vector<std::uint32_t> b = {kOpCompositeConstruct, boolVector[type], 0};
            b.insert(b.end(), lanes[type], on);
            on = emit(b);
        }
        const std::uint32_t chosen = emit({kOpSelect, type, 0, on, replacement[type], texel});
        m.insts.insert(m.insts.begin() + static_cast<std::ptrdiff_t>(i), body.begin(), body.end());
        i += body.size();
        m.insts[i][3] = chosen;
        ++edited;
    }
    words = m.emit();
    return edited;
}

int spirv_diag_override_sample(std::vector<std::uint32_t>& words, const char* block, const char* image,
                               std::uint32_t component, float value, std::string* why) {
    auto fail = [&](const char* reason) {
        if (why)
            *why = reason;
        return 0;
    };
    constexpr std::uint16_t kOpTypeBool = 20, kOpCompositeExtract = 81, kOpSelect = 169, kOpFOrdGreaterThan = 186,
                            kOpSampleDrefImplicit = 89, kOpSampleDrefExplicit = 90;
    Module m;
    if (!m.parse(words))
        return fail("not SPIR-V");
    if (!m.named(block))
        return fail("no such block");
    const std::uint32_t imageVar = m.named(image);
    if (!imageVar)
        return fail("no such image");
    std::set<std::uint32_t> imageLoads;
    for (const auto& w : m.insts)
        if (Module::code(w) == kOpLoad && w.size() >= 4 && w[3] == imageVar)
            imageLoads.insert(w[2]);
    const std::uint32_t fl = m.type_float();
    std::vector<std::uint32_t> samples;  // their results (floats)
    for (const auto& w : m.insts) {
        const std::uint16_t c = Module::code(w);
        if ((c == kOpSampleDrefImplicit || c == kOpSampleDrefExplicit) && w.size() >= 6 && w[1] == fl &&
            imageLoads.count(w[3]))
            samples.push_back(w[2]);
    }
    if (samples.empty())
        return fail("no depth-compare sample of the image");
    std::uint32_t intType = m.find(kOpTypeInt, {32, 1}, 1);
    if (!intType)
        intType = m.add_global({op(kOpTypeInt, 4), 0, 32, 1}, 1);
    std::uint32_t boolType = m.find(kOpTypeBool, {}, 1);
    if (!boolType)
        boolType = m.add_global({op(kOpTypeBool, 2), 0}, 1);
    const FocusBlock f = focus_block(m, intType);
    std::uint32_t valueBits, halfBits;
    std::memcpy(&valueBits, &value, 4);
    const float half = 0.5f;
    std::memcpy(&halfBits, &half, 4);
    const std::uint32_t cValue = m.constant(fl, valueBits), cHalf = m.constant(fl, halfBits),
                        cDiag = m.constant(intType, kDiagEntry);
    int edited = 0;
    for (const std::uint32_t result : samples) {
        // The sample gets a new id; right after it, `result` = the switch on ? value : the sample.
        const std::size_t at = m.defined_at(result);
        if (at >= m.insts.size())
            continue;
        const std::uint32_t raw = m.fresh();
        m.insts[at][2] = raw;
        std::vector<std::vector<std::uint32_t>> body;
        const std::uint32_t values = focus_entry_load(m, f, cDiag, body);
        const std::uint32_t flag = m.fresh(), on = m.fresh();
        body.push_back({op(kOpCompositeExtract, 5), fl, flag, values, component});
        body.push_back({op(kOpFOrdGreaterThan, 5), boolType, on, flag, cHalf});
        body.push_back({op(kOpSelect, 6), fl, result, on, cValue, raw});
        m.insts.insert(m.insts.begin() + static_cast<std::ptrdiff_t>(at + 1), body.begin(), body.end());
        ++edited;
    }
    words = m.emit();
    return edited;
}

}  // namespace xplane
