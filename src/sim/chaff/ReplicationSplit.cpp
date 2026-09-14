#include "sim/chaff/ReplicationSplit.h"

namespace immune::sim {
namespace {

struct SplitLookTable {
    ReplicationSplitParams look[kFamilyCount];
};

SplitLookTable& split_looks() {
    static SplitLookTable table;
    return table;
}

} // namespace

const ReplicationSplitParams& family_replication_split(PathogenFamily family) {
    const u32 i = static_cast<u32>(family);
    return split_looks().look[i < kFamilyCount ? i : 0u];
}

void set_family_replication_split(PathogenFamily family, const ReplicationSplitParams& params) {
    const u32 i = static_cast<u32>(family);
    if (i < kFamilyCount) split_looks().look[i] = params;
}

} // namespace immune::sim
