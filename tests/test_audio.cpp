// Procedural audio: the mixer's contracts (pools, priority, limiter, beds),
// every patch rendering cleanly, the scene's horde policy (aggregation, rate
// limits, culling, loudness growth), and the offline audition end to end.
// None of this needs an audio device: Mixer and SoundScape are SDL-free.
#include "app/AudioRender.h"
#include "audio/Mixer.h"
#include "audio/Patches.h"
#include "audio/SoundScape.h"
#include "sim/CombatEvents.h"
#include "sim/CombatMetrics.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

using namespace immune;
using namespace immune::audio;

namespace {

constexpr f32 kRate = 48000.0f;

struct Level {
    f32 peak = 0.0f;
    f32 left = 0.0f;   // summed |L|
    f32 right = 0.0f;  // summed |R|
    bool finite = true;
};

std::unique_ptr<Mixer> make_mixer(f32 master = 1.0f) {
    auto m = std::make_unique<Mixer>();
    m->init(kRate);
    Command c;
    c.type = CommandType::MasterGain;
    c.gain = master;
    m->apply(c);
    return m;
}

Level render(Mixer& m, f32 seconds) {
    Level lv;
    std::vector<f32> buf(1024 * 2);
    u32 left = static_cast<u32>(seconds * kRate);
    while (left > 0) {
        const u32 n = std::min<u32>(left, 1024);
        m.render(buf.data(), n);
        for (u32 i = 0; i < n; ++i) {
            const f32 l = buf[i * 2], r = buf[i * 2 + 1];
            if (!std::isfinite(l) || !std::isfinite(r)) lv.finite = false;
            lv.peak = std::max({lv.peak, std::fabs(l), std::fabs(r)});
            lv.left += std::fabs(l);
            lv.right += std::fabs(r);
        }
        left -= n;
    }
    return lv;
}

Command voice(SoundId id, f32 gain = 1.0f, f32 pan = 0.0f, f32 delay = 0.0f) {
    Command c;
    c.type = CommandType::Voice;
    c.id = static_cast<u8>(id);
    c.gain = gain;
    c.pan = pan;
    c.delay = delay;
    return c;
}

std::vector<Command> drain(CommandRing& ring) {
    std::vector<Command> out;
    Command c;
    while (ring.pop(c)) out.push_back(c);
    return out;
}

u32 count_voices(const std::vector<Command>& cs, SoundId id) {
    return static_cast<u32>(std::count_if(cs.begin(), cs.end(), [id](const Command& c) {
        return c.type == CommandType::Voice && c.id == static_cast<u8>(id);
    }));
}

u32 count_type(const std::vector<Command>& cs, CommandType t) {
    return static_cast<u32>(std::count_if(cs.begin(), cs.end(), [t](const Command& c) { return c.type == t; }));
}

const Command* last_bed(const std::vector<Command>& cs, BedId id) {
    const Command* found = nullptr;
    for (const Command& c : cs) {
        if (c.type == CommandType::Bed && c.id == static_cast<u8>(id)) found = &c;
    }
    return found;
}

sim::CombatEvent event(sim::CombatEventType type, Vec2 at, TowerType source = TowerType::Count,
                       PathogenFamily family = PathogenFamily::Count, f32 magnitude = 1.0f) {
    sim::CombatEvent e;
    e.type = type;
    e.origin = at;
    e.secondary = at;
    e.source = source;
    e.target_family = family;
    e.magnitude = magnitude;
    return e;
}

struct Scene {
    std::unique_ptr<CommandRing> ring = std::make_unique<CommandRing>();
    SoundScape scape;
    HordeState horde;
    Scene() {
        scape.set_output(ring.get());
        scape.set_view(Rect{Vec2{-50.0f, -50.0f}, Vec2{50.0f, 50.0f}});
        horde.live = true;
    }
    std::vector<Command> frame(const std::vector<sim::CombatEvent>& events, f32 dt = 1.0f / 60.0f) {
        scape.on_combat_events(events.data(), events.size());
        scape.update(horde, dt);
        return drain(*ring);
    }
};

} // namespace

