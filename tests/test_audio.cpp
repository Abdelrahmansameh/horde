// Tests for Wave 3C's procedural audio mixer.
//
// AudioEngine's public surface (Audio.h, frozen) exposes no way to inspect
// voice-pool or ring-buffer internals, so most of this file is white-box:
// it includes audio/AudioInternal.h directly and drives EventRing/Mixer/
// trigger_event/pump as free functions. A smaller set of black-box tests
// drives the real AudioEngine to check the public contract (dropped_events,
// ready(), null_device safety).
#include "audio/Audio.h"
#include "audio/AudioInternal.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

using namespace immune;
using namespace immune::audio;
using namespace immune::audio::internal;

namespace {

AudioEvent make_event(SoundId id, Vec2 pos = {0.0f, 0.0f}, f32 gain = 1.0f, f32 pitch = 1.0f,
                       f32 intensity = 1.0f) {
    AudioEvent ev;
    ev.id = id;
    ev.position = pos;
    ev.gain = gain;
    ev.pitch = pitch;
    ev.intensity = intensity;
    return ev;
}

u32 count_active(const Mixer& m) {
    u32 n = 0;
    for (u32 i = 0; i < m.voice_limit; ++i)
        if (m.voices[i].active) ++n;
    return n;
}

} // namespace

// ---------------------------------------------------------------------
// EventRing: SPSC ring buffer.
// ---------------------------------------------------------------------
TEST_CASE("EventRing preserves FIFO order", "[audio][ring]") {
    EventRing ring;
    for (u16 i = 1; i <= 10; ++i) {
        REQUIRE(ring.push(make_event(static_cast<SoundId>(i))));
    }
    for (u16 i = 1; i <= 10; ++i) {
        AudioEvent out;
        REQUIRE(ring.pop(out));
        REQUIRE(out.id == static_cast<SoundId>(i));
    }
    AudioEvent out;
    REQUIRE_FALSE(ring.pop(out)); // now empty
}

TEST_CASE("EventRing accepts exactly capacity events then drops", "[audio][ring]") {
    EventRing ring;
    for (u32 i = 0; i < EventRing::capacity(); ++i) {
        REQUIRE(ring.push(make_event(SoundId::UiClick)));
    }
    REQUIRE(ring.size() == EventRing::capacity());
    // One more push must be dropped (queue full), not overwrite anything.
    REQUIRE_FALSE(ring.push(make_event(SoundId::UiInvalid)));
    REQUIRE(ring.size() == EventRing::capacity());

    // Draining frees space for further pushes.
    AudioEvent out;
    REQUIRE(ring.pop(out));
    REQUIRE(ring.push(make_event(SoundId::UiInvalid)));
}

TEST_CASE("EventRing pop on empty ring fails without corrupting state", "[audio][ring]") {
    EventRing ring;
    AudioEvent out;
    REQUIRE_FALSE(ring.pop(out));
    REQUIRE(ring.push(make_event(SoundId::TowerPlace)));
    REQUIRE(ring.pop(out));
    REQUIRE(out.id == SoundId::TowerPlace);
    REQUIRE_FALSE(ring.pop(out));
}

// ---------------------------------------------------------------------
// AudioEngine::dropped_events() — the ring buffer viewed through the
// public contract.
// ---------------------------------------------------------------------
TEST_CASE("dropped_events increments once the ring buffer overflows", "[audio][engine]") {
    AudioEngine engine;
    AudioConfig cfg;
    cfg.null_device = true;
    REQUIRE(engine.init(cfg));
    REQUIRE(engine.ready());
    REQUIRE(engine.dropped_events() == 0);

    // Post more than the ring can hold *without* calling update(), so
    // nothing drains it — this isolates the ring-buffer-full path.
    const u32 total = EventRing::capacity() + 50;
    for (u32 i = 0; i < total; ++i) {
        engine.post(make_event(SoundId::TowerFire));
    }
    REQUIRE(engine.dropped_events() == total - EventRing::capacity());
    engine.shutdown();
}

