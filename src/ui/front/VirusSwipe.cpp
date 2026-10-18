#include "ui/front/VirusSwipe.h"

#include "core/Rng.h"
#include "gui/core/Gui.h"
#include "gui/draw/DrawList.h"
#include "gui/widgets/Builders.h"
#include "render/ChaffBatcher.h"

#include <algorithm>
#include <cmath>

namespace immune::ui {

namespace {
f32 individual_progress(f32 elapsed, f32 duration, f32 lag, f32 pace,
                        const VirusSwipeConfig& config, bool exiting) {
    const f32 stagger = math::min(config.stagger_seconds / duration, 0.65f) * lag;
    const f32 delay = exiting ? 0.0f : duration * stagger;
    f32 t = math::saturate((elapsed - delay) / (duration - delay));
    // Vary speed without reaching the middle early and waiting there.
    // Exit lag changes acceleration, rather than parking viruses at rest.
    const f32 variation = math::clamp(config.speed_variation * (2.0f * pace - 1.0f) -
                                     (exiting ? stagger : 0.0f), -0.85f, 0.85f);
    t = math::saturate(t + variation * std::sin(t * math::kPi) / math::kPi);
    // Zero velocity only at the OFFSCREEN ends. The two curves retain a
    // forward velocity at their shared midpoint instead of easing to a stop.
    return exiting ? t + t * t - t * t * t : t * t * (2.0f - t);
}
} // namespace

VirusSwipe::VirusSwipe(std::string id) : Widget(std::move(id)) {
    gui::anchor(*this, Vec2{0, 0}, Vec2{0, 0});
    layout.width = gui::Size::pct(1.0f);
    layout.height = gui::Size::pct(1.0f);
    blocks_pointer = true;
    visible = false;
}

void VirusSwipe::start(const VirusSwipeConfig& config) {
    config_ = config;
    elapsed_ = motion_time_ = 0.0f;
    populated_size_ = Vec2{};
    viruses_.clear();
    phase_ = Phase::Cover;
    visible = true;
}

void VirusSwipe::reveal() {
    if (phase_ != Phase::Covered) return;
    phase_ = Phase::Reveal;
    // Preserve sub-frame overshoot across the screen swap. An authored hold
    // controls the brief delay before departure.
    const f32 moving = config_.duration_seconds - config_.hold_seconds;
    elapsed_ -= moving * config_.cover_fraction + config_.hold_seconds;
}

void VirusSwipe::finish() {
    phase_ = Phase::Idle;
    visible = false;
}

void VirusSwipe::update(f32 dt) {
    if (!active() || !std::isfinite(dt) || dt < 0.0f) return;
    motion_time_ += dt;
    // Wait for the destination without consuming its reveal clock.
    if (phase_ == Phase::Covered) return;
    elapsed_ += dt;
    const f32 moving = config_.duration_seconds - config_.hold_seconds;
    if (phase_ == Phase::Cover && elapsed_ >= moving * config_.cover_fraction +
                                            config_.hold_seconds * config_.switch_fraction) {
        phase_ = Phase::Covered;
    } else if (phase_ == Phase::Reveal && elapsed_ >= moving * (1.0f - config_.cover_fraction)) {
        finish();
    }
}

void VirusSwipe::populate(Vec2 viewport) {
    populated_size_ = viewport;
    viruses_.clear();
    viruses_.reserve(static_cast<usize>(config_.virus_count));
    Rng rng(config_.seed);
    // Stratification fills the whole height, even at very low counts; size
    // sorting gives larger foreground viruses a natural overlapping depth.
    const i32 rows = math::clamp(static_cast<i32>(std::sqrt(static_cast<f32>(config_.virus_count) *
                                                           viewport.y / math::max(viewport.x, 1.0f))),
                                 1, config_.virus_count);
    i32 remaining = config_.virus_count;
    for (i32 row = 0; row < rows; ++row) {
        const i32 columns = remaining / (rows - row);
        remaining -= columns;
        for (i32 col = 0; col < columns; ++col) {
            Virus v;
            v.at = Vec2{(static_cast<f32>(col) + 0.5f + rng.range_f(-0.38f, 0.38f)) / static_cast<f32>(columns),
                        (static_cast<f32>(row) + 0.5f + rng.range_f(-0.38f, 0.38f)) / static_cast<f32>(rows)};
            v.size = rng.range_f(config_.min_size, config_.max_size) * std::sqrt(config_.density);
            v.angle = rng.range_f(0.0f, math::kTwoPi);
            v.phase = rng.range_f(0.0f, math::kTwoPi);
            v.spin = rng.range_f(-1.0f, 1.0f);
            v.enter_lag = rng.next_f32();
            v.exit_lag = rng.next_f32();
            v.pace = rng.next_f32();
            v.bend = rng.range_f(-1.0f, 1.0f);
            v.sway = rng.range_f(0.6f, 1.5f);
            viruses_.push_back(v);
        }
    }
    std::stable_sort(viruses_.begin(), viruses_.end(), [](const Virus& a, const Virus& b) { return a.size < b.size; });
}

void VirusSwipe::draw_self(gui::DrawList& dl) {
    if (!active() || gui() == nullptr || size().x <= 0.0f || size().y <= 0.0f) return;
    if (populated_size_ != size()) populate(size());
    const f32 w = size().x, h = size().y;
    const f32 largest = config_.max_size * std::sqrt(config_.density);
    const f32 margin = largest * 1.6f + config_.edge_irregularity + config_.drift;
    const f32 distance = w + 2.0f * margin;
    const f32 moving = config_.duration_seconds - config_.hold_seconds;
    const bool revealing = phase_ == Phase::Reveal;

    dl.push_clip_rect(rect());
    dl.push_transform(gui::Affine2::translate(rect().min));
    if (!config_.from_left) {
        dl.push_transform(gui::Affine2::translate(Vec2{w, 0.0f}) * gui::Affine2::scale(Vec2{-1.0f, 1.0f}));
    }
    // Share both the gameplay material (chaff.frag) and its live family
    // colour/animation tuning. Only positions and sizes are screen-space.
    const Vec4 family_color = render::family_color(PathogenFamily::Virus);
    const auto& visual = render::family_visual(PathogenFamily::Virus);
    for (const Virus& v : viruses_) {
        f32 progress = 1.0f;
        f32 offset = 0.0f;
        if (phase_ == Phase::Cover) {
            progress = individual_progress(elapsed_, moving * config_.cover_fraction, v.enter_lag, v.pace, config_, false);
            offset = -distance * (1.0f - progress);
        } else if (revealing) {
            progress = individual_progress(elapsed_, moving * (1.0f - config_.cover_fraction), v.exit_lag, 1.0f - v.pace, config_, true);
            offset = distance * progress;
        }
        const f32 wave = std::sin(v.at.y * 11.0f + 0.6f) * config_.edge_irregularity;
        Vec2 p{-margin + v.at.x * (w + 2.0f * margin) + offset + wave,
               -largest * 0.5f + v.at.y * (h + largest)};
        p += Vec2{std::sin(motion_time_ * 6.0f * v.sway + v.phase),
                  std::cos(motion_time_ * 5.0f * v.sway + v.phase)} * config_.drift;
        p.y += std::sin(progress * math::kPi) * v.bend * config_.path_spread;
        // Cull without allocating a widget or a separate draw call per virus.
        const f32 radius = v.size * 1.2f;
        if (p.x + radius < 0.0f || p.x - radius > w || p.y + radius < 0.0f || p.y - radius > h) continue;
        const f32 angle = v.angle + motion_time_ * config_.spin * v.spin;
        dl.push_transform(gui::Affine2::translate(p) * gui::Affine2::rotate(angle));
        const gui::Color tint = config_.tint * family_color;
        const Vec2 half{v.size * 0.5f, v.size * 0.5f};
        dl.gameplay_virus(Rect{-half, half}, tint, v.phase + motion_time_ * visual.tempo, visual.wobble);
        dl.pop_transform();
    }
    if (!config_.from_left) dl.pop_transform();
    dl.pop_transform();
    dl.pop_clip_rect();
}

} // namespace immune::ui
