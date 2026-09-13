// The hit flash, end to end.
//
// The effect is a chain of four links, and each one is a separate way for it to
// silently do nothing:
//
//   1. DAMAGE RAISES IT. Every damage source in the game goes through
//      ChaffBuffers::apply_density_loss, so the flash has to appear no matter
//      which one landed the hit. That is the property that made the choke point
//      the right home for it, and it is the one that would rot first.
//   2. IT FADES, AND ONLY THE MOVEMENT KERNEL FADES IT. A flash that is set but
//      never cleared is a permanently white horde.
//   3. IT SURVIVES COMPACTION. Agents are swap-removed under the renderer every
//      tick; a flash that stays with the SLOT rather than with the AGENT lights
//      up whoever happens to be moved into it.
//   4. IT REACHES THE GPU. The batcher packs it into the last free byte of the
//      instance flags word, which is the one link no sim-side assertion covers.
//
// And the fifth thing, which is not a link but a contract: none of it may move
// SimWorld::state_hash(). See sim/chaff/HitFlash.h.
//
// What is deliberately NOT asserted: the shipped numbers. `strength`, `curve`
// and friends are art direction, and they were put in enemies.json precisely so
// they can move without breaking a build. Every test below either sets the
// params it depends on or asserts a relationship that holds for any of them.
#include "sim/chaff/HitFlash.h"

#include "platform/Window.h"
#include "render/Camera.h"
#include "render/ChaffBatcher.h"
#include "render/Renderer.h"
#include "sim/CombatEvents.h"
#include "sim/SimWorld.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/chaff/ChaffSystem.h"
#include "sim/damage/DamageField.h"
#include "sim/flowfield/FlowField.h"
#include "sim/spatial/SpatialHash.h"
#include "sim/squad/Squads.h"

#include "core/Rng.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace immune;

namespace {

/// Installs `params` on every family and puts the shipped table back when it
/// goes out of scope. The table is process-wide (see HitFlash.h for why), so a
/// test that leaves it modified silently retunes every test that runs after it.
class ScopedFlash {
public:
    explicit ScopedFlash(const sim::HitFlashParams& params) {
        for (u32 f = 0; f < kFamilyCount; ++f) {
            const auto family = static_cast<PathogenFamily>(f);
            saved_[f] = sim::family_hit_flash(family);
            sim::set_family_hit_flash(family, params);
        }
    }
    ~ScopedFlash() {
        for (u32 f = 0; f < kFamilyCount; ++f) {
            sim::set_family_hit_flash(static_cast<PathogenFamily>(f), saved_[f]);
        }
    }
    ScopedFlash(const ScopedFlash&) = delete;
    ScopedFlash& operator=(const ScopedFlash&) = delete;

private:
    sim::HitFlashParams saved_[kFamilyCount];
};

/// A flash whose numbers are round, so an expectation can be written as an
/// exact fraction instead of as a tolerance around the shipped art.
sim::HitFlashParams plain_flash() {
    sim::HitFlashParams p;
    p.enabled = true;
    p.retrigger = sim::HitFlashRetrigger::Highest;
    p.color = Vec4{1.0f, 1.0f, 1.0f, 1.0f};
    p.strength = 1.0f;
    p.duration = 0.25f;     // 15 ticks at 60 Hz
    p.curve = 1.0f;         // linear, so stored == drawn
    p.gain = 1.0f;
    p.min_fraction = 0.0f;
    p.scale_punch = 0.0f;
    return p;
}

sim::ChaffBuffers make_buffers(usize capacity, PathogenFamily family, f32 density) {
    sim::ChaffBuffers b;
    b.reserve(capacity);
    sim::ChaffSpawnParams p;
    p.family = family;
    p.density = density;
    b.spawn(p);
    return b;
}

} // namespace

// ---------------------------------------------------------------------------
// Link 1: the choke point raises it, and shapes it the way the file says.
// ---------------------------------------------------------------------------

TEST_CASE("damage through the only damage path raises a hit flash", "[chaff][hitflash]") {
    const ScopedFlash scoped(plain_flash());
    sim::ChaffBuffers b = make_buffers(8, PathogenFamily::Virus, 1.0f);

    REQUIRE(b.hit_flash[0] == 0.0f);
    b.apply_density_loss(0, 0.4f);
    // gain 1, so the stored ramp is exactly the fraction of density removed.
    CHECK(b.hit_flash[0] > 0.39f);
    CHECK(b.hit_flash[0] < 0.41f);
}

