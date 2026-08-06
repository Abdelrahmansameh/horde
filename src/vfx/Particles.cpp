// Wave 6 contract stub. Real implementation is owned by Wave 6B.
// Compiles and is safe to call; emits nothing yet.
#include "vfx/Particles.h"

namespace immune::vfx {

void ParticleSystem::init(usize capacity, u64 seed) {
    capacity_ = capacity;
    rng_state_ = seed ? seed : 0x9E3779B97F4A7C15ull;
    pos_x.assign(capacity, 0.0f);
    pos_y.assign(capacity, 0.0f);
    vel_x.assign(capacity, 0.0f);
    vel_y.assign(capacity, 0.0f);
    size_.assign(capacity, 0.0f);
    rot_.assign(capacity, 0.0f);
    spin_.assign(capacity, 0.0f);
    age_.assign(capacity, 0.0f);
    life_.assign(capacity, 0.0f);
    drag_.assign(capacity, 0.0f);
    buoy_.assign(capacity, 0.0f);
    end_x_.assign(capacity, 0.0f);
    end_y_.assign(capacity, 0.0f);
    seed_.assign(capacity, 0.0f);
    color_.assign(capacity, 0u);
    kind_.assign(capacity, 0u);
    blend_.assign(capacity, 0u);
    clear();
}

void ParticleSystem::shutdown() {
    pos_x.clear(); pos_y.clear(); vel_x.clear(); vel_y.clear();
    size_.clear(); rot_.clear(); spin_.clear(); age_.clear(); life_.clear();
    drag_.clear(); buoy_.clear(); end_x_.clear(); end_y_.clear(); seed_.clear();
    color_.clear(); kind_.clear(); blend_.clear();
    count_ = 0;
    capacity_ = 0;
}

void ParticleSystem::clear() {
    count_ = 0;
    stats_ = ParticleStats{};
}

bool ParticleSystem::spawn(const ParticleSpawnParams&) {
    // Wave 6B implements the real append here.
    return false;
}

void ParticleSystem::emit_for_event(const sim::CombatEvent&) {
    // Wave 6B authors the per-event, per-tower, per-tier bursts here. This is
    // where the art direction actually lives.
}

void ParticleSystem::emit_for_events(const sim::CombatEvent* events, usize count) {
    for (usize i = 0; i < count; ++i) emit_for_event(events[i]);
}

void ParticleSystem::update(f32, JobSystem*) {
    // Wave 6B implements integrate + fade + swap-remove compaction here.
}

void ParticleSystem::build_instances(BlendMode, std::vector<ParticleInstance>& out) const {
    out.clear();
}

} // namespace immune::vfx
