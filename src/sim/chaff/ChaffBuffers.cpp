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
    squad_id.assign(max_agents, kNoSquad);
    hit_flash.assign(max_agents, 0.0f);
    replication_pulse.assign(max_agents, 0.0f);
    replication_origin_x.assign(max_agents, 0.0f);
    replication_origin_y.assign(max_agents, 0.0f);
    slow_remaining.assign(max_agents, 0.0f);
    slow_factor.assign(max_agents, chaff_flags::kDefaultSlowFactor);
    host_index.assign(max_agents, 0u);
    host_generation.assign(max_agents, 0u);
    host_kind.assign(max_agents, host_kind::kNone);
    latch_heading.assign(max_agents, 0.0f);
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
        squad_id[i] = kNoSquad;
        hit_flash[i] = 0.0f;
        replication_pulse[i] = 0.0f;
        replication_origin_x[i] = 0.0f;
        replication_origin_y[i] = 0.0f;
        slow_remaining[i] = 0.0f;
        slow_factor[i] = chaff_flags::kDefaultSlowFactor;
        host_index[i] = 0u;
        host_generation[i] = 0u;
        host_kind[i] = host_kind::kNone;
        latch_heading[i] = 0.0f;
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
    squad_id[i] = p.squad_id;
    // A recycled slot can still be carrying the flash of whatever died in it.
    hit_flash[i] = 0.0f;
    replication_pulse[i] = p.replication_pulse;
    replication_origin_x[i] = p.replication_origin.x;
    replication_origin_y[i] = p.replication_origin.y;
    // A spawn flagged kSlowed by its caller has no zone behind it: it slows at
    // the default factor, and with no timer the first zone upkeep clears it.
    slow_remaining[i] = 0.0f;
    slow_factor[i] = chaff_flags::kDefaultSlowFactor;
    // Nothing spawns already latched: a host is something the hostile pass
    // finds, never something a spawner hands out. The flag is stripped so a
    // caller cannot create an agent the kernel will freeze and nothing will
    // ever move.
    flags[i] &= static_cast<u8>(~chaff_flags::kLatched);
    host_index[i] = 0u;
    host_generation[i] = 0u;
    host_kind[i] = host_kind::kNone;
    latch_heading[i] = 0.0f;
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

    // Hit feedback. Raised off `removed` rather than off `amount`, so a hit
    // that was mostly overkill flashes for the part that actually landed --
    // the same reason DamageField's apply_and_record reads the clamped effect
    // for its accounting. Purely cosmetic and unhashed; see HitFlash.h.
    const u8 fam = family[index];
    const HitFlashParams& flash =
        family_hit_flash(static_cast<PathogenFamily>(fam < kFamilyCount ? fam : 0u));
    const f32 incoming = hit_flash_intensity(flash, before, removed);
    if (incoming > 0.0f) {
        hit_flash[index] = hit_flash_combine(flash.retrigger, hit_flash[index], incoming);
    }
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
            squad_id[i] = squad_id[last];
            hit_flash[i] = hit_flash[last];
            replication_pulse[i] = replication_pulse[last];
            replication_origin_x[i] = replication_origin_x[last];
            replication_origin_y[i] = replication_origin_y[last];
            slow_remaining[i] = slow_remaining[last];
            slow_factor[i] = slow_factor[last];
            host_index[i] = host_index[last];
            host_generation[i] = host_generation[last];
            host_kind[i] = host_kind[last];
            latch_heading[i] = latch_heading[last];
        }
        --count_;
        flags[count_] = 0;
        generation[count_] = 0;   // the retired id is never reissued
        squad_id[count_] = kNoSquad;
        hit_flash[count_] = 0.0f;
        replication_pulse[count_] = 0.0f;
        replication_origin_x[count_] = 0.0f;
        replication_origin_y[count_] = 0.0f;
        slow_remaining[count_] = 0.0f;
        slow_factor[count_] = chaff_flags::kDefaultSlowFactor;
        host_index[count_] = 0u;
        host_generation[count_] = 0u;
        host_kind[count_] = host_kind::kNone;
        latch_heading[count_] = 0.0f;
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
    assert(squad_id.size() == capacity_);
    assert(hit_flash.size() == capacity_);
    assert(replication_pulse.size() == capacity_);
    assert(replication_origin_x.size() == capacity_ && replication_origin_y.size() == capacity_);
    assert(slow_remaining.size() == capacity_ && slow_factor.size() == capacity_);
    assert(host_index.size() == capacity_ && host_generation.size() == capacity_);
    assert(host_kind.size() == capacity_);
    assert(latch_heading.size() == capacity_);
    for (usize i = 0; i < count_; ++i) {
        assert((flags[i] & chaff_flags::kAlive) != 0);               // I1
        assert((flags[i] & chaff_flags::kPendingKill) == 0);         // post-compact
        assert(density[i] > 0.0f);                                   // I4
        assert(generation[i] != 0u);
    }
#endif
}

} // namespace immune::sim
