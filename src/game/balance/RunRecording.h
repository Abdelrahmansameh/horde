#pragma once

#include "config/Json.h"
#include "game/balance/BalanceController.h"
#include "game/config/GameConfig.h"
#include "game/meta/MetaProgression.h"
#include "game/session/LevelSession.h"

#include <string>
#include <vector>

namespace immune::game::balance {

/// Observes an existing player session. Does not tick, configure, spend, save,
/// or replay anything. App owns optional activation and durable file writes.
class RunRecording {
public:
    /// Supply the original parsed document for full precision. The canonical
    /// LevelDef writer is a marked fallback because it rounds to 3 decimals.
    void begin(const LevelDef& level, const GameConfig& base,
               const MetaProgression& meta, bool sandbox = false,
               const config::Json& level_document = {});
    bool active() const { return active_; }
    /// Accepted actions only, using the quantity actually deployed and the
    /// pre-step simulation tick. Wave starts may be appended after observing
    /// the Prep->Spawning transition using the previous tick index.
    void append(const Action& accepted_actual_action);
    /// Caller chooses cadence. A same-tick observation replaces the old one.
    void sample(const sim::SimWorld& world, const Economy& economy,
                const WaveDirector& waves);
    /// InProgress represents an aborted/incomplete player run. Serializes the
    /// current observation and deactivates; no progression reward is applied.
    config::Json finish(const sim::SimWorld& world, const Economy& economy,
                        const WaveDirector& waves,
                        SessionOutcome outcome = SessionOutcome::InProgress);

private:
    bool active_ = false;
    bool sandbox_ = false;
    std::string level_name_;
    GameConfig base_;
    MetaProgression initial_meta_;
    config::Json recording_;
};

/// Compare the selected observed state, economy, wave clocks and combat/TTK
/// counters with a standard balance-run report produced from the pinned base.
/// Clears `errors`; extra report fields are permitted. An aborted recording
/// corresponds to a replay stopping at its tick bound with status "timeout".
bool recording_matches(const config::Json& recording, const config::Json& report,
                       std::vector<std::string>& errors);

} // namespace immune::game::balance
