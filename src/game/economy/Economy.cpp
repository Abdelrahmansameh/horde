// Wave 0: the ledger is real; income curve tuning is owned by Wave 3A.
#include "game/economy/Economy.h"

namespace immune::game {

void Economy::configure(const EconomyConfig& cfg) {
    cfg_ = cfg;
    reset();
}

void Economy::reset() {
    atp_ = cfg_.starting_atp;
    total_earned_ = cfg_.starting_atp;
    total_spent_ = 0;
    fractional_ = 0.0f;
    damage_income_ = bounty_income_ = refunds_ = 0;
}

void Economy::credit_kills(f32 density_removed) {
    if (density_removed <= 0.0f) return;
    fractional_ += density_removed * cfg_.atp_per_density;
    const u32 whole = static_cast<u32>(fractional_);
    if (whole > 0) {
        fractional_ -= static_cast<f32>(whole);
        atp_ += whole;
        total_earned_ += whole;
        damage_income_ += whole;
    }
}

void Economy::credit_bounty(u32 atp) {
    atp_ += atp;
    total_earned_ += atp;
    bounty_income_ += atp;
}

bool Economy::spend(u32 cost) {
    if (atp_ < cost) return false;
    atp_ -= cost;
    total_spent_ += cost;
    return true;
}

u32 Economy::refund(u32 original_cost) {
    const u32 back = static_cast<u32>(static_cast<f32>(original_cost) * cfg_.refund_fraction);
    atp_ += back;
    refunds_ += back;
    return back;
}

EconomySnapshot Economy::snapshot() const {
    EconomySnapshot s;
    s.atp = atp_;
    s.total_earned = total_earned_;
    s.total_spent = total_spent_;
    s.opening_atp = cfg_.starting_atp;
    s.damage_income = damage_income_;
    s.bounty_income = bounty_income_;
    s.refunds = refunds_;
    s.fractional_income = fractional_;
    return s;
}

} // namespace immune::game
