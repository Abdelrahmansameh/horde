#pragma once
#include "config/Json.h"
#include "game/config/GameConfig.h"
#include "game/meta/MetaProgression.h"
#include <functional>
#include <string_view>

namespace immune::sim { class SimWorld; }
namespace immune::game { struct LevelDef; class Economy; class WaveDirector;
class TowerSystem; class ActiveAbilitySystem; }
namespace immune::game::balance {
struct CaptureView {
    const sim::SimWorld& world;
    const LevelDef& level;
    const Economy& economy;
    const WaveDirector& waves;
    const TowerSystem& towers;
    const ActiveAbilitySystem& abilities;
};
using CaptureCallback = std::function<void(const CaptureView&)>;
/// Observes completed campaign attempts without changing the simulation.
using AttemptCallback = std::function<void(const config::Json&)>;
// Runs in process with an isolated progression object. Never accesses a user save
// unless an explicit read-only snapshot path is supplied in the specification.
config::Json execute(const config::Json& spec, const GameConfig& base,
                      const CaptureCallback& capture = {}, const AttemptCallback& attempt_completed = {});
u64 fingerprint(std::string_view text);
} // namespace immune::game::balance
