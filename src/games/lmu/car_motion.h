// LMU's other cars, tracked for their motion vectors (docs/recon-lmu.md "Other cars' motion vectors"). LMU draws every
// object with its world matrix in one per-draw constant buffer it rewrites before each draw (lmu-ai_frame11731.rdc:
// 1,412 of 1,471 eye-pass draws, one buffer). Each car part (a body piece, a wheel: its own matrix) is a draw with a mesh
// of its own, so its draw's key (vertex buffer, index buffer, their offsets, index range, base vertex) names it from frame
// to frame (83 of 87 keys seen again 26 s later); a level of detail switch makes a new key. This keeps, per car part, the
// world matrix it was drawn with this frame and the frame before, and a slot for it (the GPU's copy, a later stage). Both
// eye passes draw a part: the frame's first draw of it moves this frame's matrix to the frame before's.
//
// The table is a flat one (open addressing, linear probing, at most half full; a deletion shifts its run back instead of
// leaving a tombstone): no allocation per key, a key's lookup one or two cache lines (run 21: ~3,400 draws a frame).
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
    size_t live = 0;             // keys held now
};

// Is this world matrix the identity (row-major)? The static world draws with it.
bool car_matrix_identity(const float (&m)[16]);

class CarMotion {
public:
    explicit CarMotion(size_t slots = 4096, uint64_t evictAfter = 120);
    // A new frame: the matrices drawn from now on are this frame's.
    void frame_begin(uint64_t frame);
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
    size_t slot_count() const { return entries_.size(); }

private:
    struct Entry {  // a slot's
        CarDrawKey key;
        uint64_t frame = 0;  // the last frame it was drawn in
        uint32_t hash = 0;
        bool used = false;
        bool moved = false;  // this frame's draw has the frame before's to go with it
        uint8_t now = 0;     // which of m is this frame's (the other the frame before's)
        float m[2][16] = {};
    };
    struct Bucket {
        uint32_t hash = 0;
        int32_t slot = -1;  // -1: empty
    };
    static uint32_t hash_of(const CarDrawKey& k);
    void erase_bucket(size_t i);
    std::vector<Entry> entries_;  // by slot
    std::vector<Bucket> buckets_;
    size_t mask_ = 0;
    std::vector<int> free_;
    size_t live_ = 0;
    uint64_t frame_ = 0, evictAfter_ = 120;
    CarMotionStats stats_;
};

}  // namespace lmu