TEST_CASE("a hit flashes for the damage that landed, not the damage requested",
          "[chaff][hitflash]") {
    const ScopedFlash scoped(plain_flash());
    sim::ChaffBuffers b = make_buffers(8, PathogenFamily::Virus, 1.0f);

    // Massive overkill on a full-health agent: `removed` clamps to 1.0, so the
    // ramp saturates at 1 rather than reporting 50x.
    b.apply_density_loss(0, 50.0f);
    CHECK(b.hit_flash[0] == 1.0f);
}

TEST_CASE("the flash is a fraction of what the agent has left, not of what it started with",
          "[chaff][hitflash]") {
    const ScopedFlash scoped(plain_flash());
    sim::ChaffBuffers healthy = make_buffers(8, PathogenFamily::Virus, 10.0f);
    sim::ChaffBuffers wounded = make_buffers(8, PathogenFamily::Virus, 1.0f);

    healthy.apply_density_loss(0, 0.5f);
    wounded.apply_density_loss(0, 0.5f);

    // Identical damage; the nearly-dead one lights up far harder. That read --
    // "the next kill is coming from over there" -- is the reason the formula
    // divides by current density and not by the family's spawn density.
    CHECK(wounded.hit_flash[0] > healthy.hit_flash[0] * 5.0f);
}

TEST_CASE("min_fraction floors out the smallest ticks", "[chaff][hitflash]") {
    sim::HitFlashParams params = plain_flash();
    params.min_fraction = 0.25f;
    const ScopedFlash scoped(params);
    sim::ChaffBuffers b = make_buffers(8, PathogenFamily::Virus, 1.0f);

    b.apply_density_loss(0, 0.1f);          // 10% -- under the floor
    CHECK(b.hit_flash[0] == 0.0f);
    b.apply_density_loss(0, 0.5f);          // 55% of what is left -- over it
    CHECK(b.hit_flash[0] > 0.0f);
}

TEST_CASE("a disabled family stores nothing at all", "[chaff][hitflash]") {
    sim::HitFlashParams params = plain_flash();
    params.enabled = false;
    const ScopedFlash scoped(params);
    sim::ChaffBuffers b = make_buffers(8, PathogenFamily::Virus, 1.0f);

    b.apply_density_loss(0, 0.9f);
    CHECK(b.hit_flash[0] == 0.0f);
    // Same for the other off switch: a zero fade is no effect, not an infinite one.
    params.enabled = true;
    params.duration = 0.0f;
    const ScopedFlash zero_duration(params);
    b.apply_density_loss(0, 0.05f);
    CHECK(b.hit_flash[0] == 0.0f);
}

TEST_CASE("retrigger decides what a second hit does", "[chaff][hitflash]") {
    SECTION("highest keeps the brighter of the two") {
        sim::HitFlashParams params = plain_flash();
        params.retrigger = sim::HitFlashRetrigger::Highest;
        const ScopedFlash scoped(params);
        sim::ChaffBuffers b = make_buffers(8, PathogenFamily::Virus, 100.0f);

        b.apply_density_loss(0, 50.0f);      // 50% of 100
        const f32 big = b.hit_flash[0];
        b.apply_density_loss(0, 1.0f);       // 2% of the 50 that is left
        CHECK(b.hit_flash[0] == big);        // the chip did not dim the cannon
    }
    SECTION("refresh lets the newest hit win outright") {
        sim::HitFlashParams params = plain_flash();
        params.retrigger = sim::HitFlashRetrigger::Refresh;
        const ScopedFlash scoped(params);
        sim::ChaffBuffers b = make_buffers(8, PathogenFamily::Virus, 100.0f);

        b.apply_density_loss(0, 50.0f);
        const f32 big = b.hit_flash[0];
        b.apply_density_loss(0, 1.0f);
        CHECK(b.hit_flash[0] < big);
    }
    SECTION("accumulate piles up and saturates") {
        sim::HitFlashParams params = plain_flash();
        params.retrigger = sim::HitFlashRetrigger::Accumulate;
        const ScopedFlash scoped(params);
        sim::ChaffBuffers b = make_buffers(8, PathogenFamily::Virus, 100.0f);

        b.apply_density_loss(0, 10.0f);
        const f32 first = b.hit_flash[0];
        b.apply_density_loss(0, 10.0f);
        CHECK(b.hit_flash[0] > first);
        for (int i = 0; i < 20; ++i) b.apply_density_loss(0, 1.0f);
        CHECK(b.hit_flash[0] <= 1.0f);       // never past full white
    }
}

