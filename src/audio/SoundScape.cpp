#include "audio/SoundScape.h"

#include "sim/CombatMetrics.h"

#include <algorithm>
#include <cmath>

namespace immune::audio {

namespace {

/// Combat categories, in the order a frame spends its voice budget: losses
/// first, then the horde hurting you, then your own attacks.
enum class Cat : u8 {
    TowerLost = 0,
    CellDeath,
    ScarBreak,
    Harpoon,
    Latch,
    Glug,
    Spit,
    Chew,
    Sizzle,
    MucusBurst,
    Stab,
    Engulf,
    Knock,
    ShooterFire,
    Zap,
    RoundHit,   ///< grains
    Count,
    None = 255,
};
constexpr u32 kCatCount = static_cast<u32>(Cat::Count);

struct Rule {
    SoundId sound = SoundId::UiClick;
    bool grain = false;
    GrainKind grain_kind = GrainKind::Tick;
    f32 grain_hz = 0.0f;
    /// Token bucket: sustained sounds per second, and how many may bank up.
    f32 rate = 4.0f;
    f32 burst = 2.0f;
    u8 per_frame = 1;
    f32 gain = 1.0f;
    /// Extra gain per doubling of events folded into one sound, and its cap.
    /// 0.2 per doubling is ~+1.6 dB: a crowd reads as bigger, never as louder
    /// than the one sound that matters.
    f32 growth = 0.2f;
    f32 max_boost = 1.5f;
    /// Audibility kept however far off screen. 0 culls.
    f32 floor = 0.0f;
    /// Pentatonic degrees above the patch's base note to pick from; empty =
    /// unpitched, detuned by `jitter` semitones instead.
    i8 degrees[5] = {0, 0, 0, 0, 0};
    u8 degree_count = 0;
    f32 jitter = 0.0f;
};

const Rule* rules() {
    static const Rule* table = [] {
        static Rule t[kCatCount];
        const auto at = [](Cat c) -> Rule& { return t[static_cast<u32>(c)]; };
        { Rule& r = at(Cat::TowerLost);  r.sound = SoundId::TowerLost;  r.rate = 2; r.burst = 2; r.per_frame = 1; r.max_boost = 1.4f; r.floor = 0.5f; }
        { Rule& r = at(Cat::CellDeath);  r.sound = SoundId::CellDeath;  r.rate = 5; r.burst = 3; r.per_frame = 2; r.growth = 0.22f; r.max_boost = 1.6f; r.floor = 0.15f;
          r.degrees[0] = 2; r.degrees[1] = 3; r.degrees[2] = 4; r.degree_count = 3; }
        { Rule& r = at(Cat::ScarBreak);  r.sound = SoundId::ScarBreak;  r.rate = 3; r.burst = 2; r.per_frame = 1; r.max_boost = 1.4f; r.floor = 0.3f; r.jitter = 1.0f; }
        { Rule& r = at(Cat::Harpoon);    r.sound = SoundId::Harpoon;    r.rate = 4; r.burst = 2; r.per_frame = 1; r.floor = 0.1f; r.jitter = 0.5f; }
        { Rule& r = at(Cat::Latch);      r.sound = SoundId::Latch;      r.rate = 6; r.burst = 3; r.per_frame = 2; r.growth = 0.25f; r.max_boost = 1.6f; r.jitter = 1.0f; }
        { Rule& r = at(Cat::Glug);       r.sound = SoundId::Glug;       r.rate = 4; r.burst = 2; r.per_frame = 1; r.max_boost = 1.4f; r.jitter = 0.7f; }
        { Rule& r = at(Cat::Spit);       r.sound = SoundId::Spit;       r.rate = 8; r.burst = 3; r.per_frame = 2; r.jitter = 1.5f; }
        { Rule& r = at(Cat::Chew);       r.sound = SoundId::Chew;       r.rate = 6; r.burst = 2; r.per_frame = 1; r.growth = 0.25f; r.max_boost = 1.6f; r.jitter = 1.0f; }
        { Rule& r = at(Cat::Sizzle);     r.sound = SoundId::Sizzle;     r.rate = 5; r.burst = 2; r.per_frame = 1; r.jitter = 2.0f; }
        { Rule& r = at(Cat::MucusBurst); r.sound = SoundId::MucusBurst; r.rate = 3; r.burst = 2; r.per_frame = 1; r.floor = 0.1f; r.jitter = 1.0f; }
        { Rule& r = at(Cat::Stab);       r.sound = SoundId::Stab;       r.rate = 6; r.burst = 3; r.per_frame = 2;
          r.degrees[0] = 0; r.degrees[1] = 2; r.degrees[2] = 3; r.degrees[3] = 4; r.degree_count = 4; }
        { Rule& r = at(Cat::Engulf);     r.sound = SoundId::Engulf;     r.rate = 5; r.burst = 2; r.per_frame = 1;
          r.degrees[0] = 0; r.degrees[1] = 3; r.degree_count = 2; }
        { Rule& r = at(Cat::Knock);      r.sound = SoundId::Knock;      r.rate = 6; r.burst = 3; r.per_frame = 2; r.growth = 0.15f; r.max_boost = 1.4f; r.floor = 0.05f;
          r.degrees[0] = 1; r.degrees[1] = 2; r.degrees[2] = 3; r.degree_count = 3; }
        { Rule& r = at(Cat::ShooterFire); r.sound = SoundId::ShooterFire; r.rate = 9; r.burst = 3; r.per_frame = 2; r.growth = 0.18f; r.max_boost = 1.6f;
          r.degrees[0] = 0; r.degrees[1] = 2; r.degrees[2] = 3; r.degree_count = 3; }
        { Rule& r = at(Cat::Zap);        r.sound = SoundId::Zap;        r.rate = 6; r.burst = 2; r.per_frame = 1; r.jitter = 1.0f; }
        { Rule& r = at(Cat::RoundHit);   r.grain = true; r.grain_kind = GrainKind::Tick; r.grain_hz = 2600.0f;
          r.rate = 24; r.burst = 6; r.per_frame = 4; r.gain = 0.11f; r.growth = 0.1f; r.max_boost = 1.3f; r.jitter = 3.0f; }
        return t;
    }();
    return table;
}

/// How each pathogen family dies, as a grain cloud. Small pathogens pop high
/// and light, big ones lower and wetter, every pitch inside the scale.
struct DeathVoice {
    GrainKind kind;
    f32 base_hz;
    i8 degrees[5];
    u8 degree_count;
    f32 gain;
};
constexpr DeathVoice kDeathVoices[kFamilyCount] = {
    /* Virus    */ {GrainKind::Bubble, kD5, {0, 1, 2, 3, 4}, 5, 0.16f},
    /* Bacteria */ {GrainKind::Pop, kD4, {2, 3, 4, 5, 6}, 5, 0.2f},
    /* Parasite */ {GrainKind::Squelch, kD3, {3, 4, 5, 6, 0}, 4, 0.24f},
    /* Spore    */ {GrainKind::Puff, kD4, {0, 1, 2, 3, 0}, 4, 0.18f},
};

// Pathogen-death grain bucket, shared by every family, and the rates the
// fizz bed crossfades over (deaths per second, audible).
constexpr f32 kDeathRate = 110.0f;
constexpr f32 kDeathBurst = 12.0f;
constexpr u32 kDeathPerFrame = 12;
constexpr f32 kDensityReference = 12.0f;  // deaths/s at which pops start thinning
constexpr f32 kFizzStart = 80.0f;
constexpr f32 kFizzFull = 1500.0f;

// Leaks into the objective: the most important sound in the game.
constexpr f32 kLeakRate = 2.5f;
constexpr f32 kLeakBurst = 2.0f;

constexpr f32 kCueCooldown[kCueCount] = {
    0.045f,  // UiHover
    0.03f,   // UiClick
    0.12f,   // UiDeny
    0.1f,    // UiOpen
    0.1f,    // UiClose
    0.04f,   // Deploy
    0.05f,   // Purchase
    0.05f,   // Ability
    0.5f,    // WaveStart
    0.5f,    // WaveClear
    1.0f,    // Victory
    1.0f,    // Defeat
};

f32 ratio_for_degree(i32 degree) { return dsp::semitones_to_ratio(static_cast<f32>(degree_semitones(degree))); }

f32 boost(const Rule& r, f32 events, f32 played) {
    if (played <= 0.0f || events <= played) return 1.0f;
    return std::min(r.max_boost, 1.0f + r.growth * std::log2(events / played));
}

f32 smoothstep01(f32 x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

Cat classify(const sim::CombatEvent& e) {
    using T = sim::CombatEventType;
    switch (e.type) {
        case T::MuzzleFlash: return Cat::ShooterFire;
        case T::ProjectileImpact:
            switch (e.source) {
                case TowerType::Macrophage: return Cat::Engulf;
                case TowerType::CytotoxicT: return Cat::Stab;
                // A builder reaching its spot; ScarBuilt is the sound of it.
                case TowerType::Fibroblast: return Cat::None;
                default:
                    // Wall impacts carry no family: only hits on a pathogen tick.
                    return e.target_family == PathogenFamily::Count ? Cat::None : Cat::RoundHit;
            }
        case T::Explosion: return Cat::MucusBurst;
        case T::BeamFired:
        case T::ChainArc:
        case T::ConePulse:
        case T::BladeSlash: return Cat::Zap;
        case T::TowerDestroyed: return Cat::TowerLost;
        // The loss sound is for cells the horde killed. A bomber detonating or
        // a builder spent on its wall already sounds as that attack, and a
        // released unit ageing out is not a loss at all. Unknown (0) is a
        // hand-raised event (gym `vfx swarmer`) and plays.
        case T::SwarmerDeath:
            return e.cause == static_cast<u8>(sim::CombatRetirement::Damage) ||
                           e.cause == static_cast<u8>(sim::CombatRetirement::Unknown)
                       ? Cat::CellDeath
                       : Cat::None;
        case T::PathogenLatch: return Cat::Latch;
        case T::ScarBuilt:
        case T::ScarReinforced: return Cat::Knock;
        case T::ScarChipped: return Cat::Chew;
        // Chewed through, not merely expired.
        case T::ScarDestroyed: return e.magnitude >= 0.5f ? Cat::ScarBreak : Cat::None;
        case T::HarpoonPulse: return e.magnitude > 0.0f ? Cat::Glug : Cat::Harpoon;
        case T::ToxinFired: return Cat::Spit;
        case T::ToxinHit: return Cat::Sizzle;
        // Freeze/Shatter/splash/expiry are garnish the eye covers; ChaffDeath
        // is handled as a grain cloud, not a category.
        default: return Cat::None;
    }
}

} // namespace

const char* cue_name(Cue c) {
    switch (c) {
        case Cue::UiHover: return "hover";
        case Cue::UiClick: return "click";
        case Cue::UiDeny: return "deny";
        case Cue::UiOpen: return "open";
        case Cue::UiClose: return "close";
        case Cue::Deploy: return "deploy";
        case Cue::Purchase: return "purchase";
        case Cue::Ability: return "ability";
        case Cue::WaveStart: return "wave_start";
        case Cue::WaveClear: return "wave_clear";
        case Cue::Victory: return "victory";
        case Cue::Defeat: return "defeat";
        case Cue::Count: break;
    }
    return "";
}

void measure_presence(const f32* x, const f32* y, usize count, const Rect& view, HordeState& out) {
    out.visible = 0;
    out.visible_pan = 0.0f;
    if (x == nullptr || y == nullptr || count == 0) return;
    const Vec2 c = (view.min + view.max) * 0.5f;
    const Vec2 h{std::max((view.max.x - view.min.x) * 0.5f, 1e-3f), std::max((view.max.y - view.min.y) * 0.5f, 1e-3f)};
    // A margin of a fifth of the view: the crowd just past the edge is still
    // "here" to the ear.
    const f32 reach = 1.2f;
    const usize stride = std::max<usize>(1, count / 4096);
    u32 hits = 0;
    f32 pan_sum = 0.0f;
    for (usize i = 0; i < count; i += stride) {
        const f32 u = (x[i] - c.x) / h.x;
        const f32 v = (y[i] - c.y) / h.y;
        if (std::fabs(u) <= reach && std::fabs(v) <= reach) {
            ++hits;
            pan_sum += std::clamp(u, -1.0f, 1.0f);
        }
    }
    out.visible = static_cast<u32>(static_cast<u64>(hits) * stride);
    out.visible_pan = hits > 0 ? pan_sum / static_cast<f32>(hits) * 0.6f : 0.0f;
}

SoundScape::SoundScape() {
    const Rule* r = rules();
    for (u32 i = 0; i < kCatCount; ++i) tallies_[i].tokens = r[i].burst;
    death_tokens_ = kDeathBurst;
}

void SoundScape::set_view(const Rect& visible_world) {
    view_ = visible_world;
    view_center_ = (view_.min + view_.max) * 0.5f;
    view_half_ = Vec2{std::max((view_.max.x - view_.min.x) * 0.5f, 1e-3f),
                      std::max((view_.max.y - view_.min.y) * 0.5f, 1e-3f)};
}

u32 SoundScape::rand_u32() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}

f32 SoundScape::rand01() { return static_cast<f32>(rand_u32() >> 8) * (1.0f / 16777216.0f); }

f32 SoundScape::audibility(Vec2 world, f32* pan) const {
    const f32 u = (world.x - view_center_.x) / view_half_.x;
    const f32 v = (world.y - view_center_.y) / view_half_.y;
    if (pan != nullptr) *pan = std::clamp(u, -1.0f, 1.0f) * 0.7f;
    const f32 d = std::max(std::fabs(u), std::fabs(v));
    // Gentle centre emphasis on screen, then a fade to nothing 0.4 views out.
    if (d <= 1.0f) return 1.0f - 0.25f * d * d;
    return 0.75f * std::max(0.0f, 1.0f - (d - 1.0f) / 0.4f);
}

void SoundScape::tally(u32 category, Vec2 world, f32 size) {
    Tally& t = tallies_[category];
    ++t.seen;
    f32 pan = 0.0f;
    f32 a = audibility(world, &pan);
    const f32 floor = rules()[category].floor;
    if (a < floor) a = floor;
    if (a <= 0.0f) {
        ++stats_.culled;
        return;
    }
    // Reservoir sample: after n events each one has had a kReps/n chance.
    const u32 n = ++t.count;
    u32 slot = n - 1;
    if (n > kReps) {
        slot = rand_u32() % n;
        if (slot >= kReps) return;
    }
    t.reps[slot] = Rep{pan, a, size};
}

void SoundScape::on_combat_events(const sim::CombatEvent* events, usize count) {
    if (events == nullptr) return;
    stats_.events_seen += count;
    for (usize i = 0; i < count; ++i) {
        const sim::CombatEvent& e = events[i];
        if (e.type == sim::CombatEventType::ChaffDeath) {
            const u32 f = static_cast<u32>(e.target_family);
            if (f >= kFamilyCount) continue;
            DeathTally& d = deaths_[f];
            ++d.seen;
            f32 pan = 0.0f;
            const f32 a = audibility(e.origin, &pan);
            if (a <= 0.0f) {
                ++stats_.culled;
                continue;
            }
            const u32 n = ++d.count;
            u32 slot = n - 1;
            if (n > kDeathReps) {
                slot = rand_u32() % n;
                if (slot >= kDeathReps) continue;
            }
            d.reps[slot] = Rep{pan, a, e.size_scale};
            continue;
        }
        const Cat c = classify(e);
        if (c == Cat::None) continue;
        if (c == Cat::Latch) {
            f32 pan = 0.0f;
            audibility(e.origin, &pan);
            latch_pan_ += (pan - latch_pan_) * 0.15f;
        }
        tally(static_cast<u32>(c), e.origin, 1.0f);
    }
}

void SoundScape::post(const Command& c) {
    if (out_ == nullptr) return;
    if (!out_->push(c)) ++stats_.posts_dropped;
}

void SoundScape::voice(SoundId id, f32 gain, f32 pitch, f32 pan, f32 delay) {
    Command c;
    c.type = CommandType::Voice;
    c.id = static_cast<u8>(id);
    c.gain = gain;
    c.pitch = pitch;
    c.pan = pan;
    c.delay = delay;
    post(c);
    ++stats_.voices_posted;
    ++frame_voices_;
}

void SoundScape::grain(GrainKind kind, f32 hz, f32 gain, f32 pan, f32 delay, f32 decay_scale) {
    Command c;
    c.type = CommandType::Grain;
    c.id = static_cast<u8>(kind);
    c.gain = gain;
    c.pitch = hz;
    c.pan = pan;
    c.delay = delay;
    c.param = decay_scale;
    post(c);
    ++stats_.grains_posted;
    ++frame_grains_;
}

void SoundScape::bed(BedId id, f32 level, f32 pan, f32 activity) {
    Command c;
    c.type = CommandType::Bed;
    c.id = static_cast<u8>(id);
    c.gain = level;
    c.pan = pan;
    c.param = activity;
    post(c);
}

void SoundScape::cue(Cue c, u8 variant) {
    const u32 i = static_cast<u32>(c);
    if (i >= kCueCount || cue_cooldown_[i] > 0.0f) return;
    cue_cooldown_[i] = kCueCooldown[i];
    play_cue(c, 0.0f, variant);
}

void SoundScape::cue_at(Cue c, Vec2 world, u8 variant) {
    const u32 i = static_cast<u32>(c);
    if (i >= kCueCount || cue_cooldown_[i] > 0.0f) return;
    cue_cooldown_[i] = kCueCooldown[i];
    f32 pan = 0.0f;
    audibility(world, &pan);
    play_cue(c, pan, variant);
}

void SoundScape::play_cue(Cue c, f32 pan, u8 variant) {
    const auto degree = [](i32 d) { return ratio_for_degree(d); };
    switch (c) {
        case Cue::UiHover: voice(SoundId::UiHover, 1.0f, 1.0f, 0.0f, 0.0f); break;
        case Cue::UiClick: voice(SoundId::UiClick, 1.0f, 1.0f, 0.0f, 0.0f); break;
        case Cue::UiDeny:
            // F#4 falling to D4: a gentle "uh-uh", never a buzzer.
            voice(SoundId::UiDeny, 1.0f, 1.0f, 0.0f, 0.0f);
            voice(SoundId::UiDeny, 0.9f, dsp::semitones_to_ratio(-4.0f), 0.0f, 0.085f);
            break;
        case Cue::UiOpen:
            voice(SoundId::UiChime, 0.8f, degree(0), 0.0f, 0.0f);
            voice(SoundId::UiChime, 0.7f, degree(3), 0.0f, 0.06f);
            break;
        case Cue::UiClose:
            voice(SoundId::UiChime, 0.7f, degree(3), 0.0f, 0.0f);
            voice(SoundId::UiChime, 0.6f, degree(0), 0.0f, 0.06f);
            break;
        case Cue::Deploy: {
            // Each cell type lands on its own note, so a player learns them.
            static constexpr i8 kTowerDegree[kTowerTypeCount] = {3, 0, 4, 2, 1};
            const i32 d = variant < kTowerTypeCount ? kTowerDegree[variant] : 0;
            voice(SoundId::Plop, 1.0f, degree(d), pan, 0.0f);
            voice(SoundId::UiChime, 0.45f, degree(d + 5), pan, 0.035f);
            break;
        }
        case Cue::Purchase: {
            static constexpr i8 kArpeggio[4] = {0, 2, 3, 5};  // D F# A D
            for (i32 k = 0; k < 4; ++k) voice(SoundId::UiChime, 0.8f - 0.08f * k, degree(kArpeggio[k]), 0.0f, 0.06f * k);
            break;
        }
        case Cue::Ability:
            if (variant == 0) {
                for (i32 k = 0; k < 6; ++k) voice(SoundId::Sparkle, 1.0f - 0.1f * k, degree(k), pan, 0.035f * k);
            } else if (variant == 1) {
                voice(SoundId::Whoosh, 1.0f, 1.0f, pan, 0.0f);
            } else {
                voice(SoundId::ClotThud, 1.0f, 1.0f, pan, 0.0f);
                voice(SoundId::Knock, 0.8f, degree(0), pan, 0.02f);
            }
            break;
        case Cue::WaveStart:
            voice(SoundId::Thump, 0.5f, 1.0f, 0.0f, 0.0f);
            voice(SoundId::WaveSwell, 0.8f, 1.0f, -0.2f, 0.0f);
            voice(SoundId::WaveSwell, 0.7f, degree(3), 0.2f, 0.0f);
            voice(SoundId::WaveSwell, 0.45f, degree(5), 0.0f, 0.05f);
            break;
        case Cue::WaveClear:
            voice(SoundId::UiChime, 0.7f, degree(0), 0.0f, 0.0f);
            voice(SoundId::UiChime, 0.65f, degree(3), 0.0f, 0.08f);
            voice(SoundId::UiChime, 0.6f, degree(5), 0.0f, 0.16f);
            break;
        case Cue::Victory: {
            static constexpr i8 kRun[5] = {0, 2, 3, 5, 7};
            for (i32 k = 0; k < 5; ++k) voice(SoundId::UiChime, 0.85f, degree(kRun[k]), 0.0f, 0.1f * k);
            voice(SoundId::WaveSwell, 0.55f, 1.0f, -0.2f, 0.4f);
            voice(SoundId::WaveSwell, 0.5f, degree(3), 0.2f, 0.4f);
            break;
        }
        case Cue::Defeat: {
            voice(SoundId::Thump, 0.9f, 1.0f, 0.0f, 0.0f);
            static constexpr i8 kFall[4] = {3, 2, 1, 0};
            for (i32 k = 0; k < 4; ++k) voice(SoundId::Lament, 1.0f, degree(kFall[k]), 0.0f, 0.12f + 0.35f * k);
            break;
        }
        case Cue::Count: break;
    }
}

void SoundScape::spend_categories(f32 dt) {
    const Rule* table = rules();
    const f32 spread = std::min(dt, 1.0f / 30.0f) * 0.8f;

    for (u32 ci = 0; ci < kCatCount; ++ci) {
        const Rule& r = table[ci];
        Tally& t = tallies_[ci];
        t.tokens = std::min(r.burst, t.tokens + r.rate * dt);
        t.backlog *= std::exp(-dt / 0.25f);
        if (t.count == 0) continue;

        u32 budget_left = r.grain ? (kGrainBudget > frame_grains_ ? kGrainBudget - frame_grains_ : 0u)
                                  : (kVoiceBudget > frame_voices_ ? kVoiceBudget - frame_voices_ : 0u);
        u32 n = std::min<u32>({t.count, static_cast<u32>(t.tokens), r.per_frame, budget_left, kReps});
        const f32 events = static_cast<f32>(t.count) + t.backlog;
        if (n == 0) {
            // Rate-limited: carried into the loudness of the next sound.
            t.backlog = std::min(t.backlog + static_cast<f32>(t.count), 256.0f);
            stats_.folded += t.count;
            continue;
        }
        t.tokens -= static_cast<f32>(n);
        const f32 g = boost(r, events, static_cast<f32>(n));
        stats_.folded += t.count > n ? t.count - n : 0u;
        t.backlog = 0.0f;

        const u32 reps = std::min(t.count, kReps);
        for (u32 k = 0; k < n; ++k) {
            const Rep& rep = t.reps[(k + rand_u32()) % reps];
            f32 pitch = 1.0f;
            if (r.degree_count > 0) {
                pitch = ratio_for_degree(r.degrees[rand_u32() % r.degree_count]);
            }
            if (r.jitter > 0.0f) pitch *= dsp::semitones_to_ratio((rand01() * 2.0f - 1.0f) * r.jitter);
            // The first sound of a frame is on time; the rest spread across it.
            const f32 delay = k == 0 ? 0.0f : spread * (static_cast<f32>(k) + rand01()) / static_cast<f32>(n);
            const f32 gain = r.gain * rep.audibility * g;
            if (r.grain) {
                grain(r.grain_kind, r.grain_hz * pitch, gain, rep.pan, delay);
            } else {
                voice(r.sound, gain, pitch, rep.pan, delay);
            }
        }
    }
}

void SoundScape::spend_deaths(f32 dt) {
    u32 total = 0;
    u32 sampled = 0;
    f32 audible_weight = 0.0f;
    for (const DeathTally& d : deaths_) {
        total += d.count;
        for (u32 k = 0; k < std::min(d.count, kDeathReps); ++k) {
            audible_weight += d.reps[k].audibility;
            ++sampled;
        }
    }
    // Smoothed audible death rate drives both the pop thinning and the fizz.
    const f32 mean_aud = sampled > 0 ? audible_weight / static_cast<f32>(sampled) : 0.0f;
    const f32 instant = dt > 0.0f ? static_cast<f32>(total) * mean_aud / dt : 0.0f;
    const f32 k = 1.0f - std::exp(-dt / 0.3f);
    stats_.death_rate += (instant - stats_.death_rate) * k;

    death_tokens_ = std::min(kDeathBurst, death_tokens_ + kDeathRate * dt);
    if (total == 0) return;

    const u32 budget_left = kGrainBudget > frame_grains_ ? kGrainBudget - frame_grains_ : 0u;
    u32 n = std::min<u32>({total, static_cast<u32>(death_tokens_), kDeathPerFrame, budget_left});
    if (n == 0) {
        stats_.folded += total;
        return;
    }
    death_tokens_ -= static_cast<f32>(n);
    stats_.folded += total > n ? total - n : 0u;

    // Many pops each a little quieter: summed energy grows like sqrt(N), so
    // shaving each grain keeps a slaughter from being louder than a skirmish.
    const f32 thinning = std::clamp(std::pow(std::max(stats_.death_rate, 1.0f) / kDensityReference, -0.3f), 0.45f, 1.0f);
    const f32 spread = std::min(dt, 1.0f / 30.0f) * 0.9f;

    // Share the grains between families by how many of each died, at least
    // one each while there is budget, so a mixed wave sounds mixed.
    u32 spent = 0;
    for (u32 f = 0; f < kFamilyCount && spent < n; ++f) {
        const DeathTally& d = deaths_[f];
        if (d.count == 0) continue;
        u32 share = std::max(1u, static_cast<u32>(static_cast<u64>(n) * d.count / total));
        share = std::min({share, n - spent, d.count});
        const DeathVoice& dv = kDeathVoices[f];
        const u32 reps = std::min(d.count, kDeathReps);
        for (u32 j = 0; j < share; ++j) {
            const Rep& rep = d.reps[(j + rand_u32()) % reps];
            // Bigger bodies a scale step lower.
            const i32 size_shift = static_cast<i32>(std::lround(-2.0f * std::log2(std::clamp(rep.size, 0.25f, 4.0f))));
            const i32 deg = dv.degrees[rand_u32() % dv.degree_count] + size_shift;
            const f32 detune = dsp::semitones_to_ratio((rand01() * 2.0f - 1.0f) * 0.08f);
            const f32 hz = dv.base_hz * ratio_for_degree(deg) * detune;
            const f32 delay = spent == 0 ? 0.0f : spread * rand01();
            const f32 decay = 0.85f + 0.3f * rand01();
            grain(dv.kind, hz, dv.gain * rep.audibility * thinning, rep.pan, delay, decay);
            ++spent;
        }
    }
}

void SoundScape::update(const HordeState& horde, f32 dt) {
    dt = std::clamp(dt, 0.0f, 0.1f);
    frame_dt_ = dt;
    for (f32& c : cue_cooldown_) c = std::max(0.0f, c - dt);

    // Leaks first: they outrank everything and must never be budgeted out.
    leak_tokens_ = std::min(kLeakBurst, leak_tokens_ + kLeakRate * dt);
    if (!leak_baseline_ || horde.leaked_total < leaked_seen_) {
        leaked_seen_ = horde.leaked_total;
        leak_baseline_ = true;
    } else if (horde.leaked_total > leaked_seen_) {
        leaks_pending_ += static_cast<u32>(std::min<u64>(horde.leaked_total - leaked_seen_, 1000));
        leaked_seen_ = horde.leaked_total;
    }
    if (leaks_pending_ > 0 && leak_tokens_ >= 1.0f) {
        leak_tokens_ -= 1.0f;
        const f32 g = std::min(1.5f, 1.0f + 0.2f * std::log2(static_cast<f32>(leaks_pending_)));
        voice(SoundId::Thump, g, 1.0f, 0.0f, 0.0f);
        voice(SoundId::Warn, g, 1.0f, 0.0f, 0.01f);
        leaks_pending_ = 0;
    }

    spend_categories(dt);
    spend_deaths(dt);

    // Beds: continuous quantities, logarithmic in the crowd.
    const auto log_level = [](f32 n, f32 knee, f32 full) {
        if (n <= 0.0f) return 0.0f;
        return std::clamp(std::log2(1.0f + n / knee) / std::log2(1.0f + full / knee), 0.0f, 1.0f);
    };
    if (horde.live) {
        const f32 murmur = log_level(static_cast<f32>(horde.visible), 10.0f, 3000.0f);
        bed(BedId::Murmur, murmur, horde.visible_pan, 0.0f);
        const f32 gnaw = log_level(static_cast<f32>(horde.latched), 1.0f, 150.0f);
        bed(BedId::Gnaw, gnaw, latch_pan_, gnaw);
        const f32 fizz = smoothstep01((std::log2(std::max(stats_.death_rate, 1.0f)) - std::log2(kFizzStart)) /
                                      (std::log2(kFizzFull) - std::log2(kFizzStart)));
        bed(BedId::Fizz, fizz * 0.9f, 0.0f, fizz);
    } else {
        bed(BedId::Murmur, 0.0f, 0.0f, 0.0f);
        bed(BedId::Gnaw, 0.0f, 0.0f, 0.0f);
        bed(BedId::Fizz, 0.0f, 0.0f, 0.0f);
        stats_.death_rate *= std::exp(-dt / 0.3f);
    }
    latch_pan_ *= std::exp(-dt / 2.0f);

    stats_.last_frame_voices = frame_voices_;
    stats_.last_frame_grains = frame_grains_;
    frame_voices_ = 0;
    frame_grains_ = 0;
    for (u32 i = 0; i < kCatCount; ++i) {
        tallies_[i].count = 0;
        tallies_[i].seen = 0;
    }
    for (DeathTally& d : deaths_) {
        d.count = 0;
        d.seen = 0;
    }
}

void SoundScape::reset() {
    const Rule* r = rules();
    for (u32 i = 0; i < kCatCount; ++i) tallies_[i] = Tally{};
    for (u32 i = 0; i < kCatCount; ++i) tallies_[i].tokens = r[i].burst;
    for (DeathTally& d : deaths_) d = DeathTally{};
    death_tokens_ = kDeathBurst;
    leak_baseline_ = false;
    leaks_pending_ = 0;
    leak_tokens_ = kLeakBurst;
    latch_pan_ = 0.0f;
    stats_.death_rate = 0.0f;
    Command stop;
    stop.type = CommandType::StopAll;
    post(stop);
}

} // namespace immune::audio