// ---- Ring ---------------------------------------------------------------------

TEST_CASE("the command ring is FIFO and drops rather than blocks when full", "[audio][ring]") {
    auto ring = std::make_unique<CommandRing>();
    for (u32 i = 0; i < CommandRing::kCapacity; ++i) {
        Command c;
        c.type = CommandType::Voice;
        c.gain = static_cast<f32>(i);
        REQUIRE(ring->push(c));
    }
    CHECK_FALSE(ring->push(Command{}));
    CHECK(ring->dropped() == 1);
    Command out;
    for (u32 i = 0; i < CommandRing::kCapacity; ++i) {
        REQUIRE(ring->pop(out));
        REQUIRE(out.gain == static_cast<f32>(i));
    }
    CHECK_FALSE(ring->pop(out));
}

// ---- Mixer --------------------------------------------------------------------

TEST_CASE("an idle mixer renders exact silence", "[audio][mixer]") {
    auto m = make_mixer();
    const Level lv = render(*m, 0.5f);
    CHECK(lv.peak == 0.0f);
}

TEST_CASE("every patch renders audible, finite, bounded and ends on time", "[audio][mixer][patches]") {
    for (u32 i = 0; i < kSoundCount; ++i) {
        const auto id = static_cast<SoundId>(i);
        INFO("patch " << sound_name(id));
        auto m = make_mixer();
        m->apply(voice(id));
        const Level lv = render(*m, patch(id).length + 0.05f);
        CHECK(lv.finite);
        CHECK(lv.peak > 0.005f);
        CHECK(lv.peak <= 1.0f);
        CHECK(m->instances(id) == 0);
        CHECK(m->stats().active_voices == 0);
    }
}

TEST_CASE("every grain kind renders and is over within a fifth of a second", "[audio][mixer]") {
    for (u32 k = 0; k < kGrainKindCount; ++k) {
        INFO("grain " << grain_shape(static_cast<GrainKind>(k)).name);
        auto m = make_mixer();
        Command c;
        c.type = CommandType::Grain;
        c.id = static_cast<u8>(k);
        c.pitch = 600.0f;
        c.gain = 1.0f;
        m->apply(c);
        const Level lv = render(*m, 0.5f);
        CHECK(lv.finite);
        CHECK(lv.peak > 0.005f);
        CHECK(m->stats().active_grains == 0);
    }
}

TEST_CASE("a patch's instance cap steals its own oldest voice", "[audio][mixer]") {
    auto m = make_mixer();
    const u8 cap = patch(SoundId::ShooterFire).max_instances;
    for (u32 i = 0; i < 10u; ++i) m->apply(voice(SoundId::ShooterFire));
    CHECK(m->instances(SoundId::ShooterFire) <= cap);
    CHECK(m->stats().voices_stolen >= 10u - cap);
}

TEST_CASE("a full pool never steals a protected voice and rejects lower priorities", "[audio][mixer]") {
    auto m = make_mixer();
    m->apply(voice(SoundId::Thump));
    m->apply(voice(SoundId::TowerLost));
    // Fill every slot with Normal-or-better voices, up to each patch's cap.
    u32 started = 2;
    for (u32 i = 0; i < kSoundCount && started < Mixer::kMaxVoices; ++i) {
        const auto id = static_cast<SoundId>(i);
        const Patch& p = patch(id);
        if (p.priority == Priority::Low || p.priority == Priority::Protected) continue;
        for (u32 k = 0; k < p.max_instances && started < Mixer::kMaxVoices; ++k, ++started) m->apply(voice(id));
    }
    REQUIRE(started == Mixer::kMaxVoices);
    m->apply(voice(SoundId::ShooterFire));  // Low: nothing it may steal
    CHECK(m->stats().voices_rejected == 1);
    CHECK(m->instances(SoundId::ShooterFire) == 0);
    for (u32 i = 0; i < 20; ++i) m->apply(voice(SoundId::Sparkle));  // High: steals, but not Protected
    CHECK(m->instances(SoundId::Thump) == 1);
    CHECK(m->instances(SoundId::TowerLost) == 1);
}