TEST_CASE("posting fewer events than ring capacity per frame drops nothing", "[audio][engine]") {
    AudioEngine engine;
    AudioConfig cfg;
    cfg.null_device = true;
    REQUIRE(engine.init(cfg));

    for (int frame = 0; frame < 20; ++frame) {
        for (int i = 0; i < 5; ++i) engine.post(make_event(SoundId::UiClick));
        engine.update(Vec2{0.0f, 0.0f}, 0.3f, 1.0f / 60.0f);
    }
    REQUIRE(engine.dropped_events() == 0);
    engine.shutdown();
}

TEST_CASE("two AudioEngine instances track dropped_events independently", "[audio][engine]") {
    AudioEngine a, b;
    AudioConfig cfg;
    cfg.null_device = true;
    REQUIRE(a.init(cfg));
    REQUIRE(b.init(cfg));

    for (u32 i = 0; i < EventRing::capacity() + 10; ++i) a.post(make_event(SoundId::TowerFire));
    b.post(make_event(SoundId::UiClick));

    REQUIRE(a.dropped_events() == 10);
    REQUIRE(b.dropped_events() == 0);

    a.shutdown();
    b.shutdown();
}

// ---------------------------------------------------------------------
// Voice pool: max_voices cap + priority-aware stealing.
// ---------------------------------------------------------------------
TEST_CASE("voice pool never exceeds voice_limit", "[audio][voices]") {
    Mixer m;
    m.voice_limit = 4;
    for (int i = 0; i < 4; ++i) trigger_event(m, make_event(SoundId::TowerFire));
    REQUIRE(count_active(m) == 4);

    // A 5th Normal-priority event must steal, not grow the pool.
    trigger_event(m, make_event(SoundId::TowerFire));
    REQUIRE(count_active(m) == 4);
}

TEST_CASE("stealing never touches a Protected voice while a lower-priority one exists",
          "[audio][voices][priority]") {
    Mixer m;
    m.voice_limit = 3;

    trigger_event(m, make_event(SoundId::BossTelegraph)); // Protected
    trigger_event(m, make_event(SoundId::TowerFire));      // Normal
    trigger_event(m, make_event(SoundId::TowerFire));      // Normal
    REQUIRE(count_active(m) == 3);

    // Pool is full; this must steal one of the Normal voices, never Boss.
    trigger_event(m, make_event(SoundId::TowerAbility)); // High

    REQUIRE(count_active(m) == 3);
    int boss_count = 0;
    for (u32 i = 0; i < m.voice_limit; ++i) {
        if (m.voices[i].active && m.voices[i].source == SoundId::BossTelegraph) {
            ++boss_count;
            REQUIRE(m.voices[i].priority == Priority::Protected);
        }
    }
    REQUIRE(boss_count == 1);

    bool ability_present = false;
    for (u32 i = 0; i < m.voice_limit; ++i)
        if (m.voices[i].active && m.voices[i].source == SoundId::TowerAbility) ability_present = true;
    REQUIRE(ability_present);
}

TEST_CASE("a pool saturated with Protected voices drops new events rather than stealing",
          "[audio][voices][priority]") {
    Mixer m;
    m.voice_limit = 1;
    trigger_event(m, make_event(SoundId::ObjectiveCritical)); // Protected, fills the only slot
    REQUIRE(count_active(m) == 1);

    trigger_event(m, make_event(SoundId::TowerFire)); // nothing to steal from
    REQUIRE(count_active(m) == 1);
    REQUIRE(m.voices[0].source == SoundId::ObjectiveCritical);
    REQUIRE(m.voices[0].priority == Priority::Protected);
}

