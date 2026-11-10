// DXBC edits (dxbc_patch.h). Token layout: d3d10tokenizedprogramformat.hpp / d3d11tokenizedprogramformat.hpp in the
// Windows SDK's driver kit; checked on LMU's 129 scene vertex shaders (docs/recon-lmu.md "Multi-view rendering").
#include "engine/dxbc_patch.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace simqvfr::dxbc {

namespace {

// ---- the checksum: MD5's compression with DXBC's final block ----

uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

void md5_block(uint32_t (&s)[4], const uint8_t* block) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                              14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              4,  11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t m[16];
    std::memcpy(m, block, 64);
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        f += a + K[i] + m[g];
        a = d;
        d = c;
        c = b;
        b += rotl(f, R[i]);
    }
    s[0] += a;
    s[1] += b;
    s[2] += c;
    s[3] += d;
}

// ---- tokens ----

enum : uint32_t {
    kOpAnd = 1, kOpMad = 50, kOpCustomData = 53, kOpMov = 54, kOpMovc = 55, kOpMul = 56, kOpRet = 62, kOpRetc = 63,
    kOpUshr = 85, kOpDclConstantBuffer = 89, kOpDclIndexRange = 91, kOpDclInput = 95, kOpDclInputSgv = 96,
    kOpDclInputSiv = 97, kOpDclOutput = 101, kOpDclOutputSgv = 102, kOpDclOutputSiv = 103, kOpDclTemps = 104,
    kOpDclGlobalFlags = 106,
};
enum : uint32_t { kTemp = 0, kInput = 1, kOutput = 2, kImm32 = 4, kImm64 = 5, kConstantBuffer = 8 };
// System value names (declarations' name tokens and the signatures' system values share them).
enum : uint32_t { kNamePosition = 1, kNameRenderTargetArrayIndex = 4, kNameViewportArrayIndex = 5, kNameInstanceId = 8 };
enum : uint32_t { kComponentUint32 = 1 };

uint32_t opcode_of(uint32_t token) { return token & 0x7ff; }
uint32_t length_of(const uint32_t* t) { return opcode_of(t[0]) == kOpCustomData ? t[1] : (t[0] >> 24) & 0x7f; }

// Declarations: D3D10's (88-106), D3D11's (143-162, 206) and customdata (an immediate constant buffer).
bool is_declaration(uint32_t op) {
    return (op >= 88 && op <= 106) || (op >= 143 && op <= 162) || op == 206 || op == kOpCustomData;
}

uint32_t instruction(uint32_t op, uint32_t length) { return op | (length << 24); }
// An operand token: components (0, 1 or 4: field 0/1/2), selection (0 mask, 1 swizzle, 2 select-1) and its bits,
// type, index dimension, immediate32 indices.
uint32_t operand(uint32_t comps, uint32_t selection, uint32_t bits, uint32_t type, uint32_t dims) {
    return comps | (selection << 2) | (bits << 4) | (type << 12) | (dims << 20);
}
uint32_t swizzle(int x, int y, int z, int w) { return static_cast<uint32_t>(x | (y << 2) | (z << 4) | (w << 6)); }
void dst(std::vector<uint32_t>& out, uint32_t type, uint32_t index, uint32_t mask) {
    out.push_back(operand(2, 0, mask, type, 1));
    out.push_back(index);
}
void src_temp(std::vector<uint32_t>& out, uint32_t index, uint32_t swz) {
    out.push_back(operand(2, 1, swz, kTemp, 1));
    out.push_back(index);
}
void src_cb(std::vector<uint32_t>& out, uint32_t slot, uint32_t element, uint32_t swz) {
    out.push_back(operand(2, 1, swz, kConstantBuffer, 2));
    out.push_back(slot);
    out.push_back(element);
}

struct Operand {
    uint32_t type = 0, dims = 0;
    bool index0Immediate = false;
    uint32_t index0 = 0;
};