TEST_CASE("pan places a voice in the stereo field", "[audio][mixer]") {
    auto left = make_mixer();
    left->apply(voice(SoundId::UiChime, 1.0f, -1.0f));
    const Level l = render(*left, 0.4f);
    CHECK(l.left > 10.0f * l.right);

    // A dry patch: the room is deliberately decorrelated left/right.
    REQUIRE(patch(SoundId::UiHover).reverb == 0.0f);
    auto centre = make_mixer();
    centre->apply(voice(SoundId::UiHover, 1.0f, 0.0f));
    const Level c = render(*centre, 0.2f);
    CHECK(c.left > 0.0f);
    CHECK(std::fabs(c.left - c.right) < 0.01f * c.left);
}

TEST_CASE("the master limiter keeps a pile-up of loud voices inside full scale", "[audio][mixer]") {
    auto m = make_mixer(1.0f);
    const SoundId loud[] = {SoundId::MucusBurst, SoundId::Thump, SoundId::TowerLost, SoundId::ClotThud,
                            SoundId::ScarBreak, SoundId::Engulf, SoundId::Harpoon, SoundId::CellDeath};
    for (u32 i = 0; i < 64; ++i) m->apply(voice(loud[i % 8], 4.0f, (i % 3) - 1.0f));
    const Level lv = render(*m, 1.0f);
    CHECK(lv.finite);
    CHECK(lv.peak <= 1.0f);
    CHECK(m->stats().limiter_gain < 1.0f);
}

TEST_CASE("non-finite commands are refused at the door", "[audio][mixer]") {
    auto m = make_mixer();
    Command c = voice(SoundId::UiClick);
    c.gain = std::numeric_limits<f32>::quiet_NaN();
    m->apply(c);
    c = voice(SoundId::UiClick);
    c.pan = std::numeric_limits<f32>::infinity();
    m->apply(c);
    CHECK(m->stats().voices_started == 0);
    CHECK(render(*m, 0.1f).peak == 0.0f);
}

TEST_CASE("a delayed voice starts late", "[audio][mixer]") {
    auto m = make_mixer();
    m->apply(voice(SoundId::UiChime, 1.0f, 0.0f, 0.1f));
    CHECK(render(*m, 0.09f).peak == 0.0f);
    CHECK(render(*m, 0.1f).peak > 0.01f);
}

TEST_CASE("beds follow their target and fall silent again", "[audio][mixer][beds]") {
    for (u32 b = 0; b < kBedCount; ++b) {
        const auto id = static_cast<BedId>(b);
        INFO("bed " << b);
        auto m = make_mixer();
        Command c;
        c.type = CommandType::Bed;
        c.id = static_cast<u8>(b);
        c.gain = 1.0f;
        c.param = 1.0f;
        m->apply(c);
        const Level on = render(*m, 3.0f);
        CHECK(on.finite);
        CHECK(m->bed_level(id) > 0.9f);
        CHECK(on.peak > 0.005f);
        CHECK(on.peak < 0.5f);  // a floor under the mix, never the foreground
        c.gain = 0.0f;
        m->apply(c);
        render(*m, 7.0f);   // the slowest bed (murmur) releases with a 0.6 s time constant
        CHECK(m->bed_level(id) < 1e-3f);
        CHECK(render(*m, 0.5f).peak < 1e-3f);
    }
}

