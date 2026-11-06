#include "spirv_patch.h"

#include <algorithm>
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

bool spirv_layer_from_dispatch_z(std::vector<std::uint32_t>& words) {
    constexpr std::uint16_t kOpDecorate = 71, kOpIEqual = 170, kOpSelect = 169, kOpCompositeConstruct = 80,
                            kOpBitcast = 124;
    constexpr std::uint32_t kBuiltIn = 11, kGlobalInvocationId = 28;
    if (words.size() < kHeader || words[0] != kMagic)
        return false;
    std::uint32_t gid = 0;
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> consts;  // id -> (type, value)
    for (std::size_t i = kHeader; i < words.size();) {
        const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16), code = words[i] & 0xffff;
        if (count == 0 || i + count > words.size())
            return false;
        if (code == kOpDecorate && count >= 4 && words[i + 2] == kBuiltIn && words[i + 3] == kGlobalInvocationId)
            gid = words[i + 1];
        if (code == kOpConstant && count == 4)
            consts[words[i + 2]] = {words[i + 1], words[i + 3]};
        i += count;
    }
    if (!gid)
        return false;
    // z: OpLoad of an access chain gl_GlobalInvocationID[2]; eye: OpSelect on OpIEqual(z, 1).
    std::uint32_t zPtr = 0, z = 0, zType = 0, eq = 0, eye = 0, eyeType = 0, one = 0;
    std::size_t zLoadAt = 0, eqAt = 0, constructAt = 0;
    int eqZOperand = 0;
    for (std::size_t i = kHeader; i < words.size();) {
        const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16), code = words[i] & 0xffff;
        const std::uint32_t* w = &words[i];
        if ((code == kOpAccessChain || code == kOpInBoundsAccessChain) && count == 5 && w[3] == gid) {
            const auto c = consts.find(w[4]);
            if (c != consts.end() && c->second.second == 2)
                zPtr = w[2];
        } else if (code == kOpLoad && count >= 4 && zPtr && w[3] == zPtr && !z) {
            zType = w[1];
            z = w[2];
            zLoadAt = i;
        } else if (code == kOpIEqual && count == 5 && z && !eq) {
            for (int k = 0; k < 2; ++k) {
                const auto c = consts.find(w[4 - k]);
                if (w[3 + k] == z && c != consts.end() && c->second.second == 1) {
                    eq = w[2];
                    eqAt = i;
                    eqZOperand = 3 + k;
                    one = w[4 - k];
                }
            }
        } else if (code == kOpSelect && count == 6 && eq && w[3] == eq && !eye) {
            eyeType = w[1];
            eye = w[2];
        } else if (code == kOpCompositeConstruct && count == 6 && eye && w[5] == eye && !constructAt) {
            constructAt = i;
        }
        i += count;
    }
    if (!zLoadAt || !eqAt || !eye || !constructAt || consts[one].first != zType)
        return false;
    const std::uint32_t zMasked = words[3], zAsInt = words[3] + 1;
    std::vector<std::uint32_t> out(words.begin(), words.begin() + kHeader);
    out.reserve(words.size() + 10);
    for (std::size_t i = kHeader; i < words.size();) {
        const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16);
        const std::size_t start = out.size();
        out.insert(out.end(), words.begin() + static_cast<std::ptrdiff_t>(i),
                   words.begin() + static_cast<std::ptrdiff_t>(i + count));
        if (i == zLoadAt) {  // right after z: z & 1, and z as the eye's int type
            out.insert(out.end(), {op(kOpBitwiseAnd, 5), zType, zMasked, z, one});
            out.insert(out.end(), {op(kOpBitcast, 4), eyeType, zAsInt, z});
        } else if (i == eqAt) {
            out[start + static_cast<std::size_t>(eqZOperand)] = zMasked;
        } else if (i == constructAt) {
            out[start + 5] = zAsInt;
        }
        i += count;
    }
    out[3] = words[3] + 2;
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

