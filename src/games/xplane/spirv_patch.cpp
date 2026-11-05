#include "spirv_patch.h"

#include <cstring>
#include <map>
#include <set>

namespace xplane {
namespace {

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

std::uint32_t op(std::uint16_t code, std::uint16_t count) { return (static_cast<std::uint32_t>(count) << 16) | code; }

// The literal string starting at word `at` of an instruction `count` words long.
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

}  // namespace

bool spirv_add_to_input(std::vector<std::uint32_t>& words, const char* name, std::int32_t add, std::string* why) {
    auto fail = [&](const char* reason) {
        if (why)
            *why = reason;
        return false;
    };
    if (words.size() < kHeader || words[0] != kMagic)
        return fail("not SPIR-V");
    std::uint32_t var = 0, typeOfLoad = 0, loadId = 0;
    std::size_t loadAt = 0, loads = 0;
    // The variable's id, from its OpName.
    for (std::size_t i = kHeader; i < words.size();) {
        const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16), code = words[i] & 0xffff;
        if (count == 0 || i + count > words.size())
            return fail("malformed instruction");
        if (code == kOpName && count >= 3 && literal(words, i + 2, i + count) == name)
            var = words[i + 1];
        i += count;
    }
    if (!var)
        return fail("no variable of that name");
    // It must be an Input variable, loaded exactly once; the load's result type must be a 32-bit signed int.
    bool input = false;
    std::size_t intTypeAt = 0;
    for (std::size_t i = kHeader; i < words.size();) {
        const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16), code = words[i] & 0xffff;
        if (code == kOpVariable && count >= 4 && words[i + 2] == var)
            input = words[i + 3] == 1;  // StorageClass Input
        if (code == kOpLoad && count >= 4 && words[i + 3] == var) {
            ++loads;
            loadAt = i;
            typeOfLoad = words[i + 1];
            loadId = words[i + 2];
        }
        i += count;
    }
    if (!input)
        return fail("not an Input variable");
    if (loads != 1)
        return fail("not loaded exactly once");
    for (std::size_t i = kHeader; i < words.size();) {
        const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16), code = words[i] & 0xffff;
        if (code == kOpTypeInt && count == 4 && words[i + 1] == typeOfLoad && words[i + 2] == 32 && words[i + 3] == 1)
            intTypeAt = i;
        i += count;
    }
    if (!intTypeAt)
        return fail("loaded type is not a 32-bit signed int");

    const std::uint32_t constant = words[3], newLoad = words[3] + 1;
    std::vector<std::uint32_t> out;
    out.reserve(words.size() + 9);
    for (std::size_t i = 0; i < words.size();) {
        if (i < kHeader) {
            out.push_back(words[i]);
            ++i;
            continue;
        }
        const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16);
        if (i == loadAt) {
            // %newLoad = OpLoad %int %var, then %loadId = OpIAdd %int %newLoad %constant
            out.insert(out.end(), {words[i], typeOfLoad, newLoad, var});
            for (std::size_t k = 4; k < count; ++k)  // memory operands, if any
                out.push_back(words[i + k]);
            out.insert(out.end(), {op(kOpIAdd, 5), typeOfLoad, loadId, newLoad, constant});
        } else {
            out.insert(out.end(), words.begin() + static_cast<std::ptrdiff_t>(i),
                       words.begin() + static_cast<std::ptrdiff_t>(i + count));
            if (i == intTypeAt)  // the constant right after its type
                out.insert(out.end(), {op(kOpConstant, 4), typeOfLoad, constant, static_cast<std::uint32_t>(add)});
        }
        i += count;
    }
    out[3] = words[3] + 2;  // the id bound
    words.swap(out);
    return true;
}