TEST_CASE("ambient/bed-tier voices are stolen before Normal/High voices", "[audio][voices][priority]") {
    Mixer m;
    m.voice_limit = 2;
    trigger_event(m, make_event(SoundId::UiClick));  // Ambient
    trigger_event(m, make_event(SoundId::TowerFire)); // Normal
    REQUIRE(count_active(m) == 2);

    trigger_event(m, make_event(SoundId::TowerAbility)); // High: should steal the Ambient one
    REQUIRE(count_active(m) == 2);

    bool ui_click_survived = false, tower_fire_survived = false, ability_present = false;
    for (u32 i = 0; i < m.voice_limit; ++i) {
        if (!m.voices[i].active) continue;
        if (m.voices[i].source == SoundId::UiClick) ui_click_survived = true;
        if (m.voices[i].source == SoundId::TowerFire) tower_fire_survived = true;
        if (m.voices[i].source == SoundId::TowerAbility) ability_present = true;
    }
    REQUIRE_FALSE(ui_click_survived);
    REQUIRE(tower_fire_survived);
    REQUIRE(ability_present);
}

// ---------------------------------------------------------------------
// ChaffDissolve: density-scaled bed, one persistent voice.
// ---------------------------------------------------------------------
TEST_CASE("ChaffDissolve reuses one persistent voice instead of allocating per event",
          "[audio][voices][chaff]") {
    Mixer m;
    m.voice_limit = 64;
    for (int i = 0; i < 200; ++i) {
        trigger_event(m, make_event(SoundId::ChaffDissolve, Vec2{0, 0}, 1.0f, 1.0f,
                                     0.5f + 0.001f * static_cast<f32>(i)));
    }
    REQUIRE(count_active(m) == 1);
    REQUIRE(m.chaff_bed_slot >= 0);
    REQUIRE(m.voices[static_cast<u32>(m.chaff_bed_slot)].source == SoundId::ChaffDissolve);
}

TEST_CASE("ChaffDissolve bed gain tracks the latest event's intensity", "[audio][voices][chaff]") {
    Mixer m;
    trigger_event(m, make_event(SoundId::ChaffDissolve, Vec2{0, 0}, 1.0f, 1.0f, 0.1f));
    const f32 low_target = m.voices[static_cast<u32>(m.chaff_bed_slot)].target_gain;

    trigger_event(m, make_event(SoundId::ChaffDissolve, Vec2{0, 0}, 1.0f, 1.0f, 1.0f));
    const f32 high_target = m.voices[static_cast<u32>(m.chaff_bed_slot)].target_gain;

    REQUIRE(high_target > low_target);
}

TEST_CASE("ChaffDissolve bed fades out and frees its slot once unrefreshed", "[audio][voices][chaff]") {
    Mixer m;
    trigger_event(m, make_event(SoundId::ChaffDissolve, Vec2{0, 0}, 1.0f, 1.0f, 1.0f));
    REQUIRE(count_active(m) == 1);

    // Advance well past the silence-fade threshold without another
    // ChaffDissolve event arriving.
    for (int i = 0; i < 400; ++i) pump(m, 0.01f); // 4 simulated seconds

    REQUIRE(count_active(m) == 0);
    REQUIRE(m.chaff_bed_slot == -1);
}

// ---------------------------------------------------------------------
// Envelope / voice lifetime.
// ---------------------------------------------------------------------
TEST_CASE("a one-shot voice frees itself once its duration elapses", "[audio][voices][envelope]") {
    Mixer m;
    trigger_event(m, make_event(SoundId::TowerPlace)); // short patch, duration ~0.14s
    REQUIRE(count_active(m) == 1);

    for (int i = 0; i < 30; ++i) pump(m, 0.01f); // 0.3s, comfortably past duration
    REQUIRE(count_active(m) == 0);
}

// ---------------------------------------------------------------------
// Numeric sanity: pan/gain must never be NaN or out-of-range, including
// for pathological inputs.
// ---------------------------------------------------------------------
TEST_CASE("compute_pan stays in [-1,1] and finite for normal input", "[audio][pan]") {
    const f32 p = compute_pan(10.0f, 0.0f);
    REQUIRE(std::isfinite(p));
    REQUIRE(p <= 1.0f);
    REQUIRE(p >= -1.0f);
}

