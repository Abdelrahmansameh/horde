// game/economy/Economy.h — ATP income and spending. FROZEN CONTRACT.
// Owner: Wave 3A.
//
// Kill income is credited from DamageStats (density removed), never inferred by
// diffing agent counts: aggregate damage means "a kill" is a density threshold
// crossing, and only sim/damage knows when one happened.
#pragma once

#include "core/Types.h"

namespace immune::game {

struct EconomyConfig {
    u32 starting_atp = 300;
    /// ATP per unit of chaff density destroyed.
    f32 atp_per_density = 1.0f;
    /// Fraction of build+upgrade cost returned on sell.
    f32 refund_fraction = 0.7f;
};

struct EconomySnapshot {
    u32 atp = 0;
    u32 total_earned = 0;
    u32 total_spent = 0;
    u32 opening_atp = 0;
    u32 damage_income = 0;
    u32 bounty_income = 0;
    u32 refunds = 0;
    f32 fractional_income = 0;
};

class Economy {
public:
    void configure(const EconomyConfig& cfg);
    void reset();

    /// Credits income for density destroyed this tick (from DamageStats).
    void credit_kills(f32 density_removed);

    /// Credits a flat bounty (elite/boss kill).
    void credit_bounty(u32 atp);

    bool can_afford(u32 cost) const { return atp_ >= cost; }
    /// Spends if affordable. Returns false and changes nothing otherwise.
    bool spend(u32 cost);
    /// Returns `refund_fraction` of `original_cost`, rounded down.
    u32 refund(u32 original_cost);

    u32 atp() const { return atp_; }
    EconomySnapshot snapshot() const;

private:
    EconomyConfig cfg_{};
    u32 atp_ = 0;
    u32 total_earned_ = 0;
    u32 total_spent_ = 0;
    f32 fractional_ = 0.0f;   ///< Sub-unit income carry, so income is exact.
    u32 damage_income_ = 0;
    u32 bounty_income_ = 0;
    u32 refunds_ = 0;
};

} // namespace immune::game
