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

int spirv_inverse_from_view_projection(std::vector<std::uint32_t>& words, const char* block, const char* member,
                                       const char* transform, const char* projection) {
    constexpr std::uint16_t kOpExtInstImport = 11, kOpExtInst = 12, kOpMemberName = 6, kOpMemberDecorate = 72;
    constexpr std::uint32_t kRowMajor = 4, kColMajor = 5, kMatrixInverse = 34;
    if (words.size() < kHeader || words[0] != kMagic)
        return 0;
    std::map<std::uint32_t, std::string> names;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::string> memberNames;  // (struct, member) -> name
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> major;      // (struct, member) -> Row/ColMajor
    std::map<std::uint32_t, std::vector<std::uint32_t>> structs;
    std::map<std::uint32_t, std::uint32_t> arrayElement;
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> consts;
    std::map<std::uint32_t, std::uint32_t> pointee, varType;
    std::uint32_t glsl = 0;
    for (std::size_t i = kHeader; i < words.size();) {
        const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16), code = words[i] & 0xffff;
        if (count == 0 || i + count > words.size())
            return 0;
        const std::uint32_t* w = &words[i];
        if (code == kOpName && count >= 3)
            names[w[1]] = literal(words, i + 2, i + count);
        else if (code == kOpMemberName && count >= 4)
            memberNames[{w[1], w[2]}] = literal(words, i + 3, i + count);
        else if (code == kOpMemberDecorate && count >= 4 && (w[3] == kRowMajor || w[3] == kColMajor))
            major[{w[1], w[2]}] = w[3];
        else if (code == kOpExtInstImport && count >= 3 && literal(words, i + 2, i + count) == "GLSL.std.450")
            glsl = w[1];
        else if (code == kOpTypeStruct)
            structs[w[1]] = std::vector<std::uint32_t>(w + 2, w + count);
        else if (code == kOpTypeArray && count == 4)
            arrayElement[w[1]] = w[2];
        else if (code == kOpTypePointer && count == 4 && w[2] == kUniform)
            pointee[w[1]] = w[3];
        else if (code == kOpVariable && count >= 4 && w[3] == kUniform)
            varType[w[2]] = w[1];
        if (code == kOpConstant && count == 4)
            consts[w[2]] = {w[1], w[3]};
        i += count;
    }
    if (!glsl)
        return 0;
    // A Uniform variable of the named block type, and its member's index, struct and array element type.
    struct Found {
        std::uint32_t var = 0, structId = 0, index = 0, element = 0;
    };
    auto find = [&](const std::string& typeName, const std::string& memberName) {
        Found f;
        for (const auto& [var, ptr] : varType) {
            const auto p = pointee.find(ptr);
            if (p == pointee.end() || !structs.count(p->second))
                continue;
            const auto n = names.find(p->second);
            if (n == names.end() || (n->second != typeName && n->second + "_var" != typeName))
                continue;
            const auto& members = structs[p->second];
            for (std::uint32_t m = 0; m < members.size(); ++m) {
                const auto mn = memberNames.find({p->second, m});
                const auto el = arrayElement.find(members[m]);
                if (mn != memberNames.end() && mn->second == memberName && el != arrayElement.end())
                    f = {var, p->second, m, el->second};
            }
        }
        return f;
    };
    const Found src = find(block, member), proj = find(transform, projection);
    if (!src.var || !proj.var || src.element != proj.element)
        return 0;
    const auto srcMajor = major.find({src.structId, src.index}), projMajor = major.find({proj.structId, proj.index});
    if (srcMajor == major.end() || projMajor == major.end() || srcMajor->second != projMajor->second)
        return 0;
    // The projection's accesses: the member constant they use and the indexes they take.
    std::uint32_t projMember = 0;
    std::set<std::uint32_t> projIndexes;
    struct Access {
        std::size_t at;
        std::uint32_t result;
    };
    std::vector<Access> accesses;
    for (std::size_t i = kHeader; i < words.size();) {
        const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16), code = words[i] & 0xffff;
        if ((code == kOpAccessChain || code == kOpInBoundsAccessChain) && count == 6) {
            const auto m = consts.find(words[i + 4]);
            if (m != consts.end()) {
                if (words[i + 3] == proj.var && m->second.second == proj.index) {
                    projMember = words[i + 4];
                    projIndexes.insert(words[i + 5]);
                } else if (words[i + 3] == src.var && m->second.second == src.index) {
                    accesses.push_back({i, words[i + 2]});
                }
            }
        }
        i += count;
    }
    if (!projMember)
        return 0;
    // Indexes already folded to the eye (`i & 1`, spirv_fold_eye_arrays): the unfolded `i`.
    std::map<std::uint32_t, std::uint32_t> unfold;
    for (std::size_t i = kHeader; i < words.size();) {
        const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16), code = words[i] & 0xffff;
        if (code == kOpBitwiseAnd && count == 5) {
            const auto c = consts.find(words[i + 4]);
            if (c != consts.end() && c->second.second == 1)
                unfold[words[i + 2]] = words[i + 3];
        }
        i += count;
    }
    // Each source access with a shared index: point it at the projection; each load of it gets a new id, and
    // OpExtInst MatrixInverse gives the old id.
    std::map<std::size_t, std::uint32_t> editAccess;  // position -> the index to use
    std::set<std::uint32_t> edited;
    for (const auto& a : accesses) {
        std::uint32_t index = words[a.at + 5];
        const auto u = unfold.find(index);
        if (!projIndexes.count(index) && u != unfold.end())
            index = u->second;
        if (projIndexes.count(index)) {
            editAccess[a.at] = index;
            edited.insert(a.result);
        }
    }
    if (edited.empty())
        return 0;
    std::uint32_t bound = words[3];
    std::vector<std::uint32_t> out(words.begin(), words.begin() + kHeader);
    out.reserve(words.size() + edited.size() * 8);
    int loads = 0;
    for (std::size_t i = kHeader; i < words.size();) {
        const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16), code = words[i] & 0xffff;
        const std::size_t start = out.size();
        out.insert(out.end(), words.begin() + static_cast<std::ptrdiff_t>(i),
                   words.begin() + static_cast<std::ptrdiff_t>(i + count));
        if (editAccess.count(i)) {
            out[start + 3] = proj.var;
            out[start + 4] = projMember;
            out[start + 5] = editAccess[i];
        } else if (code == kOpLoad && count >= 4 && edited.count(words[i + 3])) {
            const std::uint32_t type = words[i + 1], result = words[i + 2], raw = bound++;
            out[start + 2] = raw;
            out.insert(out.end(), {op(kOpExtInst, 6), type, result, glsl, kMatrixInverse, raw});
            ++loads;
        }
        i += count;
    }
    // A pointer to the source element used other than by a load (e.g. a copy) would now point at the projection
    // without the inverse: give up rather than change its meaning.
    std::size_t uses = 0;
    for (std::size_t i = kHeader; i < words.size();) {
        const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16);
        for (std::size_t k = 1; k < count; ++k)
            uses += edited.count(words[i + k]) && !editAccess.count(i) ? 1 : 0;
        i += count;
    }
    if (uses != static_cast<std::size_t>(loads))
        return 0;
    out[3] = bound;
    words.swap(out);
    return loads;
}

}  // namespace xplane
