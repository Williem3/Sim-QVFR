// LMU's other cars, tracked for their motion vectors (docs/recon-lmu.md "Other cars' motion vectors"). LMU draws every
// object with its world matrix in one per-draw constant buffer it rewrites before each draw (lmu-ai_frame11731.rdc:
// 1,412 of 1,471 eye-pass draws, one buffer). Each car part (a body piece, a wheel: its own matrix) is a draw with a mesh
// of its own, so its draw's key (vertex buffer, index buffer, their offsets, index range, base vertex) names it from frame
// to frame (83 of 87 keys seen again 26 s later); a level of detail switch makes a new key. This keeps, per car part, the
// world matrix it was drawn with this frame and the frame before, and a slot for it (the GPU's copy, a later stage). Both
// eye passes draw a part: the frame's first draw of it writes this frame's matrix.
//
// The table is a flat one (open addressing, linear probing, at most half full; a deletion shifts its run back instead of
// leaving a tombstone): no allocation per key. In the game a lookup is two cache misses one after the other (the bucket,
// then the entry it names: ~60 ns a draw, run 27), so the draws are first matched by order: pass 0's draws as the frame
// before's pass 0 drew them, the other passes' as this frame's pass 0 did, each key compared in sequence (read straight
// through: no miss; a few draws ahead too, for those this pass leaves out), the table looked up only where the order
// differs. A slot's frame state is two bitmaps (drawn this frame, drawn the frame before: 512 bytes each, kept in the
// caches); its matrices are two cache lines by frame parity.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lmu {

struct CarDrawKey {
    const void* vb = nullptr;
    const void* ib = nullptr;
    uint32_t vbOffset = 0, ibOffset = 0;
    uint32_t start = 0, count = 0;  // the index range
    int32_t base = 0;               // the base vertex
    bool operator==(const CarDrawKey&) const = default;
};

struct CarMotionStats {
    uint64_t frames = 0;
    uint64_t draws = 0;          // draws given a matrix (identity ones included)
    uint64_t identityDraws = 0;  // the static world (vertices already in the world)
    uint64_t partDraws = 0;      // draws of objects with a matrix of their own (car parts, and the few others)
    uint64_t newKeys = 0;        // first seen, or seen again after a gap: no motion this frame
    uint64_t matched = 0;        // part draws whose key was drawn the frame before: a motion this frame
    uint64_t evicted = 0;        // keys not seen for a while, their slots freed
    uint64_t full = 0;           // part draws that found no free slot
    uint64_t inOrder = 0;        // part draws matched by order (no table lookup)
    uint64_t outOfOrder = 0;     // part draws looked up although the pass had an order to follow
    uint64_t noOrder = 0;        // part draws looked up in a pass without one (the first frame after a gap)
    size_t live = 0;             // keys held now
};

// Is this world matrix the identity (row-major)? The static world draws with it.
bool car_matrix_identity(const float (&m)[16]);

