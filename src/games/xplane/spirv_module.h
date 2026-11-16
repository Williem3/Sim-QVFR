// Internal to the spirv_*.cpp edits (spirv_patch.h is the public API): SPIR-V constants, Module, the focus block.
#pragma once

#include "spirv_patch.h"

#include <cstddef>
#include <utility>

namespace xplane::spirv {

// SPIR-V: a 5-word header (magic, version, generator, bound, schema), then instructions, each starting with a word
// holding its word count (high 16 bits) and opcode (low 16 bits).
constexpr std::uint32_t kMagic = 0x07230203;
constexpr std::size_t kHeader = 5;
constexpr std::uint16_t kOpName = 5, kOpTypeInt = 21, kOpConstant = 43, kOpVariable = 59, kOpLoad = 61,
                        kOpIAdd = 128, kOpTypeArray = 28, kOpTypeStruct = 30, kOpTypePointer = 32,
                        kOpAccessChain = 65, kOpInBoundsAccessChain = 66, kOpBitwiseAnd = 199;
constexpr std::uint32_t kUniform = 2;

// Instructions whose word 1 is the result type and word 2 the result id, among those that make integer indexes:
// ExtInst, Constant, SpecConstant, Load, VectorExtractDynamic, CompositeExtract, conversions, Bitcast, SNegate,
// integer arithmetic, Select, shifts and bitwise operations, Phi.
bool typed_result(std::uint16_t code);

// Every instruction with both a result type (word 1) and a result id (word 2), per the SPIR-V core grammar (1.6): the
// only ones whose word 2 is a definition. Types, OpString, OpLabel and the like hold their result in word 1;
// OpSource, OpEntryPoint, decorations and names hold other operands in word 2.
bool result_in_word2(std::uint16_t code);

std::uint32_t op(std::uint16_t code, std::uint16_t count);

// The literal string starting at word `at` of an instruction `count` words long.
std::string literal(const std::vector<std::uint32_t>& w, std::size_t at, std::size_t end);

// A parsed module for edits that add declarations: instructions as word vectors, the id bound, find-or-add helpers.
struct Module {
    std::vector<std::uint32_t> header;
    std::vector<std::vector<std::uint32_t>> insts;
    std::uint32_t bound = 0;

    static constexpr std::uint16_t kTypeFloat = 22, kTypeVector = 23, kTypePointer = 32, kConstant = 43,
                                   kVariable = 59, kName = 5, kMemberName = 6, kDecorate = 71, kMemberDecorate = 72,
                                   kEntryPoint = 15, kFunction = 54;