// ---------------------------------------------------------------------------
// Link 2: the movement kernel fades it, over the authored duration.
// ---------------------------------------------------------------------------

TEST_CASE("the movement kernel fades the flash over the authored duration",
          "[chaff][hitflash]") {
    sim::HitFlashParams params = plain_flash();
    params.duration = 0.25f;   // exactly 15 ticks
    const ScopedFlash scoped(params);

    sim::ChaffBuffers b = make_buffers(64, PathogenFamily::Virus, 1.0f);
    b.apply_density_loss(0, 1000.0f);   // saturate the ramp; the agent is now dying
    // ...but this test is about the fade, not about death, so put it back.
    b.density[0] = 1.0f;
    b.flags[0] &= static_cast<u8>(~sim::chaff_flags::kPendingKill);
    REQUIRE(b.hit_flash[0] == 1.0f);

    sim::ChaffSystem system;
    sim::FlowField flow;          // never baked: sample() is (0,0), so nothing steers
    sim::DistanceField sdf;       // never baked: the wall pass is skipped entirely
    sim::TissueMask mask;
    sim::SpatialHash hash;
    sim::SpatialHashDesc hash_desc;
    hash_desc.bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{64.0f, 64.0f}};
    hash_desc.cell_size = 4.0f;
    hash.configure(hash_desc);
    hash.rebuild(b.pos_x.data(), b.pos_y.data(), b.count(), nullptr);
    const sim::SquadRegistry squads;
    Rng rng(1234u);

    f32 previous = b.hit_flash[0];
    for (int tick = 0; tick < 14; ++tick) {
        system.update(b, flow, sdf, mask, hash, squads, rng, kFixedDt, nullptr);
        CHECK(b.hit_flash[0] < previous);       // strictly monotonic down
        CHECK(b.hit_flash[0] > 0.0f);           // ...but not there yet
        previous = b.hit_flash[0];
    }
    // Tick 15 is where a 0.25 s fade from full intensity lands on zero -- to
    // within the float residual fifteen subtractions of 1/15 leave behind,
    // which is why this is a "gone" test and not an equality one. A residual
    // that small quantizes to a flash byte of 0, so it is already invisible.
    system.update(b, flow, sdf, mask, hash, squads, rng, kFixedDt, nullptr);
    CHECK(b.hit_flash[0] < 1.0f / 512.0f);

    // And the very next tick clamps it to a hard zero rather than letting it
    // run negative, which is the property the rest of the pipeline relies on:
    // "> 0" is what the batcher tests to decide an agent is flashing at all.
    system.update(b, flow, sdf, mask, hash, squads, rng, kFixedDt, nullptr);
    CHECK(b.hit_flash[0] == 0.0f);
    system.update(b, flow, sdf, mask, hash, squads, rng, kFixedDt, nullptr);
    CHECK(b.hit_flash[0] == 0.0f);
}

// ---------------------------------------------------------------------------
// Link 3: the stream belongs to the AGENT, not to the slot.
// ---------------------------------------------------------------------------

TEST_CASE("a hit flash follows its agent through compaction", "[chaff][hitflash]") {
    const ScopedFlash scoped(plain_flash());
    sim::ChaffBuffers b;
    b.reserve(16);
    for (int i = 0; i < 3; ++i) {
        sim::ChaffSpawnParams p;
        p.family = PathogenFamily::Virus;
        p.density = 1.0f;
        b.spawn(p);
    }

    // Light up the LAST agent, then retire the first. compact() swap-removes,
    // so the flashing agent lands in slot 0 and must arrive still flashing.
    b.apply_density_loss(2, 0.5f);
    const f32 flash = b.hit_flash[2];
    REQUIRE(flash > 0.0f);
    const u32 flashing_generation = b.generation[2];

    b.kill(0);
    REQUIRE(b.compact() == 1);
    REQUIRE(b.count() == 2);

    const usize moved = b.resolve(sim::ChaffHandle{2u, flashing_generation});
    REQUIRE(moved != sim::ChaffBuffers::npos);
    CHECK(b.hit_flash[moved] == flash);
    // And the vacated tail slot is dark, so the next agent spawned into it does
    // not inherit a stranger's hit.
    CHECK(b.hit_flash[b.count()] == 0.0f);
}