// One operand at t (n tokens left): its length in tokens, 0 when it can't be read.
size_t read_operand(const uint32_t* t, size_t n, Operand& o) {
    if (n == 0)
        return 0;
    const uint32_t token = t[0];
    size_t at = 1;
    o.type = (token >> 12) & 0xff;
    o.dims = (token >> 20) & 3;
    for (bool extended = (token >> 31) != 0; extended; ++at) {
        if (at >= n)
            return 0;
        extended = (t[at] >> 31) != 0;
    }
    const uint32_t comps = token & 3;
    if (o.type == kImm32)
        at += comps == 1 ? 1 : comps == 2 ? 4 : 0;
    else if (o.type == kImm64)
        at += comps == 1 ? 2 : comps == 2 ? 8 : 0;
    for (uint32_t d = 0; d < o.dims; ++d) {
        const uint32_t representation = (token >> (22 + 3 * d)) & 7;
        if (representation == 0 || representation == 3) {
            if (at >= n)
                return 0;
            if (d == 0) {
                o.index0Immediate = representation == 0;
                o.index0 = t[at];
            }
            ++at;
        } else if (representation == 1 || representation == 4) {
            at += 2;
        }
        if (representation >= 2) {  // a relative index: an operand of its own
            Operand inner;
            const size_t len = read_operand(t + at, at < n ? n - at : 0, inner);
            if (!len)
                return 0;
            at += len;
        }
    }
    return at <= n ? at : 0;
}

struct Chunk {
    uint32_t fourcc;
    std::vector<uint8_t> data;
};

uint32_t fourcc(const char* s) {
    return static_cast<uint32_t>(s[0]) | (static_cast<uint32_t>(s[1]) << 8) | (static_cast<uint32_t>(s[2]) << 16) |
           (static_cast<uint32_t>(s[3]) << 24);
}

bool read_chunks(const uint8_t* data, size_t size, std::vector<Chunk>& chunks) {
    if (size < 32 || std::memcmp(data, "DXBC", 4) != 0)
        return false;
    uint32_t total = 0, count = 0;
    std::memcpy(&total, data + 24, 4);
    std::memcpy(&count, data + 28, 4);
    if (total != size || 32 + size_t{count} * 4 > size)
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = 0, cc = 0, len = 0;
        std::memcpy(&at, data + 32 + i * 4, 4);
        if (size_t{at} + 8 > size)
            return false;
        std::memcpy(&cc, data + at, 4);
        std::memcpy(&len, data + at + 4, 4);
        if (size_t{at} + 8 + len > size)
            return false;
        chunks.push_back({cc, std::vector<uint8_t>(data + at + 8, data + at + 8 + len)});
    }
    return true;
}

std::vector<uint8_t> write_chunks(const std::vector<Chunk>& chunks) {
    const uint32_t count = static_cast<uint32_t>(chunks.size());
    uint32_t total = 32 + count * 4;
    for (const Chunk& c : chunks)
        total += 8 + static_cast<uint32_t>(c.data.size());
    std::vector<uint8_t> out(total, 0);
    std::memcpy(out.data(), "DXBC", 4);
    const uint32_t one = 1;
    std::memcpy(out.data() + 20, &one, 4);
    std::memcpy(out.data() + 24, &total, 4);
    std::memcpy(out.data() + 28, &count, 4);
    uint32_t at = 32 + count * 4;
    for (uint32_t i = 0; i < count; ++i) {
        std::memcpy(out.data() + 32 + i * 4, &at, 4);
        const uint32_t len = static_cast<uint32_t>(chunks[i].data.size());
        std::memcpy(out.data() + at, &chunks[i].fourcc, 4);
        std::memcpy(out.data() + at + 4, &len, 4);
        if (len)
            std::memcpy(out.data() + at + 8, chunks[i].data.data(), len);
        at += 8 + len;
    }
    sign(out);
    return out;
}

// An output signature (OSGN: 24-byte elements, names after them).
struct Element {
    std::string name;
    uint32_t index = 0, systemValue = 0, componentType = 0, reg = 0;
    uint8_t mask = 0, neverWritten = 0;
};

bool read_signature(const std::vector<uint8_t>& d, std::vector<Element>& out) {
    if (d.size() < 8)
        return false;
    uint32_t count = 0;
    std::memcpy(&count, d.data(), 4);
    if (8 + size_t{count} * 24 > d.size())
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = d.data() + 8 + i * 24;
        Element el;
        uint32_t nameAt = 0;
        std::memcpy(&nameAt, e, 4);
        std::memcpy(&el.index, e + 4, 4);
        std::memcpy(&el.systemValue, e + 8, 4);
        std::memcpy(&el.componentType, e + 12, 4);
        std::memcpy(&el.reg, e + 16, 4);
        el.mask = e[20];
        el.neverWritten = e[21];
        if (nameAt >= d.size())
            return false;
        const auto* begin = reinterpret_cast<const char*>(d.data() + nameAt);
        el.name.assign(begin, strnlen(begin, d.size() - nameAt));
        out.push_back(el);
    }
    return true;
}