    bool parse(const std::vector<std::uint32_t>& words) {
        if (words.size() < kHeader || words[0] != kMagic)
            return false;
        header.assign(words.begin(), words.begin() + kHeader);
        bound = words[3];
        for (std::size_t i = kHeader; i < words.size();) {
            const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16);
            if (count == 0 || i + count > words.size())
                return false;
            insts.emplace_back(words.begin() + static_cast<std::ptrdiff_t>(i),
                               words.begin() + static_cast<std::ptrdiff_t>(i + count));
            i += count;
        }
        return true;
    }
    std::vector<std::uint32_t> emit() {
        std::vector<std::uint32_t> out = header;
        out[3] = bound;
        for (auto& w : insts) {
            w[0] = op(code(w), static_cast<std::uint16_t>(w.size()));
            out.insert(out.end(), w.begin(), w.end());
        }
        return out;
    }
    static std::uint16_t code(const std::vector<std::uint32_t>& w) { return static_cast<std::uint16_t>(w[0] & 0xffff); }
    std::uint32_t fresh() { return bound++; }
    std::size_t first_function() const {
        for (std::size_t i = 0; i < insts.size(); ++i)
            if (code(insts[i]) == kFunction)
                return i;
        return insts.size();
    }
    // A global (type, constant, variable) added before the first function; `resultAt` is its result id's word.
    std::uint32_t add_global(std::vector<std::uint32_t> w, std::size_t resultAt) {
        const std::uint32_t id = fresh();
        w[resultAt] = id;
        insts.insert(insts.begin() + static_cast<std::ptrdiff_t>(first_function()), std::move(w));
        return id;
    }
    // An instruction of opcode `c` whose words after the opcode, leaving out the result id at `resultAt`, are
    // `operands`: its result id (0 if none).
    std::uint32_t find(std::uint16_t c, const std::vector<std::uint32_t>& operands, std::size_t resultAt) const {
        for (const auto& w : insts) {
            if (code(w) != c || w.size() != operands.size() + 2)
                continue;
            bool same = true;
            std::size_t o = 0;
            for (std::size_t k = 1; k < w.size(); ++k) {
                if (k == resultAt)
                    continue;
                if (w[k] != operands[o++]) {
                    same = false;
                    break;
                }
            }
            if (same)
                return w[resultAt];
        }
        return 0;
    }
    std::uint32_t type_float() {
        const std::uint32_t t = find(kTypeFloat, {32}, 1);
        return t ? t : add_global({op(kTypeFloat, 3), 0, 32}, 1);
    }
    std::uint32_t type_vector(std::uint32_t component, std::uint32_t n) {
        const std::uint32_t t = find(kTypeVector, {component, n}, 1);
        return t ? t : add_global({op(kTypeVector, 4), 0, component, n}, 1);
    }
    std::uint32_t type_pointer(std::uint32_t storage, std::uint32_t type) {
        const std::uint32_t t = find(kTypePointer, {storage, type}, 1);
        return t ? t : add_global({op(kTypePointer, 4), 0, storage, type}, 1);
    }
    std::uint32_t constant(std::uint32_t type, std::uint32_t bits) {
        const std::uint32_t c = find(kConstant, {type, bits}, 2);
        return c ? c : add_global({op(kConstant, 4), type, 0, bits}, 2);
    }
    std::uint32_t named(const char* name) const {
        for (const auto& w : insts)
            if (code(w) == kName && w.size() >= 3 && literal(w, 2, w.size()) == name)
                return w[1];
        return 0;
    }
    // The variable named `name` with the given storage class (0 if none), and its pointee type.
    std::uint32_t variable(const char* name, std::uint32_t storage, std::uint32_t* pointee) const {
        const std::uint32_t id = named(name);
        if (!id)
            return 0;
        for (const auto& w : insts)
            if (code(w) == kVariable && w.size() >= 4 && w[2] == id && w[3] == storage) {
                if (pointee)
                    for (const auto& t : insts)
                        if (code(t) == kTypePointer && t[1] == w[1])
                            *pointee = t[3];
                return id;
            }
        return 0;
    }
    // Where the value with result id `id` is defined: among instructions with a result type and a result id, word 2
    // (result_in_word2; types, whose result is word 1, are found with `find`). insts.size() if none.
    std::size_t defined_at(std::uint32_t id) const {
        for (std::size_t i = 0; i < insts.size(); ++i)
            if (insts[i].size() >= 3 && insts[i][2] == id && result_in_word2(code(insts[i])))
                return i;
        return insts.size();
    }
    bool location_used(std::uint32_t location) const {
        for (const auto& w : insts)
            if (code(w) == kDecorate && w.size() == 4 && w[2] == 30 && w[3] == location)
                return true;
        return false;
    }
    // An interface variable with a Location (and Flat), listed on the entry point.
    std::uint32_t add_interface(std::uint32_t pointer, std::uint32_t storage, std::uint32_t location) {
        const std::uint32_t var = add_global({op(kVariable, 4), pointer, 0, storage}, 2);
        std::size_t at = 0;
        for (std::size_t i = 0; i < insts.size(); ++i)
            if (code(insts[i]) == kDecorate || code(insts[i]) == kMemberDecorate)
                at = i + 1;
        const std::vector<std::vector<std::uint32_t>> decorations = {{op(kDecorate, 4), var, 30, location},
                                                                     {op(kDecorate, 3), var, 14}};
        insts.insert(insts.begin() + static_cast<std::ptrdiff_t>(at), decorations.begin(), decorations.end());
        for (auto& w : insts)
            if (code(w) == kEntryPoint) {
                w.push_back(var);
                break;
            }
        return var;
    }
};

constexpr std::uint16_t kOpFAdd = 129, kOpFSub = 131, kOpFMul = 133, kOpFDiv = 136, kOpVectorShuffle = 79,
                        kOpCompositeConstruct = 80, kOpStore = 62;

// The focus boxes' push constant block (spirv_patch.h kFocusBlockEntries): `vec4 qvfr_focus[3]`, eye e's box in [e]
// (centre tangents xy, size zw), the values that follow its size in [2]. One per module: a later edit finds the earlier
// one's by name.
struct FocusBlock {
    std::uint32_t var = 0, ptrV4 = 0, member = 0, v4 = 0, v2 = 0, values = 0;
};
constexpr std::uint32_t kPushConstant = 9;
constexpr const char kFocusBlockName[] = "qvfr_focus";

// Declares the block in `m` (if not there yet), its types and constants: before an edit computes where it inserts.
FocusBlock focus_block(Module& m, std::uint32_t intType);

// Appends to `body` the load of entry `entry` (an eye bit 0 or 1 of any integer type, or f.values); returns the vec4's
// id.
std::uint32_t focus_entry_load(Module& m, const FocusBlock& f, std::uint32_t entry,
                               std::vector<std::vector<std::uint32_t>>& body);
// Two components of a loaded entry (xy: an eye's centre, zw: its size): the vec2's id.
std::uint32_t focus_pair(Module& m, const FocusBlock& f, std::uint32_t entry, std::uint32_t first,
                         std::vector<std::vector<std::uint32_t>>& body);
constexpr std::uint32_t kInput = 1, kOutput = 3, kFloatOne = 0x3f800000, kFloatHalf = 0x3f000000;

// The variable decorated BuiltIn `builtIn` with the given storage class (0 if none), and its pointee type.
std::uint32_t builtin_variable(const Module& m, std::uint32_t builtIn, std::uint32_t storage, std::uint32_t* pointee);

}  // namespace xplane::spirv
