// SoA chaff storage. Owner: Wave 1B.
//
// GENERATIONS
// `generation` is drawn from a monotonically increasing per-world counter rather
// than being a per-slot "times reused" count. The Wave 0 reference used the
// latter, which made generations collide across slots (every slot started at 1),
// so resolve()'s fallback scan could return the wrong agent. A monotonic counter
// makes a generation a globally unique agent id for the lifetime of a level,
// which is exactly what a handle needs. The value still only ever increases and
// a retired slot's id is never reissued, so the header's contract holds.
#include "sim/chaff/ChaffBuffers.h"

#include <cassert>

namespace immune::sim {

void ChaffBuffers::reserve(usize max_agents) {
    capacity_ = max_agents;
    pos_x.assign(max_agents, 0.0f);
    pos_y.assign(max_agents, 0.0f);
    vel_x.assign(max_agents, 0.0f);
    vel_y.assign(max_agents, 0.0f);
    family.assign(max_agents, 0u);
    density.assign(max_agents, 0.0f);
    flags.assign(max_agents, 0u);
    generation.assign(max_agents, 0u);
    next_generation_ = 1u;   // 0 is the reserved "invalid handle" generation.
    clear();
}

void ChaffBuffers::clear() {
    count_ = 0;
    total_density_ = 0.0f;
    for (u32& c : family_counts_) c = 0;
    for (u64& c : spawned_by_family_) c = 0;
    for (usize i = 0; i < capacity_; ++i) {
        flags[i] = 0;
        generation[i] = 0;
    }
    // Deliberately NOT resetting next_generation_: handles taken before a clear()
    // must not silently resolve to a freshly spawned agent.
    assert_invariants();
}

ChaffHandle ChaffBuffers::spawn(const ChaffSpawnParams& p) {
    if (count_ >= capacity_) return ChaffHandle{};   // I3: never grows.
    const usize i = count_++;
    pos_x[i] = p.position.x;
    pos_y[i] = p.position.y;
    vel_x[i] = p.velocity.x;
    vel_y[i] = p.velocity.y;
    family[i] = static_cast<u8>(p.family);
    density[i] = p.density;
    flags[i] = static_cast<u8>((p.flags | chaff_flags::kAlive) & ~chaff_flags::kPendingKill);
    generation[i] = next_generation_++;
    if (next_generation_ == 0u) next_generation_ = 1u;   // never hand out 0
    total_density_ += p.density;
    ++family_counts_[static_cast<u32>(p.family)];
    ++spawned_by_family_[static_cast<u32>(p.family)];
    return ChaffHandle{static_cast<u32>(i), generation[i]};
}

void ChaffBuffers::kill(usize index) {
    if (index >= count_) return;
    flags[index] |= chaff_flags::kPendingKill;
}

void ChaffBuffers::apply_density_loss(usize index, f32 amount) {
    if (index >= count_ || amount <= 0.0f) return;
    const f32 before = density[index];
    const f32 removed = amount < before ? amount : before;
    density[index] = before - removed;
    total_density_ -= removed;
    if (density[index] <= 0.0f) flags[index] |= chaff_flags::kPendingKill;
}

usize ChaffBuffers::compact(u32* removed_by_family) {
    usize removed = 0;
    usize i = 0;
    while (i < count_) {
        if ((flags[i] & chaff_flags::kPendingKill) == 0) {
            ++i;
            continue;
        }
        // Retire slot i.
        total_density_ -= density[i];
        const u32 fam = family[i];
        if (fam < kFamilyCount) {
            if (family_counts_[fam] > 0) --family_counts_[fam];
            if (removed_by_family != nullptr) ++removed_by_family[fam];
        }
        ++removed;

        const usize last = count_ - 1;
        if (i != last) {
            pos_x[i] = pos_x[last];
            pos_y[i] = pos_y[last];
            vel_x[i] = vel_x[last];
            vel_y[i] = vel_y[last];
            family[i] = family[last];
            density[i] = density[last];
            flags[i] = flags[last];
            // The surviving agent carries its unique generation to its new slot,
            // which is what keeps its ChaffHandle resolvable across compaction.
            generation[i] = generation[last];
        }
        --count_;
        flags[count_] = 0;
        generation[count_] = 0;   // the retired id is never reissued
        // Do not advance i: the swapped-in agent must be tested too.
    }
    if (total_density_ < 0.0f) total_density_ = 0.0f;
    assert_invariants();
    return removed;
}

usize ChaffBuffers::resolve(ChaffHandle handle) const {
    if (!handle.valid()) return npos;
    // Fast path: the handle still names its own slot (true until a compaction
    // moves the agent). Generations are globally unique, so this cannot alias.
    if (handle.index < count_ && generation[handle.index] == handle.generation) {
        return handle.index;
    }
    // Slow path: the agent moved. O(count), but chaff is fought as a mass and
    // essentially nothing holds a handle across ticks (see ChaffBuffers.h).
    for (usize i = 0; i < count_; ++i) {
        if (generation[i] == handle.generation) return i;
    }
    return npos;
}

void ChaffBuffers::assert_invariants() const {
#ifndef NDEBUG
    // I3: count never exceeds capacity, and no stream ever grew.
    assert(count_ <= capacity_);
    // I2: every stream is parallel and at least `count` long.
    assert(pos_x.size() == capacity_ && pos_y.size() == capacity_);
    assert(vel_x.size() == capacity_ && vel_y.size() == capacity_);
    assert(family.size() == capacity_ && density.size() == capacity_);
    assert(flags.size() == capacity_ && generation.size() == capacity_);
    for (usize i = 0; i < count_; ++i) {
        assert((flags[i] & chaff_flags::kAlive) != 0);               // I1
        assert((flags[i] & chaff_flags::kPendingKill) == 0);         // post-compact
        assert(density[i] > 0.0f);                                   // I4
        assert(generation[i] != 0u);
    }
#endif
}

} // namespace immune::sim