TEST_CASE("a recycled slot never inherits the previous occupant's flash",
          "[chaff][hitflash]") {
    const ScopedFlash scoped(plain_flash());
    sim::ChaffBuffers b = make_buffers(8, PathogenFamily::Virus, 1.0f);

    b.apply_density_loss(0, 1000.0f);           // flash it and kill it
    REQUIRE(b.hit_flash[0] > 0.0f);
    REQUIRE(b.compact() == 1);

    sim::ChaffSpawnParams p;
    p.family = PathogenFamily::Virus;
    p.density = 1.0f;
    REQUIRE(b.spawn(p).valid());
    CHECK(b.hit_flash[0] == 0.0f);
}

// ---------------------------------------------------------------------------
// Link 4: it reaches the instance buffer.
// ---------------------------------------------------------------------------

namespace {

/// The flash byte the batcher packed for instance `i` of family `f`.
u32 packed_flash(const std::vector<render::ChaffInstance>& dest, u32 per_family, u32 f, u32 i) {
    return (dest[static_cast<usize>(f) * per_family + i].flags >> 24) & 0xFFu;
}

render::ChaffBatchResult batch(const sim::ChaffBuffers& chaff,
                               std::vector<render::ChaffInstance>& dest, u32 per_family) {
    render::OccupancyGrid occ;   // invalid on purpose: every agent draws as a sprite
    render::ChaffBatchParams params;
    params.lod_blob_enabled = false;
    params.per_family_capacity = per_family;
    dest.assign(static_cast<usize>(kFamilyCount) * per_family, render::ChaffInstance{});
    return build_chaff_batches(chaff, occ, params, dest.data(), nullptr);
}

} // namespace

TEST_CASE("the batcher packs the flash into the top byte of the instance flags",
          "[chaff][hitflash][render]") {
    sim::HitFlashParams params = plain_flash();
    params.strength = 1.0f;
    params.curve = 1.0f;
    const ScopedFlash scoped(params);

    sim::ChaffBuffers b;
    b.reserve(16);
    for (int i = 0; i < 2; ++i) {
        sim::ChaffSpawnParams p;
        p.family = PathogenFamily::Virus;
        p.density = 1.0f;
        b.spawn(p);
    }
    b.apply_density_loss(0, 0.5f);   // agent 0 flashes at ~0.5, agent 1 not at all

    std::vector<render::ChaffInstance> dest;
    const u32 per_family = 8;
    const render::ChaffBatchResult result = batch(b, dest, per_family);

    REQUIRE(result.instances_total == 2);
    CHECK(result.agents_flashing == 1);

    const u32 lit = packed_flash(dest, per_family, static_cast<u32>(PathogenFamily::Virus), 0);
    const u32 dark = packed_flash(dest, per_family, static_cast<u32>(PathogenFamily::Virus), 1);
    CHECK(dark == 0u);
    CHECK(lit > 120u);
    CHECK(lit < 136u);   // ~0.5 * 255

    // The other three bytes of the word still say what they always said: the
    // flash claimed bits 24..31 and nothing else.
    const render::ChaffInstance& inst = dest[0];
    CHECK((inst.flags & 0xFFu) == static_cast<u32>(b.flags[0]));
    CHECK(((inst.flags >> 8) & 0xFFu) == static_cast<u32>(PathogenFamily::Virus));
}

TEST_CASE("the draw-time shaping comes from the table, not from the stream",
          "[chaff][hitflash][render]") {
    sim::ChaffBuffers b = make_buffers(16, PathogenFamily::Virus, 1.0f);
    {
        const ScopedFlash scoped(plain_flash());
        b.apply_density_loss(0, 0.5f);
    }
    const f32 stored = b.hit_flash[0];
    REQUIRE(stored > 0.0f);

    std::vector<render::ChaffInstance> dest;
    const u32 per_family = 8;

    // Same untouched stream, three different tables, three different pixels.
    // This is the property that makes the effect hot-reloadable: an agent that
    // is ALREADY mid-flash re-renders under a new curve.
    u32 linear = 0;
    u32 crushed = 0;
    u32 halved = 0;
    {
        sim::HitFlashParams p = plain_flash();
        p.curve = 1.0f;
        const ScopedFlash scoped(p);
        batch(b, dest, per_family);
        linear = packed_flash(dest, per_family, 0, 0);
    }
    {
        sim::HitFlashParams p = plain_flash();
        p.curve = 3.0f;              // crush the low end
        const ScopedFlash scoped(p);
        batch(b, dest, per_family);
        crushed = packed_flash(dest, per_family, 0, 0);
    }
    {
        sim::HitFlashParams p = plain_flash();
        p.curve = 1.0f;
        p.strength = 0.5f;           // half the peak opacity
        const ScopedFlash scoped(p);
        batch(b, dest, per_family);
        halved = packed_flash(dest, per_family, 0, 0);
    }
    CHECK(b.hit_flash[0] == stored);   // nothing above touched the sim
    CHECK(crushed < linear);
    CHECK(halved < linear);
}