TEST_CASE("StopAll silences everything", "[audio][mixer]") {
    auto m = make_mixer();
    for (u32 i = 0; i < 8; ++i) m->apply(voice(SoundId::Lament));
    Command bed;
    bed.type = CommandType::Bed;
    bed.id = static_cast<u8>(BedId::Murmur);
    bed.gain = 1.0f;
    m->apply(bed);
    render(*m, 0.2f);
    Command stop;
    stop.type = CommandType::StopAll;
    m->apply(stop);
    render(*m, 3.0f);
    CHECK(m->stats().active_voices == 0);
    CHECK(render(*m, 0.5f).peak < 1e-3f);
}

// ---- Scene: the horde policy -------------------------------------------------

TEST_CASE("a thousand deaths on one frame post a bounded handful of grains", "[audio][scene]") {
    Scene s;
    std::vector<sim::CombatEvent> evs;
    for (u32 i = 0; i < 1000; ++i) {
        evs.push_back(event(sim::CombatEventType::ChaffDeath, Vec2{-40.0f + 0.08f * i, 10.0f}, TowerType::Count,
                            static_cast<PathogenFamily>(i % kFamilyCount)));
    }
    const auto out = s.frame(evs);
    const u32 grains = count_type(out, CommandType::Grain);
    CHECK(grains >= 4);
    CHECK(grains <= SoundScape::kGrainBudget);
    CHECK(count_type(out, CommandType::Voice) == 0);
    // Every family that died is heard.
    for (u32 k = 0; k < kFamilyCount; ++k) {
        INFO("family " << k);
        const auto kind = static_cast<u8>(k);  // death grain kinds share the family order
        CHECK(std::any_of(out.begin(), out.end(),
                          [kind](const Command& c) { return c.type == CommandType::Grain && c.id == kind; }));
    }
}

TEST_CASE("sustained slaughter is rate-limited and the fizz bed takes over", "[audio][scene]") {
    Scene s;
    u32 grains = 0;
    std::vector<Command> last;
    for (u32 f = 0; f < 120; ++f) {   // two seconds, 3000 deaths a second
        std::vector<sim::CombatEvent> evs;
        for (u32 i = 0; i < 50; ++i) {
            evs.push_back(event(sim::CombatEventType::ChaffDeath, Vec2{-30.0f + i, 0.0f}, TowerType::Count,
                                PathogenFamily::Virus));
        }
        last = s.frame(evs);
        grains += count_type(last, CommandType::Grain);
    }
    CHECK(grains <= 2 * 110 + 12 + 2);   // rate * seconds + burst (+ rounding)
    CHECK(grains >= 150);
    const Command* fizz = last_bed(last, BedId::Fizz);
    REQUIRE(fizz != nullptr);
    CHECK(fizz->gain > 0.4f);

    // A trickle leaves the fizz bed silent.
    Scene calm;
    std::vector<Command> out;
    for (u32 f = 0; f < 120; ++f) {
        std::vector<sim::CombatEvent> evs;
        if (f % 10 == 0) evs.push_back(event(sim::CombatEventType::ChaffDeath, Vec2{0.0f, 0.0f}, TowerType::Count,
                                             PathogenFamily::Virus));
        out = calm.frame(evs);
    }
    fizz = last_bed(out, BedId::Fizz);
    REQUIRE(fizz != nullptr);
    CHECK(fizz->gain == 0.0f);
}

TEST_CASE("off-screen events are culled but a lost tower is always heard", "[audio][scene]") {
    Scene s;
    auto out = s.frame({event(sim::CombatEventType::ChaffDeath, Vec2{500.0f, 0.0f}, TowerType::Count,
                              PathogenFamily::Virus),
                        event(sim::CombatEventType::PathogenLatch, Vec2{-500.0f, 0.0f})});
    CHECK(count_type(out, CommandType::Grain) == 0);
    CHECK(count_voices(out, SoundId::Latch) == 0);
    CHECK(s.scape.stats().culled == 2);

    out = s.frame({event(sim::CombatEventType::TowerDestroyed, Vec2{500.0f, 0.0f}, TowerType::Macrophage)});
    REQUIRE(count_voices(out, SoundId::TowerLost) == 1);
    for (const Command& c : out) {
        if (c.type == CommandType::Voice) {
            CHECK(c.gain > 0.0f);
            CHECK(c.pan > 0.5f);  // still on the side it happened
        }
    }
}

