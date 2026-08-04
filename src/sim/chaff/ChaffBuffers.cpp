// Wave 0 reference implementation of the SoA store itself.
// The *movement* kernel (ChaffSystem) is owned by Wave 1B; this file only
// provides storage semantics so the invariants are testable from day one.
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
    generation.assign(max_agents, 1u);
    clear();
}

void ChaffBuffers::clear() {
    count_ = 0;
    total_density_ = 0.0f;
    for (u32& c : family_counts_) c = 0;
    for (usize i = 0; i < capacity_; ++i) flags[i] = 0;
}

ChaffHandle ChaffBuffers::spawn(const ChaffSpawnParams& p) {
    if (count_ >= capacity_) return ChaffHandle{};
    const usize i = count_++;
    pos_x[i] = p.position.x;
    pos_y[i] = p.position.y;
    vel_x[i] = p.velocity.x;
    vel_y[i] = p.velocity.y;
    family[i] = static_cast<u8>(p.family);
    density[i] = p.density;
    flags[i] = static_cast<u8>(p.flags | chaff_flags::kAlive);
    total_density_ += p.density;
    ++family_counts_[static_cast<u32>(p.family)];
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

usize ChaffBuffers::compact() {
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
        if (family_counts_[fam] > 0) --family_counts_[fam];
        ++generation[i];
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
            // The moved agent keeps its own generation counter, so a handle
            // that named it must be resolved by scanning; see resolve().
            const u32 moved_gen = generation[last];
            generation[last] = generation[i];
            generation[i] = moved_gen;
        }
        --count_;
        flags[count_] = 0;
        // Do not advance i: the swapped-in agent must be tested too.
    }
    if (total_density_ < 0.0f) total_density_ = 0.0f;
    return removed;
}

usize ChaffBuffers::resolve(ChaffHandle handle) const {
    if (!handle.valid()) return npos;
    if (handle.index < count_ && generation[handle.index] == handle.generation &&
        (flags[handle.index] & chaff_flags::kAlive) != 0) {
        return handle.index;
    }
    for (usize i = 0; i < count_; ++i) {
        if (generation[i] == handle.generation) return i;
    }
    return npos;
}

} // namespace immune::sim
