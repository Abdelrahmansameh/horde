// sim/chaff/ReplicationSplit.h — authorable visual language for viral division.
//
// This table lives beside HitFlash because the split timer is per-agent cosmetic
// state in ChaffBuffers. Sim consumes duration/separation; render consumes the
// easing and seam values. Neither path may make gameplay decisions from it.
#pragma once

#include "core/Types.h"

namespace immune::sim {

struct ReplicationSplitParams {
    bool enabled = true;              ///< False skips the parent-to-daughters morph.
    f32 duration = 0.22f;             ///< Seconds from parent shell to two complete daughters.
    f32 separation_distance = 1.0f;   ///< Final centre gap, as a multiple of contact spacing.
    f32 pull_ease = 1.0f;             ///< >1 delays separation; <1 makes it snap outward early.
    f32 reveal_distance = 0.60f;      ///< Local-SDF distance that reveals each missing half.
    f32 reveal_ease = 1.0f;           ///< >1 keeps the seam longer; <1 completes daughters early.
    f32 seam_softness = 0.035f;       ///< Edge feather in local-SDF units; 0 gives a hard cut.
};

const ReplicationSplitParams& family_replication_split(PathogenFamily family);
void set_family_replication_split(PathogenFamily family, const ReplicationSplitParams& params);

} // namespace immune::sim