TEST_CASE("scale_punch is off unless it is asked for", "[chaff][hitflash][render]") {
    sim::ChaffBuffers b = make_buffers(16, PathogenFamily::Virus, 1.0f);
    std::vector<render::ChaffInstance> dest;
    const u32 per_family = 8;

    sim::HitFlashParams p = plain_flash();
    {
        const ScopedFlash scoped(p);
        b.apply_density_loss(0, 1000.0f);   // saturate
        b.density[0] = 1.0f;
        b.flags[0] &= static_cast<u8>(~sim::chaff_flags::kPendingKill);
    }
    REQUIRE(b.hit_flash[0] == 1.0f);

    f32 unpunched = 0.0f;
    {
        const ScopedFlash scoped(p);        // scale_punch defaults to 0
        batch(b, dest, per_family);
        unpunched = dest[0].scale;
    }
    CHECK(unpunched == render::family_visual(PathogenFamily::Virus).silhouette);

    p.scale_punch = 0.5f;
    {
        const ScopedFlash scoped(p);
        batch(b, dest, per_family);
        CHECK(dest[0].scale > unpunched * 1.4f);
    }
}

// ---------------------------------------------------------------------------
// The contract: all of the above is invisible to the sim's own state.
// ---------------------------------------------------------------------------

TEST_CASE("retuning or disabling the hit flash cannot move state_hash",
          "[chaff][hitflash][determinism]") {
    auto run = [](const sim::HitFlashParams& params) {
        const ScopedFlash scoped(params);
        sim::SimDesc desc;
        desc.seed = 0x1234'5678'9ABC'DEF0ull;
        desc.max_chaff = 512;
        desc.world_bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{100.0f, 100.0f}};
        sim::SimWorld world;
        world.init(desc, nullptr);

        // Alternating densities against the kill rate below, so that over the
        // run some agents die (exercising compaction with flashes in flight)
        // and some only get thinned (exercising the fade).
        for (int i = 0; i < 40; ++i) {
            sim::ChaffSpawnParams p;
            p.position = Vec2{40.0f + static_cast<f32>(i % 8), 40.0f + static_cast<f32>(i / 8)};
            p.family = static_cast<PathogenFamily>(i % kFamilyCount);
            p.density = (i % 2 == 0) ? 0.5f : 4.0f;
            world.chaff().spawn(p);
        }
        // A field that thins the crowd every tick without wiping it out, so the
        // damage path -- and therefore the flash -- is live for the whole run.
        // A long lifetime keeps it registered across ticks without an owner to
        // re-submit it (see DamageSystem::clear_transient).
        sim::DamageField field;
        field.shape = sim::FieldShape::Circle;
        field.origin = Vec2{44.0f, 42.0f};
        field.radius = 12.0f;
        field.kill_rate = 2.0f;
        field.lifetime = 1000.0f;
        world.damage().submit(field);

        world.run_ticks(30);
        // The run has to have been a real one, or "the hashes match" is a
        // statement about two empty simulations: agents died, agents survived,
        // and the survivors took damage on the way.
        REQUIRE(world.chaff().count() > 0);
        REQUIRE(world.chaff().count() < 40);
        REQUIRE(world.chaff().total_density() < 80.0f);
        return world.state_hash();
    };

    sim::HitFlashParams shipped = plain_flash();

    sim::HitFlashParams off = shipped;
    off.enabled = false;

    sim::HitFlashParams loud = shipped;
    loud.retrigger = sim::HitFlashRetrigger::Accumulate;
    loud.gain = 40.0f;
    loud.duration = 5.0f;
    loud.scale_punch = 2.0f;

    const u64 baseline = run(shipped);
    CHECK(run(off) == baseline);
    CHECK(run(loud) == baseline);
}