std::vector<uint8_t> write_signature(const std::vector<Element>& elements) {
    std::vector<uint8_t> d(8 + elements.size() * 24, 0);
    const uint32_t count = static_cast<uint32_t>(elements.size()), eight = 8;
    std::memcpy(d.data(), &count, 4);
    std::memcpy(d.data() + 4, &eight, 4);
    std::map<std::string, uint32_t> names;
    for (const Element& el : elements) {
        if (names.count(el.name))
            continue;
        names[el.name] = static_cast<uint32_t>(d.size());
        d.insert(d.end(), el.name.begin(), el.name.end());
        d.push_back(0);
    }
    while (d.size() % 4)
        d.push_back(0xab);  // the compiler's padding
    for (size_t i = 0; i < elements.size(); ++i) {
        uint8_t* e = d.data() + 8 + i * 24;
        const Element& el = elements[i];
        const uint32_t nameAt = names[el.name];
        std::memcpy(e, &nameAt, 4);
        std::memcpy(e + 4, &el.index, 4);
        std::memcpy(e + 8, &el.systemValue, 4);
        std::memcpy(e + 12, &el.componentType, 4);
        std::memcpy(e + 16, &el.reg, 4);
        e[20] = el.mask;
        e[21] = el.neverWritten;
    }
    return d;
}

}  // namespace

