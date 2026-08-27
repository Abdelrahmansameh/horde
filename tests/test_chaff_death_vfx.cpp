// Chaff death bursts, both halves.
//
// SIM SIDE (SimWorld::tick step 4f): the event has to be raised for exactly the
// agents the PLAYER killed, at the last moment they still have a position, and
// it must stay invisible to state_hash().
//
// VFX SIDE (vfx/Particles.cpp, vfx/DeathVfx.h): the burst has to come from the
// per-family table rather than from the killer, so that retuning a family in
// enemies.json actually changes what the player sees.
//
// What is deliberately NOT asserted: particle counts, sizes and lifetimes. Those
// are art-direction numbers, and they were put in a file precisely so they can
// move without breaking a build.
#include "sim/SimWorld.h"

#include "platform/Window.h"
#include "render/Camera.h"
#include "render/Renderer.h"
#include "render/Screenshot.h"
#include "sim/CombatEvents.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/damage/DamageField.h"
#include "sim/flowfield/FlowField.h"
#include "vfx/DeathVfx.h"
#include "vfx/Particles.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

using namespace immune;

namespace {

/// A world with no flow field, no goal and generous bounds, so the only way an
/// agent can leave is the one each test arranges for it.
sim::SimDesc bare_desc(usize death_budget) {
    sim::SimDesc desc;
    desc.seed = 0xBEEF'1234'5678'9ABCull;
    desc.max_chaff = 4096;
    desc.max_chaff_death_events = death_budget;
    desc.world_bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{200.0f, 200.0f}};
    return desc;
}

void spawn_chaff(sim::SimWorld& world, Vec2 at, PathogenFamily family, f32 density) {
    sim::ChaffSpawnParams p;
    p.position = at;
    p.family = family;
    p.density = density;
    REQUIRE(world.chaff().spawn(p).valid());
}

/// One tick's worth of a circular field. `rate` is density per second, so a
/// huge one erases everything under it in a single tick and a modest one thins
/// the crowd over several.
void burn(sim::SimWorld& world, Vec2 at, f32 radius, f32 rate) {
    sim::DamageField f;
    f.shape = sim::FieldShape::Circle;
    f.origin = at;
    f.radius = radius;
    f.kill_rate = rate;
    f.falloff = 0.0f;
    f.lifetime = kFixedDt;
    world.damage().submit(f);
}

