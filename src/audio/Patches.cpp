#include "audio/Patches.h"

#include <array>

namespace immune::audio {

namespace {

// Built during static initialisation, before any audio thread exists, so the
// callback's lookups never race a lazy first-use construction.
std::array<Patch, kSoundCount> build_patches() {
    std::array<Patch, kSoundCount> t{};
    const auto at = [&t](SoundId id) -> Patch& { return t[static_cast<usize>(id)]; };

    // ---- UI: short, clean, high, dry ----------------------------------------
    {
        Patch& p = at(SoundId::UiHover);
        p.name = "ui_hover"; p.bus = Bus::Ui; p.priority = Priority::High; p.max_instances = 2;
        p.freq = 1318.5f;                    // E6
        p.partial_ratio = 2.0f; p.partial_gain = 0.15f; p.partial_decay = 0.006f;
        p.attack = 0.001f; p.decay = 0.012f; p.length = 0.08f;
        p.gain = 0.07f; p.reverb = 0.0f;
    }
    {
        Patch& p = at(SoundId::UiClick);
        p.name = "ui_click"; p.bus = Bus::Ui; p.priority = Priority::High; p.max_instances = 3;
        p.freq = 880.0f;                     // A5, chirping down into it
        p.pitch_semis = 5.0f; p.pitch_time = 0.006f;
        p.partial_ratio = 2.0f; p.partial_gain = 0.4f; p.partial_decay = 0.02f;
        p.noise = 0.05f; p.noise_decay = 0.002f;
        p.attack = 0.0008f; p.decay = 0.045f; p.length = 0.25f;
        p.gain = 0.26f; p.reverb = 0.04f;
    }
    {
        Patch& p = at(SoundId::UiDeny);
        p.name = "ui_deny"; p.bus = Bus::Ui; p.priority = Priority::High; p.max_instances = 2;
        p.wave = Wave::Triangle; p.freq = 369.99f;   // F#4; the scene adds D4 after
        p.filter = FilterMode::LowPass; p.cutoff = 1400.0f; p.q = 0.8f;
        p.attack = 0.003f; p.decay = 0.07f; p.length = 0.35f;
        p.gain = 0.26f; p.reverb = 0.03f;
    }
    {
        Patch& p = at(SoundId::UiChime);
        p.name = "ui_chime"; p.bus = Bus::Ui; p.priority = Priority::High; p.max_instances = 8;
        p.freq = kD5;
        p.partial_ratio = 2.0f; p.partial_gain = 0.3f; p.partial_decay = 0.08f;
        p.fm_ratio = 1.0f; p.fm_index = 0.15f; p.fm_decay = 0.03f;
        p.attack = 0.002f; p.decay = 0.22f; p.length = 1.2f;
        p.gain = 0.18f; p.reverb = 0.18f;
    }
    {
        Patch& p = at(SoundId::Plop);
        p.name = "plop"; p.bus = Bus::Friendly; p.priority = Priority::High; p.max_instances = 3;
        p.freq = kD4; p.pitch_semis = -12.0f; p.pitch_time = 0.02f;   // rises into the note
        p.noise = 0.1f; p.noise_decay = 0.003f;
        p.filter = FilterMode::LowPass; p.cutoff = 2500.0f;
        p.attack = 0.001f; p.decay = 0.07f; p.length = 0.4f;
        p.gain = 0.3f; p.reverb = 0.12f;
    }

    // ---- Alerts: low register, where combat never sits -------------------------
    {
        Patch& p = at(SoundId::WaveSwell);
        p.name = "wave_swell"; p.bus = Bus::Alert; p.priority = Priority::High; p.max_instances = 4;
        p.wave = Wave::Saw; p.freq = kD3;
        p.filter = FilterMode::LowPass; p.cutoff = 1400.0f; p.q = 0.9f;
        p.cutoff_octaves = -1.6f; p.cutoff_time = 0.35f;              // opens as it swells
        p.attack = 0.35f; p.hold = 0.2f; p.decay = 0.45f; p.length = 2.0f;
        p.gain = 0.13f; p.reverb = 0.35f;
    }
    {
        Patch& p = at(SoundId::Thump);
        p.name = "thump"; p.bus = Bus::Alert; p.priority = Priority::Protected; p.max_instances = 3;
        p.freq = kD2; p.pitch_semis = 12.0f; p.pitch_time = 0.03f;
        // The octave partial is what a laptop speaker actually reproduces.
        p.partial_ratio = 2.0f; p.partial_gain = 0.5f; p.partial_decay = 0.12f;
        p.noise = 0.25f; p.noise_decay = 0.008f;
        p.filter = FilterMode::LowPass; p.cutoff = 900.0f;
        p.attack = 0.001f; p.decay = 0.16f; p.length = 0.8f;
        p.gain = 0.55f; p.reverb = 0.2f;
    }
    {
        Patch& p = at(SoundId::Warn);
        p.name = "warn"; p.bus = Bus::Alert; p.priority = Priority::High; p.max_instances = 2;
        p.wave = Wave::Triangle; p.freq = 123.47f;                    // B2
        p.partial_ratio = 1.5f; p.partial_gain = 0.5f;                // F#3 above it
        p.fm_ratio = 1.0f; p.fm_index = 0.3f; p.fm_decay = 0.2f;
        p.filter = FilterMode::LowPass; p.cutoff = 1200.0f;
        p.attack = 0.01f; p.decay = 0.25f; p.length = 0.9f;
        p.gain = 0.2f; p.reverb = 0.2f;
    }
    {
        Patch& p = at(SoundId::Lament);
        p.name = "lament"; p.bus = Bus::Alert; p.priority = Priority::High; p.max_instances = 6;
        p.wave = Wave::Triangle; p.freq = kD4; p.pitch_drift = -0.6f;
        p.filter = FilterMode::LowPass; p.cutoff = 1500.0f;
        p.attack = 0.02f; p.decay = 0.5f; p.length = 2.0f;
        p.gain = 0.2f; p.reverb = 0.35f;
    }

    // ---- Friendly attacks: tonal and bright --------------------------------------
    {
        Patch& p = at(SoundId::ShooterFire);
        p.name = "shooter_fire"; p.bus = Bus::Friendly; p.priority = Priority::Low; p.max_instances = 6;
        p.wave = Wave::Triangle; p.freq = kD5;
        p.pitch_semis = 9.0f; p.pitch_time = 0.012f;                  // a soft downward "pew"
        p.noise = 0.04f; p.noise_decay = 0.003f;
        p.filter = FilterMode::LowPass; p.cutoff = 3200.0f; p.key_track = true;
        p.attack = 0.0008f; p.decay = 0.03f; p.length = 0.18f;
        p.gain = 0.11f; p.reverb = 0.05f;
    }
    {
        Patch& p = at(SoundId::Engulf);
        p.name = "engulf"; p.bus = Bus::Friendly; p.priority = Priority::Normal; p.max_instances = 4;
        p.freq = kD3; p.pitch_semis = -5.0f; p.pitch_time = 0.03f;    // a gulp, rising
        p.partial_ratio = 2.0f; p.partial_gain = 0.45f; p.partial_decay = 0.05f;
        p.fm_ratio = 1.0f; p.fm_index = 0.6f; p.fm_decay = 0.05f;
        p.noise = 0.08f; p.noise_decay = 0.01f;
        p.filter = FilterMode::LowPass; p.cutoff = 1100.0f; p.q = 1.2f;
        p.cutoff_octaves = 1.0f; p.cutoff_time = 0.04f;
        p.attack = 0.004f; p.decay = 0.09f; p.length = 0.45f;
        p.gain = 0.26f; p.reverb = 0.12f;
    }
    {
        Patch& p = at(SoundId::Stab);
        p.name = "stab"; p.bus = Bus::Friendly; p.priority = Priority::Normal; p.max_instances = 4;
        p.freq = kD5; p.pitch_semis = 3.0f; p.pitch_time = 0.01f;
        p.partial_ratio = 3.0f; p.partial_gain = 0.2f; p.partial_decay = 0.02f;
        p.fm_ratio = 2.0f; p.fm_index = 0.8f; p.fm_decay = 0.025f;   // harmonic, so it rings in key
        p.attack = 0.001f; p.decay = 0.06f; p.length = 0.3f;
        p.gain = 0.12f; p.reverb = 0.08f;
    }
    {
        Patch& p = at(SoundId::MucusBurst);
        p.name = "mucus_burst"; p.bus = Bus::Friendly; p.priority = Priority::Normal; p.max_instances = 4;
        // A wet splat, not a boom: the body sits at D3 so a run of bombs does
        // not pile up under 250 Hz and muddy everything else.
        p.freq = kD3; p.tone = 0.6f; p.pitch_semis = 12.0f; p.pitch_time = 0.02f;
        p.partial_ratio = 2.0f; p.partial_gain = 0.3f; p.partial_decay = 0.04f;
        p.noise = 1.0f; p.noise_decay = 0.05f;
        p.filter = FilterMode::LowPass; p.cutoff = 1400.0f; p.q = 0.9f;
        p.cutoff_octaves = 1.5f; p.cutoff_time = 0.04f;
        p.attack = 0.001f; p.decay = 0.12f; p.length = 0.6f;
        p.gain = 0.32f; p.reverb = 0.2f;
    }
    {
        Patch& p = at(SoundId::Knock);
        p.name = "knock"; p.bus = Bus::Friendly; p.priority = Priority::Normal; p.max_instances = 4;
        p.freq = kD4;
        p.partial_ratio = 4.0f; p.partial_gain = 0.35f; p.partial_decay = 0.012f;   // marimba bar
        p.noise = 0.15f; p.noise_decay = 0.002f;
        p.filter = FilterMode::LowPass; p.cutoff = 3000.0f;
        p.attack = 0.0008f; p.decay = 0.065f; p.length = 0.4f;
        p.gain = 0.22f; p.reverb = 0.1f;
    }
    {
        Patch& p = at(SoundId::Zap);
        p.name = "zap"; p.bus = Bus::Friendly; p.priority = Priority::Low; p.max_instances = 4;
        p.wave = Wave::Saw; p.freq = kD5;
        p.filter = FilterMode::BandPass; p.cutoff = 2000.0f; p.q = 2.0f;
        p.cutoff_octaves = -1.0f; p.cutoff_time = 0.05f;
        p.attack = 0.001f; p.decay = 0.08f; p.length = 0.3f;
        p.gain = 0.09f; p.reverb = 0.08f;
    }
    {
        Patch& p = at(SoundId::Sparkle);
        p.name = "sparkle"; p.bus = Bus::Friendly; p.priority = Priority::High; p.max_instances = 8;
        p.freq = kD6;
        p.partial_ratio = 2.0f; p.partial_gain = 0.25f; p.partial_decay = 0.04f;
        p.fm_ratio = 4.0f; p.fm_index = 0.2f; p.fm_decay = 0.02f;
        p.attack = 0.001f; p.decay = 0.12f; p.length = 0.8f;
        p.gain = 0.13f; p.reverb = 0.3f;
    }
    {
        Patch& p = at(SoundId::Whoosh);
        p.name = "whoosh"; p.bus = Bus::Friendly; p.priority = Priority::High; p.max_instances = 2;
        p.freq = kD3; p.tone = 0.35f; p.pitch_semis = -12.0f; p.pitch_time = 0.3f;
        p.noise = 1.0f;
        p.filter = FilterMode::BandPass; p.cutoff = 1600.0f; p.q = 1.4f;
        p.cutoff_octaves = -2.0f; p.cutoff_time = 0.25f;              // sweeps up from ~400 Hz
        p.attack = 0.12f; p.hold = 0.05f; p.decay = 0.35f; p.length = 1.6f;
        p.gain = 0.3f; p.reverb = 0.3f;
    }
    {
        Patch& p = at(SoundId::ClotThud);
        p.name = "clot_thud"; p.bus = Bus::Friendly; p.priority = Priority::High; p.max_instances = 2;
        p.freq = kD2; p.pitch_semis = 10.0f; p.pitch_time = 0.04f;
        p.partial_ratio = 2.0f; p.partial_gain = 0.5f; p.partial_decay = 0.1f;
        p.noise = 0.4f; p.noise_decay = 0.02f;
        p.filter = FilterMode::LowPass; p.cutoff = 700.0f; p.q = 1.0f;
        p.attack = 0.001f; p.decay = 0.25f; p.length = 1.0f;
        p.gain = 0.42f; p.reverb = 0.25f;
    }

    // ---- Friendly deaths: falling, darkening ----------------------------------
    {
        Patch& p = at(SoundId::CellDeath);
        p.name = "cell_death"; p.bus = Bus::Friendly; p.priority = Priority::High; p.max_instances = 4;
        p.wave = Wave::Triangle; p.freq = kD4;
        p.pitch_semis = 2.0f; p.pitch_time = 0.01f; p.pitch_drift = -16.0f;
        p.noise = 0.25f; p.noise_decay = 0.012f;
        p.filter = FilterMode::LowPass; p.cutoff = 1400.0f; p.q = 1.0f; p.key_track = true;
        p.cutoff_octaves = 1.2f; p.cutoff_time = 0.06f;               // closes as it falls
        p.attack = 0.002f; p.decay = 0.13f; p.length = 0.6f;
        p.gain = 0.24f; p.reverb = 0.15f;
    }
    {
        Patch& p = at(SoundId::TowerLost);
        p.name = "tower_lost"; p.bus = Bus::Friendly; p.priority = Priority::Protected; p.max_instances = 2;
        p.freq = kD2; p.pitch_semis = 10.0f; p.pitch_time = 0.05f; p.pitch_drift = -3.0f;
        p.partial_ratio = 1.5f; p.partial_gain = 0.5f; p.partial_decay = 0.3f;
        p.noise = 0.6f; p.noise_decay = 0.08f;
        p.filter = FilterMode::LowPass; p.cutoff = 1200.0f;
        p.cutoff_octaves = 1.0f; p.cutoff_time = 0.15f;
        p.attack = 0.002f; p.decay = 0.45f; p.length = 2.0f;
        p.gain = 0.5f; p.reverb = 0.35f;
    }
    {
        Patch& p = at(SoundId::ScarBreak);
        p.name = "scar_break"; p.bus = Bus::Friendly; p.priority = Priority::High; p.max_instances = 3;
        p.freq = kD3; p.tone = 0.6f; p.pitch_semis = 5.0f; p.pitch_time = 0.02f;
        p.noise = 1.0f; p.noise_decay = 0.05f;
        p.filter = FilterMode::LowPass; p.cutoff = 1800.0f; p.q = 0.9f;
        p.cutoff_octaves = 1.0f; p.cutoff_time = 0.03f;
        p.attack = 0.001f; p.decay = 0.1f; p.length = 0.6f;
        p.gain = 0.3f; p.reverb = 0.18f;
    }

    // ---- Horde attacks: organic, noisy ------------------------------------------
    {
        Patch& p = at(SoundId::Latch);
        p.name = "latch"; p.bus = Bus::Horde; p.priority = Priority::Normal; p.max_instances = 4;
        p.freq = kD3; p.tone = 0.4f; p.pitch_semis = -7.0f; p.pitch_time = 0.04f;
        p.noise = 1.0f;
        p.filter = FilterMode::BandPass; p.cutoff = 650.0f; p.q = 3.0f;
        p.cutoff_octaves = 1.2f; p.cutoff_time = 0.05f;               // a falling "thwop"
        p.attack = 0.003f; p.decay = 0.07f; p.length = 0.35f;
        p.gain = 0.2f; p.reverb = 0.06f;
    }
    {
        Patch& p = at(SoundId::Spit);
        p.name = "spit"; p.bus = Bus::Horde; p.priority = Priority::Low; p.max_instances = 4;
        p.freq = 185.0f; p.tone = 0.5f; p.pitch_semis = 7.0f; p.pitch_time = 0.015f;
        p.noise = 1.0f; p.noise_decay = 0.02f;
        p.filter = FilterMode::BandPass; p.cutoff = 1500.0f; p.q = 1.6f;
        p.cutoff_octaves = 0.8f; p.cutoff_time = 0.02f;
        p.attack = 0.001f; p.decay = 0.04f; p.length = 0.25f;
        p.gain = 0.13f; p.reverb = 0.05f;
    }
    {
        Patch& p = at(SoundId::Sizzle);
        p.name = "sizzle"; p.bus = Bus::Horde; p.priority = Priority::Low; p.max_instances = 3;
        p.tone = 0.0f; p.noise = 1.0f;
        p.filter = FilterMode::HighPass; p.cutoff = 3500.0f; p.q = 0.8f;
        p.attack = 0.002f; p.decay = 0.08f; p.length = 0.4f;
        p.gain = 0.06f; p.reverb = 0.04f;
    }
    {
        Patch& p = at(SoundId::Harpoon);
        p.name = "harpoon"; p.bus = Bus::Horde; p.priority = Priority::High; p.max_instances = 3;
        p.wave = Wave::Saw; p.freq = 123.47f;                         // B2
        p.pitch_semis = 12.0f; p.pitch_time = 0.035f;                 // "thwip"
        p.noise = 0.2f; p.noise_decay = 0.004f;
        p.filter = FilterMode::LowPass; p.cutoff = 1800.0f; p.q = 2.0f;
        p.cutoff_octaves = 1.0f; p.cutoff_time = 0.04f;               // resonant twang
        p.attack = 0.001f; p.decay = 0.12f; p.length = 0.6f;
        p.gain = 0.2f; p.reverb = 0.12f;
    }
    {
        Patch& p = at(SoundId::Glug);
        p.name = "glug"; p.bus = Bus::Horde; p.priority = Priority::Normal; p.max_instances = 3;
        p.freq = 110.0f; p.pitch_semis = 4.0f; p.pitch_time = 0.03f;  // A2
        p.partial_ratio = 2.0f; p.partial_gain = 0.4f; p.partial_decay = 0.05f;
        p.fm_ratio = 0.5f; p.fm_index = 0.8f; p.fm_decay = 0.06f;
        p.filter = FilterMode::LowPass; p.cutoff = 600.0f;
        p.attack = 0.004f; p.decay = 0.08f; p.length = 0.4f;
        p.gain = 0.22f; p.reverb = 0.1f;
    }
    {
        Patch& p = at(SoundId::Chew);
        p.name = "chew"; p.bus = Bus::Horde; p.priority = Priority::Low; p.max_instances = 3;
        p.wave = Wave::Triangle; p.freq = kD3; p.tone = 0.35f;
        p.noise = 1.0f; p.noise_decay = 0.025f;
        p.filter = FilterMode::LowPass; p.cutoff = 1300.0f; p.q = 1.2f;
        p.cutoff_octaves = 0.6f; p.cutoff_time = 0.02f;
        p.attack = 0.001f; p.decay = 0.04f; p.length = 0.25f;
        p.gain = 0.14f; p.reverb = 0.06f;
    }
    return t;
}

std::array<GrainShape, kGrainKindCount> build_grains() {
    std::array<GrainShape, kGrainKindCount> t{};
    t[static_cast<usize>(GrainKind::Bubble)] = {"bubble", Bus::Horde, 0.035f, 0.9f, 0.1f, 0.002f, 0.6f, 0.08f};
    t[static_cast<usize>(GrainKind::Pop)] = {"pop", Bus::Horde, 0.04f, -0.7f, 0.25f, 0.003f, 0.45f, 0.08f};
    t[static_cast<usize>(GrainKind::Squelch)] = {"squelch", Bus::Horde, 0.055f, -1.0f, 0.6f, 0.012f, 0.25f, 0.1f};
    t[static_cast<usize>(GrainKind::Puff)] = {"puff", Bus::Horde, 0.05f, -0.4f, 1.2f, 0.025f, 0.2f, 0.1f};
    t[static_cast<usize>(GrainKind::Tick)] = {"tick", Bus::Friendly, 0.009f, -0.3f, 0.5f, 0.002f, 0.7f, 0.02f};
    return t;
}

const std::array<Patch, kSoundCount> kPatches = build_patches();
const std::array<GrainShape, kGrainKindCount> kGrains = build_grains();

} // namespace

const Patch& patch(SoundId id) {
    const usize i = static_cast<usize>(id);
    return kPatches[i < kSoundCount ? i : 0];
}

const char* sound_name(SoundId id) { return patch(id).name; }

const GrainShape& grain_shape(GrainKind kind) {
    const usize i = static_cast<usize>(kind);
    return kGrains[i < kGrainKindCount ? i : 0];
}

} // namespace immune::audio