TEST_CASE("pan follows the event's screen x", "[audio][scene]") {
    Scene s;
    auto out = s.frame({event(sim::CombatEventType::SwarmerDeath, Vec2{-45.0f, 0.0f}, TowerType::Neutrophil)});
    REQUIRE(count_voices(out, SoundId::CellDeath) == 1);
    CHECK(out[0].pan < -0.4f);
}

TEST_CASE("only a cell the horde killed plays the loss sound", "[audio][scene]") {
    const auto death = [](TowerType source, sim::CombatRetirement cause) {
        sim::CombatEvent e = event(sim::CombatEventType::SwarmerDeath, Vec2{0.0f, 0.0f}, source);
        e.cause = static_cast<u8>(cause);
        return e;
    };
    Scene s;
    // A bomber detonating and a builder spent on its wall sound as that
    // attack; a released unit ageing out is not a loss.
    auto out = s.frame({event(sim::CombatEventType::Explosion, Vec2{0.0f, 0.0f}, TowerType::GobletCell),
                        death(TowerType::GobletCell, sim::CombatRetirement::Consumed),
                        event(sim::CombatEventType::ScarBuilt, Vec2{10.0f, 0.0f}, TowerType::Fibroblast),
                        death(TowerType::Fibroblast, sim::CombatRetirement::Consumed),
                        death(TowerType::Neutrophil, sim::CombatRetirement::Expired)});
    CHECK(count_voices(out, SoundId::MucusBurst) == 1);
    CHECK(count_voices(out, SoundId::Knock) == 1);
    CHECK(count_voices(out, SoundId::CellDeath) == 0);

    out = s.frame({death(TowerType::GobletCell, sim::CombatRetirement::Damage)});
    CHECK(count_voices(out, SoundId::CellDeath) == 1);
}

TEST_CASE("a crowd is louder than one, but only logarithmically", "[audio][scene]") {
    Scene one;
    auto a = one.frame({event(sim::CombatEventType::PathogenLatch, Vec2{0.0f, 0.0f})});
    REQUIRE(count_voices(a, SoundId::Latch) == 1);
    const f32 single = a[0].gain;

    Scene crowd;
    std::vector<sim::CombatEvent> evs(256, event(sim::CombatEventType::PathogenLatch, Vec2{0.0f, 0.0f}));
    auto b = crowd.frame(evs);
    const u32 n = count_voices(b, SoundId::Latch);
    CHECK(n >= 1);
    CHECK(n <= 2);
    f32 g = 0.0f;
    for (const Command& c : b) {
        if (c.type == CommandType::Voice) g = std::max(g, c.gain);
    }
    CHECK(g > single * 1.2f);
    CHECK(g <= single * 1.6f + 1e-4f);   // 256 events, at most +4 dB
}

TEST_CASE("leaks become rate-limited objective thumps", "[audio][scene]") {
    Scene s;
    s.horde.leaked_total = 40;   // a level in progress: the baseline, not 40 hits
    CHECK(count_voices(s.frame({}), SoundId::Thump) == 0);
    s.horde.leaked_total = 45;
    auto out = s.frame({});
    CHECK(count_voices(out, SoundId::Thump) == 1);
    CHECK(count_voices(out, SoundId::Warn) == 1);
    u32 thumps = 0;
    for (u32 f = 0; f < 60; ++f) {   // a leak every frame for a second
        ++s.horde.leaked_total;
        thumps += count_voices(s.frame({}), SoundId::Thump);
    }
    CHECK(thumps >= 2);
    CHECK(thumps <= 4);
}