// ---------------------------------------------------------------------------
// The shader half: the flash reaches actual pixels, in the authored colour.
//
// Everything above stops at the instance buffer. The last link -- chaff.frag
// reading bits 24..31 and mixing toward u_hit_flash_color[family] -- can only
// be checked by drawing, and it has two distinct ways to fail silently. The
// shader could ignore the byte entirely (agents never brighten), or it could
// brighten toward a HARDCODED white, which would look perfectly correct in the
// shipped game and quietly make `color` a knob that does nothing.
//
// So this renders the same agents three times -- unhit, flashed white, flashed
// red -- and asserts on the channel balance of each.
// ---------------------------------------------------------------------------

namespace {

struct HeadlessGl {
    immune::platform::Window window;
    bool ok = false;
    HeadlessGl(i32 w, i32 h) { ok = immune::platform::create_headless_gl(window, w, h); }
    ~HeadlessGl() { window.destroy(); }
};

/// Mean R, G and B over the pixels an agent actually painted. The clear colour
/// is a very dark plum, so a brightness floor separates bodies from background
/// and from their own drop shadows without needing a mask.
struct Channels {
    f64 r = 0.0, g = 0.0, b = 0.0;
    usize lit = 0;
};

Channels render_channels(render::Renderer& renderer, const render::Camera& camera,
                         const sim::ChaffBuffers& chaff, const sim::SpatialHash& hash) {
    renderer.begin_frame(camera, 0.0f);
    renderer.submit_chaff(chaff, hash);
    renderer.end_frame();

    std::vector<u8> pixels;
    i32 w = 0, h = 0;
    Channels out;
    if (!renderer.read_pixels(pixels, w, h)) return out;
    for (usize i = 0; i + 3 < pixels.size(); i += 4) {
        const u32 sum = static_cast<u32>(pixels[i]) + pixels[i + 1] + pixels[i + 2];
        if (sum <= 150u) continue;
        out.r += pixels[i];
        out.g += pixels[i + 1];
        out.b += pixels[i + 2];
        ++out.lit;
    }
    if (out.lit > 0) {
        const f64 n = static_cast<f64>(out.lit);
        out.r /= n;
        out.g /= n;
        out.b /= n;
    }
    return out;
}

} // namespace

TEST_CASE("a flashed agent is drawn in the authored flash colour",
          "[chaff][hitflash][render][gl]") {
    HeadlessGl gl(800, 300);
    if (!gl.ok) {
        WARN("headless GL unavailable; skipping");
        return;
    }
    render::RendererDesc rd;
    rd.framebuffer_width = 800;
    rd.framebuffer_height = 300;
    render::Renderer renderer;
    REQUIRE(renderer.init(rd));

    // Viruses: a strongly GREEN family, so "did it move toward white" and "did
    // it move toward red" are both unambiguous in the channel means.
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{40.0f, 15.0f}};
    sim::ChaffBuffers chaff;
    chaff.reserve(16);
    for (u32 i = 0; i < 3; ++i) {
        sim::ChaffSpawnParams p;
        p.family = PathogenFamily::Virus;
        p.position = Vec2{12.0f + static_cast<f32>(i) * 8.0f, 7.5f};
        p.velocity = Vec2{2.0f, 0.9f};
        p.density = 1.0f;
        chaff.spawn(p);
    }

    sim::SpatialHash hash;
    sim::SpatialHashDesc hd;
    hd.bounds = bounds;
    hd.cell_size = 4.0f;
    hash.configure(hd);
    hash.rebuild(chaff.pos_x.data(), chaff.pos_y.data(), chaff.count(), nullptr);

    render::Camera camera;
    camera.set_viewport(800, 300);
    camera.set_bounds(bounds);
    camera.set_center(Vec2{20.0f, 7.5f});
    camera.set_view_height(9.0f);
    camera.clamp_to_bounds();

    sim::HitFlashParams params = plain_flash();   // strength 1, curve 1
    params.color = Vec4{1.0f, 1.0f, 1.0f, 1.0f};

    Channels unhit;
    {
        const ScopedFlash scoped(params);
        unhit = render_channels(renderer, camera, chaff, hash);
    }
    REQUIRE(unhit.lit > 0);
    // Sanity: a virus is green, and the test is meaningless if it is not.
    REQUIRE(unhit.g > unhit.r * 1.5);

    // Light every agent all the way up. Writing the stream directly is the
    // point: this test is about the DRAW, and routing through the damage path
    // would only add a way for it to fail for an unrelated reason.
    for (usize i = 0; i < chaff.count(); ++i) chaff.hit_flash[i] = 1.0f;

    Channels white;
    {
        const ScopedFlash scoped(params);
        white = render_channels(renderer, camera, chaff, hash);
    }
    // Toward white: the two channels a virus was NOT using both climb hard,
    // and the body ends up close to neutral.
    CHECK(white.r > unhit.r * 2.0);
    CHECK(white.b > unhit.b * 2.0);
    CHECK(white.r > 180.0);
    CHECK(white.g > 180.0);
    CHECK(white.b > 180.0);

    // And now the knob. Same agents, same stream, a red flash colour: if the
    // shader were mixing toward a hardcoded white this would come out
    // indistinguishable from the run above.
    params.color = Vec4{1.0f, 0.0f, 0.0f, 1.0f};
    Channels red;
    {
        const ScopedFlash scoped(params);
        red = render_channels(renderer, camera, chaff, hash);
    }
    CHECK(red.r > 180.0);
    CHECK(red.g < white.g * 0.5);
    CHECK(red.b < white.b * 0.5);

    renderer.shutdown();
}


