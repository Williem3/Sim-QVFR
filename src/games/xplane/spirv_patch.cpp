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
    // same t for both projections. So ndc_eye = s ndc_view + k, s = Pe[0][0] / Pv[0][0], k = Pe[2][0]/Pe[2][3] -
    // s Pv[2][0]/Pv[2][3]; likewise y with [1][1], [2][1]. uv = (ndc_x + 1) / 2 across, (1 - ndc_y) / 2 down (README).
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

}  // namespace xplane