TEST_CASE("compute_pan clamps a far off-screen position without NaN", "[audio][pan]") {
    REQUIRE(compute_pan(1.0e6f, 0.0f) == 1.0f);
    REQUIRE(compute_pan(-1.0e6f, 0.0f) == -1.0f);
    REQUIRE(std::isfinite(compute_pan(1.0e30f, 0.0f)));
}

TEST_CASE("compute_pan never propagates NaN/Inf input", "[audio][pan]") {
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();
    REQUIRE(std::isfinite(compute_pan(nan, 0.0f)));
    REQUIRE(std::isfinite(compute_pan(0.0f, nan)));
    REQUIRE(std::isfinite(compute_pan(inf, 0.0f)));
    REQUIRE(std::isfinite(compute_pan(-inf, 5.0f)));
}

TEST_CASE("envelope_gain stays within [0,1] across a voice's lifetime", "[audio][envelope]") {
    Voice v;
    v.duration = 0.2f;
    v.attack = 0.02f;
    for (f32 age = 0.0f; age <= 0.3f; age += 0.01f) {
        v.age = age;
        const f32 g = envelope_gain(v);
        REQUIRE(std::isfinite(g));
        REQUIRE(g >= 0.0f);
        REQUIRE(g <= 1.0f);
    }
}

TEST_CASE("trigger_event sanitizes NaN/out-of-range gain, pitch and intensity", "[audio][voices][sanity]") {
    Mixer m;
    AudioEvent ev = make_event(SoundId::TowerAbility);
    ev.gain = std::numeric_limits<f32>::quiet_NaN();
    ev.pitch = std::numeric_limits<f32>::infinity();
    ev.intensity = -5.0f;
    ev.position = Vec2{std::numeric_limits<f32>::quiet_NaN(), 0.0f};

    trigger_event(m, ev);
    REQUIRE(count_active(m) == 1);
    for (u32 i = 0; i < m.voice_limit; ++i) {
        if (!m.voices[i].active) continue;
        REQUIRE(std::isfinite(m.voices[i].freq));
        REQUIRE(std::isfinite(m.voices[i].gain));
        REQUIRE(std::isfinite(m.voices[i].target_gain));
        REQUIRE(std::isfinite(m.voices[i].position.x));
    }
}

TEST_CASE("pump handles zero dt without producing NaN or blowing up gain", "[audio][envelope][sanity]") {
    Mixer m;
    trigger_event(m, make_event(SoundId::ChaffDissolve, Vec2{0, 0}, 1.0f, 1.0f, 0.7f));
    const f32 gain_before = m.voices[static_cast<u32>(m.chaff_bed_slot)].gain;

    pump(m, 0.0f);

    const f32 gain_after = m.voices[static_cast<u32>(m.chaff_bed_slot)].gain;
    REQUIRE(std::isfinite(gain_after));
    REQUIRE(gain_after == gain_before); // zero dt => no change
}

TEST_CASE("update_listener_and_music never produces NaN for intensity 0, 1, or zero dt",
          "[audio][music][sanity]") {
    Mixer m;
    update_listener_and_music(m, Vec2{0, 0}, 0.0f, 0.0f);
    REQUIRE(std::isfinite(m.music_intensity.load()));
    update_listener_and_music(m, Vec2{0, 0}, 1.0f, 0.0f);
    REQUIRE(std::isfinite(m.music_intensity.load()));
    update_listener_and_music(m, Vec2{1e6f, -1e6f}, 1.0f, 1.0f / 60.0f);
    REQUIRE(std::isfinite(m.music_intensity.load()));
    REQUIRE(m.music_intensity.load() >= 0.0f);
    REQUIRE(m.music_intensity.load() <= 1.0f);
}

