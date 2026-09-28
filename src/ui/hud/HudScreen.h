// ui/hud/HudScreen.h — the in-match HUD, built on the gui framework.
//
// Layout and look follow the design canvas's five HUD artboards
// (docs/ui-concepts/canvas: Main, Placing, Inspect, Prep, Critical):
//
//   top-left     organ integrity panel (turns red and pulses when critical)
//   top-centre   prep banner (countdown, "Send now"), "Wave N cleared" toast,
//                "Organ failing" banner
//   top-right    pause / 1x / 2x / menu, then the next-wave panel
//   bottom       build dock (ATP, then Attack and Control cards), ability cells
//   world        range rings, placement ghost, aim reticle, tower health pips,
//                the selected tower's popup, the incoming composition at the
//                spawn during prep
//
// The screen owns the HUD's interaction state (armed tower, armed ability,
// selected tower) — what used to be Hud.cpp's file statics — and reports what
// the player asked for as ui::Intent, exactly like the ImGui HUD did.
//
// Frame order (app/): UiBridge fills a HudModel → sync() → gui.frame() →
// handle_input() → apply the intents.
#pragma once

#include "core/Types.h"
#include "ui/Intent.h"
#include "ui/hud/HudModel.h"

#include <array>
#include <map>
#include <vector>

namespace immune::platform { class InputState; }
namespace immune::gui { class Gui; class Widget; class Panel; class Label; class Meter; class Button; class Ring;
                        class Icon; }

namespace immune::ui {

class BuildCard;
class AbilityCell;
class WaveRow;
class RangeRing;
class TowerPopup;
class HintPill;

class HudScreen {
public:
    explicit HudScreen(gui::Gui& gui);
    ~HudScreen();
    HudScreen(const HudScreen&) = delete;
    HudScreen& operator=(const HudScreen&) = delete;

    void set_visible(bool v);
    bool visible() const { return visible_; }

    /// Brings every widget up to date with `m`. `world_cursor` is the world
    /// point under the pointer (the aim reticle follows it).
    void sync(const HudModel& m, Vec2 world_cursor);

    /// World clicks and hotkeys, plus whatever the HUD's buttons queued during
    /// gui.frame(). `pointer_over_ui` is gui.wants_pointer().
    void handle_input(const platform::InputState& in, Vec2 world_cursor, bool pointer_over_ui,
                      std::vector<Intent>& out);

    /// Escape: disarms a cursor or closes the popup. False when there was
    /// nothing to cancel (so Escape can open the pause menu instead).
    bool cancel();

    // ---- Interaction state --------------------------------------------------------
    bool has_build_cursor() const { return armed_tower_ != TowerType::Count; }
    TowerType build_cursor() const { return armed_tower_; }
    bool has_cast_cursor() const { return armed_ability_ != game::AbilityId::Count; }
    game::AbilityId cast_cursor() const { return armed_ability_; }
    EntityId selected() const { return selected_; }

    void arm_tower(TowerType t);
    void arm_ability(game::AbilityId a);
    void select(EntityId id);

private:
    void build();
    void sync_world(const HudModel& m, Vec2 world_cursor);
    void push(const Intent& i) { pending_.push_back(i); }
    void press_ability(game::AbilityId a);

    gui::Gui& gui_;
    bool visible_ = true;
    HudModel model_{};

    TowerType armed_tower_ = TowerType::Count;
    game::AbilityId armed_ability_ = game::AbilityId::Count;
    EntityId selected_{};
    f32 resume_scale_ = 1.0f;
    std::vector<Intent> pending_;

    // Wave-cleared toast.
    u32 last_wave_number_ = 0;
    HudWavePhase last_phase_ = HudWavePhase::Prep;
    f32 toast_until_ = -1.0f;  ///< gui time the toast disappears at

    // ---- Widgets (owned by the gui layers) ----
    gui::Widget* hud_ = nullptr;
    gui::Widget* world_ = nullptr;
    gui::Widget* popup_layer_root_ = nullptr;

    gui::Panel* vignette_ = nullptr;
    gui::Panel* organ_ = nullptr;
    gui::Label* organ_pct_ = nullptr;
    gui::Meter* organ_bar_ = nullptr;
    gui::Panel* critical_banner_ = nullptr;

    gui::Panel* prep_banner_ = nullptr;
    gui::Ring* prep_ring_ = nullptr;
    gui::Label* prep_clock_ = nullptr;
    gui::Label* prep_wave_ = nullptr;
    gui::Panel* toast_ = nullptr;
    gui::Label* toast_text_ = nullptr;

    std::array<gui::Button*, 4> controls_{};
    gui::Panel* wave_panel_ = nullptr;
    gui::Label* wave_title_ = nullptr;
    gui::Label* wave_total_ = nullptr;
    std::array<WaveRow*, kFamilyCount> wave_rows_{};
    gui::Panel* elite_row_ = nullptr;
    gui::Label* elite_count_ = nullptr;

    gui::Panel* dock_ = nullptr;
    gui::Icon* atp_icon_ = nullptr;
    gui::Label* atp_value_ = nullptr;
    gui::Label* atp_rate_ = nullptr;
    std::vector<gui::Widget*> dock_group_widgets_;
    std::array<BuildCard*, kTowerTypeCount> cards_{};
    gui::Panel* abilities_ = nullptr;
    std::array<AbilityCell*, game::kAbilityCount> ability_cells_{};

    HintPill* hint_ = nullptr;
    gui::Panel* cursor_cost_ = nullptr;
    gui::Label* cursor_cost_value_ = nullptr;
    gui::Label* cursor_cost_left_ = nullptr;

    // World overlays.
    struct TowerOverlay {
        RangeRing* ring = nullptr;
        gui::Meter* pip = nullptr;
    };
    std::map<u32, TowerOverlay> tower_overlays_;
    RangeRing* ghost_ring_ = nullptr;
    gui::Icon* ghost_icon_ = nullptr;
    RangeRing* reticle_ = nullptr;
    gui::Ring* selection_ring_ = nullptr;
    TowerPopup* popup_ = nullptr;
    gui::Panel* spawn_pill_ = nullptr;
    std::array<gui::Widget*, kFamilyCount> spawn_entries_{};
    std::array<gui::Label*, kFamilyCount> spawn_counts_{};
};

} // namespace immune::ui
