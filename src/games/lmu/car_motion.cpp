// LMU's other cars, tracked for their motion vectors (car_motion.h).
#include "car_motion.h"

#include <cmath>
#include <cstring>

namespace lmu {

bool car_matrix_identity(const float (&m)[16]) {
    // The translation first: an object's (a car part's) is rarely zero, so most of them stop there.
    static constexpr int kOrder[16] = {12, 13, 14, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 15};
    for (const int i : kOrder)
        if (std::fabs(m[i] - (i % 5 == 0 ? 1.0f : 0.0f)) > 1e-6f)
            return false;
    return true;
}

uint32_t CarMotion::hash_of(const CarDrawKey& k) {
    uint64_t h = reinterpret_cast<uintptr_t>(k.vb) * 0x9E3779B97F4A7C15ull;
    const auto mix = [&](uint64_t v) { h = (h ^ v) * 0xBF58476D1CE4E5B9ull + (h >> 29); };
    mix(reinterpret_cast<uintptr_t>(k.ib));
    mix((static_cast<uint64_t>(k.vbOffset) << 32) | k.ibOffset);
    mix((static_cast<uint64_t>(k.start) << 32) | k.count);
    mix(static_cast<uint32_t>(k.base));
    h ^= h >> 31;
    return static_cast<uint32_t>(h ^ (h >> 32));
}

CarMotion::CarMotion(size_t slots, uint64_t evictAfter) : entries_(slots), evictAfter_(evictAfter) {
    size_t buckets = 16;
    while (buckets < 2 * slots)  // at most half full
        buckets *= 2;
    buckets_.resize(buckets);
    mask_ = buckets - 1;
    free_.reserve(slots);
    for (size_t i = slots; i > 0; --i)
        free_.push_back(static_cast<int>(i - 1));
}

// Bucket i emptied: the entries after it in its run that would no longer be found from their home move back.
void CarMotion::erase_bucket(size_t i) {
    for (size_t j = (i + 1) & mask_; buckets_[j].slot >= 0; j = (j + 1) & mask_) {
        const size_t home = buckets_[j].hash & mask_;
        const bool stays = i <= j ? (i < home && home <= j) : (i < home || home <= j);  // home cyclically in (i, j]
        if (!stays) {
            buckets_[i] = buckets_[j];
            i = j;
        }
    }
    buckets_[i] = Bucket{};
}

void CarMotion::frame_begin(uint64_t frame) {
    frame_ = frame;
    ++stats_.frames;
    if (frame % 64 != 0)  // now and then: the keys not drawn for a while go (a car out of sight, a level of detail left)
        return;
    for (size_t s = 0; s < entries_.size(); ++s) {
        Entry& e = entries_[s];
        if (!e.used || frame <= e.frame + evictAfter_)
            continue;
        for (size_t i = e.hash & mask_;; i = (i + 1) & mask_)  // its bucket (it is there: every used entry has one)
            if (buckets_[i].slot == static_cast<int32_t>(s)) {
                erase_bucket(i);
                break;
            }
        e.used = false;
        free_.push_back(static_cast<int>(s));
        --live_;
        ++stats_.evicted;
    }
}

int CarMotion::draw(const CarDrawKey& key, const float (&m)[16], bool& moved) {
    moved = false;
    ++stats_.draws;
    if (car_matrix_identity(m)) {
        ++stats_.identityDraws;
        return -1;
    }
    ++stats_.partDraws;
    const uint32_t h = hash_of(key);
    for (size_t i = h & mask_;; i = (i + 1) & mask_) {
        Bucket& b = buckets_[i];
        if (b.slot < 0) {  // not held: a slot for it, here
            if (free_.empty()) {
                ++stats_.full;
                return -1;
            }
            const int s = free_.back();
            free_.pop_back();
            Entry& e = entries_[s];
            e.key = key;
            e.hash = h;
            e.used = true;
            e.moved = false;
            e.now = 0;
            e.frame = frame_;
            std::memcpy(e.m[0], m, sizeof(e.m[0]));
            b = {h, s};
            ++live_;
            ++stats_.newKeys;
            return s;
        }
        if (b.hash != h)
            continue;
        Entry& e = entries_[b.slot];
        if (!(e.key == key))
            continue;
        if (e.frame != frame_) {  // the frame's first draw of it: the matrix it had becomes the frame before's
            e.moved = e.frame + 1 == frame_;
            e.now ^= 1;
            e.frame = frame_;
            if (!e.moved)
                ++stats_.newKeys;
        }
        std::memcpy(e.m[e.now], m, sizeof(e.m[0]));  // the second eye's draw: the same matrix (the frame's object)
        moved = e.moved;
        stats_.matched += moved ? 1 : 0;
        return b.slot;
    }
}

const float* CarMotion::current(int slot) const {
    if (slot < 0 || static_cast<size_t>(slot) >= entries_.size() || !entries_[slot].used)
        return nullptr;
    return entries_[slot].m[entries_[slot].now];
}

const float* CarMotion::previous(int slot) const {
    if (slot < 0 || static_cast<size_t>(slot) >= entries_.size())
        return nullptr;
    const Entry& e = entries_[slot];
    return e.used && e.moved ? e.m[e.now ^ 1] : nullptr;
}

size_t CarMotion::motion_matrices(std::vector<float>& out) const {
    static constexpr float kIdentity[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    out.resize(entries_.size() * 12);
    size_t moving = 0;
    for (size_t s = 0; s < entries_.size(); ++s) {
        float* d = out.data() + s * 12;
        std::memcpy(d, kIdentity, sizeof(kIdentity));
        const Entry& e = entries_[s];
        if (!e.used || !e.moved || e.frame != frame_)
            continue;
        const float* w = e.m[e.now];
        const float* p = e.m[e.now ^ 1];
        // W = [A 0; t 1] (row vectors: x_world = x_object A + t); W^-1 = [A^-1 0; -t A^-1 1]; D = W^-1 W_prev.
        const double a[3][3] = {{w[0], w[1], w[2]}, {w[4], w[5], w[6]}, {w[8], w[9], w[10]}};
        const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                           a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                           a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
        if (!(std::fabs(det) > 1e-12))
            continue;
        const double inv[3][3] = {
            {(a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det, (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det,
             (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det},
            {(a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det, (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det,
             (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det},
            {(a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det, (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det,
             (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det}};
        const double t[3] = {w[12], w[13], w[14]};
        double ti[3];  // -t A^-1
        for (int c = 0; c < 3; ++c)
            ti[c] = -(t[0] * inv[0][c] + t[1] * inv[1][c] + t[2] * inv[2][c]);
        const double ap[3][3] = {{p[0], p[1], p[2]}, {p[4], p[5], p[6]}, {p[8], p[9], p[10]}};
        const double tp[3] = {p[12], p[13], p[14]};
        for (int r = 0; r < 3; ++r)  // rows 0-2: A^-1 A_prev
            for (int c = 0; c < 3; ++c)
                d[r * 3 + c] = static_cast<float>(inv[r][0] * ap[0][c] + inv[r][1] * ap[1][c] + inv[r][2] * ap[2][c]);
        for (int c = 0; c < 3; ++c)  // row 3: -t A^-1 A_prev + t_prev
            d[9 + c] = static_cast<float>(ti[0] * ap[0][c] + ti[1] * ap[1][c] + ti[2] * ap[2][c] + tp[c]);
        ++moving;
    }
    return moving;
}

CarMotionStats CarMotion::stats() const {
    CarMotionStats s = stats_;
    s.live = live_;
    return s;
}

}  // namespace lmu
