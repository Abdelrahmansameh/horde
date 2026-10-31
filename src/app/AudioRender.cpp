#include "app/AudioRender.h"

#include "audio/Mixer.h"
#include "audio/SoundScape.h"
#include "core/Log.h"
#include "platform/FileIO.h"
#include "sim/CombatEvents.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>

namespace immune::app {

namespace {

using audio::Cue;
using sim::CombatEvent;
using sim::CombatEventType;

constexpr f32 kFrameDt = 1.0f / 60.0f;
const Rect kView{Vec2{-80.0f, -45.0f}, Vec2{80.0f, 45.0f}};

/// One frame of the timeline: what a segment may do to the scene.
struct Frame {
    u32 index = 0;          ///< Frame number within the segment.
    f32 t = 0.0f;           ///< Seconds into the segment.
    f32 dt = kFrameDt;
    audio::SoundScape* scene = nullptr;
    std::vector<CombatEvent>* events = nullptr;
    audio::HordeState* horde = nullptr;
    u32* rng = nullptr;

    f32 rand01() const {
        u32& s = *rng;
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<f32>(s >> 8) * (1.0f / 16777216.0f);
    }
    /// A random point in the view, or (20% of the time when `spill`) just off it.
    Vec2 somewhere(bool spill = false) const {
        const f32 reach = spill && rand01() < 0.2f ? 1.3f : 0.95f;
        const Vec2 c = (kView.min + kView.max) * 0.5f;
        const Vec2 h = (kView.max - kView.min) * 0.5f;
        return Vec2{c.x + (rand01() * 2.0f - 1.0f) * h.x * reach, c.y + (rand01() * 2.0f - 1.0f) * h.y * reach};
    }
    void push(CombatEventType type, TowerType source, PathogenFamily family, Vec2 at, f32 magnitude = 1.0f,
              f32 size = 1.0f) const {
        CombatEvent e;
        e.type = type;
        e.source = source;
        e.target_family = family;
        e.origin = at;
        e.secondary = at;
        e.magnitude = magnitude;
        e.size_scale = size;
        events->push_back(e);
    }
    /// True on the frame nearest `when` (seconds into the segment). By index,
    /// not by comparing times: a time on an exact frame boundary would
    /// otherwise fall between two frames through rounding and never fire.
    bool at(f32 when) const { return static_cast<u32>(std::lround(when / dt)) == index; }
};

struct Segment {
    const char* name;
    f32 seconds;
    std::function<void(const Frame&)> step;
};

/// Integer events from a continuous rate, carried between frames.
u32 emit(f32& carry, f32 per_second, f32 dt) {
    carry += per_second * dt;
    const u32 n = static_cast<u32>(carry);
    carry -= static_cast<f32>(n);
    return n;
}

std::vector<Segment> timeline() {
    std::vector<Segment> s;
    s.push_back({"ui", 3.6f, [](const Frame& f) {
        const Cue cues[] = {Cue::UiHover, Cue::UiHover, Cue::UiClick, Cue::UiDeny,
                            Cue::UiOpen, Cue::UiClose, Cue::Purchase};
        for (u32 i = 0; i < 7; ++i) {
            if (f.at(0.1f + 0.5f * static_cast<f32>(i))) f.scene->cue(cues[i]);
        }
    }});
    s.push_back({"deploy", 2.6f, [](const Frame& f) {
        for (u32 i = 0; i < kTowerTypeCount; ++i) {
            if (f.at(0.1f + 0.45f * static_cast<f32>(i))) {
                f.scene->cue_at(Cue::Deploy, Vec2{-60.0f + 30.0f * static_cast<f32>(i), 0.0f}, static_cast<u8>(i));
            }
        }
    }});
    s.push_back({"abilities", 3.4f, [](const Frame& f) {
        for (u32 i = 0; i < 3; ++i) {
            if (f.at(0.1f + 1.1f * static_cast<f32>(i))) f.scene->cue_at(Cue::Ability, Vec2{0.0f, 0.0f}, static_cast<u8>(i));
        }
    }});
    s.push_back({"friendly_attacks", 6.4f, [](const Frame& f) {
        struct Kind { CombatEventType type; TowerType source; PathogenFamily family; };
        const Kind kinds[] = {
            {CombatEventType::MuzzleFlash, TowerType::Neutrophil, PathogenFamily::Count},
            {CombatEventType::ProjectileImpact, TowerType::Neutrophil, PathogenFamily::Virus},
            {CombatEventType::ProjectileImpact, TowerType::Macrophage, PathogenFamily::Bacteria},
            {CombatEventType::ProjectileImpact, TowerType::CytotoxicT, PathogenFamily::Virus},
            {CombatEventType::Explosion, TowerType::GobletCell, PathogenFamily::Count},
            {CombatEventType::ScarBuilt, TowerType::Fibroblast, PathogenFamily::Count},
        };
        for (u32 k = 0; k < 6; ++k) {
            for (u32 r = 0; r < 3; ++r) {
                if (f.at(0.1f + 1.05f * static_cast<f32>(k) + 0.25f * static_cast<f32>(r))) {
                    f.push(kinds[k].type, kinds[k].source, kinds[k].family, Vec2{-40.0f + 40.0f * static_cast<f32>(r), 0.0f});
                }
            }
        }
    }});
    s.push_back({"friendly_deaths", 3.6f, [](const Frame& f) {
        for (u32 r = 0; r < 3; ++r) {
            if (f.at(0.1f + 0.3f * static_cast<f32>(r))) {
                f.push(CombatEventType::SwarmerDeath, TowerType::Neutrophil, PathogenFamily::Count, Vec2{-30.0f + 30.0f * r, 0.0f});
            }
        }
        if (f.at(1.3f)) f.push(CombatEventType::TowerDestroyed, TowerType::Macrophage, PathogenFamily::Count, Vec2{0.0f, 0.0f});
        if (f.at(2.6f)) f.push(CombatEventType::ScarDestroyed, TowerType::Fibroblast, PathogenFamily::Count, Vec2{20.0f, 0.0f}, 1.0f);
    }});
    s.push_back({"horde_attacks", 6.6f, [](const Frame& f) {
        struct Kind { CombatEventType type; PathogenFamily family; f32 magnitude; };
        const Kind kinds[] = {
            {CombatEventType::PathogenLatch, PathogenFamily::Virus, 1.0f},
            {CombatEventType::ToxinFired, PathogenFamily::Bacteria, 1.0f},
            {CombatEventType::ToxinHit, PathogenFamily::Bacteria, 1.0f},
            {CombatEventType::HarpoonPulse, PathogenFamily::Spore, 0.0f},
            {CombatEventType::HarpoonPulse, PathogenFamily::Spore, 5.0f},
            {CombatEventType::ScarChipped, PathogenFamily::Count, 1.0f},
        };
        for (u32 k = 0; k < 6; ++k) {
            for (u32 r = 0; r < 3; ++r) {
                if (f.at(0.1f + 1.08f * static_cast<f32>(k) + 0.27f * static_cast<f32>(r))) {
                    f.push(kinds[k].type, TowerType::Count, kinds[k].family, Vec2{-40.0f + 40.0f * static_cast<f32>(r), 0.0f},
                           kinds[k].magnitude);
                }
            }
        }
    }});
    s.push_back({"horde_deaths", 5.0f, [](const Frame& f) {
        for (u32 fam = 0; fam < kFamilyCount; ++fam) {
            for (u32 r = 0; r < 4; ++r) {
                if (f.at(0.1f + 0.9f * static_cast<f32>(fam) + 0.2f * static_cast<f32>(r))) {
                    f.push(CombatEventType::ChaffDeath, TowerType::Count, static_cast<PathogenFamily>(fam),
                           Vec2{-30.0f + 20.0f * r, 0.0f}, 1.0f, 0.85f + 0.1f * static_cast<f32>(r));
                }
            }
        }
        // A bomb going off in a packed lane: twenty deaths on one frame.
        if (f.at(4.0f)) {
            for (u32 i = 0; i < 20; ++i) {
                f.push(CombatEventType::ChaffDeath, TowerType::Count, PathogenFamily::Virus, f.somewhere(), 1.0f,
                       0.8f + 0.4f * f.rand01());
            }
        }
    }});
    s.push_back({"stingers", 9.0f, [](const Frame& f) {
        if (f.at(0.1f)) f.scene->cue(Cue::WaveStart);
        if (f.at(2.2f)) f.scene->cue(Cue::WaveClear);
        if (f.at(3.4f)) f.horde->leaked_total += 1;
        if (f.at(4.4f)) f.horde->leaked_total += 6;
        if (f.at(5.4f)) f.scene->cue(Cue::Victory);
        if (f.at(7.4f)) f.scene->cue(Cue::Defeat);
    }});
    // Ten seconds from a trickle to a slaughter. Rates are exponential in
    // time, so every doubling gets equal airtime.
    s.push_back({"horde_stress", 10.0f, [](const Frame& f) {
        static f32 carry[8];
        if (f.index == 0) std::fill(std::begin(carry), std::end(carry), 0.0f);
        const f32 x = f.t / 10.0f;
        const auto ramp = [x](f32 lo, f32 hi) { return lo * std::pow(hi / lo, x); };
        f.horde->live = true;
        f.horde->visible = static_cast<u32>(ramp(20.0f, 8000.0f));
        f.horde->visible_pan = 0.1f;
        f.horde->latched = static_cast<u32>(ramp(1.0f, 300.0f));
        const PathogenFamily fams[4] = {PathogenFamily::Virus, PathogenFamily::Virus, PathogenFamily::Bacteria,
                                        PathogenFamily::Parasite};
        for (u32 n = emit(carry[0], ramp(5.0f, 3000.0f), f.dt); n > 0; --n) {
            f.push(CombatEventType::ChaffDeath, TowerType::Count, fams[static_cast<u32>(f.rand01() * 4.0f) & 3u],
                   f.somewhere(true), 1.0f, 0.8f + 0.4f * f.rand01());
        }
        for (u32 n = emit(carry[1], ramp(10.0f, 600.0f), f.dt); n > 0; --n) {
            f.push(CombatEventType::MuzzleFlash, TowerType::Neutrophil, PathogenFamily::Count, f.somewhere(true));
            f.push(CombatEventType::ProjectileImpact, TowerType::Neutrophil, PathogenFamily::Virus, f.somewhere(true));
        }
        for (u32 n = emit(carry[2], ramp(2.0f, 60.0f), f.dt); n > 0; --n) {
            f.push(CombatEventType::PathogenLatch, TowerType::Neutrophil, PathogenFamily::Virus, f.somewhere(true));
        }
        for (u32 n = emit(carry[3], ramp(2.0f, 80.0f), f.dt); n > 0; --n) {
            f.push(CombatEventType::ToxinFired, TowerType::Count, PathogenFamily::Bacteria, f.somewhere(true));
        }
        for (u32 n = emit(carry[4], ramp(0.5f, 30.0f), f.dt); n > 0; --n) {
            f.push(CombatEventType::SwarmerDeath, TowerType::Neutrophil, PathogenFamily::Count, f.somewhere(true));
        }
        for (u32 n = emit(carry[5], ramp(0.5f, 10.0f), f.dt); n > 0; --n) {
            f.push(CombatEventType::HarpoonPulse, TowerType::Count, PathogenFamily::Spore, f.somewhere(true), 0.0f);
        }
        for (u32 n = emit(carry[6], ramp(1.0f, 40.0f), f.dt); n > 0; --n) {
            f.push(CombatEventType::Explosion, TowerType::GobletCell, PathogenFamily::Count, f.somewhere(true));
        }
        if (f.at(6.0f)) f.horde->leaked_total += 3;
        if (f.at(8.5f)) f.horde->leaked_total += 12;
    }});
    s.push_back({"tail", 1.5f, [](const Frame& f) { f.horde->live = false; }});
    return s;
}

f32 to_dbfs(f32 linear) { return linear > 1e-9f ? 20.0f * std::log10(linear) : -180.0f; }

} // namespace

AudioRenderReport render_audition(f32 sample_rate, std::vector<f32>* pcm) {
    using clock = std::chrono::steady_clock;
    AudioRenderReport report;
    report.sample_rate = sample_rate;

    auto mixer = std::make_unique<audio::Mixer>();
    auto ring = std::make_unique<audio::CommandRing>();
    mixer->init(sample_rate);
    audio::Command master;
    master.type = audio::CommandType::MasterGain;
    master.gain = 0.8f;
    mixer->apply(master);

    audio::SoundScape scene;
    scene.set_output(ring.get());
    scene.set_view(kView);

    std::vector<CombatEvent> events;
    events.reserve(4096);
    audio::HordeState horde;
    u32 rng = 0x2545F491u;
    std::vector<f32> block;

    // Fractional samples per frame are carried, so the render is exactly
    // sample_rate samples per second of timeline.
    f64 sample_carry = 0.0;
    for (const Segment& seg : timeline()) {
        AudioSegmentReport sr;
        sr.name = seg.name;
        f64 sum_sq = 0.0;
        u64 samples = 0;
        const audio::SceneStats before = scene.stats();
        const u32 frames = static_cast<u32>(std::lround(seg.seconds / kFrameDt));
        for (u32 fi = 0; fi < frames; ++fi) {
            Frame f;
            f.index = fi;
            f.t = static_cast<f32>(fi) * kFrameDt;
            f.scene = &scene;
            f.events = &events;
            f.horde = &horde;
            f.rng = &rng;
            events.clear();
            seg.step(f);
            sr.events += events.size();
            scene.on_combat_events(events.data(), events.size());
            scene.update(horde, kFrameDt);

            sample_carry += static_cast<f64>(sample_rate) * kFrameDt;
            const u32 n = static_cast<u32>(sample_carry);
            sample_carry -= n;
            block.resize(static_cast<usize>(n) * 2u);

            const auto t0 = clock::now();
            mixer->drain(*ring);
            mixer->render(block.data(), n);
            const f64 spent = std::chrono::duration<f64>(clock::now() - t0).count();
            const f64 budget = static_cast<f64>(n) / sample_rate;
            sr.load += spent;
            sr.load_worst_frame = std::max(sr.load_worst_frame, budget > 0.0 ? spent / budget : 0.0);
            report.render_seconds += spent;

            for (const f32 x : block) {
                if (!std::isfinite(x)) {
                    ++report.non_finite_samples;
                    continue;
                }
                const f32 a = std::fabs(x);
                sr.peak = std::max(sr.peak, a);
                sum_sq += static_cast<f64>(x) * x;
                if (a >= 0.999f) ++report.clipped_samples;
            }
            samples += block.size();
            sr.max_voices = std::max(sr.max_voices, mixer->stats().active_voices);
            sr.max_grains = std::max(sr.max_grains, mixer->stats().active_grains);
            if (pcm != nullptr) pcm->insert(pcm->end(), block.begin(), block.end());
        }
        sr.seconds = static_cast<f64>(frames) * kFrameDt;
        sr.rms = samples > 0 ? static_cast<f32>(std::sqrt(sum_sq / static_cast<f64>(samples))) : 0.0f;
        sr.load = sr.seconds > 0.0 ? sr.load / sr.seconds : 0.0;
        sr.voices_posted = scene.stats().voices_posted - before.voices_posted;
        sr.grains_posted = scene.stats().grains_posted - before.grains_posted;
        report.seconds += sr.seconds;
        report.segments.push_back(sr);
    }

    const audio::MixerStats& ms = mixer->stats();
    report.voices_started = ms.voices_started;
    report.voices_stolen = ms.voices_stolen;
    report.voices_rejected = ms.voices_rejected;
    report.grains_started = ms.grains_started;
    report.grains_stolen = ms.grains_stolen;
    report.events_seen = scene.stats().events_seen;
    report.events_culled = scene.stats().culled;
    report.events_folded = scene.stats().folded;
    report.ring_dropped = ring->dropped();
    return report;
}

int run_audio_render(const Options& options) {
    constexpr f32 kRate = 48000.0f;
    std::vector<f32> pcm;
    const AudioRenderReport r = render_audition(kRate, &pcm);

    // 16-bit PCM WAV, stereo.
    const u32 frames = static_cast<u32>(pcm.size() / 2u);
    const u32 data_bytes = frames * 4u;
    std::vector<u8> wav(44u + data_bytes);
    const auto put32 = [&wav](usize at, u32 v) {
        for (u32 b = 0; b < 4; ++b) wav[at + b] = static_cast<u8>(v >> (8u * b));
    };
    const auto put16 = [&wav](usize at, u16 v) {
        wav[at] = static_cast<u8>(v);
        wav[at + 1] = static_cast<u8>(v >> 8u);
    };
    const char* riff = "RIFF";
    const char* wave = "WAVEfmt ";
    const char* data = "data";
    for (u32 i = 0; i < 4; ++i) wav[i] = static_cast<u8>(riff[i]);
    put32(4, 36u + data_bytes);
    for (u32 i = 0; i < 8; ++i) wav[8 + i] = static_cast<u8>(wave[i]);
    put32(16, 16);
    put16(20, 1);                                  // PCM
    put16(22, 2);                                  // stereo
    put32(24, static_cast<u32>(kRate));
    put32(28, static_cast<u32>(kRate) * 4u);       // byte rate
    put16(32, 4);                                  // block align
    put16(34, 16);                                 // bits
    for (u32 i = 0; i < 4; ++i) wav[36 + i] = static_cast<u8>(data[i]);
    put32(40, data_bytes);
    for (usize i = 0; i < pcm.size(); ++i) {
        const f32 x = std::clamp(std::isfinite(pcm[i]) ? pcm[i] : 0.0f, -1.0f, 1.0f);
        const i16 v = static_cast<i16>(std::lround(x * 32767.0f));
        put16(44u + i * 2u, static_cast<u16>(v));
    }
    if (!platform::write_binary_file(options.audio_render_path, wav.data(), wav.size())) {
        std::fprintf(stderr, "error: could not write %s\n", options.audio_render_path.c_str());
        return 1;
    }

    nlohmann::ordered_json j;
    j["out"] = options.audio_render_path;
    j["sample_rate"] = r.sample_rate;
    j["seconds"] = r.seconds;
    j["render_ms"] = r.render_seconds * 1000.0;
    j["load"] = r.seconds > 0.0 ? r.render_seconds / r.seconds : 0.0;
    j["clipped_samples"] = r.clipped_samples;
    j["non_finite_samples"] = r.non_finite_samples;
    j["mixer"] = {{"voices_started", r.voices_started}, {"voices_stolen", r.voices_stolen},
                  {"voices_rejected", r.voices_rejected}, {"grains_started", r.grains_started},
                  {"grains_stolen", r.grains_stolen}, {"ring_dropped", r.ring_dropped}};
    j["scene"] = {{"events_seen", r.events_seen}, {"culled", r.events_culled}, {"folded", r.events_folded}};
    nlohmann::ordered_json segs = nlohmann::ordered_json::array();
    for (const AudioSegmentReport& s : r.segments) {
        const auto round1 = [](f64 v) { return std::round(v * 10.0) / 10.0; };
        segs.push_back({{"name", s.name},
                        {"seconds", s.seconds},
                        {"peak_dbfs", round1(to_dbfs(s.peak))},
                        {"rms_dbfs", round1(to_dbfs(s.rms))},
                        {"load", s.load},
                        {"load_worst_frame", s.load_worst_frame},
                        {"max_voices", s.max_voices},
                        {"max_grains", s.max_grains},
                        {"events", s.events},
                        {"voices_posted", s.voices_posted},
                        {"grains_posted", s.grains_posted}});
    }
    j["segments"] = segs;
    std::printf("%s\n", j.dump(2).c_str());
    return r.non_finite_samples == 0 ? 0 : 1;
}

} // namespace immune::app