bool spirv_eye_columns_resolve(std::vector<std::uint32_t>& words, std::string* why) {
    constexpr std::uint16_t kOpMemberName = 6, kOpTypeBool = 20, kOpTypeFloat = 22, kOpFAdd = 129, kOpFSub = 131,
                            kOpSelect = 169, kOpFOrdLessThanEqual = 188;
    auto fail = [&](const char* reason) {
        if (why)
            *why = reason;
        return false;
    };
    if (words.size() < kHeader || words[0] != kMagic)
        return fail("not SPIR-V");
    std::map<std::uint32_t, std::string> names;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::string> memberNames;
    std::map<std::uint32_t, std::uint32_t> pointee, varType;
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> consts;
    std::uint32_t boolType = 0, floatType = 0, intType = 0, eyeVar = 0;
    std::size_t floatAt = 0, intAt = 0;
    for (std::size_t i = kHeader; i < words.size();) {
        const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16), code = words[i] & 0xffff;
        if (count == 0 || i + count > words.size())
            return fail("malformed instruction");
        const std::uint32_t* w = &words[i];
        if (code == kOpName && count >= 3)
            names[w[1]] = literal(words, i + 2, i + count);
        else if (code == kOpMemberName && count >= 4)
            memberNames[{w[1], w[2]}] = literal(words, i + 3, i + count);
        else if (code == kOpTypeBool && count == 2 && !boolType)
            boolType = w[1];
        else if (code == kOpTypeFloat && count == 3 && w[2] == 32 && !floatType) {
            floatType = w[1];
            floatAt = i;
        } else if (code == kOpTypeInt && count == 4 && w[2] == 32 && w[3] == 1 && !intType) {
            intType = w[1];
            intAt = i;
        } else if (code == kOpTypePointer && count == 4 && w[2] == kUniform)
            pointee[w[1]] = w[3];
        else if (code == kOpVariable && count >= 4 && w[3] == kUniform)
            varType[w[2]] = w[1];
        if (code == kOpConstant && count == 4)
            consts[w[2]] = {w[1], w[3]};
        i += count;
    }
    if (!boolType || !floatType || !intType)
        return fail("no bool, float or int type");
    for (const auto& [id, n] : names)
        if (n == "v_eye")
            eyeVar = id;
    if (!eyeVar)
        return fail("no v_eye");
    // The Uniform variable whose block has a member "u_y_offset", and that member's index.
    std::uint32_t blockVar = 0, member = 0;
    for (const auto& [var, ptr] : varType) {
        const auto p = pointee.find(ptr);
        if (p == pointee.end())
            continue;
        for (const auto& [key, n] : memberNames)
            if (key.first == p->second && n == "u_y_offset") {
                blockVar = var;
                member = key.second;
            }
    }
    if (!blockVar)
        return fail("no u_y_offset");
    // var.u_y_offset -> its loads -> the one OpFSub of a load; the one load of v_eye.
    std::set<std::uint32_t> yPtrs, yLoads;
    std::size_t subAt = 0, eyeLoadAt = 0;
    int subs = 0, eyeLoads = 0;
    for (std::size_t i = kHeader; i < words.size();) {
        const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16), code = words[i] & 0xffff;
        const std::uint32_t* w = &words[i];
        if ((code == kOpAccessChain || code == kOpInBoundsAccessChain) && count == 5 && w[3] == blockVar) {
            const auto c = consts.find(w[4]);
            if (c != consts.end() && c->second.second == member)
                yPtrs.insert(w[2]);
        } else if (code == kOpLoad && count >= 4 && yPtrs.count(w[3])) {
            yLoads.insert(w[2]);
        } else if (code == kOpFSub && count == 5 && w[1] == floatType && yLoads.count(w[3])) {
            ++subs;
            subAt = i;
        } else if (code == kOpLoad && count >= 4 && w[3] == eyeVar) {
            ++eyeLoads;
            eyeLoadAt = i;
        }
        i += count;
    }
    if (subs != 1)
        return fail("not exactly one u_y_offset - y");
    if (eyeLoads != 1)
        return fail("v_eye not loaded exactly once");
    if (eyeLoadAt < subAt)
        return fail("v_eye loaded before the row is known");
    const std::uint32_t zero = words[3], one = zero + 1, two = zero + 2, yRaw = zero + 3, row = zero + 4,
                        yUp = zero + 5, eyeRaw = zero + 6, eyeUp = zero + 7, bound = zero + 8;
    const std::uint32_t y = words[subAt + 2], eye = words[eyeLoadAt + 2], eyeType = words[eyeLoadAt + 1];
    if (eyeType != intType)
        return fail("v_eye is not the 32-bit int");
    std::vector<std::uint32_t> out(words.begin(), words.begin() + kHeader);
    out.reserve(words.size() + 40);
    for (std::size_t i = kHeader; i < words.size();) {
        const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16);
        const std::size_t start = out.size();
        out.insert(out.end(), words.begin() + static_cast<std::ptrdiff_t>(i),
                   words.begin() + static_cast<std::ptrdiff_t>(i + count));
        if (i == floatAt) {
            out.insert(out.end(), {op(kOpConstant, 4), floatType, zero, 0x00000000u});  // 0.0f
            out.insert(out.end(), {op(kOpConstant, 4), floatType, one, 0x3F800000u});   // 1.0f
        } else if (i == intAt) {
            out.insert(out.end(), {op(kOpConstant, 4), intType, two, 2});
        } else if (i == subAt) {
            out[start + 2] = yRaw;
            out.insert(out.end(), {op(kOpFOrdLessThanEqual, 5), boolType, row, yRaw, zero});
            out.insert(out.end(), {op(kOpFAdd, 5), floatType, yUp, yRaw, one});
            out.insert(out.end(), {op(kOpSelect, 6), floatType, y, row, yUp, yRaw});
        } else if (i == eyeLoadAt) {
            out[start + 2] = eyeRaw;
            out.insert(out.end(), {op(kOpIAdd, 5), intType, eyeUp, eyeRaw, two});
            // The upper row (v > 0) takes the focus view: X-Plane's blit into the D3D11 image flips it vertically
            // (capture XPlane_frame2096.rdc), so there it is the lower row, as eye columns have it.
            out.insert(out.end(), {op(kOpSelect, 6), intType, eye, row, eyeRaw, eyeUp});
        }
        i += count;
    }
    out[3] = bound;
    words.swap(out);
    return true;
}

