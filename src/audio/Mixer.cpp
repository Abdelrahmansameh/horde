#include "audio/Mixer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#define IMMUNE_AUDIO_FTZ 1
#endif

namespace immune::audio {

namespace {

/// Control-rate period: pitch, cutoff and FM depth are recomputed every this
/// many samples (0.33 ms at 48 kHz) and held in between. Fast enough that the
/// sharpest sweep in the patch table moves well under a semitone per step.
constexpr u32 kControl = 16;

/// A voice or grain below this amplitude is inaudible (-80 dB) and is retired.
constexpr f32 kSilent = 1e-4f;

/// Combat ducking applied when an Alert voice starts, and how fast it lets go.
constexpr f32 kAlertDuck = 0.35f;
constexpr f32 kDuckRelease = 0.35f;

/// Master compressor: soft knee above -6 dBFS at 4:1, then the soft clipper.
constexpr f32 kCompThreshold = 0.5f;
constexpr f32 kCompRatio = 4.0f;

// Reverb: Freeverb's tunings, scaled to the device rate. A small, dark room --
// the inside of a body, not a cathedral.
constexpr u32 kCombTuning[4] = {1116, 1188, 1277, 1356};
constexpr u32 kAllpassTuning[2] = {556, 441};
constexpr u32 kStereoSpread = 23;
constexpr f32 kRoomFeedback = 0.78f;
constexpr f32 kRoomDamp = 0.35f;
constexpr f32 kRoomInput = 0.06f;
constexpr f32 kRoomWet = 1.0f;

// Bed output gains. Deliberately low: beds are the floor of the mix that the
// discrete sounds sit on, never the foreground.
constexpr f32 kMurmurGain = 0.10f;
constexpr f32 kGnawGain = 0.16f;
constexpr f32 kFizzGain = 0.14f;

u32 seconds_to_samples(f32 seconds, f32 sample_rate) {
    const f32 s = seconds * sample_rate;
    return s <= 0.0f ? 0u : static_cast<u32>(s + 0.5f);
}

u32 next_rng(u32& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

bool finite(f32 x) { return std::isfinite(x); }

} // namespace

void Mixer::init(f32 sample_rate) {
    sample_rate_ = sample_rate > 8000.0f ? sample_rate : 48000.0f;
    inv_sr_ = 1.0f / sample_rate_;
    clock_ = 0;
    voices_.fill(Voice{});
    grains_.fill(Grain{});
    stats_ = MixerStats{};

    master_ = 0.0f;
    master_k_ = dsp::decay_coeff(0.03f, sample_rate_);
    duck_ = 0.0f;
    duck_k_ = dsp::decay_coeff(kDuckRelease, sample_rate_);
    kill_k_ = dsp::decay_coeff(0.002f, sample_rate_);
    comp_env_ = 0.0f;
    comp_attack_k_ = dsp::decay_coeff(0.0015f, sample_rate_);
    comp_release_k_ = dsp::decay_coeff(0.12f, sample_rate_);

    const f32 scale = sample_rate_ / 44100.0f;
    for (u32 c = 0; c < 8; ++c) {
        const u32 len = static_cast<u32>(static_cast<f32>(kCombTuning[c % 4] + (c >= 4 ? kStereoSpread : 0)) * scale);
        combs_[c].line.assign(std::max(len, 1u), 0.0f);
        combs_[c].index = 0;
        combs_[c].store = 0.0f;
    }
    for (u32 a = 0; a < 4; ++a) {
        const u32 len = static_cast<u32>(static_cast<f32>(kAllpassTuning[a % 2] + (a >= 2 ? kStereoSpread : 0)) * scale);
        allpasses_[a].line.assign(std::max(len, 1u), 0.0f);
        allpasses_[a].index = 0;
    }

    const f32 bed_tau[kBedCount] = {0.6f, 0.12f, 0.1f};
    for (u32 b = 0; b < kBedCount; ++b) {
        beds_[b] = Bed{};
        beds_[b].level_k = 1.0f - dsp::decay_coeff(bed_tau[b], sample_rate_);
        beds_[b].rng = 0x1234567u * (b + 1);
        beds_[b].env_k = dsp::decay_coeff(0.022f, sample_rate_);
    }
}

u32 Mixer::instances(SoundId id) const {
    u32 n = 0;
    for (const Voice& v : voices_) n += (v.active && !v.stolen && v.id == id) ? 1u : 0u;
    return n;
}

u32 Mixer::drain(CommandRing& ring) {
    u32 n = 0;
    Command c;
    while (ring.pop(c)) {
        apply(c);
        ++n;
    }
    return n;
}

void Mixer::apply(const Command& c) {
    // A NaN reaching a filter state would poison that voice -- or, through
    // the reverb, the whole mix -- until restart. Refuse it at the door.
    if (!finite(c.gain) || !finite(c.pitch) || !finite(c.pan) || !finite(c.param) || !finite(c.delay)) return;
    switch (c.type) {
        case CommandType::Voice:
            if (c.id < kSoundCount) start_voice(c);
            break;
        case CommandType::Grain:
            if (c.id < kGrainKindCount) start_grain(c);
            break;
        case CommandType::Bed:
            if (c.id < kBedCount) {
                Bed& b = beds_[c.id];
                b.target = std::clamp(c.gain, 0.0f, 1.0f);
                b.pan_target = std::clamp(c.pan, -1.0f, 1.0f);
                b.activity = std::clamp(c.param, 0.0f, 1.0f);
            }
            break;
        case CommandType::MasterGain:
            master_target_ = std::clamp(c.gain, 0.0f, 1.0f);
            break;
        case CommandType::StopAll:
            for (Voice& v : voices_) {
                if (v.active) v.stolen = true;
                if (v.delay > 0) v.active = false;
            }
            for (Grain& g : grains_) g.active = false;
            for (Bed& b : beds_) b.target = 0.0f;
            break;
        case CommandType::None:
            break;
    }
}

void Mixer::start_voice(const Command& c) {
    const SoundId id = static_cast<SoundId>(c.id);
    const Patch& p = patch(id);

    // Per-patch cap: the oldest instance of the same sound makes way. Stealing
    // within a patch is inaudible in a way stealing across patches is not.
    u32 same = 0;
    Voice* oldest_same = nullptr;
    for (Voice& v : voices_) {
        if (!v.active || v.stolen || v.id != id) continue;
        ++same;
        if (oldest_same == nullptr || v.started < oldest_same->started) oldest_same = &v;
    }
    if (same >= p.max_instances && oldest_same != nullptr) {
        oldest_same->stolen = true;
        ++stats_.voices_stolen;
    }

    Voice* slot = nullptr;
    for (Voice& v : voices_) {
        if (!v.active) {
            slot = &v;
            break;
        }
    }
    if (slot == nullptr) {
        // Full pool: take the quietest voice of the lowest priority, never a
        // Protected one, and never one more important than the newcomer.
        Voice* victim = nullptr;
        f32 victim_level = 0.0f;
        u8 victim_priority = 255;
        for (Voice& v : voices_) {
            const Patch& vp = patch(v.id);
            const u8 pr = v.stolen ? 0 : static_cast<u8>(vp.priority);
            if (vp.priority == Priority::Protected && !v.stolen) continue;
            const f32 level = v.amp * v.gain;
            if (victim == nullptr || pr < victim_priority || (pr == victim_priority && level < victim_level)) {
                victim = &v;
                victim_priority = pr;
                victim_level = level;
            }
        }
        if (victim == nullptr || victim_priority > static_cast<u8>(p.priority)) {
            ++stats_.voices_rejected;
            return;
        }
        if (!victim->stolen) ++stats_.voices_stolen;
        slot = victim;
    }

    Voice& v = *slot;
    v = Voice{};
    v.active = true;
    v.id = id;
    v.started = clock_;
    v.delay = seconds_to_samples(std::clamp(c.delay, 0.0f, 2.0f), sample_rate_);
    v.hold = seconds_to_samples(p.hold, sample_rate_);
    v.remaining = std::max(seconds_to_samples(p.length, sample_rate_), 1u);
    const u32 attack = std::max(seconds_to_samples(p.attack, sample_rate_), 1u);
    v.attack_inc = 1.0f / static_cast<f32>(attack);
    v.decay_k = dsp::decay_coeff(std::max(p.decay, 0.001f), sample_rate_);
    v.ratio = std::clamp(c.pitch, 0.05f, 16.0f);

    const f32 control_rate = sample_rate_ / static_cast<f32>(kControl);
    v.pitch_k = dsp::decay_coeff(std::max(p.pitch_time, 0.0005f), control_rate);
    v.fm_k = dsp::decay_coeff(std::max(p.fm_decay, 0.0005f), control_rate);
    v.cut_k = dsp::decay_coeff(std::max(p.cutoff_time, 0.0005f), control_rate);
    v.partial_k = p.partial_decay > 0.0f ? dsp::decay_coeff(p.partial_decay, sample_rate_) : 1.0f;
    v.noise_k = p.noise_decay > 0.0f ? dsp::decay_coeff(p.noise_decay, sample_rate_) : 1.0f;

    v.gain = std::clamp(c.gain, 0.0f, 4.0f) * p.gain;
    f32 left = 0.0f, right = 0.0f;
    dsp::pan_gains(c.pan, left, right);
    v.gl = v.gain * left;
    v.gr = v.gain * right;
    v.send = v.gain * p.reverb;

    switch (p.filter) {
        case FilterMode::LowPass: v.mix_low = 1.0f; break;
        // Normalised: the band output peaks at Q, so scale it back to unity.
        case FilterMode::BandPass: v.mix_band = 1.0f / std::max(p.q, 0.05f); break;
        case FilterMode::HighPass: v.mix_high = 1.0f; break;
        case FilterMode::Off: break;
    }
    v.rng = next_rng(rng_) | 1u;
    // Start mid-cycle phases at random so stacked copies of one sound do not
    // reinforce each other sample-for-sample (the "flanged machine gun").
    v.ph2 = dsp::noise01(v.rng);
    v.phm = dsp::noise01(v.rng);

    if (p.bus == Bus::Alert) duck_ = std::max(duck_, kAlertDuck);
    ++stats_.voices_started;
}

void Mixer::start_grain(const Command& c) {
    const GrainShape& s = grain_shape(static_cast<GrainKind>(c.id));
    Grain* slot = nullptr;
    for (Grain& g : grains_) {
        if (!g.active) {
            slot = &g;
            break;
        }
    }
    if (slot == nullptr) {
        // Grains are a few tens of milliseconds long: the quietest one is
        // nearly over anyway, and cutting it is masked by the newcomer.
        Grain* quietest = &grains_[0];
        for (Grain& g : grains_) {
            if (g.amp < quietest->amp) quietest = &g;
        }
        slot = quietest;
        ++stats_.grains_stolen;
    }

    Grain& g = *slot;
    g = Grain{};
    g.active = true;
    g.bus = static_cast<u8>(s.bus);
    g.delay = seconds_to_samples(std::clamp(c.delay, 0.0f, 2.0f), sample_rate_);
    const f32 decay = std::clamp(s.decay * c.param, 0.002f, 0.5f);
    g.remaining = std::max(seconds_to_samples(decay * 8.0f, sample_rate_), 1u);
    g.inc = std::clamp(c.pitch, 20.0f, 0.4f * sample_rate_) * inv_sr_;
    g.inc_mul = std::exp2(s.glide_octaves / (3.0f * decay * sample_rate_));
    g.amp = 1.0f;
    g.amp_k = dsp::decay_coeff(decay, sample_rate_);
    g.namp = s.noise;
    g.namp_k = dsp::decay_coeff(std::max(s.noise_decay, 0.0005f), sample_rate_);
    g.nlp_a = std::clamp(s.noise_lowpass, 0.01f, 1.0f);
    const f32 gain = std::clamp(c.gain, 0.0f, 4.0f);
    f32 left = 0.0f, right = 0.0f;
    dsp::pan_gains(c.pan, left, right);
    g.gl = gain * left;
    g.gr = gain * right;
    g.send = gain * s.reverb;
    g.rng = next_rng(rng_) | 1u;
    ++stats_.grains_started;
}

void Mixer::render(f32* out, u32 frames) {
#ifdef IMMUNE_AUDIO_FTZ
    // Flush denormals to zero for this thread while rendering: a decaying
    // reverb tail or filter state otherwise drifts into the denormal range and
    // costs ~100x per operation, which is exactly how audio threads glitch.
    const unsigned csr = _mm_getcsr();
    _mm_setcsr(csr | 0x8040u);
#endif
    u32 done = 0;
    while (done < frames) {
        const u32 n = std::min(kBlock, frames - done);
        render_block(out + done * 2u, n);
        done += n;
    }
    stats_.frames_rendered += frames;
#ifdef IMMUNE_AUDIO_FTZ
    _mm_setcsr(csr);
#endif
}

void Mixer::render_block(f32* out, u32 n) {
    for (u32 b = 0; b < kBusCount; ++b) {
        std::memset(bus_l_[b], 0, sizeof(f32) * n);
        std::memset(bus_r_[b], 0, sizeof(f32) * n);
    }
    std::memset(send_, 0, sizeof(f32) * n);

    u32 live_voices = 0;
    for (Voice& v : voices_) {
        if (!v.active) continue;
        render_voice(v, n);
        live_voices += v.active ? 1u : 0u;
    }
    u32 live_grains = 0;
    for (Grain& g : grains_) {
        if (!g.active) continue;
        render_grain(g, n);
        live_grains += g.active ? 1u : 0u;
    }
    for (u32 b = 0; b < kBedCount; ++b) render_bed(static_cast<BedId>(b), beds_[b], n);

    stats_.active_voices = live_voices;
    stats_.active_grains = live_grains;
    stats_.peak_voices = std::max(stats_.peak_voices, live_voices);
    stats_.peak_grains = std::max(stats_.peak_grains, live_grains);

    const f32* ui_l = bus_l_[static_cast<u32>(Bus::Ui)];
    const f32* ui_r = bus_r_[static_cast<u32>(Bus::Ui)];
    const f32* al_l = bus_l_[static_cast<u32>(Bus::Alert)];
    const f32* al_r = bus_r_[static_cast<u32>(Bus::Alert)];
    const f32* fr_l = bus_l_[static_cast<u32>(Bus::Friendly)];
    const f32* fr_r = bus_r_[static_cast<u32>(Bus::Friendly)];
    const f32* ho_l = bus_l_[static_cast<u32>(Bus::Horde)];
    const f32* ho_r = bus_r_[static_cast<u32>(Bus::Horde)];

    f32 peak = stats_.peak;
    f32 gain_reduction = 1.0f;
    for (u32 i = 0; i < n; ++i) {
        // Room.
        const f32 in = send_[i] * kRoomInput;
        f32 wl = 0.0f, wr = 0.0f;
        for (u32 c = 0; c < 4; ++c) wl += combs_[c].process(in, kRoomFeedback, kRoomDamp);
        for (u32 c = 4; c < 8; ++c) wr += combs_[c].process(in, kRoomFeedback, kRoomDamp);
        wl = allpasses_[1].process(allpasses_[0].process(wl));
        wr = allpasses_[3].process(allpasses_[2].process(wr));

        duck_ *= duck_k_;
        const f32 combat = 1.0f - duck_;
        f32 l = ui_l[i] + al_l[i] + combat * (fr_l[i] + ho_l[i]) + wl * kRoomWet;
        f32 r = ui_r[i] + al_r[i] + combat * (fr_r[i] + ho_r[i]) + wr * kRoomWet;

        master_ = master_target_ + (master_ - master_target_) * master_k_;
        l *= master_;
        r *= master_;

        // Peak compressor with a fast attack and slow release, then a soft
        // clipper for whatever the attack let through.
        const f32 level = std::max(std::fabs(l), std::fabs(r));
        const f32 k = level > comp_env_ ? comp_attack_k_ : comp_release_k_;
        comp_env_ = level + (comp_env_ - level) * k;
        f32 g = 1.0f;
        if (comp_env_ > kCompThreshold) {
            g = (kCompThreshold + (comp_env_ - kCompThreshold) / kCompRatio) / comp_env_;
        }
        gain_reduction = std::min(gain_reduction, g);
        l = dsp::soft_clip(l * g);
        r = dsp::soft_clip(r * g);
        out[i * 2u] = l;
        out[i * 2u + 1u] = r;
        peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
    }
    stats_.peak = peak;
    stats_.limiter_gain = std::min(stats_.limiter_gain, gain_reduction);
    clock_ += n;
}

void Mixer::render_voice(Voice& v, u32 n) {
    u32 i = 0;
    if (v.delay > 0) {
        if (v.delay >= n) {
            v.delay -= n;
            return;
        }
        i = v.delay;
        v.delay = 0;
    }
    const Patch& p = patch(v.id);
    const u32 bus = static_cast<u32>(p.bus);
    f32* L = bus_l_[bus];
    f32* R = bus_r_[bus];
    const bool has_tone = p.tone > 0.0f;
    const bool has_partial = p.partial_ratio > 0.0f && p.partial_gain > 0.0f;
    const bool has_noise = p.noise > 0.0f;
    const bool has_filter = p.filter != FilterMode::Off;
    const bool phase_mod = p.fm_ratio > 0.0f && p.fm_index > 0.0f &&
                           (p.wave == Wave::Sine || p.wave == Wave::Triangle);

    while (i < n && v.active) {
        const u32 end = std::min(n, i + std::min(kControl, v.remaining));
        const u32 count = end - i;

        // ---- control rate ----
        f32 ratio = v.ratio;
        const f32 semis = p.pitch_semis * v.pitch_env + p.pitch_drift * v.t;
        if (semis != 0.0f) ratio *= dsp::semitones_to_ratio(semis);
        v.pitch_env *= v.pitch_k;
        const f32 inc1 = std::min(p.freq * ratio * inv_sr_, 0.45f);
        const f32 inc2 = std::min(inc1 * p.partial_ratio, 0.45f);
        const f32 incm = inc1 * p.fm_ratio;
        const f32 fm_index = phase_mod ? p.fm_index * v.fm_env : 0.0f;
        v.fm_env *= v.fm_k;
        if (has_filter) {
            f32 cutoff = p.cutoff * (p.key_track ? ratio : 1.0f);
            if (p.cutoff_octaves != 0.0f) cutoff *= std::exp2(p.cutoff_octaves * v.cut_env);
            v.cut_env *= v.cut_k;
            v.svf.set(cutoff, p.q, sample_rate_);
        }

        // ---- audio rate ----
        for (u32 s = i; s < end; ++s) {
            f32 x = 0.0f;
            if (has_tone) {
                f32 o;
                switch (p.wave) {
                    case Wave::Sine:
                        o = dsp::sine(phase_mod ? dsp::wrap01(v.ph1 + fm_index * dsp::sine(v.phm)) : v.ph1);
                        break;
                    case Wave::Triangle:
                        o = dsp::triangle(phase_mod ? dsp::wrap01(v.ph1 + fm_index * dsp::sine(v.phm)) : v.ph1);
                        break;
                    case Wave::Saw: o = dsp::saw(v.ph1, inc1); break;
                    case Wave::Square:
                    default: o = dsp::square(v.ph1, inc1); break;
                }
                x = p.tone * o;
                v.ph1 += inc1;
                if (v.ph1 >= 1.0f) v.ph1 -= 1.0f;
                if (phase_mod) {
                    v.phm += incm;
                    if (v.phm >= 1.0f) v.phm -= static_cast<f32>(static_cast<i32>(v.phm));
                }
            }
            if (has_partial) {
                x += p.partial_gain * v.partial_env * dsp::sine(v.ph2);
                v.ph2 += inc2;
                if (v.ph2 >= 1.0f) v.ph2 -= 1.0f;
                v.partial_env *= v.partial_k;
            }
            if (has_noise) {
                x += p.noise * v.noise_env * dsp::noise(v.rng);
                v.noise_env *= v.noise_k;
            }
            if (has_filter) {
                f32 lo, bp, hi;
                v.svf.tick(x, lo, bp, hi);
                x = lo * v.mix_low + bp * v.mix_band + hi * v.mix_high;
            }

            if (v.stage == 0) {
                v.amp += v.attack_inc;
                if (v.amp >= 1.0f) {
                    v.amp = 1.0f;
                    v.stage = v.hold > 0 ? 1 : 2;
                }
            } else if (v.stage == 1) {
                if (--v.hold == 0) v.stage = 2;
            } else {
                v.amp *= v.decay_k;
            }
            if (v.stolen) v.amp *= kill_k_;

            const f32 y = x * v.amp;
            L[s] += y * v.gl;
            R[s] += y * v.gr;
            send_[s] += y * v.send;
        }
        v.t += static_cast<f32>(count) * inv_sr_;
        v.remaining -= count;
        i = end;
        if (v.remaining == 0 || ((v.stage == 2 || v.stolen) && v.amp < kSilent)) v.active = false;
    }
}

void Mixer::render_grain(Grain& g, u32 n) {
    u32 i = 0;
    if (g.delay > 0) {
        if (g.delay >= n) {
            g.delay -= n;
            return;
        }
        i = g.delay;
        g.delay = 0;
    }
    f32* L = bus_l_[g.bus];
    f32* R = bus_r_[g.bus];
    const u32 end = std::min(n, i + g.remaining);
    for (u32 s = i; s < end; ++s) {
        const f32 tone = dsp::sine(g.ph) * g.amp;
        g.ph += g.inc;
        if (g.ph >= 1.0f) g.ph -= 1.0f;
        g.inc = std::min(g.inc * g.inc_mul, 0.45f);
        g.nlp += g.nlp_a * (dsp::noise(g.rng) - g.nlp);
        const f32 y = tone + g.nlp * g.namp * g.amp;
        g.amp *= g.amp_k;
        g.namp *= g.namp_k;
        L[s] += y * g.gl;
        R[s] += y * g.gr;
        send_[s] += y * g.send;
    }
    g.remaining -= end - i;
    if (g.remaining == 0 || g.amp < kSilent) g.active = false;
}

void Mixer::render_bed(BedId id, Bed& b, u32 n) {
    if (b.target <= 0.0f && b.level < kSilent) {
        b.level = 0.0f;
        return;
    }
    // Control rate for the whole block: pan, LFOs, filter cutoffs.
    const f32 block_seconds = static_cast<f32>(n) * inv_sr_;
    b.pan += (b.pan_target - b.pan) * std::min(1.0f, block_seconds * 3.0f);
    f32 left = 0.0f, right = 0.0f;
    dsp::pan_gains(b.pan, left, right);
    f32* L = bus_l_[static_cast<u32>(Bus::Horde)];
    f32* R = bus_r_[static_cast<u32>(Bus::Horde)];

    switch (id) {
        case BedId::Murmur: {
            // Slow "breathing" of low noise: two incommensurate LFOs so the
            // swell never repeats on a loop the ear can find.
            b.lfo1 = dsp::wrap01(b.lfo1 + 0.13f * block_seconds);
            b.lfo2 = dsp::wrap01(b.lfo2 + 0.21f * block_seconds);
            // Low-mid, not sub: a rumble under 250 Hz masks every other sound
            // and is inaudible on a laptop anyway.
            const f32 breath = 0.75f + 0.15f * dsp::sine(b.lfo1) + 0.1f * dsp::sine(b.lfo2);
            b.f1.set(420.0f * (1.0f + 0.25f * dsp::sine(b.lfo2)), 0.6f, sample_rate_);
            b.f2.set(1100.0f, 0.7f, sample_rate_);
            const f32 band_norm = 1.0f / 0.7f;
            for (u32 s = 0; s < n; ++s) {
                b.level += (b.target - b.level) * b.level_k;
                const f32 x = dsp::noise(b.rng);
                f32 lo, bp, hi, lo2, bp2, hi2;
                b.f1.tick(x, lo, bp, hi);
                b.f2.tick(x, lo2, bp2, hi2);
                const f32 y = (lo + 0.3f * bp2 * band_norm) * breath * b.level * kMurmurGain;
                L[s] += y * left;
                R[s] += y * right;
            }
            break;
        }
        case BedId::Gnaw: {
            // Chewing: band-passed noise gated by irregular bites. More feeders
            // means denser bites, not just a louder hiss.
            b.f1.set(1050.0f, 2.5f, sample_rate_);
            b.f2.set(2400.0f, 3.0f, sample_rate_);
            const f32 n1 = 1.0f / 2.5f, n2 = 1.0f / 3.0f;
            const f32 rate = 0.6f + 0.9f * b.activity;
            for (u32 s = 0; s < n; ++s) {
                b.level += (b.target - b.level) * b.level_k;
                if (--b.next_pulse == 0) {
                    b.env = 0.55f + 0.45f * dsp::noise01(b.rng);
                    const f32 gap = (0.05f + 0.09f * dsp::noise01(b.rng)) / rate;
                    b.next_pulse = std::max(seconds_to_samples(gap, sample_rate_), 1u);
                }
                b.env *= b.env_k;
                const f32 x = dsp::noise(b.rng);
                f32 lo, bp, hi, lo2, bp2, hi2;
                b.f1.tick(x, lo, bp, hi);
                b.f2.tick(x, lo2, bp2, hi2);
                const f32 y = (bp * n1 + 0.5f * bp2 * n2) * b.env * b.level * kGnawGain * 2.2f;
                L[s] += y * left;
                R[s] += y * right;
            }
            break;
        }
        case BedId::Fizz: {
            // Soda fizz: sparse random impulses rung through a bright band.
            // Its density, not its loudness, tracks how much is dying.
            b.f1.set(3200.0f, 1.2f, sample_rate_);
            const f32 n1 = 1.0f / 1.2f;
            const f32 probability = 0.002f + 0.018f * b.activity;
            for (u32 s = 0; s < n; ++s) {
                b.level += (b.target - b.level) * b.level_k;
                const f32 r = dsp::noise01(b.rng);
                const f32 impulse = r < probability ? dsp::noise(b.rng) : 0.0f;
                f32 lo, bp, hi;
                b.f1.tick(impulse * 6.0f + dsp::noise(b.rng) * 0.04f, lo, bp, hi);
                const f32 y = bp * n1 * b.level * kFizzGain;
                L[s] += y * left;
                R[s] += y * right;
            }
            break;
        }
        case BedId::Count:
            break;
    }
}

} // namespace immune::audio
