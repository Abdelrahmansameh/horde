// Wave 0 stub. Implementation is owned by Wave 3B.
#include "ui/Hud.h"

#include "game/economy/Economy.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "platform/Input.h"
#include "render/Camera.h"
#include "sim/SimWorld.h"

namespace immune::ui {

bool Hud::init() {
    initialized_ = true;
    return true;
}

void Hud::shutdown() { initialized_ = false; }
void Hud::begin_frame(platform::InputState&) {}

void Hud::build(const sim::SimWorld&, const game::Economy&, const game::WaveDirector&,
                const game::TowerSystem&, const render::Camera&, platform::InputState&,
                std::vector<Intent>&) {}

void Hud::render() {}
void Hud::update_threat_overlay(const sim::SimWorld&) { threats_.clear(); }

} // namespace immune::ui
