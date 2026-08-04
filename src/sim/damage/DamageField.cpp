// Wave 0 stub. Implementation is owned by Wave 2A.
#include "sim/damage/DamageField.h"

#include "core/Rng.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/spatial/SpatialHash.h"

namespace immune::sim {

u32 DamageSystem::submit(const DamageField& field) {
    fields_.push_back(field);
    return static_cast<u32>(fields_.size() - 1);
}

void DamageSystem::clear_transient(f32 dt) {
    usize write = 0;
    for (usize i = 0; i < fields_.size(); ++i) {
        DamageField f = fields_[i];
        if (f.lifetime <= 0.0f) continue; // persistent: owner re-submits
        f.lifetime -= dt;
        if (f.lifetime <= 0.0f) continue; // expired
        fields_[write++] = f;
    }
    fields_.resize(write);
}

DamageStats DamageSystem::apply(ChaffBuffers&, const SpatialHash&, Rng&, f32) {
    // Wave 2A: for each field, query overlapped cells, test each agent against
    // the exact shape + family_mask, then thin per `mode_`.
    DamageStats stats;
    stats.fields_evaluated = static_cast<u32>(fields_.size());
    return stats;
}

f32 DamageSystem::measure_density(const ChaffBuffers&, const SpatialHash&,
                                  const DamageField&) const {
    return 0.0f;
}

} // namespace immune::sim
