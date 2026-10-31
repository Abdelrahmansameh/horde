// app/AudioRender.h — offline audition of the game's audio.
//
// `immune --audio-render out.wav` drives the real SoundScape and Mixer through
// a scripted timeline -- every UI cue, every combat sound one at a time, the
// run stingers, then a ten-second horde that escalates from a trickle to
// thousands of deaths a second -- and writes the result as a WAV you can
// listen to, plus a JSON report of loudness and render cost per segment.
//
// It exists because sound quality cannot be unit-tested: this is how a change
// to a patch or to the horde policy is auditioned without playing a level,
// and how the audio thread's cost is measured on a given machine.
#pragma once

#include "app/Cli.h"
#include "core/Types.h"

#include <string>
#include <vector>

namespace immune::app {

struct AudioSegmentReport {
    std::string name;
    f64 seconds = 0.0;
    f32 peak = 0.0f;          ///< Largest |sample|, linear.
    f32 rms = 0.0f;           ///< Over the whole segment, linear.
    /// Render time over audio time: 0.02 means the mixer used 2% of one core.
    f64 load = 0.0;
    f64 load_worst_frame = 0.0;
    u32 max_voices = 0;
    u32 max_grains = 0;
    u64 events = 0;           ///< Combat events fed to the scene.
    u64 voices_posted = 0;
    u64 grains_posted = 0;
};

struct AudioRenderReport {
    f32 sample_rate = 48000.0f;
    f64 seconds = 0.0;
    f64 render_seconds = 0.0;
    u64 clipped_samples = 0;  ///< |x| >= 0.999 after the limiter.
    u64 non_finite_samples = 0;
    u64 voices_started = 0, voices_stolen = 0, voices_rejected = 0;
    u64 grains_started = 0, grains_stolen = 0;
    u64 events_seen = 0, events_culled = 0, events_folded = 0;
    u64 ring_dropped = 0;
    std::vector<AudioSegmentReport> segments;
};

/// Renders the audition timeline. `pcm` (optional) receives interleaved
/// stereo float samples.
AudioRenderReport render_audition(f32 sample_rate, std::vector<f32>* pcm);

/// --audio-render: renders, writes a 16-bit WAV, prints the report as JSON.
int run_audio_render(const Options& options);

} // namespace immune::app
