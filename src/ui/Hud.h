// ui/Hud.h — HUD, build menu, threat overlays. FROZEN CONTRACT.
// Owner: Wave 3B.
//
// RATIONALE (DESIGN.md §2 readability pillar)
// At 10,000 agents the player cannot read individual units, so the HUD carries
// the readability load: per-lane threat indicators summarize each lane's density
// and dominant family into one glanceable mark. Those indicators are computed
// from aggregate sim data (SpatialHash occupancy + family counts), never by
// iterating agents.
//
// The UI emits *intents*, it does not mutate the sim. app/ translates intents
// into sim commands so that every state change goes through one auditable path
// (which is also how --sim-test scripts drive the game).
#pragma once

#include "core/Types.h"
#include "game/abilities/ActiveAbilities.h"

#include <string>
#include <vector>

namespace immune::platform { class InputState; class Window; }
namespace immune::render { class Camera; }
namespace immune::sim { class SimWorld; }

namespace immune::game { class Economy; class WaveDirector; class TowerSystem; }

namespace immune::ui {

/// What the player asked for this frame. Consumed by app/, not by ui/.
enum class IntentKind : u8 {
    None = 0,
    PlaceTower,
    SelectTower,
    UpgradeTower,
    SellTower,
    TriggerAbility,
    CastAbility,     ///< A DESIGN.md §5.6 active ability, distinct from a tower's own TriggerAbility.
    SetTimeScale,
    StartWaveEarly,
    OpenMenu,
    QuitToMenu,
};

struct Intent {
    IntentKind kind = IntentKind::None;
    TowerType tower_type = TowerType::Macrophage;
    Vec2 world_position{0.0f, 0.0f};
    EntityId entity{};
    f32 value = 0.0f;   ///< SetTimeScale payload.
    game::AbilityId ability_id = game::AbilityId::ComplementCascadeBurst; ///< CastAbility payload.
};

/// One lane's summarized threat, rendered as a single indicator.
struct LaneThreat {
    std::string lane_id;
    Vec2 anchor{0.0f, 0.0f};        ///< Where to draw the indicator.
    f32 density = 0.0f;             ///< Total chaff density in the lane.
    PathogenFamily dominant = PathogenFamily::Virus;
    f32 severity01 = 0.0f;          ///< Normalized 0..1 for colour ramp.
};

class Hud {
public:
    /// Sets up the ImGui context and the SDL2/GL3 backends. `window` must
    /// already own a live GL context (Window::create() already called) since
    /// ImGui_ImplOpenGL3_Init needs a current context and
    /// ImGui_ImplSDL2_InitForOpenGL needs the SDL_Window/SDL_GLContext pair.
    /// Also installs itself as InputState's raw-event sink so ImGui sees SDL
    /// events, since InputState::poll() owns the one SDL_PollEvent loop.
    bool init(platform::Window& window, platform::InputState& input);
    void shutdown();

    /// Begins an ImGui frame and forwards capture flags back into InputState so
    /// clicks on the build menu do not also place a tower.
    void begin_frame(platform::InputState& input);

    /// Builds the HUD for this frame and appends any player intents.
    void build(const sim::SimWorld& world,
               const game::Economy& economy,
               const game::WaveDirector& waves,
               const game::TowerSystem& towers,
               const game::ActiveAbilitySystem& abilities,
               const render::Camera& camera,
               platform::InputState& input,
               std::vector<Intent>& out_intents);

    /// Issues the ImGui draw data. Must run after Renderer::end_frame.
    void render();

    /// Recomputes lane threat summaries. Cheap: reads aggregate data only.
    void update_threat_overlay(const sim::SimWorld& world);
    const std::vector<LaneThreat>& lane_threats() const { return threats_; }

    void set_threat_overlay_visible(bool v) { threat_overlay_ = v; }
    void set_debug_overlay_visible(bool v) { debug_overlay_ = v; }
    bool debug_overlay_visible() const { return debug_overlay_; }

    /// Squad routes/anchors overlay. Separate from the debug (flow-arrow)
    /// overlay rather than folded into it: the two are drawn over the same
    /// ground and reading either one is much harder with the other on.
    void set_squad_overlay_visible(bool v) { squad_overlay_ = v; }
    bool squad_overlay_visible() const { return squad_overlay_; }

    /// Tower currently armed on the build cursor, if any.
    bool has_build_cursor() const { return build_cursor_active_; }
    TowerType build_cursor_type() const { return build_cursor_type_; }
    void set_build_cursor(TowerType type) { build_cursor_active_ = true; build_cursor_type_ = type; }
    void clear_build_cursor() { build_cursor_active_ = false; }

private:
    std::vector<LaneThreat> threats_;
    bool threat_overlay_ = true;
    bool debug_overlay_ = false;
    bool squad_overlay_ = false;
    bool build_cursor_active_ = false;
    bool initialized_ = false;
    TowerType build_cursor_type_ = TowerType::Macrophage;
};

} // namespace immune::ui