std::vector<sim::CombatEvent> deaths(const sim::SimWorld& world) {
    std::vector<sim::CombatEvent> out;
    for (const sim::CombatEvent& e : world.combat_events().events()) {
        if (e.type == sim::CombatEventType::ChaffDeath) out.push_back(e);
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Sim side
// ---------------------------------------------------------------------------

TEST_CASE("a killed chaff agent raises one death event carrying its identity",
          "[sim][vfx][death]") {
    sim::SimWorld world;
    world.init(bare_desc(512), nullptr);

    const Vec2 at{100.0f, 100.0f};
    spawn_chaff(world, at, PathogenFamily::Bacteria, 1.0f);
    burn(world, at, 3.0f, 10'000.0f);
    world.tick();

    const std::vector<sim::CombatEvent> raised = deaths(world);
    REQUIRE(raised.size() == 1);
    // The burst is looked up per family, so the family is the one piece of
    // information the event absolutely has to carry.
    REQUIRE(raised[0].target_family == PathogenFamily::Bacteria);
    // Raised before compaction, so the position is still the agent's own -- and
    // specifically its POST-movement one, since the pass runs at the end of the
    // tick. Hence the tolerance: one tick of jitter, not an exact spawn point.
    REQUIRE(std::fabs(raised[0].origin.x - at.x) < 0.5f);
    REQUIRE(std::fabs(raised[0].origin.y - at.y) < 0.5f);
    // Sized off the family's body radius. A zero would collapse the whole burst
    // to a point.
    REQUIRE(raised[0].radius > 0.0f);
    // Nothing knows what killed it by the time this is raised, and the burst is
    // the enemy's identity anyway. See CombatEventType::ChaffDeath.
    REQUIRE(raised[0].source == TowerType::Count);

    // And the agent really is gone, i.e. the event was raised in the last
    // window in which it could have been.
    REQUIRE(world.chaff().count() == 0);
}

TEST_CASE("an agent that reaches the objective dies without a burst", "[sim][vfx][death]") {
    // THE distinction this pass exists to make. A leak and a kill both arrive at
    // compaction carrying nothing but kPendingKill; only the kill zeroed the
    // agent's density. Rewarding a leak with a death pop would tell the player
    // they won a fight they just lost.
    sim::SimWorld world;
    world.init(bare_desc(512), nullptr);

    const Vec2 goal{100.0f, 100.0f};
    world.chaff_system().set_goal(goal, Vec2{4.0f, 4.0f});
    spawn_chaff(world, goal, PathogenFamily::Virus, 1.0f);

    world.tick();

    REQUIRE(world.chaff().count() == 0);               // it was consumed...
    REQUIRE(world.snapshot().chaff_leaked_total == 1);  // ...as a leak...
    REQUIRE(deaths(world).empty());                     // ...and drew nothing.
}

TEST_CASE("an agent that leaves the world dies without a burst", "[sim][vfx][death]") {
    sim::SimWorld world;
    world.init(bare_desc(512), nullptr);

    // Outside world_bounds, so pass C despawns it on the first tick.
    spawn_chaff(world, Vec2{260.0f, 100.0f}, PathogenFamily::Virus, 1.0f);
    world.tick();

    REQUIRE(world.chaff().count() == 0);
    REQUIRE(deaths(world).empty());
}

TEST_CASE("death events are capped per tick", "[sim][vfx][death]") {
    // A wave breaking kills hundreds of agents on one tick. The cap is what
    // stops that from filling the shared sink and dropping every tower's own
    // effect for the frame.
    constexpr usize kBudget = 8;
    sim::SimWorld world;
    world.init(bare_desc(kBudget), nullptr);

    const Vec2 at{100.0f, 100.0f};
    for (u32 i = 0; i < 64; ++i) spawn_chaff(world, at, PathogenFamily::Virus, 1.0f);
    burn(world, at, 10.0f, 10'000.0f);
    world.tick();

    REQUIRE(world.chaff().count() == 0);        // all 64 really died
    REQUIRE(deaths(world).size() == kBudget);   // only 8 were announced
}

TEST_CASE("raising death events cannot move the simulation", "[sim][vfx][death]") {
    // The standing contract for this whole channel (sim/CombatEvents.h): events
    // are an OUTPUT of the tick. A budget of 0 turns the pass off entirely, so
    // running one seed with it and without it tests exactly that.
    const auto run = [](usize budget) {
        sim::SimWorld world;
        world.init(bare_desc(budget), nullptr);
        for (u32 i = 0; i < 40; ++i) {
            spawn_chaff(world,
                        Vec2{90.0f + static_cast<f32>(i % 8), 90.0f + static_cast<f32>(i / 8)},
                        i % 2 == 0 ? PathogenFamily::Virus : PathogenFamily::Bacteria, 4.0f);
        }
        // Gentle enough that agents die across many ticks rather than all on the
        // first, so the pass runs repeatedly against a changing array.
        u64 seen = 0;
        for (u32 t = 0; t < 60; ++t) {
            burn(world, Vec2{92.0f, 92.0f}, 3.0f, 30.0f);
            world.tick();
            seen += deaths(world).size();
            world.combat_events().clear();
        }
        return std::pair<u64, u64>{world.state_hash(), seen};
    };

    const auto with_events = run(512);
    const auto without = run(0);
    REQUIRE(with_events.second > 0);   // the pass actually did something
    REQUIRE(without.second == 0);      // and a zero budget really is off
    REQUIRE(with_events.first == without.first);
}

// ---------------------------------------------------------------------------
// VFX side
// ---------------------------------------------------------------------------

namespace {

sim::CombatEvent death_of(PathogenFamily family) {
    sim::CombatEvent e;
    e.type = sim::CombatEventType::ChaffDeath;
    e.target_family = family;
    e.source = TowerType::Count;
    e.origin = Vec2{12.0f, 8.0f};
    e.direction = Vec2{1.0f, 0.0f};
    e.magnitude = 6.0f;   // speed at death, for this event type
    e.radius = 0.8f;
    return e;
}

} // namespace

TEST_CASE("both families draw a death burst, and they are not the same burst",
          "[vfx][death]") {
    // Both shipped families explode into hard-edged shards now -- that shared
    // grammar is deliberate (see vfx/DeathVfx.cpp), so the old "one throws
    // shards and the other throws droplets" check no longer describes the
    // effect. What still has to hold is that a player can tell them apart, so
    // this asserts they differ on the axes that actually carry the read.
    vfx::ParticleSystem virus;
    virus.init(4096, 11);
    virus.emit_for_event(death_of(PathogenFamily::Virus));

    vfx::ParticleSystem bacteria;
    bacteria.init(4096, 11);
    bacteria.emit_for_event(death_of(PathogenFamily::Bacteria));

    REQUIRE(virus.live_count() > 0);
    REQUIRE(bacteria.live_count() > 0);
    // The bigger body blows into more pieces.
    REQUIRE(bacteria.live_count() > virus.live_count());

    // And into differently-coloured ones, which is the read that survives a
    // glance at a screen full of them.
    const auto debris_rgb = [](const vfx::ParticleSystem& ps) {
        std::vector<vfx::ParticleInstance> out;
        ps.build_instances(vfx::BlendMode::AlphaBlend, out);
        REQUIRE_FALSE(out.empty());
        return out.front().tint_rgba8 & 0x00FF'FFFFu;
    };
    REQUIRE(debris_rgb(virus) != debris_rgb(bacteria));
}

TEST_CASE("a death burst is over quickly", "[vfx][death]") {
    // A death is an instant, not a lingering cloud: at a hundred kills a second
    // during a wave clear, debris that outlives its own burst piles up into a
    // permanent haze over the lane. Both families must be fully retired inside
    // half a second -- comfortably longer than the authored lifetimes, so this
    // catches a runaway rather than pinning an art number.
    for (u32 f = 0; f < kFamilyCount; ++f) {
        vfx::ParticleSystem ps;
        ps.init(4096, 606);
        ps.emit_for_event(death_of(static_cast<PathogenFamily>(f)));
        REQUIRE(ps.live_count() > 0);

        // Stepped at a real frame rate rather than jumped, so staged particles
        // still get to live their lives.
        for (u32 t = 0; t < 30; ++t) ps.update(1.0f / 60.0f, nullptr);
        INFO("family index " << f << " still had particles after 0.5s");
        REQUIRE(ps.live_count() == 0);
    }
}

TEST_CASE("style selects the debris shape", "[vfx][death]") {
    // `style` is the one thing about a burst that is not a number, so it needs
    // its own guard -- nothing shipped uses Lyse today, and an unexercised enum
    // is one refactor away from silently doing nothing.
    const vfx::FamilyDeathVfx original = vfx::family_death_vfx(PathogenFamily::Virus);

    const auto kinds_of = [](vfx::DeathStyle style, vfx::ParticleKind want) {
        vfx::FamilyDeathVfx look = vfx::family_death_vfx(PathogenFamily::Virus);
        look.style = style;
        vfx::set_family_death_vfx(PathogenFamily::Virus, look);

        vfx::ParticleSystem ps;
        ps.init(4096, 909);
        ps.emit_for_event(death_of(PathogenFamily::Virus));
        std::vector<vfx::ParticleInstance> out;
        ps.build_instances(vfx::BlendMode::AlphaBlend, out);
        for (const vfx::ParticleInstance& inst : out) {
            if ((inst.kind_blend & 0xFFFFu) == static_cast<u32>(want)) return true;
        }
        return false;
    };

    REQUIRE(kinds_of(vfx::DeathStyle::Burst, vfx::ParticleKind::Shard));
    REQUIRE_FALSE(kinds_of(vfx::DeathStyle::Burst, vfx::ParticleKind::Tracer));
    REQUIRE(kinds_of(vfx::DeathStyle::Lyse, vfx::ParticleKind::Tracer));
    REQUIRE_FALSE(kinds_of(vfx::DeathStyle::Lyse, vfx::ParticleKind::Shard));

    vfx::set_family_death_vfx(PathogenFamily::Virus, original);
}

TEST_CASE("the death burst is driven by the family table, not by the killer",
          "[vfx][death]") {
    // What makes enemies.json's death_vfx block real: change the table, the
    // burst changes. Restores the table afterwards -- it is process-global, the
    // same way render::set_family_color is.
    const vfx::FamilyDeathVfx original = vfx::family_death_vfx(PathogenFamily::Virus);

    vfx::ParticleSystem before;
    before.init(4096, 77);
    before.emit_for_event(death_of(PathogenFamily::Virus));
    const usize base = before.live_count();
    REQUIRE(base > 0);

    vfx::FamilyDeathVfx louder = original;
    louder.bit_count = original.bit_count + 20;
    louder.bloom_count = original.bloom_count + 5;
    vfx::set_family_death_vfx(PathogenFamily::Virus, louder);

    vfx::ParticleSystem after;
    after.init(4096, 77);
    after.emit_for_event(death_of(PathogenFamily::Virus));
    REQUIRE(after.live_count() == base + 25);

    // And the off switch really is off.
    vfx::FamilyDeathVfx silent = original;
    silent.enabled = false;
    vfx::set_family_death_vfx(PathogenFamily::Virus, silent);

    vfx::ParticleSystem quiet;
    quiet.init(4096, 77);
    quiet.emit_for_event(death_of(PathogenFamily::Virus));
    REQUIRE(quiet.live_count() == 0);

    vfx::set_family_death_vfx(PathogenFamily::Virus, original);
}

TEST_CASE("a death burst carries the family's colour", "[vfx][death]") {
    // "Small shapes that match the colour" is the whole brief for this effect.
    // The debris is the part that must not be washed toward white -- the core
    // flash deliberately is -- so the debris is what gets checked.
    const vfx::FamilyDeathVfx& look = vfx::family_death_vfx(PathogenFamily::Bacteria);

    vfx::ParticleSystem ps;
    ps.init(4096, 5150);
    ps.emit_for_event(death_of(PathogenFamily::Bacteria));

    std::vector<vfx::ParticleInstance> debris;
    ps.build_instances(vfx::BlendMode::AlphaBlend, debris);
    REQUIRE_FALSE(debris.empty());

    const auto quantize = [](f32 v) {
        const f32 c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<u32>(c * 255.0f + 0.5f);
    };
    const u32 want_rgb = quantize(look.color.r) | (quantize(look.color.g) << 8) |
                         (quantize(look.color.b) << 16);
    for (const vfx::ParticleInstance& inst : debris) {
        REQUIRE((inst.tint_rgba8 & 0x00FF'FFFFu) == want_rgb);
    }
}

// ---------------------------------------------------------------------------
// VISUAL PROOF
//
// Same fallback tests/test_render_vfx.cpp and test_towers.cpp already use: the
// --screenshot CLI never calls submit_particles(), so it cannot show this work.
// This drives the real headless-GL Renderer through its public API and writes a
// PNG to $TEMP for manual read-back. The assertions here only check that pixels
// landed where the deaths happened -- how the burst LOOKS is a human's call.
// ---------------------------------------------------------------------------

namespace {

struct HeadlessGl {
    platform::Window window;
    bool ok = false;
    HeadlessGl(i32 w, i32 h) { ok = platform::create_headless_gl(window, w, h); }
};

std::string scratch_path(const std::string& filename) {
    const char* t = std::getenv("TEMP");
    if (t == nullptr) t = std::getenv("TMP");
    const std::string dir = t != nullptr ? std::string(t) : std::string(".");
    return dir + "/" + filename;
}

constexpr f32 kShowW = 90.0f;
constexpr f32 kShowH = 50.0f;

} // namespace

TEST_CASE("VISUAL: a wave of chaff dying, virus left and bacteria right",
          "[vfx][death][visual][gl]") {
    HeadlessGl gl(1600, 900);
    if (!gl.ok) { WARN("headless GL unavailable; skipping"); return; }

    sim::SimWorld world;
    {
        sim::SimDesc desc = bare_desc(512);
        desc.world_bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{kShowW, kShowH}};
        world.init(desc, nullptr);
    }
    // An open arena, so the horde has somewhere to stand and the burst has the
    // real substrate to read against rather than a flat clear colour.
    sim::TissueMask& mask = world.tissue();
    mask.resize(static_cast<i32>(kShowW), static_cast<i32>(kShowH), 1.0f, Vec2{0.0f, 0.0f});
    for (i32 y = 3; y < static_cast<i32>(kShowH) - 3; ++y)
        for (i32 x = 3; x < static_cast<i32>(kShowW) - 3; ++x) mask.set_walkable(x, y, true);
    world.sdf().bake(mask);

    const Vec2 virus_at{28.0f, 25.0f};
    const Vec2 bacteria_at{62.0f, 25.0f};

    vfx::ParticleSystem particles;
    particles.init(vfx::ParticleSystem::kDefaultCapacity, 0x0DEA'DBEEFull);

    const auto pump = [&] {
        const auto& events = world.combat_events().events();
        particles.emit_for_events(events.data(), events.size());
        world.combat_events().clear();
        particles.update(kFixedDt, nullptr);
    };

    const auto seed_crowd = [&](Vec2 at, PathogenFamily family, f32 density) {
        for (u32 i = 0; i < 60; ++i) {
            spawn_chaff(world,
                        at + Vec2{static_cast<f32>(i % 10) * 0.9f,
                                  static_cast<f32>(i / 10) * 0.9f},
                        family, density);
        }
    };

    // A crowd of each family, then a field over each hot enough to take a slice
    // out of it every tick. The capture happens THREE ticks after the last kill
    // -- 50 ms, which is inside every authored lifetime -- because a burst is an
    // instant and a frame taken half a second later shows an empty lane.
    seed_crowd(virus_at, PathogenFamily::Virus, 1.0f);
    seed_crowd(bacteria_at, PathogenFamily::Bacteria, 1.8f);

    usize total_deaths = 0;
    for (u32 t = 0; t < 6; ++t) {
        burn(world, virus_at + Vec2{4.0f, 4.0f}, 4.5f, 22.0f);
        burn(world, bacteria_at + Vec2{4.0f, 4.0f}, 4.5f, 40.0f);
        world.tick();
        total_deaths += deaths(world).size();
        pump();
    }

    INFO("deaths: " << total_deaths << ", live particles: " << particles.live_count());
    REQUIRE(total_deaths > 0);
    REQUIRE(particles.live_count() > 0);

    render::RendererDesc rd;
    rd.framebuffer_width = gl.window.width();
    rd.framebuffer_height = gl.window.height();
    rd.max_chaff_instances = static_cast<u32>(world.desc().max_chaff);
    render::Renderer renderer;
    REQUIRE(renderer.init(rd));

    std::vector<vfx::ParticleInstance> scratch;
    const auto capture = [&](Vec2 center, f32 view_height, const std::string& name) {
        render::Camera camera;
        camera.set_viewport(gl.window.width(), gl.window.height());
        camera.set_bounds(world.desc().world_bounds);
        camera.set_center(center);
        camera.set_view_height(view_height);

        renderer.begin_frame(camera, 0.0f);
        renderer.submit_tissue(world.tissue(), world.sdf(), 0.0f);
        renderer.submit_chaff(world.chaff(), world.spatial());
        renderer.submit_fields(world.damage().fields().data(), world.damage().fields().size());
        particles.build_instances(vfx::BlendMode::Additive, scratch);
        renderer.submit_particles(scratch.data(), scratch.size(), vfx::BlendMode::Additive);
        particles.build_instances(vfx::BlendMode::AlphaBlend, scratch);
        renderer.submit_particles(scratch.data(), scratch.size(), vfx::BlendMode::AlphaBlend);
        renderer.end_frame();

        const render::FrameStats stats = renderer.stats();
        std::vector<u8> pixels;
        i32 w = 0, h = 0;
        REQUIRE(renderer.read_pixels(pixels, w, h));
        const std::string out = scratch_path(name);
        REQUIRE(render::write_png_rgba(out, pixels.data(), w, h));
        std::fprintf(stderr, "[death vfx] %s  particles_drawn=%u\n", out.c_str(),
                     stats.particle_instances_drawn);
        return stats.particle_instances_drawn;
    };

    // Whole board, then a close-up of each family. Nothing advances between the
    // three, so they are one instant seen at three framings -- the debris is a
    // fraction of a world unit across and does not survive a full-board view.
    REQUIRE(capture(world.desc().world_bounds.center(), kShowH, "chaff_death_vfx.png") > 0);
    REQUIRE(capture(virus_at + Vec2{4.0f, 4.0f}, 14.0f, "chaff_death_virus.png") > 0);
    REQUIRE(capture(bacteria_at + Vec2{4.0f, 4.0f}, 14.0f, "chaff_death_bacteria.png") > 0);
}

TEST_CASE("VISUAL: one virus and one bacterium dying, close up",
          "[vfx][death][visual][gl]") {
    // The crowd capture above is the stress case -- sixty overlapping bursts.
    // This is the read that actually matters: what ONE death looks like, which
    // is what a player sees for most of a level.
    HeadlessGl gl(1600, 900);
    if (!gl.ok) { WARN("headless GL unavailable; skipping"); return; }

    sim::SimWorld world;
    {
        sim::SimDesc desc = bare_desc(512);
        desc.world_bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{kShowW, kShowH}};
        world.init(desc, nullptr);
    }
    sim::TissueMask& mask = world.tissue();
    mask.resize(static_cast<i32>(kShowW), static_cast<i32>(kShowH), 1.0f, Vec2{0.0f, 0.0f});
    for (i32 y = 3; y < static_cast<i32>(kShowH) - 3; ++y)
        for (i32 x = 3; x < static_cast<i32>(kShowW) - 3; ++x) mask.set_walkable(x, y, true);
    world.sdf().bake(mask);

    const Vec2 at{45.0f, 25.0f};
    spawn_chaff(world, at + Vec2{-6.0f, 0.0f}, PathogenFamily::Virus, 1.0f);
    spawn_chaff(world, at + Vec2{6.0f, 0.0f}, PathogenFamily::Bacteria, 1.8f);

    vfx::ParticleSystem particles;
    particles.init(vfx::ParticleSystem::kDefaultCapacity, 0x51'1161'E5ull);

    burn(world, at, 12.0f, 10'000.0f);   // both, on one tick
    world.tick();
    REQUIRE(deaths(world).size() == 2);

    const auto& events = world.combat_events().events();
    particles.emit_for_events(events.data(), events.size());
    world.combat_events().clear();
    // Three render frames in. Both bursts now share a grammar -- fast launch
    // against heavy drag -- so both cover most of their travel in the first two
    // or three frames and then decelerate. This is where the fragments have
    // cleared the body and nothing has started fading.
    for (u32 f = 0; f < 3; ++f) particles.update(1.0f / 60.0f, nullptr);

    render::RendererDesc rd;
    rd.framebuffer_width = gl.window.width();
    rd.framebuffer_height = gl.window.height();
    rd.max_chaff_instances = static_cast<u32>(world.desc().max_chaff);
    render::Renderer renderer;
    REQUIRE(renderer.init(rd));

    render::Camera camera;
    camera.set_viewport(gl.window.width(), gl.window.height());
    camera.set_bounds(world.desc().world_bounds);
    camera.set_center(at);
    camera.set_view_height(18.0f);

    std::vector<vfx::ParticleInstance> scratch;
    renderer.begin_frame(camera, 0.0f);
    renderer.submit_tissue(world.tissue(), world.sdf(), 0.0f);
    particles.build_instances(vfx::BlendMode::Additive, scratch);
    renderer.submit_particles(scratch.data(), scratch.size(), vfx::BlendMode::Additive);
    particles.build_instances(vfx::BlendMode::AlphaBlend, scratch);
    renderer.submit_particles(scratch.data(), scratch.size(), vfx::BlendMode::AlphaBlend);
    renderer.end_frame();

    REQUIRE(renderer.stats().particle_instances_drawn > 0);
    std::vector<u8> pixels;
    i32 w = 0, h = 0;
    REQUIRE(renderer.read_pixels(pixels, w, h));
    const std::string out = scratch_path("chaff_death_single.png");
    REQUIRE(render::write_png_rgba(out, pixels.data(), w, h));
    std::fprintf(stderr, "[death vfx] %s  particles_drawn=%u\n", out.c_str(),
                 renderer.stats().particle_instances_drawn);
}