void checksum(const uint8_t* data, size_t size, uint32_t (&out)[4]) {
    uint32_t s[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const uint8_t* p = data + 20;
    const uint32_t n = static_cast<uint32_t>(size - 20), full = n & ~63u, rest = n - full;
    for (uint32_t i = 0; i < full; i += 64)
        md5_block(s, p + i);
    const uint32_t bits = n * 8, tail = (n * 2) | 1;
    uint8_t block[64] = {};
    if (rest >= 56) {
        std::memcpy(block, p + full, rest);
        block[rest] = 0x80;
        md5_block(s, block);
        std::memset(block, 0, 64);
        std::memcpy(block, &bits, 4);
        std::memcpy(block + 60, &tail, 4);
    } else {
        std::memcpy(block, &bits, 4);
        std::memcpy(block + 4, p + full, rest);
        block[4 + rest] = 0x80;
        std::memcpy(block + 60, &tail, 4);
    }
    md5_block(s, block);
    std::memcpy(out, s, 16);
}

bool checksum_ok(const uint8_t* data, size_t size) {
    if (size < 32)
        return false;
    uint32_t sum[4];
    checksum(data, size, sum);
    return std::memcmp(sum, data + 4, 16) == 0;
}

void sign(std::vector<uint8_t>& container) {
    uint32_t sum[4];
    checksum(container.data(), container.size(), sum);
    std::memcpy(container.data() + 4, sum, 16);
}

SecondView add_second_view(const uint8_t* data, size_t size, const SecondViewOptions& options) {
    SecondView r;
    std::vector<Chunk> chunks;
    if (!read_chunks(data, size, chunks)) {
        r.error = "not a DXBC container";
        return r;
    }
    Chunk* program = nullptr;
    Chunk* signature = nullptr;
    for (Chunk& c : chunks) {
        if (c.fourcc == fourcc("SHDR") || c.fourcc == fourcc("SHEX"))
            program = &c;
        else if (c.fourcc == fourcc("OSGN"))
            signature = &c;
    }
    if (!program || !signature || program->data.size() < 8) {
        r.error = !program ? "no SHDR/SHEX" : "no OSGN (OSG5/OSG1 not handled)";
        return r;
    }
    std::vector<Element> outputs;
    if (!read_signature(signature->data, outputs)) {
        r.error = "unreadable output signature";
        return r;
    }
    bool havePosition = false;
    uint32_t nextReg = 0;
    for (const Element& el : outputs) {
        if (el.systemValue == 1) {  // D3D_NAME_POSITION
            r.positionRegister = el.reg;
            havePosition = true;
        }
        if (el.name == kRightSemantic) {
            r.error = "already converted";
            return r;
        }
        nextReg = std::max(nextReg, el.reg + 1);
    }
    if (!havePosition) {
        r.error = "no SV_Position output";
        return r;
    }
    const auto* tokens = reinterpret_cast<const uint32_t*>(program->data.data());
    const uint32_t version = tokens[0], length = tokens[1];
    if ((version >> 16) != 1) {
        r.error = "not a vertex shader";
        return r;
    }
    if (size_t{length} * 4 > program->data.size() || length < 2) {
        r.error = "program length";
        return r;
    }
    const uint32_t maxOutputs = ((version >> 4) & 0xf) >= 5 ? 32u : 16u;
    r.rightRegister = nextReg;
    r.hasMask = options.viewportMask;
    r.maskRegister = nextReg + 1;
    if ((r.hasMask ? r.maskRegister : r.rightRegister) + 1 > maxOutputs) {
        r.error = "no free output register";
        return r;
    }

    // First pass: the declarations (temps, where outputs and constant buffers end) and the program's soundness.
    uint32_t temps = 0;
    size_t tempsAt = 0, lastOutputDcl = 0, lastCbDcl = 0, firstInstruction = 0, globalFlagsEnd = 2;
    for (size_t i = 2; i < length;) {
        const uint32_t n = length_of(tokens + i);
        if (n == 0 || i + n > length) {
            r.error = "instruction walk";
            return r;
        }
        const uint32_t op = opcode_of(tokens[i]);
        if (!firstInstruction && !is_declaration(op))
            firstInstruction = i;
        if (op == kOpDclTemps) {
            temps = tokens[i + 1];
            tempsAt = i;
        } else if (op == kOpDclOutput || op == kOpDclOutputSiv || op == kOpDclOutputSgv) {
            lastOutputDcl = i + n;
        } else if (op == kOpDclConstantBuffer) {
            lastCbDcl = i + n;
            Operand o;
            if (read_operand(tokens + i + 1, n - 1, o) && tokens[i + 2] == options.cbSlot) {
                r.error = "the remap's constant buffer slot is in use";
                return r;
            }
        } else if (op == kOpDclGlobalFlags) {
            globalFlagsEnd = i + n;
        } else if (op == kOpDclIndexRange) {
            Operand o;
            if (read_operand(tokens + i + 1, n - 1, o) && o.type == kOutput && o.index0 <= r.positionRegister &&
                r.positionRegister < o.index0 + tokens[i + n - 1]) {
                r.error = "SV_Position in an index range";
                return r;
            }
        } else if (!is_declaration(op)) {
            // Every operand read; SV_Position may only be the first (the destination) and never relatively indexed.
            size_t at = i + 1;
            for (bool extended = (tokens[i] >> 31) != 0; extended; ++at)
                extended = (tokens[at] >> 31) != 0;
            for (int k = 0; at < i + n; ++k) {
                Operand o;
                const size_t len = read_operand(tokens + at, i + n - at, o);
                if (!len) {
                    r.error = "unreadable operand (opcode " + std::to_string(op) + ")";
                    return r;
                }
                if (o.type == kOutput && (!o.index0Immediate || o.index0 == r.positionRegister)) {
                    if (k != 0 || !o.index0Immediate) {
                        r.error = "SV_Position written other than as a first operand";
                        return r;
                    }
                    ++r.positionWrites;
                }
                at += len;
            }
        }
        i += n;
    }
    if (!firstInstruction || r.positionWrites == 0) {
        r.error = r.positionWrites ? "no instructions" : "SV_Position never written";
        return r;
    }
    const uint32_t tempT = temps, tempU = temps + 1;

    // The new declarations.
    std::vector<uint32_t> outputDcls;
    const auto declare = [&](uint32_t reg, uint32_t mask) {
        outputDcls.push_back(instruction(kOpDclOutput, 3));
        dst(outputDcls, kOutput, reg, mask);
    };
    declare(r.rightRegister, 0xf);
    if (r.hasMask)
        declare(r.maskRegister, 0x1);
    std::vector<uint32_t> cbDcl = {instruction(kOpDclConstantBuffer, 4), operand(2, 1, swizzle(0, 1, 2, 3), kConstantBuffer, 2),
                                   options.cbSlot, 1};
    // Before each ret: U.xy = T.ww * (b, d); right.xy = T.xy * (a, c) + U.xy; right.zw = T.zw; mask = its value.
    std::vector<uint32_t> epilogue;
    epilogue.push_back(instruction(kOpMul, 8));
    dst(epilogue, kTemp, tempU, 0x3);
    src_temp(epilogue, tempT, swizzle(3, 3, 3, 3));
    src_cb(epilogue, options.cbSlot, 0, swizzle(1, 3, 1, 1));
    epilogue.push_back(instruction(kOpMad, 10));
    dst(epilogue, kOutput, r.rightRegister, 0x3);
    src_temp(epilogue, tempT, swizzle(0, 1, 0, 0));
    src_cb(epilogue, options.cbSlot, 0, swizzle(0, 2, 0, 0));
    src_temp(epilogue, tempU, swizzle(0, 1, 0, 0));
    epilogue.push_back(instruction(kOpMov, 5));
    dst(epilogue, kOutput, r.rightRegister, 0xc);
    src_temp(epilogue, tempT, swizzle(2, 2, 2, 3));
    if (r.hasMask) {  // mov o<mask>.x, l(mask)
        epilogue.push_back(instruction(kOpMov, 5));
        dst(epilogue, kOutput, r.maskRegister, 0x1);
        epilogue.insert(epilogue.end(), {operand(1, 0, 0, kImm32, 0), options.mask});
    }

    // Second pass: copy, inserting the declarations, the clones and the epilogues.
    const size_t cbAt = lastCbDcl ? lastCbDcl : globalFlagsEnd;
    const size_t outputsAt = lastOutputDcl ? lastOutputDcl : firstInstruction;
    std::vector<uint32_t> out(tokens, tokens + 2);
    for (size_t i = 2; i < length;) {
        if (i == cbAt)
            out.insert(out.end(), cbDcl.begin(), cbDcl.end());
        if (i == outputsAt) {
            out.insert(out.end(), outputDcls.begin(), outputDcls.end());
            if (!tempsAt)
                out.insert(out.end(), {instruction(kOpDclTemps, 2), 2u});
        }
        const uint32_t n = length_of(tokens + i);
        const uint32_t op = opcode_of(tokens[i]);
        if (op == kOpRet || op == kOpRetc)
            out.insert(out.end(), epilogue.begin(), epilogue.end());
        const size_t start = out.size();
        out.insert(out.end(), tokens + i, tokens + i + n);
        if (i == tempsAt)
            out[start + 1] = temps + 2;
        if (!is_declaration(op)) {
            size_t at = 1;
            for (bool extended = (tokens[i] >> 31) != 0; extended; ++at)
                extended = (tokens[i + at] >> 31) != 0;
            Operand o;
            const size_t len = read_operand(tokens + i + at, n - at, o);
            if (len && o.type == kOutput && o.index0Immediate && o.index0 == r.positionRegister) {
                // The clone: the same instruction into temp T. Its index is the token after the operand token and
                // any extended operand tokens.
                std::vector<uint32_t> clone(tokens + i, tokens + i + n);
                clone[at] = (clone[at] & ~(0xffu << 12)) | (kTemp << 12);
                size_t index = at + 1;
                for (bool extended = (clone[at] >> 31) != 0; extended; ++index)
                    extended = (clone[index] >> 31) != 0;
                clone[index] = tempT;
                out.insert(out.end(), clone.begin(), clone.end());
            }
        }
        i += n;
    }
    if (cbAt >= length)
        out.insert(out.end(), cbDcl.begin(), cbDcl.end());
    out[1] = static_cast<uint32_t>(out.size());

    outputs.push_back({kRightSemantic, 0, 0, 3, r.rightRegister, 0xf, 0});
    if (r.hasMask)
        outputs.push_back({kMaskSemantic, 0, 0, 1, r.maskRegister, 0x1, 0});
    program->data.assign(reinterpret_cast<const uint8_t*>(out.data()),
                         reinterpret_cast<const uint8_t*>(out.data()) + out.size() * 4);
    signature->data = write_signature(outputs);
    r.bytecode = write_chunks(chunks);
    return r;
}

InstancedView add_instanced_view(const uint8_t* data, size_t size, const InstancedViewOptions& options) {
    InstancedView r;
    std::vector<Chunk> chunks;
    if (!read_chunks(data, size, chunks)) {
        r.error = "not a DXBC container";
        return r;
    }
    int programAt = -1, inputsAt = -1, outputsAt = -1, featuresAt = -1;
    for (size_t i = 0; i < chunks.size(); ++i) {
        const uint32_t cc = chunks[i].fourcc;
        if (cc == fourcc("SHDR") || cc == fourcc("SHEX"))
            programAt = static_cast<int>(i);
        else if (cc == fourcc("ISGN"))
            inputsAt = static_cast<int>(i);
        else if (cc == fourcc("OSGN"))
            outputsAt = static_cast<int>(i);
        else if (cc == fourcc("SFI0"))
            featuresAt = static_cast<int>(i);
    }
    if (programAt < 0 || outputsAt < 0 || inputsAt < 0 || chunks[programAt].data.size() < 8) {
        r.error = programAt < 0   ? "no SHDR/SHEX"
                  : outputsAt < 0 ? "no OSGN (OSG5/OSG1 not handled)"
                  : inputsAt < 0  ? "no ISGN (ISG1 not handled)"
                                  : "program length";
        return r;
    }
    std::vector<Element> outputs, inputs;
    if (!read_signature(chunks[outputsAt].data, outputs) || !read_signature(chunks[inputsAt].data, inputs)) {
        r.error = "unreadable signature";
        return r;
    }
    bool havePosition = false;
    uint8_t positionMask = 0xf;
    uint32_t nextOut = 0, nextIn = 0;
    for (const Element& el : outputs) {
        if (el.systemValue == kNamePosition) {
            r.positionRegister = el.reg;
            positionMask = el.mask ? el.mask : 0xf;
            havePosition = true;
        }
        if (el.systemValue == kNameViewportArrayIndex || el.systemValue == kNameRenderTargetArrayIndex) {
            r.error = "already writes a viewport or render target array index";
            return r;
        }
        nextOut = std::max(nextOut, el.reg + 1);
    }
    for (const Element& el : inputs) {
        if (el.systemValue == kNameInstanceId) {
            r.hadInstance = true;
            r.instanceRegister = el.reg;
        }
        nextIn = std::max(nextIn, el.reg + 1);
    }
    if (!havePosition) {
        r.error = "no SV_Position output";
        return r;
    }
    const auto* tokens = reinterpret_cast<const uint32_t*>(chunks[programAt].data.data());
    const uint32_t version = tokens[0], length = tokens[1];
    if ((version >> 16) != 1) {
        r.error = "not a vertex shader";
        return r;
    }
    if (size_t{length} * 4 > chunks[programAt].data.size() || length < 2) {
        r.error = "program length";
        return r;
    }
    const uint32_t registers = ((version >> 4) & 0xf) >= 5 ? 32u : 16u;  // inputs and outputs: vs_4_x 16, vs_5_0 32

    // First pass: the declarations (temps, where inputs, outputs and constant buffers end), the program's soundness, the
    // position's writes and the instance's reads.
    uint32_t temps = 0;
    size_t tempsAt = 0, lastInputDcl = 0, lastOutputDcl = 0, lastCbDcl = 0, firstInstruction = 0, globalFlagsEnd = 2;
    for (size_t i = 2; i < length;) {
        const uint32_t n = length_of(tokens + i);
        if (n == 0 || i + n > length) {
            r.error = "instruction walk";
            return r;
        }
        const uint32_t op = opcode_of(tokens[i]);
        if (!firstInstruction && !is_declaration(op))
            firstInstruction = i;
        if (op == kOpDclTemps) {
            temps = tokens[i + 1];
            tempsAt = i;
        } else if (op == kOpDclInput || op == kOpDclInputSgv || op == kOpDclInputSiv) {
            lastInputDcl = i + n;
            Operand o;
            if (read_operand(tokens + i + 1, n - 1, o) && o.type == kInput && o.index0Immediate)
                nextIn = std::max(nextIn, o.index0 + 1);
        } else if (op == kOpDclOutput || op == kOpDclOutputSiv || op == kOpDclOutputSgv) {
            lastOutputDcl = i + n;
            Operand o;
            if (read_operand(tokens + i + 1, n - 1, o) && o.type == kOutput && o.index0Immediate)
                nextOut = std::max(nextOut, o.index0 + 1);
        } else if (op == kOpDclConstantBuffer) {
            lastCbDcl = i + n;
            Operand o;
            if (read_operand(tokens + i + 1, n - 1, o) && tokens[i + 2] == options.cbSlot) {
                r.error = "the remap's constant buffer slot is in use";
                return r;
            }
        } else if (op == kOpDclGlobalFlags) {
            globalFlagsEnd = i + n;
        } else if (op == kOpDclIndexRange) {
            Operand o;
            if (read_operand(tokens + i + 1, n - 1, o)) {
                const uint32_t count = tokens[i + n - 1];
                if (o.type == kOutput && o.index0 <= r.positionRegister && r.positionRegister < o.index0 + count) {
                    r.error = "SV_Position in an index range";
                    return r;
                }
                if (o.type == kInput && r.hadInstance && o.index0 <= r.instanceRegister &&
                    r.instanceRegister < o.index0 + count) {
                    r.error = "SV_InstanceID in an index range";
                    return r;
                }
            }
        } else if (!is_declaration(op)) {
            size_t at = i + 1;
            for (bool extended = (tokens[i] >> 31) != 0; extended; ++at)
                extended = (tokens[at] >> 31) != 0;
            for (int k = 0; at < i + n; ++k) {
                Operand o;
                const size_t len = read_operand(tokens + at, i + n - at, o);
                if (!len) {
                    r.error = "unreadable operand (opcode " + std::to_string(op) + ")";
                    return r;
                }
                if (o.type == kOutput && (!o.index0Immediate || o.index0 == r.positionRegister)) {
                    if (k != 0 || !o.index0Immediate) {
                        r.error = "SV_Position written other than as a first operand";
                        return r;
                    }
                    ++r.positionWrites;
                }
                if (o.type == kInput && r.hadInstance) {
                    if (!o.index0Immediate) {
                        r.error = "an input read by a relative index";
                        return r;
                    }
                    r.instanceReads += o.index0 == r.instanceRegister ? 1 : 0;
                }
                at += len;
            }
        }
        i += n;
    }
    if (!firstInstruction || r.positionWrites == 0) {
        r.error = r.positionWrites ? "no instructions" : "SV_Position never written";
        return r;
    }
    if (!r.hadInstance)
        r.instanceRegister = nextIn;
    r.viewportRegister = nextOut;
    if (r.viewportRegister + 1 > registers || r.instanceRegister + 1 > registers) {
        r.error = r.viewportRegister + 1 > registers ? "no free output register" : "no free input register";
        return r;
    }
    const uint32_t tempT = temps, tempU = temps + 1, tempI = temps + 2;  // the position; its remap; instance and view

    // The new declarations.
    std::vector<uint32_t> inputDcl;
    if (!r.hadInstance)  // dcl_input_sgv v<i>.x, instance_id
        inputDcl = {instruction(kOpDclInputSgv, 4), operand(2, 0, 0x1, kInput, 1), r.instanceRegister, kNameInstanceId};
    // dcl_output_siv o<vp>.x, viewport_array_index
    const std::vector<uint32_t> outputDcl = {instruction(kOpDclOutputSiv, 4), operand(2, 0, 0x1, kOutput, 1),
                                             r.viewportRegister, kNameViewportArrayIndex};
    const std::vector<uint32_t> cbDcl = {instruction(kOpDclConstantBuffer, 4),
                                         operand(2, 1, swizzle(0, 1, 2, 3), kConstantBuffer, 2), options.cbSlot, 1};
    // At the start: I.x = instance >> 1 (the game's own instance), I.y = instance & 1 (the view).
    std::vector<uint32_t> prologue;
    const auto instance_and_one = [&] {
        prologue.push_back(operand(2, 1, swizzle(0, 0, 0, 0), kInput, 1));
        prologue.push_back(r.instanceRegister);
        prologue.insert(prologue.end(), {operand(1, 0, 0, kImm32, 0), 1u});
    };
    prologue.push_back(instruction(kOpUshr, 7));
    dst(prologue, kTemp, tempI, 0x1);
    instance_and_one();
    prologue.push_back(instruction(kOpAnd, 7));
    dst(prologue, kTemp, tempI, 0x2);
    instance_and_one();
    // Before each ret: U.xy = T.ww * (b, d); U.xy = T.xy * (a, c) + U.xy; U.zw = T.zw; position = view ? U : T;
    // viewport index = view.
    std::vector<uint32_t> epilogue;
    epilogue.push_back(instruction(kOpMul, 8));
    dst(epilogue, kTemp, tempU, 0x3);
    src_temp(epilogue, tempT, swizzle(3, 3, 3, 3));
    src_cb(epilogue, options.cbSlot, 0, swizzle(1, 3, 1, 1));
    epilogue.push_back(instruction(kOpMad, 10));
    dst(epilogue, kTemp, tempU, 0x3);
    src_temp(epilogue, tempT, swizzle(0, 1, 0, 0));
    src_cb(epilogue, options.cbSlot, 0, swizzle(0, 2, 0, 0));
    src_temp(epilogue, tempU, swizzle(0, 1, 0, 0));
    epilogue.push_back(instruction(kOpMov, 5));
    dst(epilogue, kTemp, tempU, 0xc);
    src_temp(epilogue, tempT, swizzle(2, 2, 2, 3));
    epilogue.push_back(instruction(kOpMovc, 9));
    dst(epilogue, kOutput, r.positionRegister, positionMask);
    src_temp(epilogue, tempI, swizzle(1, 1, 1, 1));
    src_temp(epilogue, tempU, swizzle(0, 1, 2, 3));
    src_temp(epilogue, tempT, swizzle(0, 1, 2, 3));
    epilogue.push_back(instruction(kOpMov, 5));
    dst(epilogue, kOutput, r.viewportRegister, 0x1);
    src_temp(epilogue, tempI, swizzle(1, 1, 1, 1));

    // Second pass: copy, inserting the declarations, the prologue and the epilogues; the position's writes go to T and
    // the instance's reads come from I.x.
    const size_t cbAt = lastCbDcl ? lastCbDcl : globalFlagsEnd;
    const size_t outputsAt2 = lastOutputDcl ? lastOutputDcl : firstInstruction;
    const size_t inputsAt2 = lastInputDcl ? lastInputDcl : outputsAt2;
    // An operand (at `at` in `code`) made a register of `type` numbered `index`: its type bits and its first index (the
    // token after the operand token and any extended operand tokens).
    const auto retarget = [](std::vector<uint32_t>& code, size_t at, uint32_t type, uint32_t index) {
        code[at] = (code[at] & ~(0xffu << 12)) | (type << 12);
        size_t idx = at + 1;
        for (bool extended = (code[at] >> 31) != 0; extended; ++idx)
            extended = (code[idx] >> 31) != 0;
        code[idx] = index;
    };
    std::vector<uint32_t> out(tokens, tokens + 2);
    for (size_t i = 2; i < length;) {
        if (i == cbAt)
            out.insert(out.end(), cbDcl.begin(), cbDcl.end());
        if (i == inputsAt2)
            out.insert(out.end(), inputDcl.begin(), inputDcl.end());
        if (i == outputsAt2) {
            out.insert(out.end(), outputDcl.begin(), outputDcl.end());
            if (!tempsAt)
                out.insert(out.end(), {instruction(kOpDclTemps, 2), 3u});
        }
        if (i == firstInstruction)
            out.insert(out.end(), prologue.begin(), prologue.end());
        const uint32_t n = length_of(tokens + i);
        const uint32_t op = opcode_of(tokens[i]);
        if (op == kOpRet || op == kOpRetc)
            out.insert(out.end(), epilogue.begin(), epilogue.end());
        const size_t start = out.size();
        out.insert(out.end(), tokens + i, tokens + i + n);
        if (i == tempsAt)
            out[start + 1] = temps + 3;
        if (!is_declaration(op)) {
            size_t at = 1;
            for (bool extended = (tokens[i] >> 31) != 0; extended; ++at)
                extended = (tokens[i + at] >> 31) != 0;
            for (int k = 0; at < n; ++k) {
                Operand o;
                const size_t len = read_operand(tokens + i + at, n - at, o);
                if (!len)
                    break;  // not reached: the first pass read every operand
                if (k == 0 && o.type == kOutput && o.index0Immediate && o.index0 == r.positionRegister)
                    retarget(out, start + at, kTemp, tempT);
                else if (r.hadInstance && o.type == kInput && o.index0Immediate && o.index0 == r.instanceRegister)
                    retarget(out, start + at, kTemp, tempI);  // its swizzle picks .x: the halved instance is there
                at += len;
            }
        }
        i += n;
    }
    if (cbAt >= length)
        out.insert(out.end(), cbDcl.begin(), cbDcl.end());
    out[1] = static_cast<uint32_t>(out.size());

    outputs.push_back({kViewportIndexSemantic, 0, kNameViewportArrayIndex, kComponentUint32, r.viewportRegister, 0x1, 0xe});
    if (!r.hadInstance)
        inputs.push_back({kInstanceIdSemantic, 0, kNameInstanceId, kComponentUint32, r.instanceRegister, 0x1, 0x1});
    chunks[programAt].data.assign(reinterpret_cast<const uint8_t*>(out.data()),
                                  reinterpret_cast<const uint8_t*>(out.data()) + out.size() * 4);
    chunks[outputsAt].data = write_signature(outputs);
    chunks[inputsAt].data = write_signature(inputs);
    uint64_t flags = 0;
    if (featuresAt >= 0 && chunks[featuresAt].data.size() >= 8)
        std::memcpy(&flags, chunks[featuresAt].data.data(), 8);
    flags |= kRequiresViewportIndexFromAnyShader;
    std::vector<uint8_t> sfi0(8);
    std::memcpy(sfi0.data(), &flags, 8);
    if (featuresAt >= 0)
        chunks[featuresAt].data = sfi0;
    else  // after the program, where the compiler puts it
        chunks.insert(chunks.begin() + programAt + 1, Chunk{fourcc("SFI0"), sfi0});
    r.bytecode = write_chunks(chunks);
    return r;
}

}  // namespace simqvfr::dxbc
