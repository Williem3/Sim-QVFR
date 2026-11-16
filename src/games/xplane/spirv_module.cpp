#include "spirv_module.h"

#include <algorithm>
#include <cstring>

namespace xplane::spirv {

bool typed_result(std::uint16_t code) {
    switch (code) {
    case 12: case 43: case 50: case 61: case 77: case 81: case 109: case 110: case 113: case 114: case 124: case 126:
    case 128: case 130: case 132: case 134: case 135: case 137: case 138: case 139: case 169: case 194: case 195:
    case 196: case 197: case 198: case 199: case 200: case 245:
        return true;
    default:
        return false;
    }
}

bool result_in_word2(std::uint16_t code) {
    switch (code) {
    case 1:                                            // Undef
    case 12:                                           // ExtInst
    case 41: case 42: case 43: case 44: case 45: case 46:  // ConstantTrue .. ConstantNull
    case 48: case 49: case 50: case 51: case 52:       // SpecConstantTrue .. SpecConstantOp
    case 54: case 55: case 57:                         // Function, FunctionParameter, FunctionCall
    case 59: case 60: case 61:                         // Variable, ImageTexelPointer, Load
    case 65: case 66: case 67: case 68: case 69: case 70:  // AccessChain .. InBoundsPtrAccessChain
    case 227:                                          // AtomicLoad
    case 245:                                          // Phi
    case 259:                                          // GroupAsyncCopy
    case 318: case 320: case 321:                      // AtomicFlagTestAndSet, ImageSparseRead, SizeOf
    case 400: case 401: case 402: case 403:            // CopyLogical, PtrEqual, PtrNotEqual, PtrDiff
    case 5381:                                         // IsHelperInvocationEXT
        return true;
    default:
        break;
    }
    return (code >= 77 && code <= 84) ||    // VectorExtractDynamic .. Transpose
           (code >= 86 && code <= 98) ||    // SampledImage, ImageSample* .. ImageRead (99 ImageWrite has none)
           (code >= 100 && code <= 107) ||  // Image, ImageQuery*
           (code >= 109 && code <= 124) ||  // conversions .. Bitcast
           (code >= 126 && code <= 152) ||  // SNegate .. SMulExtended
           (code >= 154 && code <= 191) ||  // Any .. FUnordGreaterThanEqual
           (code >= 194 && code <= 205) ||  // shifts, bitwise, BitField*, BitReverse, BitCount
           (code >= 207 && code <= 215) ||  // derivatives
           (code >= 229 && code <= 242) ||  // AtomicExchange .. AtomicXor
           (code >= 261 && code <= 271) ||  // GroupAll .. GroupSMax
           (code >= 305 && code <= 316) ||  // ImageSparse* .. ImageSparseTexelsResident
           (code >= 333 && code <= 366) ||  // GroupNonUniform*
           (code >= 4421 && code <= 4432);  // SubgroupBallotKHR .. SubgroupReadInvocationKHR
}

std::uint32_t op(std::uint16_t code, std::uint16_t count) { return (static_cast<std::uint32_t>(count) << 16) | code; }

std::string literal(const std::vector<std::uint32_t>& w, std::size_t at, std::size_t end) {
    std::string s;
    for (std::size_t i = at; i < end; ++i) {
        char c[4];
        std::memcpy(c, &w[i], 4);
        for (char ch : c) {
            if (!ch)
                return s;
            s += ch;
        }
    }
    return s;
}

FocusBlock focus_block(Module& m, std::uint32_t intType) {
    constexpr std::uint32_t kBlock = 2, kArrayStride = 6, kOffset = 35;
    FocusBlock f;
    const std::uint32_t fl = m.type_float();
    f.v4 = m.type_vector(fl, 4);
    f.v2 = m.type_vector(fl, 2);
    f.ptrV4 = m.type_pointer(kPushConstant, f.v4);
    f.member = m.constant(intType, 0);
    f.values = m.constant(intType, kFocusValuesEntry);
    f.var = m.variable(kFocusBlockName, kPushConstant, nullptr);
    if (f.var)
        return f;
    const std::uint32_t entries = m.constant(intType, kFocusBlockEntries);
    const std::uint32_t array = m.add_global({op(kOpTypeArray, 4), 0, f.v4, entries}, 1);
    const std::uint32_t block = m.add_global({op(kOpTypeStruct, 3), 0, array}, 1);
    f.var = m.add_global({op(Module::kVariable, 4), m.type_pointer(kPushConstant, block), 0, kPushConstant}, 2);
    // Its name (with the debug names, before the decorations) and decorations (after the last).
    std::vector<std::uint32_t> name = {op(Module::kName, 2), f.var};
    for (std::size_t i = 0; i < sizeof(kFocusBlockName); i += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, kFocusBlockName + i, std::min<std::size_t>(4, sizeof(kFocusBlockName) - i));
        name.push_back(word);
    }
    std::size_t names = 0, decorations = 0;
    for (std::size_t i = 0; i < m.insts.size(); ++i) {
        const std::uint16_t c = Module::code(m.insts[i]);
        if (c == Module::kName || c == Module::kMemberName)
            names = i + 1;
        if (c == Module::kDecorate || c == Module::kMemberDecorate)
            decorations = i + 1;
    }
    const std::vector<std::vector<std::uint32_t>> decorate = {{op(Module::kDecorate, 4), array, kArrayStride, 16},
                                                              {op(Module::kDecorate, 3), block, kBlock},
                                                              {op(Module::kMemberDecorate, 5), block, 0, kOffset, 0}};
    m.insts.insert(m.insts.begin() + static_cast<std::ptrdiff_t>(decorations), decorate.begin(), decorate.end());
    if (names > 0)
        m.insts.insert(m.insts.begin() + static_cast<std::ptrdiff_t>(names), name);
    // SPIR-V 1.4 and later list every global the entry point uses (X-Plane's modules are 1.6).
    if (m.header.size() > 1 && m.header[1] >= 0x00010400)
        for (auto& w : m.insts)
            if (Module::code(w) == Module::kEntryPoint)
                w.push_back(f.var);
    return f;
}

std::uint32_t focus_entry_load(Module& m, const FocusBlock& f, std::uint32_t entry,
                               std::vector<std::vector<std::uint32_t>>& body) {
    const std::uint32_t ptr = m.fresh(), value = m.fresh();
    body.push_back({op(kOpAccessChain, 6), f.ptrV4, ptr, f.var, f.member, entry});
    body.push_back({op(kOpLoad, 4), f.v4, value, ptr});
    return value;
}
std::uint32_t focus_pair(Module& m, const FocusBlock& f, std::uint32_t entry, std::uint32_t first,
                         std::vector<std::vector<std::uint32_t>>& body) {
    const std::uint32_t pair = m.fresh();
    body.push_back({op(kOpVectorShuffle, 7), f.v2, pair, entry, entry, first, first + 1});
    return pair;
}

std::uint32_t builtin_variable(const Module& m, std::uint32_t builtIn, std::uint32_t storage, std::uint32_t* pointee) {
    for (const auto& d : m.insts) {
        if (Module::code(d) != Module::kDecorate || d.size() != 4 || d[2] != 11 || d[3] != builtIn)
            continue;
        for (const auto& w : m.insts)
            if (Module::code(w) == Module::kVariable && w.size() >= 4 && w[2] == d[1] && w[3] == storage) {
                if (pointee)
                    for (const auto& t : m.insts)
                        if (Module::code(t) == Module::kTypePointer && t[1] == w[1])
                            *pointee = t[3];
                return w[2];
            }
    }
    return 0;
}

}  // namespace xplane::spirv