int spirv_fold_eye_arrays(std::vector<std::uint32_t>& words, const std::vector<std::string>& blocks, int* skipped) {
    if (skipped)
        *skipped = 0;
    if (words.size() < kHeader || words[0] != kMagic)
        return 0;
    std::map<std::uint32_t, std::string> names;
    std::map<std::uint32_t, std::vector<std::uint32_t>> structs;              // struct id -> member types
    std::map<std::uint32_t, std::uint32_t> arrayLength;                       // array id -> its length's constant
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> consts;  // constant id -> (type, value)
    std::map<std::uint32_t, std::uint32_t> pointee;                           // Uniform pointer type -> pointee
    std::map<std::uint32_t, std::size_t> intTypeAt;                           // 32-bit int type -> its position
    std::map<std::uint32_t, std::uint32_t> typeOf;                            // result id -> its type
    std::map<std::uint32_t, std::uint32_t> varType;                           // Uniform variable -> pointer type
    for (std::size_t i = kHeader; i < words.size();) {
        const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16), code = words[i] & 0xffff;
        if (count == 0 || i + count > words.size())
            return 0;
        const std::uint32_t* w = &words[i];
        if (code == kOpName && count >= 3)
            names[w[1]] = literal(words, i + 2, i + count);
        else if (code == kOpTypeStruct)
            structs[w[1]] = std::vector<std::uint32_t>(w + 2, w + count);
        else if (code == kOpTypeArray && count == 4)
            arrayLength[w[1]] = w[3];
        else if (code == kOpTypeInt && count == 4 && w[2] == 32)
            intTypeAt[w[1]] = i;
        else if (code == kOpTypePointer && count == 4 && w[2] == kUniform)
            pointee[w[1]] = w[3];
        else if (code == kOpVariable && count >= 4 && w[3] == kUniform)
            varType[w[2]] = w[1];
        if (code == kOpConstant && count == 4)
            consts[w[2]] = {w[1], w[3]};
        if (typed_result(code) && count >= 3)
            typeOf[w[2]] = w[1];
        i += count;
    }
    // The named block variables and, for each, the members that are arrays of two.
    std::map<std::uint32_t, std::set<std::uint32_t>> targets;
    for (const auto& [var, ptr] : varType) {
        const auto p = pointee.find(ptr);
        if (p == pointee.end() || !structs.count(p->second))
            continue;
        // X-Plane's shaders name the block's type ("u_ssr_data"); tools show the variable as "u_ssr_data_var".
        const auto varName = names.find(var), typeName = names.find(p->second);
        bool wanted = false;
        for (const auto& b : blocks) {
            wanted |= varName != names.end() && varName->second == b;
            wanted |= typeName != names.end() && (typeName->second == b || typeName->second + "_var" == b);
        }
        if (!wanted)
            continue;
        const auto& members = structs[p->second];
        for (std::uint32_t m = 0; m < members.size(); ++m) {
            const auto a = arrayLength.find(members[m]);
            if (a == arrayLength.end())
                continue;
            const auto len = consts.find(a->second);
            if (len != consts.end() && len->second.second == 2)
                targets[var].insert(m);
        }
    }
    if (targets.empty())
        return 0;
    // var[member][index]... into a two-entry member: index -> index & 1 (an OpBitwiseAnd before the access chain).
    std::uint32_t bound = words[3];
    std::map<std::uint32_t, std::uint32_t> one;  // int type -> a new constant 1 of it
    struct Edit {
        std::size_t at;
        std::uint32_t type, index, masked;
    };
    std::vector<Edit> edits;
    int skip = 0;
    for (std::size_t i = kHeader; i < words.size();) {
        const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16), code = words[i] & 0xffff;
        if ((code == kOpAccessChain || code == kOpInBoundsAccessChain) && count >= 6) {
            const auto t = targets.find(words[i + 3]);
            const auto member = consts.find(words[i + 4]);
            if (t != targets.end() && member != consts.end() && t->second.count(member->second.second)) {
                const std::uint32_t index = words[i + 5];
                const auto c = consts.find(index);
                const auto type = typeOf.find(index);
                if (c != consts.end() && c->second.second < 2) {
                    // a fixed eye, 0 or 1: nothing to do
                } else if (type != typeOf.end() && intTypeAt.count(type->second)) {
                    if (!one.count(type->second))
                        one[type->second] = bound++;
                    edits.push_back({i, type->second, index, bound++});
                } else {
                    ++skip;
                }
            }
        }
        i += count;
    }
    if (skipped)
        *skipped = skip;
    if (edits.empty())
        return 0;
    std::map<std::size_t, const Edit*> editAt;
    for (const auto& e : edits)
        editAt[e.at] = &e;
    std::map<std::size_t, std::uint32_t> oneAfter;  // an int type's position -> its new constant 1
    for (const auto& [type, id] : one)
        oneAfter[intTypeAt[type]] = id;
    std::vector<std::uint32_t> out(words.begin(), words.begin() + kHeader);
    out.reserve(words.size() + edits.size() * 5 + one.size() * 4);
    for (std::size_t i = kHeader; i < words.size();) {
        const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16);
        const auto begin = words.begin() + static_cast<std::ptrdiff_t>(i);
        const auto e = editAt.find(i);
        if (e != editAt.end()) {
            const Edit& ed = *e->second;
            out.insert(out.end(), {op(kOpBitwiseAnd, 5), ed.type, ed.masked, ed.index, one[ed.type]});
            const std::size_t start = out.size();
            out.insert(out.end(), begin, begin + count);
            out[start + 5] = ed.masked;
        } else {
            out.insert(out.end(), begin, begin + count);
            const auto o = oneAfter.find(i);
            if (o != oneAfter.end())
                out.insert(out.end(), {op(kOpConstant, 4), words[i + 1], o->second, 1});
        }
        i += count;
    }
    out[3] = bound;
    words.swap(out);
    return static_cast<int>(edits.size());
}

}  // namespace xplane
