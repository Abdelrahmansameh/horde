// A cosmetic, screen-sized virus swarm. Navigation waits at peak coverage;
// reveal() is called only once the destination screen has been installed.
#pragma once

#include "gui/core/Widget.h"
#include "ui/front/VirusSwipeConfig.h"

#include <vector>

namespace immune::ui {

class VirusSwipe : public gui::Widget {
public:
    explicit VirusSwipe(std::string id = "virus_swipe");
    void start(const VirusSwipeConfig& config);
    bool active() const { return phase_ != Phase::Idle; }
    bool ready_to_reveal() const { return phase_ == Phase::Covered; }
    void reveal();
    void finish();
    void update(f32 dt) override;
    void draw_self(gui::DrawList& dl) override;

private:
    enum class Phase { Idle, Cover, Covered, Reveal };
    struct Virus {
        Vec2 at;
        f32 size;
        f32 angle;
        f32 phase;
        f32 spin;
        f32 enter_lag;
        f32 exit_lag;
        f32 pace;
        f32 bend;
        f32 sway;
    };
    void populate(Vec2 viewport);

    VirusSwipeConfig config_;
    Phase phase_ = Phase::Idle;
    f32 elapsed_ = 0.0f;
    f32 motion_time_ = 0.0f;
    Vec2 populated_size_{};
    std::vector<Virus> viruses_;
};

} // namespace immune::ui