class CarMotion {
public:
    explicit CarMotion(size_t slots = 4096, uint64_t evictAfter = 120);
    // A new frame: the matrices drawn from now on are this frame's.
    void frame_begin(uint64_t frame);
    // A pass of the frame begins (0, 1 the eye passes; 2-3 the focus passes where they draw the scene): its draws follow
    // pass 0's order (pass 0: the frame before's, if the frame before had one; the others: this frame's). Without calls
    // to it every draw is looked up.
    void pass_begin(int pass);
    // A draw with world matrix `m` (row-major). The static world (identity): -1. A part: its slot, and `moved` true when
    // the frame before had it too (then previous() is that frame's matrix). -1 too when no slot is free.
    int draw(const CarDrawKey& key, const float (&m)[16], bool& moved);
    const float* current(int slot) const;
    const float* previous(int slot) const;  // null when the frame before didn't draw it
    CarMotionStats stats() const;
    // Each slot's motion this frame as 12 floats (a float4x3, rows: a world point [x y z 1] times it is where the part's
    // point was the frame before): inverse(this frame's world) x the frame before's, for the slots drawn this frame with
    // a frame before; the identity for every other slot (or a world matrix that can't be inverted). `out` gets 12 per
    // slot of the table. Returns how many slots have a motion.
    size_t motion_matrices(std::vector<float>& out) const;
    // The same, cheaper, for an upload each eye pass: only the slots first drawn this frame since the last call (with a
    // frame before) are written into `table` (12 floats a slot, sized to the table's slots by the caller); the rest keep
    // what they held (a slot without this frame's motion isn't read). A frame's eye passes draw the same parts, so the
    // second pass adds only the few the first didn't draw. Returns how many were written, [first, last] their range
    // (first > last: none).
    size_t update_motions(std::vector<float>& table, size_t& first, size_t& last);
    size_t slot_count() const { return entries_.size(); }

private:
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // padded to its alignment: meant
#endif
    struct alignas(64) Entry {  // a slot's, one cache line (its matrices in mats_, its frame state in the bitmaps)
        CarDrawKey key;
        uint64_t frame = 0;  // the last frame it was drawn in (the sweep's)
        uint32_t hash = 0;
        bool used = false;
    };
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    struct alignas(64) Matrix {  // one cache line
        float m[16] = {};
    };
    struct Bucket {
        uint32_t hash = 0;
        int32_t slot = -1;  // -1: empty
    };
    struct Ordered {  // a draw of pass 0, in its order
        CarDrawKey key;
        int32_t slot = -1;
    };
    static_assert(sizeof(Entry) == 64 && sizeof(Matrix) == 64);
    // A slot's matrix of frame `frame` (by its parity: this frame's and the frame before's are the two).
    const float* matrix(int slot, uint64_t frame) const { return mats_[static_cast<size_t>(slot) * 2 + (frame & 1)].m; }
    float* matrix(int slot, uint64_t frame) { return mats_[static_cast<size_t>(slot) * 2 + (frame & 1)].m; }
    static bool bit(const std::vector<uint64_t>& b, int s) { return (b[static_cast<size_t>(s) >> 6] >> (s & 63)) & 1; }
    static void set_bit(std::vector<uint64_t>& b, int s) { b[static_cast<size_t>(s) >> 6] |= uint64_t{1} << (s & 63); }
    static void clear_bit(std::vector<uint64_t>& b, int s) {
        b[static_cast<size_t>(s) >> 6] &= ~(uint64_t{1} << (s & 63));
    }
    static uint32_t hash_of(const CarDrawKey& k);
    bool motion_of(int slot, float* d) const;  // inverse(now) x before as 12 floats; false: not invertible
    int look_up(const CarDrawKey& key);        // the table: the key's slot, a new one, or -1 (full)
    void erase_bucket(size_t i);
    std::vector<int> pending_;  // slots first drawn this frame with a frame before, since the last update_motions
    std::vector<Entry> entries_;  // by slot
    std::vector<Matrix> mats_;    // by slot, two each (by frame parity)
    std::vector<uint64_t> drawnNow_, drawnBefore_;  // by slot: drawn this frame, drawn the frame before
    // Pass 0's order: [0] the last one finished (in frame orderFrame_[0]), [1] this frame's (being drawn, or done);
    // orderAt_[i][slot]: the slot's place in order_[i] + 1 (0: not in it).
    std::vector<Ordered> order_[2];
    std::vector<uint32_t> orderAt_[2];
    uint64_t orderFrame_[2] = {};
    const std::vector<Ordered>* follow_ = nullptr;  // the order this pass follows (null: none)
    const std::vector<uint32_t>* followAt_ = nullptr;
    size_t cursor_ = 0;       // the next draw expected in follow_
    static constexpr size_t kLookAhead = 4;  // follow_'s draws compared from the cursor before a lookup
    bool recording_ = false;  // this pass is pass 0: its draws go to order_[1]
    std::vector<Bucket> buckets_;
    size_t mask_ = 0;
    std::vector<int> free_;
    size_t live_ = 0;
    uint64_t frame_ = 0, evictAfter_ = 120;
    CarMotionStats stats_;
};

}  // namespace lmu