TEST_CASE("UI cues are rate-limited and centred", "[audio][scene]") {
    Scene s;
    for (u32 i = 0; i < 10; ++i) s.scape.cue(Cue::UiHover);
    auto out = s.frame({});
    CHECK(count_voices(out, SoundId::UiHover) == 1);
    out = s.frame({}, 0.05f);
    s.scape.cue(Cue::UiHover);
    out = s.frame({});
    CHECK(count_voices(out, SoundId::UiHover) == 1);
    s.scape.cue(Cue::UiDeny);
    out = s.frame({});
    CHECK(count_voices(out, SoundId::UiDeny) == 2);  // the two-note "uh-uh"
    for (const Command& c : out) CHECK(c.pan == 0.0f);
}

TEST_CASE("beds fade out whenever the sim is not running", "[audio][scene]") {
    Scene s;
    s.horde.visible = 2000;
    s.horde.latched = 50;
    auto out = s.frame({});
    REQUIRE(last_bed(out, BedId::Murmur) != nullptr);
    CHECK(last_bed(out, BedId::Murmur)->gain > 0.5f);
    CHECK(last_bed(out, BedId::Gnaw)->gain > 0.5f);
    s.horde.live = false;
    out = s.frame({});
    for (u32 b = 0; b < kBedCount; ++b) {
        const Command* c = last_bed(out, static_cast<BedId>(b));
        REQUIRE(c != nullptr);
        CHECK(c->gain == 0.0f);
    }
}

TEST_CASE("every combat event type is heard or deliberately silent", "[audio][scene]") {
    // A regression guard like the particle layer's: a new CombatEventType
    // must be either given a sound or added to this list on purpose.
    using T = sim::CombatEventType;
    const T silent[] = {T::ProjectileExpired, T::Freeze, T::Shatter, T::FluidSplash, T::ScarDestroyed};
    for (u8 t = 0; t < static_cast<u8>(T::Count); ++t) {
        const auto type = static_cast<T>(t);
        INFO("CombatEventType " << static_cast<int>(t));
        Scene s;
        sim::CombatEvent e = event(type, Vec2{0.0f, 0.0f}, TowerType::Neutrophil, PathogenFamily::Virus);
        if (type == T::ScarDestroyed) e.magnitude = 0.0f;   // expired, not chewed through
        const auto out = s.frame({e});
        const bool heard = count_type(out, CommandType::Voice) + count_type(out, CommandType::Grain) > 0;
        const bool expect_silent = std::find(std::begin(silent), std::end(silent), type) != std::end(silent);
        CHECK(heard != expect_silent);
    }
}

TEST_CASE("presence counts the crowd near the view, sampled at any size", "[audio][scene]") {
    std::vector<f32> x, y;
    for (u32 i = 0; i < 20000; ++i) {
        x.push_back(i < 10000 ? 10.0f : 900.0f);   // half on screen, right of centre
        y.push_back(0.0f);
    }
    HordeState h;
    measure_presence(x.data(), y.data(), x.size(), Rect{Vec2{-50.0f, -50.0f}, Vec2{50.0f, 50.0f}}, h);
    CHECK(h.visible >= 9000);
    CHECK(h.visible <= 11000);
    CHECK(h.visible_pan > 0.0f);
}

// ---- End to end -----------------------------------------------------------------

TEST_CASE("the audition renders clean audio well inside the real-time budget", "[audio][render]") {
    const app::AudioRenderReport r = app::render_audition(48000.0f, nullptr);
    CHECK(r.non_finite_samples == 0);
    CHECK(r.ring_dropped == 0);
    const u64 samples = static_cast<u64>(r.seconds * 48000.0 * 2.0);
    CHECK(r.clipped_samples * 1000 < samples);   // under 0.1%
    for (const app::AudioSegmentReport& s : r.segments) {
        INFO("segment " << s.name << " peak " << s.peak << " load " << s.load);
        CHECK(s.peak <= 1.0f);
        if (s.name != "tail") CHECK(s.peak > 0.01f);
        CHECK(s.max_voices <= Mixer::kMaxVoices);
        // Generous: the mixer typically needs a few percent of one core.
        CHECK(s.load < 0.5);
    }
}
