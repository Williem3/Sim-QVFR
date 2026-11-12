// LMU's other cars, tracked for their motion vectors (car_motion.h).
#include "car_motion.h"

#include <algorithm>
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

CarMotion::CarMotion(size_t slots, uint64_t evictAfter)
    : entries_(slots), mats_(slots * 2), drawnNow_((slots + 63) / 64), drawnBefore_((slots + 63) / 64),
      evictAfter_((std::max)(evictAfter, uint64_t{1})) {  // at least 1: a key the frame before drew stays (its order)
    size_t buckets = 16;
    while (buckets < 2 * slots)  // at most half full
        buckets *= 2;
    buckets_.resize(buckets);
    mask_ = buckets - 1;
    free_.reserve(slots);
    for (size_t i = slots; i > 0; --i)
        free_.push_back(static_cast<int>(i - 1));
    for (int i = 0; i < 2; ++i) {
        order_[i].reserve(slots);
        orderAt_[i].assign(slots, 0);
    }
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
    const bool next = frame == frame_ + 1;
    frame_ = frame;
    ++stats_.frames;
    pending_.clear();  // a frame's motions are its own
    // The frame state: this frame's draws become the frame before's (none if the frame before wasn't the last one).
    if (next)
        drawnBefore_.swap(drawnNow_);
    else
        std::fill(drawnBefore_.begin(), drawnBefore_.end(), 0);
    std::fill(drawnNow_.begin(), drawnNow_.end(), 0);
    // Pass 0's order finished in the frame just ended: the one this frame's pass 0 follows.
    if (orderFrame_[1] + 1 == frame && !order_[1].empty()) {
        std::swap(order_[0], order_[1]);
        std::swap(orderAt_[0], orderAt_[1]);
        std::swap(orderFrame_[0], orderFrame_[1]);
    }
    follow_ = nullptr;
    followAt_ = nullptr;
    recording_ = false;
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

void CarMotion::pass_begin(int pass) {
    cursor_ = 0;
    if (pass == 0) {  // its draws make this frame's order, following the frame before's
        for (const Ordered& o : order_[1])
            orderAt_[1][o.slot] = 0;
        order_[1].clear();
        orderFrame_[1] = frame_;
        recording_ = true;
        const bool before = orderFrame_[0] + 1 == frame_ && !order_[0].empty();
        follow_ = before ? &order_[0] : nullptr;
        followAt_ = before ? &orderAt_[0] : nullptr;
        return;
    }
    recording_ = false;  // the other passes follow this frame's pass 0
    const bool now = orderFrame_[1] == frame_ && !order_[1].empty();
    follow_ = now ? &order_[1] : nullptr;
    followAt_ = now ? &orderAt_[1] : nullptr;
}

int CarMotion::look_up(const CarDrawKey& key) {
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
            e.frame = frame_;
            clear_bit(drawnNow_, s);  // a new key: no frame state of the slot's last one
            clear_bit(drawnBefore_, s);
            b = {h, s};
            ++live_;
            return s;
        }
        if (b.hash == h && entries_[b.slot].key == key)
            return b.slot;
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
    int s = -1;
    if (follow_) {  // where the order has it, or a few draws on (the ones this pass doesn't draw: culled, its own few)
        const size_t end = (std::min)(cursor_ + kLookAhead, follow_->size());
        for (size_t j = cursor_; j < end; ++j)
            if ((*follow_)[j].key == key) {
                s = (*follow_)[j].slot;
                cursor_ = j + 1;
                ++stats_.inOrder;
                break;
            }
    }
    if (s < 0) {
        ++(follow_ ? stats_.outOfOrder : stats_.noOrder);
        s = look_up(key);
        if (s < 0)
            return -1;
        if (followAt_) {  // back in step: the order's draws after this one come next
            const uint32_t at = (*followAt_)[s];
            if (at)
                cursor_ = at;
        }
    }
    if (recording_) {
        order_[1].push_back({key, s});
        orderAt_[1][s] = static_cast<uint32_t>(order_[1].size());
    }
    if (!bit(drawnNow_, s)) {  // the frame's first draw of it: this frame's matrix (the second eye's is the same)
        set_bit(drawnNow_, s);
        entries_[s].frame = frame_;
        std::memcpy(matrix(s, frame_), m, sizeof(Matrix::m));
        if (bit(drawnBefore_, s))
            pending_.push_back(s);  // its motion due at the next update_motions
        else
            ++stats_.newKeys;
    }
    moved = bit(drawnBefore_, s);
    stats_.matched += moved ? 1 : 0;
    return s;
}

const float* CarMotion::current(int slot) const {
    if (slot < 0 || static_cast<size_t>(slot) >= entries_.size() || !entries_[slot].used)
        return nullptr;
    return matrix(slot, entries_[slot].frame);
}

const float* CarMotion::previous(int slot) const {
    if (slot < 0 || static_cast<size_t>(slot) >= entries_.size())
        return nullptr;
    const Entry& e = entries_[slot];
    return e.used && e.frame == frame_ && bit(drawnBefore_, slot) ? matrix(slot, frame_ - 1) : nullptr;
}

bool CarMotion::motion_of(int slot, float* d) const {
    static constexpr float kIdentity[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    std::memcpy(d, kIdentity, sizeof(kIdentity));
    const float* w = matrix(slot, frame_);
    const float* p = matrix(slot, frame_ - 1);
    // W = [A 0; t 1] (row vectors: x_world = x_object A + t); W^-1 = [A^-1 0; -t A^-1 1]; D = W^-1 W_prev.
    const double a[3][3] = {{w[0], w[1], w[2]}, {w[4], w[5], w[6]}, {w[8], w[9], w[10]}};
    const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                       a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                       a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (!(std::fabs(det) > 1e-12))
        return false;
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
    return true;
}

size_t CarMotion::motion_matrices(std::vector<float>& out) const {
    static constexpr float kIdentity[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    out.resize(entries_.size() * 12);
    size_t moving = 0;
    for (size_t s = 0; s < entries_.size(); ++s) {
        float* d = out.data() + s * 12;
        const Entry& e = entries_[s];
        if (!e.used || e.frame != frame_ || !bit(drawnBefore_, static_cast<int>(s))) {
            std::memcpy(d, kIdentity, sizeof(kIdentity));
            continue;
        }
        moving += motion_of(static_cast<int>(s), d) ? 1 : 0;
    }
    return moving;
}

size_t CarMotion::update_motions(std::vector<float>& table, size_t& first, size_t& last) {
    first = entries_.size();
    last = 0;
    size_t written = 0;
    if (table.size() < entries_.size() * 12)
        table.resize(entries_.size() * 12);
    for (const int s : pending_) {
        const Entry& e = entries_[s];
        if (!e.used || e.frame != frame_ || !bit(drawnBefore_, s))
            continue;
        motion_of(s, table.data() + static_cast<size_t>(s) * 12);  // the identity where not invertible
        first = (std::min)(first, static_cast<size_t>(s));
        last = (std::max)(last, static_cast<size_t>(s));
        ++written;
    }
    pending_.clear();
    return written;
}

CarMotionStats CarMotion::stats() const {
    CarMotionStats s = stats_;
    s.live = live_;
    return s;
}

}  // namespace lmu