TEST_CASE("adaptive music intensity audibly (numerically) tracks horde_intensity01", "[audio][music]") {
    Mixer m;
    for (int i = 0; i < 120; ++i) update_listener_and_music(m, Vec2{0, 0}, 1.0f, 1.0f / 60.0f);
    const f32 settled_high = m.music_intensity.load();
    REQUIRE(settled_high > 0.9f);

    for (int i = 0; i < 120; ++i) update_listener_and_music(m, Vec2{0, 0}, 0.0f, 1.0f / 60.0f);
    const f32 settled_low = m.music_intensity.load();
    REQUIRE(settled_low < 0.1f);
}

// ---------------------------------------------------------------------
// null_device: mixer logic runs, real SDL device APIs are never touched.
// We cannot observe "SDL was not called" from the public API directly (by
// design there is no such getter), so this checks the documented behaviour
// the header promises instead: ready() is true, event plumbing still works
// (dropped_events, voices triggering via update()'s pump), and nothing
// crashes or hangs across repeated frames/instances.
// ---------------------------------------------------------------------
TEST_CASE("null_device engine is ready and processes events without opening real audio",
          "[audio][engine][null_device]") {
    AudioEngine engine;
    AudioConfig cfg;
    cfg.null_device = true;
    cfg.max_voices = 8;
    REQUIRE(engine.init(cfg));
    REQUIRE(engine.ready());

    for (int frame = 0; frame < 100; ++frame) {
        engine.post(make_event(SoundId::TowerFire));
        engine.post(make_event(SoundId::ChaffDissolve, Vec2{0, 0}, 1.0f, 1.0f, 0.6f));
        engine.update(Vec2{static_cast<f32>(frame), 0.0f}, 0.5f, 1.0f / 60.0f);
    }
    // Reaching here without crashing/hanging across 100 simulated frames is
    // the load-bearing assertion; dropped_events staying well under total
    // posted confirms the ring buffer is actually being drained by update().
    REQUIRE(engine.dropped_events() < 200);
    engine.shutdown();
    REQUIRE_FALSE(engine.ready());
}

TEST_CASE("null_device engine survives repeated init/shutdown cycles", "[audio][engine][null_device]") {
    for (int i = 0; i < 5; ++i) {
        AudioEngine engine;
        AudioConfig cfg;
        cfg.null_device = true;
        REQUIRE(engine.init(cfg));
        engine.post(make_event(SoundId::UiClick));
        engine.update(Vec2{0, 0}, 0.2f, 1.0f / 60.0f);
        engine.shutdown();
    }
}

TEST_CASE("a real (non-null_device) engine initializes without crashing or hanging",
          "[audio][engine][real_device]") {
    // Exercises the actual SDL_OpenAudioDevice path (or its graceful
    // silent-fallback when no hardware/driver is present, e.g. on a CI
    // box) — the one path the unit tests can't reach through null_device.
    AudioEngine engine;
    AudioConfig cfg; // null_device defaults to false
    REQUIRE(engine.init(cfg));
    REQUIRE(engine.ready());

    for (int frame = 0; frame < 30; ++frame) {
        engine.post(make_event(SoundId::TowerPlace));
        engine.post(make_event(SoundId::ChaffDissolve, Vec2{0, 0}, 1.0f, 1.0f, 0.4f));
        engine.update(Vec2{0, 0}, 0.4f, 1.0f / 60.0f);
    }
    engine.shutdown();
    REQUIRE_FALSE(engine.ready());
}

TEST_CASE("set_master_gain and set_paused do not crash a null_device engine", "[audio][engine]") {
    AudioEngine engine;
    AudioConfig cfg;
    cfg.null_device = true;
    REQUIRE(engine.init(cfg));
    engine.set_master_gain(std::numeric_limits<f32>::quiet_NaN());
    engine.set_master_gain(-3.0f);
    engine.set_master_gain(1.5f);
    engine.set_paused(true);
    engine.update(Vec2{0, 0}, 0.5f, 1.0f / 60.0f);
    engine.set_paused(false);
    engine.shutdown();
}