int spirv_eye_projection_beside(std::vector<std::uint32_t>& words, const char* block, const char* member,
                                const char* transform, const char* projection) {
    constexpr std::uint16_t kOpMemberName = 6, kOpLabel = 248;
    if (words.size() < kHeader || words[0] != kMagic)
        return 0;
    std::map<std::uint32_t, std::string> names;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::string> memberNames;  // (struct, member) -> name
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> consts;    // constant id -> (type, value)
    std::map<std::uint32_t, std::uint32_t> pointee, varType, typeOf;
    std::map<std::uint32_t, std::size_t> intTypeAt;
    for (std::size_t i = kHeader; i < words.size();) {
        const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16), code = words[i] & 0xffff;
        if (count == 0 || i + count > words.size())
            return 0;
        const std::uint32_t* w = &words[i];
        if (code == kOpName && count >= 3)
            names[w[1]] = literal(words, i + 2, i + count);
        else if (code == kOpMemberName && count >= 4)
            memberNames[{w[1], w[2]}] = literal(words, i + 3, i + count);
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
    // A Uniform variable of the named block type and the index of its named member.
    auto find = [&](const std::string& typeName, const std::string& memberName, std::uint32_t& var, std::uint32_t& index) {
        for (const auto& [v, ptr] : varType) {
            const auto p = pointee.find(ptr);
            if (p == pointee.end())
                continue;
            const auto n = names.find(p->second);
            if (n == names.end() || (n->second != typeName && n->second + "_var" != typeName))
                continue;
            for (const auto& [key, mn] : memberNames)
                if (key.first == p->second && mn == memberName) {
                    var = v;
                    index = key.second;
                    return true;
                }
        }
        return false;
    };
    std::uint32_t srcVar = 0, srcMember = 0, projVar = 0, projMember = 0;
    if (!find(block, member, srcVar, srcMember) || !find(transform, projection, projVar, projMember))
        return 0;
    // Per basic block: the projection's dynamic accesses before the block's first access of the source member.
    struct Edit {
        std::size_t at;
        std::uint32_t type, index, masked;
    };
    std::vector<Edit> edits;
    std::vector<std::size_t> pending;  // the current block's projection accesses so far
    std::map<std::uint32_t, std::uint32_t> one;
    std::uint32_t bound = words[3];
    bool sourceSeen = false;
    for (std::size_t i = kHeader; i < words.size();) {
        const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16), code = words[i] & 0xffff;
        if (code == kOpLabel) {
            pending.clear();
            sourceSeen = false;
        } else if ((code == kOpAccessChain || code == kOpInBoundsAccessChain) && count == 6 && !sourceSeen) {
            const auto m = consts.find(words[i + 4]);
            if (m != consts.end() && words[i + 3] == projVar && m->second.second == projMember) {
                pending.push_back(i);
            } else if (m != consts.end() && words[i + 3] == srcVar && m->second.second == srcMember) {
                sourceSeen = true;
                for (const std::size_t at : pending) {
                    const std::uint32_t index = words[at + 5];
                    const auto c = consts.find(index);
                    const auto type = typeOf.find(index);
                    if ((c != consts.end() && c->second.second < 2) || type == typeOf.end() || !intTypeAt.count(type->second))
                        continue;  // a fixed eye already, or an index of unknown type
                    if (!one.count(type->second))
                        one[type->second] = bound++;
                    edits.push_back({at, type->second, index, bound++});
                }
                pending.clear();
            }
        }
        i += count;
    }
    if (edits.empty())
        return 0;
    std::map<std::size_t, const Edit*> editAt;
    for (const auto& e : edits)
        editAt[e.at] = &e;
    std::map<std::size_t, std::uint32_t> oneAfter;
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