// ---------------------------------------------------------------------------
// The death flash — the half that actually fires in this game.
//
// WHY THIS IS THE IMPORTANT TEST. Chaff has essentially no wounded state: a
// bot-played run of skin_1_breach drew 94,245 agent-frames and only 29 of them
// (0.03%) were an agent that had taken damage and was still alive. At shipped
// tower rates an agent loses all of its density inside one tick and is
// compacted away before any frame is drawn, so for almost every enemy in the
// game THE KILLING BLOW IS THE ONLY HIT IT EVER TAKES. Everything above this
// point in the file is correct and covers 0.03% of the cases; this covers the
// rest.
// ---------------------------------------------------------------------------

namespace {

sim::CombatEvent death_at(Vec2 where, PathogenFamily family, f32 speed = 4.0f) {
    sim::CombatEvent e;
    e.type = sim::CombatEventType::ChaffDeath;
    e.origin = where;
    e.direction = Vec2{1.0f, 0.0f};
    e.magnitude = speed;
    e.target_family = family;
    e.source = TowerType::Count;
    return e;
}

} // namespace

TEST_CASE("a killed agent's body keeps being drawn, white and fading",
          "[chaff][hitflash][render][gl]") {
    HeadlessGl gl(800, 300);
    if (!gl.ok) {
        WARN("headless GL unavailable; skipping");
        return;
    }
    render::RendererDesc rd;
    rd.framebuffer_width = 800;
    rd.framebuffer_height = 300;
    render::Renderer renderer;
    REQUIRE(renderer.init(rd));

    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{40.0f, 15.0f}};
    // An EMPTY chaff store: every one of these agents is already dead and gone,
    // which is exactly the state the renderer has to draw something in.
    sim::ChaffBuffers chaff;
    chaff.reserve(16);

    sim::SpatialHash hash;
    sim::SpatialHashDesc hd;
    hd.bounds = bounds;
    hd.cell_size = 4.0f;
    hash.configure(hd);

    render::Camera camera;
    camera.set_viewport(800, 300);
    camera.set_bounds(bounds);
    camera.set_center(Vec2{20.0f, 7.5f});
    camera.set_view_height(9.0f);
    camera.clamp_to_bounds();

    sim::HitFlashParams params = plain_flash();
    params.color = Vec4{1.0f, 1.0f, 1.0f, 1.0f};
    params.curve = 1.0f;
    params.strength = 1.0f;
    params.death_linger = 0.5f;   // long, so a test is not racing a real clock

    const sim::CombatEvent deaths[3] = {
        death_at(Vec2{12.0f, 7.5f}, PathogenFamily::Virus),
        death_at(Vec2{20.0f, 7.5f}, PathogenFamily::Virus),
        death_at(Vec2{28.0f, 7.5f}, PathogenFamily::Virus),
    };

    // Corpses deliberately OUTLIVE the frame that recorded them -- that is the
    // whole feature -- so each case below has to start from an empty list or it
    // is measuring the leftovers of the one before it. Rendering one frame with
    // death_linger at 0 prunes every corpse in flight, which is also the path
    // that runs when an author turns the knob off mid-wave.
    auto flush = [&]() {
        sim::HitFlashParams off = params;
        off.death_linger = 0.0f;
        const ScopedFlash scoped(off);
        return render_channels(renderer, camera, chaff, hash);
    };

    Channels empty;
    {
        sim::HitFlashParams off = params;
        off.death_linger = 0.0f;
        const ScopedFlash scoped(off);
        renderer.submit_chaff_deaths(deaths, 3);
        empty = render_channels(renderer, camera, chaff, hash);
    }
    // death_linger 0 means the corpses were never even recorded: an empty sim
    // draws an empty screen.
    CHECK(empty.lit == 0);

    Channels fresh;
    {
        REQUIRE(flush().lit == 0);
        const ScopedFlash scoped(params);
        renderer.submit_chaff_deaths(deaths, 3);
        fresh = render_channels(renderer, camera, chaff, hash);
    }
    // Three bodies on screen that the sim has no record of whatsoever, drawn
    // white because they were just killed.
    REQUIRE(fresh.lit > 0);
    CHECK(fresh.r > 150.0);
    CHECK(fresh.g > 150.0);
    CHECK(fresh.b > 150.0);

    // Handed in already half-expired, the same body is dimmer and thinner --
    // `age_seconds` is what the screenshot harness leans on, and it is the
    // only way to test the fade without sleeping on a wall clock.
    Channels stale;
    {
        REQUIRE(flush().lit == 0);
        const ScopedFlash scoped(params);
        renderer.submit_chaff_deaths(deaths, 3, params.death_linger * 0.75f);
        stale = render_channels(renderer, camera, chaff, hash);
    }
    CHECK(stale.lit > 0);
    CHECK(stale.lit < fresh.lit);   // alpha has carried some of it away already

    // Fully expired on arrival: nothing is drawn at all.
    Channels expired;
    {
        REQUIRE(flush().lit == 0);
        const ScopedFlash scoped(params);
        renderer.submit_chaff_deaths(deaths, 3, params.death_linger * 2.0f);
        expired = render_channels(renderer, camera, chaff, hash);
    }
    CHECK(expired.lit == 0);

    // The colour knob drives corpses too, not just live agents.
    sim::HitFlashParams red = params;
    red.color = Vec4{1.0f, 0.0f, 0.0f, 1.0f};
    Channels red_ch;
    {
        REQUIRE(flush().lit == 0);
        const ScopedFlash scoped(red);
        renderer.submit_chaff_deaths(deaths, 3);
        red_ch = render_channels(renderer, camera, chaff, hash);
    }
    CHECK(red_ch.lit > 0);
    CHECK(red_ch.r > 150.0);
    CHECK(red_ch.g < fresh.g * 0.6);

    renderer.shutdown();
}

