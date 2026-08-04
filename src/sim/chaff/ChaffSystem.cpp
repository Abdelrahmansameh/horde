// Wave 0 stub. Implementation is owned by Wave 1B.
#include "sim/chaff/ChaffSystem.h"

#include "core/JobSystem.h"
#include "core/Rng.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/flowfield/FlowField.h"
#include "sim/spatial/SpatialHash.h"

namespace immune::sim {

ChaffUpdateStats ChaffSystem::update(ChaffBuffers&, const FlowField&, const SpatialHash&,
                                     Rng&, f32, JobSystem*) {
    // Wave 1B: parallel_for over [0, count) doing flow sample + separation +
    // integrate + bounds/goal despawn + replication.
    return ChaffUpdateStats{};
}

u32 ChaffSystem::spawn_burst(ChaffBuffers& buffers, PathogenFamily family, Vec2 portal,
                             f32 portal_radius, u32 count, Rng& rng) const {
    const ChaffFamilyParams& fp = tuning_.family[static_cast<u32>(family)];
    u32 spawned = 0;
    for (u32 i = 0; i < count; ++i) {
        if (buffers.full()) break;
        ChaffSpawnParams p;
        p.position = portal + rng.unit_disc() * portal_radius;
        p.family = family;
        p.density = fp.base_density > 0.0f ? fp.base_density : 1.0f;
        if (buffers.spawn(p).valid()) ++spawned;
    }
    return spawned;
}

} // namespace immune::sim
