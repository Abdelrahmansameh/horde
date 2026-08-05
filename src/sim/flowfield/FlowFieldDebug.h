// sim/flowfield/FlowFieldDebug.h — binary dump of a baked field for offline
// visualization by tools/preview_level.py. Owner: Wave 1A.
//
// This project ships no binary assets and the renderer (Wave 1C) is not up yet,
// so the only way to *see* a flow field is to dump it and draw it out of process.
// Header-only for the same reason as TissueRaster.h: the sim CMake source list
// is orchestrator-owned.
//
// Format ("IMFF", little-endian, version 1):
//   char[4]  magic  = "IMFF"
//   u32      version = 1
//   i32      width, height
//   f32      cell_size
//   f32      origin_x, origin_y
//   u8   [w*h]  walkable
//   f32  [w*h]  mask cost multiplier
//   f32  [w*h]  signed distance   (0 if no DistanceField supplied)
//   f32  [w*h]  cost-to-goal      (non-finite written as -1.0)
//   f32  [w*h*2] direction x,y
#pragma once

#include "sim/flowfield/FlowField.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace immune::sim {

/// Writes `field` (and the mask it was baked from) to `path`. Returns false on
/// any I/O failure. Debug/tooling only — never called from a sim tick.
inline bool dump_flow_field(const std::string& path, const TissueMask& mask, const FlowField& field,
                            const DistanceField* sdf = nullptr) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    const i32 w = mask.width();
    const i32 h = mask.height();
    const usize n = static_cast<usize>(w) * static_cast<usize>(h);
    const f32 cs = mask.cell_size();
    const Vec2 org = mask.world_origin();
    const u32 version = 1;

    bool ok = true;
    auto put = [&](const void* p, usize bytes) {
        if (ok && bytes > 0) ok = std::fwrite(p, 1, bytes, f) == bytes;
    };

    put("IMFF", 4);
    put(&version, sizeof(version));
    put(&w, sizeof(w));
    put(&h, sizeof(h));
    put(&cs, sizeof(cs));
    put(&org.x, sizeof(f32));
    put(&org.y, sizeof(f32));
    put(mask.walkable_data(), n * sizeof(u8));
    put(mask.cost_data(), n * sizeof(f32));

    std::vector<f32> scratch(n, 0.0f);
    if (sdf && sdf->width() == w && sdf->height() == h) {
        for (usize i = 0; i < n; ++i) scratch[i] = sdf->data()[i];
    }
    put(scratch.data(), n * sizeof(f32));

    const f32* costs = field.costs();
    for (usize i = 0; i < n; ++i) {
        const f32 c = costs[i];
        scratch[i] = std::isfinite(c) ? c : -1.0f;
    }
    put(scratch.data(), n * sizeof(f32));

    std::vector<f32> dirs(n * 2, 0.0f);
    const Vec2* d = field.directions();
    for (usize i = 0; i < n; ++i) {
        dirs[i * 2 + 0] = d[i].x;
        dirs[i * 2 + 1] = d[i].y;
    }
    put(dirs.data(), n * 2 * sizeof(f32));

    std::fclose(f);
    return ok;
}

} // namespace immune::sim