namespace {

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
    // Where the instruction with result id `id` is, among those with the result in word 2 (insts.size() if none).
    std::size_t defined_at(std::uint32_t id) const {
        for (std::size_t i = 0; i < insts.size(); ++i)
            if (insts[i].size() >= 3 && code(insts[i]) != kDecorate && code(insts[i]) != kName &&
                code(insts[i]) != kMemberName && insts[i][2] == id)
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

// Appends to `body` the load of entry `entry` (an eye bit 0 or 1 of any integer type, or f.values); returns the vec4's
// id.
std::uint32_t focus_entry_load(Module& m, const FocusBlock& f, std::uint32_t entry,
                               std::vector<std::vector<std::uint32_t>>& body) {
    const std::uint32_t ptr = m.fresh(), value = m.fresh();
    body.push_back({op(kOpAccessChain, 6), f.ptrV4, ptr, f.var, f.member, entry});
    body.push_back({op(kOpLoad, 4), f.v4, value, ptr});
    return value;
}
// Two components of a loaded entry (xy: an eye's centre, zw: its size): the vec2's id.
std::uint32_t focus_pair(Module& m, const FocusBlock& f, std::uint32_t entry, std::uint32_t first,
                         std::vector<std::vector<std::uint32_t>>& body) {
    const std::uint32_t pair = m.fresh();
    body.push_back({op(kOpVectorShuffle, 7), f.v2, pair, entry, entry, first, first + 1});
    return pair;
}
constexpr std::uint32_t kInput = 1, kOutput = 3, kFloatOne = 0x3f800000, kFloatHalf = 0x3f000000;

}  // namespace

bool spirv_view_to_eye_map_out(std::vector<std::uint32_t>& words, std::uint32_t location, std::string* why) {
    auto fail = [&](const char* reason) {
        if (why)
            *why = reason;
        return false;
    };
    Module m;
    if (!m.parse(words))
        return fail("not SPIR-V");
    std::uint32_t intType = 0;
    const std::uint32_t eyeVar = m.variable("v_eye", kOutput, &intType);
    if (!eyeVar || !intType)
        return fail("no v_eye output");
    // The transform block's variable and projection_matrix's member index.
    const std::uint32_t block = m.named("u_transform_block_fat");
    std::uint32_t member = ~0u, blockVar = 0;
    for (const auto& w : m.insts)
        if (block && Module::code(w) == Module::kMemberName && w.size() >= 4 && w[1] == block &&
            literal(w, 3, w.size()) == "projection_matrix")
            member = w[2];
    for (const auto& w : m.insts)
        if (block && Module::code(w) == Module::kVariable && w.size() >= 4 && w[3] == kUniform)
            for (const auto& t : m.insts)
                if (Module::code(t) == Module::kTypePointer && t[1] == w[1] && t[3] == block)
                    blockVar = w[2];
    if (!blockVar || member == ~0u)
        return fail("no u_transform_block_fat.projection_matrix");
    if (m.location_used(location))
        return fail("location taken");
    int stores = 0;
    for (const auto& w : m.insts)
        stores += Module::code(w) == kOpStore && w.size() >= 3 && w[1] == eyeVar;
    if (stores != 1)
        return fail("v_eye not stored exactly once");
    const std::uint32_t fl = m.type_float(), v4 = m.type_vector(fl, 4), ptrF = m.type_pointer(kUniform, fl),
                        ptrV4 = m.type_pointer(kOutput, v4);
    std::uint32_t c[4];
    for (std::uint32_t k = 0; k < 4; ++k)
        c[k] = m.constant(intType, k);
    const std::uint32_t cMember = m.constant(intType, member), one = m.constant(fl, kFloatOne),
                        half = m.constant(fl, kFloatHalf);
    const std::uint32_t mapVar = m.add_interface(ptrV4, kOutput, location);
    std::size_t at = 0;
    std::uint32_t view = 0;
    for (std::size_t i = 0; i < m.insts.size(); ++i)
        if (Module::code(m.insts[i]) == kOpStore && m.insts[i][1] == eyeVar) {
            at = i + 1;
            view = m.insts[i][2];
        }
    std::vector<std::vector<std::uint32_t>> body;
    auto emit = [&](std::vector<std::uint32_t> w) {
        const std::uint32_t id = m.fresh();
        w[2] = id;
        body.push_back(std::move(w));
        return id;
    };
    const std::uint32_t eye = emit({op(kOpBitwiseAnd, 5), intType, 0, view, c[1]});
    auto element = [&](std::uint32_t index, int col, int row) {
        const std::uint32_t ptr = emit({op(kOpAccessChain, 7), ptrF, 0, blockVar, cMember, index, c[col], c[row]});
        return emit({op(kOpLoad, 4), fl, 0, ptr});
    };
    auto bin = [&](std::uint16_t code, std::uint32_t a, std::uint32_t b) { return emit({op(code, 5), fl, 0, a, b}); };
    // Clip x = P[0][0] x + P[2][0] z, w = P[2][3] z: ndc = P[0][0] t + P[2][0] / P[2][3] with t = x / (P[2][3] z), the
    // same t for both projections. So ndc_eye = s ndc_view + k, s = Pe[0][0] / Pv[0][0], k = Pe[2][0]/Pe[2][3] - s
    // Pv[2][0]/Pv[2][3]; likewise y with [1][1], [2][1]. uv = (ndc_x + 1) / 2 across, (1 - ndc_y) / 2 down
    // (docs/recon-xplane.md).
    const std::uint32_t v00 = element(view, 0, 0), v11 = element(view, 1, 1), v20 = element(view, 2, 0),
                        v21 = element(view, 2, 1), v23 = element(view, 2, 3);
    const std::uint32_t e00 = element(eye, 0, 0), e11 = element(eye, 1, 1), e20 = element(eye, 2, 0),
                        e21 = element(eye, 2, 1), e23 = element(eye, 2, 3);
    const std::uint32_t sx = bin(kOpFDiv, e00, v00), sy = bin(kOpFDiv, e11, v11);
    const std::uint32_t kx = bin(kOpFSub, bin(kOpFDiv, e20, e23), bin(kOpFMul, sx, bin(kOpFDiv, v20, v23)));
    const std::uint32_t ky = bin(kOpFSub, bin(kOpFDiv, e21, e23), bin(kOpFMul, sy, bin(kOpFDiv, v21, v23)));
    const std::uint32_t ox = bin(kOpFMul, bin(kOpFAdd, bin(kOpFSub, one, sx), kx), half);
    const std::uint32_t oy = bin(kOpFMul, bin(kOpFSub, bin(kOpFSub, one, sy), ky), half);
    const std::uint32_t map = emit({op(kOpCompositeConstruct, 7), v4, 0, sx, sy, ox, oy});
    body.push_back({op(kOpStore, 3), mapVar, map});
    m.insts.insert(m.insts.begin() + static_cast<std::ptrdiff_t>(at), body.begin(), body.end());
    words = m.emit();
    return true;
}

bool spirv_sample_eye_box(std::vector<std::uint32_t>& words, std::uint32_t location, std::string* why) {
    auto fail = [&](const char* reason) {
        if (why)
            *why = reason;
        return false;
    };
    Module m;
    if (!m.parse(words))
        return fail("not SPIR-V");
    std::uint32_t intType = 0;
    const std::uint32_t eyeVar = m.variable("v_eye", kInput, &intType);
    const std::uint32_t fragCoord = m.variable("gl_FragCoord", kInput, nullptr);
    if (!eyeVar || !intType || !fragCoord)
        return fail("no v_eye or gl_FragCoord input");
    if (m.location_used(location))
        return fail("location taken");
    // The one load of v_eye, and the one FMul of (gl_FragCoord.xy, ...): the sample position in the view.
    std::uint32_t eyeLoad = 0, uv = 0, uvType = 0;
    int eyeLoads = 0, uvs = 0;
    std::set<std::uint32_t> fragLoads, fragXY;
    for (const auto& w : m.insts) {
        const std::uint16_t c = Module::code(w);
        if (c == kOpLoad && w.size() >= 4 && w[3] == eyeVar) {
            eyeLoad = w[2];
            ++eyeLoads;
        }
        if (c == kOpLoad && w.size() >= 4 && w[3] == fragCoord)
            fragLoads.insert(w[2]);
        if (c == kOpVectorShuffle && w.size() == 7 && fragLoads.count(w[3]) && w[5] == 0 && w[6] == 1)
            fragXY.insert(w[2]);
        if (c == kOpFMul && w.size() == 5 && (fragXY.count(w[3]) || fragXY.count(w[4]))) {
            uv = w[2];
            uvType = w[1];
            ++uvs;
        }
    }
    if (eyeLoads != 1 || uvs != 1)
        return fail("v_eye loaded or the position made other than once");
    const std::uint32_t fl = m.type_float(), v4 = m.type_vector(fl, 4), ptrV4 = m.type_pointer(kInput, v4);
    const std::uint32_t one = m.constant(intType, 1);
    const std::uint32_t mapVar = m.add_interface(ptrV4, kInput, location);
    // Renames: the load's and the FMul's results get new ids; the old ids become the edited values.
    const std::uint32_t eyeRaw = m.fresh(), uvRaw = m.fresh();
    const std::size_t eyeAt = m.defined_at(eyeLoad), uvAt = m.defined_at(uv);
    if (eyeAt >= m.insts.size() || uvAt >= m.insts.size())
        return fail("definitions not found");
    m.insts[eyeAt][2] = eyeRaw;
    m.insts[uvAt][2] = uvRaw;
    const std::uint32_t map = m.fresh(), xy = m.fresh(), zw = m.fresh(), scaled = m.fresh();
    const std::vector<std::vector<std::uint32_t>> uvBody = {
        {op(kOpLoad, 4), v4, map, mapVar},
        {op(kOpVectorShuffle, 7), uvType, xy, map, map, 0, 1},
        {op(kOpVectorShuffle, 7), uvType, zw, map, map, 2, 3},
        {op(kOpFMul, 5), uvType, scaled, uvRaw, xy},
        {op(kOpFAdd, 5), uvType, uv, scaled, zw},
    };
    const std::vector<std::vector<std::uint32_t>> eyeBody = {{op(kOpBitwiseAnd, 5), intType, eyeLoad, eyeRaw, one}};
    // The later one first, so the earlier index holds.
    auto insert = [&](std::size_t after, const std::vector<std::vector<std::uint32_t>>& b) {
        m.insts.insert(m.insts.begin() + static_cast<std::ptrdiff_t>(after + 1), b.begin(), b.end());
    };
    if (uvAt > eyeAt) {
        insert(uvAt, uvBody);
        insert(eyeAt, eyeBody);
    } else {
        insert(eyeAt, eyeBody);
        insert(uvAt, uvBody);
    }
    words = m.emit();
    return true;
}


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
                                  int* skipped, bool push) {
    if (skipped)
        *skipped = 0;
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
        const std::uint32_t two = m.constant(rawType->second, 2);
        const FocusBlock block = push ? focus_block(m, rawType->second) : FocusBlock{};
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
        body.insert(body.end(), {
            {op(kOpVectorShuffle, 9), v4, focusC, fxy, zw, 0, 1, 2, 3},
            {op(intSigned[rawType->second] ? kOpSGreaterThanEqual : kOpUGreaterThanEqual, 5), boolType, isFocus, raw,
             two},
            {op(kOpCompositeConstruct, 7), bv4, mask, isFocus, isFocus, isFocus, isFocus},
            {op(kOpSelect, 6), v4, load, mask, focusC, original},
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

namespace {

// A Location decoration with this value on any variable.
bool location_taken(const Module& m, std::uint32_t location) {
    for (const auto& w : m.insts)
        if (Module::code(w) == Module::kDecorate && w.size() == 4 && w[2] == 30 && w[3] == location)
            return true;
    return false;
}

// The variable decorated BuiltIn `builtIn` with the given storage class (0 if none), and its pointee type.
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

}  // namespace

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
        body.insert(body.end(), {
            {op(kOpFDiv, 5), fl, centre, negOffset, scale},
            {op(kOpAccessChain, 6), ptrF, sizePtr, gbufVar, cSize, cK},  // u_viewport_size[k]
            {op(kOpLoad, 4), fl, size, sizePtr},
            {op(kOpConvertSToF, 4), fl, xf, x},
            {op(kOpFSub, 5), fl, corner, centre, cHalfF},
            {op(kOpFMul, 5), fl, origin, corner, size},
            {op(kOpFMul, 5), fl, scaled, xf, cF},
            {op(kOpFAdd, 5), fl, mappedF, origin, scaled},
            {op(kOpConvertFToS, 4), intType, mapped, mappedF},
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
        std::uint32_t fromCentre = 0, f2 = 0, halfF2 = 0, bandValue = cBand;
        if (push) {
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
        const std::uint32_t eyePixel = emit({kOpFAdd, v2, 0, origin, emit({kOpFMul, v2, 0, pixel, f2})});
        const std::uint32_t eyeUv = emit({kOpFMul, v2, 0, eyePixel, emit({kOpVectorShuffle, v2, 0, gbuffer, gbuffer, 2, 3})});
        const std::uint32_t eyeCoord = emit({kOpCompositeConstruct, v3, 0, eyeUv, emit({kOpConvertSToF, fl, 0, eyeBit})});
        const std::uint32_t eyeAo = emit({kOpImageSampleExplicitLod, v4, 0, image, eyeCoord, kImageOperandLod, f0});
        // The weight: the distance to the view's nearest edge (in viewport uv), smoothstep(0, band).
        const std::uint32_t own = emit({kOpFMul, v2, 0, pixel, emit({kOpVectorShuffle, v2, 0, viewport, viewport, 2, 3})});
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

int spirv_eye_parity_test(std::vector<std::uint32_t>& words, const char* block, const char* member, const char* input) {
    constexpr std::uint16_t kOpMemberName = 6, kOpIEqual = 170, kOpINotEqual = 171;
    if (words.size() < kHeader || words[0] != kMagic)
        return 0;
    std::map<std::uint32_t, std::string> names;                               // OpName
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::string> members;  // (struct, index) -> OpMemberName
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> consts; // id -> (type, value)
    std::map<std::uint32_t, std::uint32_t> pointee, varPointer;             // pointer type -> pointee, var -> pointer
    std::map<std::uint32_t, std::uint32_t> varStorage;
    std::set<std::uint32_t> structs, int32;
    for (std::size_t i = kHeader; i < words.size();) {
        const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16), code = words[i] & 0xffff;
        if (count == 0 || i + count > words.size())
            return 0;
        const std::uint32_t* w = &words[i];
        if (code == kOpName && count >= 3)
            names[w[1]] = literal(words, i + 2, i + count);
        else if (code == kOpMemberName && count >= 4)
            members[{w[1], w[2]}] = literal(words, i + 3, i + count);
        else if (code == kOpConstant && count == 4)
            consts[w[2]] = {w[1], w[3]};
        else if (code == kOpTypeStruct && count >= 2)
            structs.insert(w[1]);
        else if (code == kOpTypeInt && count == 4 && w[2] == 32)
            int32.insert(w[1]);
        else if (code == kOpTypePointer && count == 4)
            pointee[w[1]] = w[3];
        else if (code == kOpVariable && count >= 4) {
            varPointer[w[2]] = w[1];
            varStorage[w[2]] = w[3];
        }
        i += count;
    }
    std::uint32_t blockType = 0, inputVar = 0;
    for (const auto& [id, name] : names) {
        if (name == block && structs.count(id))
            blockType = id;
        if (name == input && varStorage.count(id) && varStorage[id] == kInput)
            inputVar = id;
    }
    std::int64_t memberIndex = -1;
    for (const auto& [key, name] : members)
        if (key.first == blockType && name == member)
            memberIndex = key.second;
    if (!blockType || !inputVar || memberIndex < 0)
        return 0;
    // The module reads the member (an access chain whose first index is that member), and loads the input.
    bool reads = false;
    std::map<std::uint32_t, std::uint32_t> inputLoads;  // load result -> its type
    for (std::size_t i = kHeader; i < words.size();) {
        const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16), code = words[i] & 0xffff;
        const std::uint32_t* w = &words[i];
        if ((code == kOpAccessChain || code == kOpInBoundsAccessChain) && count >= 5 && varPointer.count(w[3]) &&
            pointee[varPointer[w[3]]] == blockType) {
            const auto c = consts.find(w[4]);
            reads |= c != consts.end() && c->second.second == static_cast<std::uint32_t>(memberIndex);
        } else if (code == kOpLoad && count >= 4 && w[3] == inputVar && int32.count(w[1])) {
            inputLoads[w[2]] = w[1];
        }
        i += count;
    }
    if (!reads || inputLoads.empty())
        return 0;
    std::uint32_t anyOne = 0;  // a 32-bit integer constant 1, for tests against 0
    for (const auto& [id, c] : consts)
        if (c.second == 1 && int32.count(c.first) && !anyOne)
            anyOne = id;
    std::uint32_t bound = words[3];
    int edited = 0;
    std::vector<std::uint32_t> out(words.begin(), words.begin() + kHeader);
    out.reserve(words.size() + 16);
    for (std::size_t i = kHeader; i < words.size();) {
        const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16), code = words[i] & 0xffff;
        const std::uint32_t* w = &words[i];
        int operand = 0;
        std::uint32_t one = 0;
        if ((code == kOpIEqual || code == kOpINotEqual) && count == 5) {
            for (int k = 0; k < 2 && !operand; ++k) {
                const auto c = consts.find(w[4 - k]);
                if (inputLoads.count(w[3 + k]) && c != consts.end() && c->second.second <= 1 && int32.count(c->second.first)) {
                    operand = 3 + k;
                    one = c->second.second == 1 ? w[4 - k] : anyOne;
                }
            }
        }
        if (operand && one) {  // masked = input & 1, then the test on masked
            const std::uint32_t masked = bound++;
            out.insert(out.end(), {op(kOpBitwiseAnd, 5), inputLoads[w[operand]], masked, w[operand], one});
            const std::size_t start = out.size();
            out.insert(out.end(), words.begin() + static_cast<std::ptrdiff_t>(i),
                       words.begin() + static_cast<std::ptrdiff_t>(i + count));
            out[start + static_cast<std::size_t>(operand)] = masked;
            ++edited;
        } else {
            out.insert(out.end(), words.begin() + static_cast<std::ptrdiff_t>(i),
                       words.begin() + static_cast<std::ptrdiff_t>(i + count));
        }
        i += count;
    }
    if (!edited)
        return 0;
    out[3] = bound;
    words.swap(out);
    return edited;
}

}  // namespace xplane