TEST_CASE("the death flash ignores events that are not chaff deaths",
          "[chaff][hitflash][render][gl]") {
    HeadlessGl gl(400, 200);
    if (!gl.ok) {
        WARN("headless GL unavailable; skipping");
        return;
    }
    render::RendererDesc rd;
    rd.framebuffer_width = 400;
    rd.framebuffer_height = 200;
    render::Renderer renderer;
    REQUIRE(renderer.init(rd));

    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{40.0f, 15.0f}};
    sim::ChaffBuffers chaff;
    chaff.reserve(8);
    sim::SpatialHash hash;
    sim::SpatialHashDesc hd;
    hd.bounds = bounds;
    hd.cell_size = 4.0f;
    hash.configure(hd);
    render::Camera camera;
    camera.set_viewport(400, 200);
    camera.set_bounds(bounds);
    camera.set_center(Vec2{20.0f, 7.5f});
    camera.set_view_height(9.0f);
    camera.clamp_to_bounds();

    // Callers hand this the WHOLE frame's event buffer, the same span the
    // particle system gets, so everything that is not a chaff death has to fall
    // straight through -- including a death whose family is "not agent
    // specific", which names no silhouette to draw.
    sim::CombatEvent noise[3];
    noise[0] = death_at(Vec2{20.0f, 7.5f}, PathogenFamily::Virus);
    noise[0].type = sim::CombatEventType::Explosion;
    noise[1] = death_at(Vec2{20.0f, 7.5f}, PathogenFamily::Virus);
    noise[1].type = sim::CombatEventType::MuzzleFlash;
    noise[2] = death_at(Vec2{20.0f, 7.5f}, PathogenFamily::Count);

    sim::HitFlashParams params = plain_flash();
    params.death_linger = 0.5f;
    const ScopedFlash scoped(params);
    renderer.submit_chaff_deaths(noise, 3);
    const Channels ch = render_channels(renderer, camera, chaff, hash);
    CHECK(ch.lit == 0);

    renderer.shutdown();
}
