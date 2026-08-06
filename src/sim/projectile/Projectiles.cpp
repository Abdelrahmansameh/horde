// Wave 6 contract stub. Real implementation is owned by Wave 6A.
// Compiles and is safe to call; does nothing useful yet.
#include "sim/projectile/Projectiles.h"

#include "sim/CombatEvents.h"

#include <cassert>

namespace immune::sim {

void ProjectileBuffers::reserve(usize max_projectiles) {
    capacity_ = max_projectiles;
    pos_x.assign(max_projectiles, 0.0f);
    pos_y.assign(max_projectiles, 0.0f);
    vel_x.assign(max_projectiles, 0.0f);
    vel_y.assign(max_projectiles, 0.0f);
    damage.assign(max_projectiles, 0.0f);
    life.assign(max_projectiles, 0.0f);
    hit_radius.assign(max_projectiles, 0.0f);
    family_mask.assign(max_projectiles, 0u);
    flags.assign(max_projectiles, 0u);
    visual_id.assign(max_projectiles, 0u);
    owner.assign(max_projectiles, EntityId{});
    clear();
}

void ProjectileBuffers::clear() {
    count_ = 0;
    for (usize i = 0; i < capacity_; ++i) flags[i] = 0;
    assert_invariants();
}

bool ProjectileBuffers::spawn(const ProjectileSpawnParams& p) {
    if (count_ >= capacity_) return false;   // P3: never grows.
    const usize i = count_++;
    pos_x[i] = p.position.x;
    pos_y[i] = p.position.y;
    vel_x[i] = p.velocity.x;
    vel_y[i] = p.velocity.y;
    damage[i] = p.damage;
    life[i] = p.lifetime;
    hit_radius[i] = p.hit_radius;
    family_mask[i] = p.family_mask;
    flags[i] = static_cast<u8>((p.flags | projectile_flags::kAlive) &
                               ~projectile_flags::kPendingKill);
    visual_id[i] = p.visual_id;
    owner[i] = p.owner;
    return true;
}

void ProjectileBuffers::kill(usize index) {
    if (index >= count_) return;
    flags[index] |= projectile_flags::kPendingKill;
}

usize ProjectileBuffers::compact() {
    usize removed = 0;
    usize i = 0;
    while (i < count_) {
        if ((flags[i] & projectile_flags::kPendingKill) == 0) { ++i; continue; }
        const usize last = count_ - 1;
        if (i != last) {
            pos_x[i] = pos_x[last];
            pos_y[i] = pos_y[last];
            vel_x[i] = vel_x[last];
            vel_y[i] = vel_y[last];
            damage[i] = damage[last];
            life[i] = life[last];
            hit_radius[i] = hit_radius[last];
            family_mask[i] = family_mask[last];
            flags[i] = flags[last];
            visual_id[i] = visual_id[last];
            owner[i] = owner[last];
        }
        --count_;
        flags[count_] = 0;
        ++removed;
        // Do not advance i: the swapped-in round must be tested too.
    }
    assert_invariants();
    return removed;
}

void ProjectileBuffers::assert_invariants() const {
#ifndef NDEBUG
    assert(count_ <= capacity_);                                  // P3
    assert(pos_x.size() == capacity_ && pos_y.size() == capacity_);  // P2
    for (usize i = 0; i < count_; ++i) {
        assert((flags[i] & projectile_flags::kAlive) != 0);        // P1
    }
#endif
}

ProjectileStats ProjectileSystem::update(ProjectileBuffers&, ChaffBuffers&,
                                         const SpatialHash&, const Rect&, Rng&,
                                         f32, CombatEventSink*) {
    // Wave 6A implements integration + approximate cell-local collision here.
    last_ = ProjectileStats{};
    return last_;
}

} // namespace immune::sim
