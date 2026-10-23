#pragma once

#include "core/Types.h"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace immune::sim {

inline constexpr Tick kUnobservedTick = std::numeric_limits<Tick>::max();
enum class CombatRetirement : u8 { Unknown, Damage, Leak, OutOfBounds, Consumed, Expired };

/// Logarithmic duration histogram in simulation ticks: bin 0 is zero ticks;
/// bin n>0 covers [2^(n-1), 2^n-1]. Last bin includes larger durations.
struct TickDistribution {
    u64 observations = 0;
    f64 sum_ticks = 0;
    Tick min_ticks = 0;
    Tick max_ticks = 0;
    std::array<u64, 64> bins{};
    void add(Tick ticks) {
        if (observations == 0 || ticks < min_ticks) min_ticks = ticks;
        max_ticks = std::max(max_ticks, ticks);
        ++observations;
        sum_ticks += static_cast<f64>(ticks);
        usize bin = 0;
        for (Tick remaining = ticks; remaining != 0 && bin < 63; remaining >>= 1) ++bin;
        ++bins[bin];
    }
};

struct LifecycleMetrics {
    u64 spawned = 0;
    u64 entered_alive = 0; ///< Alive when observation began; birth is unknown.
    u64 alive = 0;         ///< Right-censored at the current observation end.
    u64 hit_alive = 0;     ///< Alive after at least one observed effective hit.
    u64 first_hits = 0;    ///< All entities effectively hit, including competing exits.
    u64 damage_deaths = 0;
    u64 leaked = 0;
    u64 out_of_bounds = 0;
    u64 consumed = 0;
    u64 expired = 0;
    u64 unknown = 0;
    f64 damage_taken = 0;
    TickDistribution damage_ttk;
    TickDistribution damage_lifetime;
    TickDistribution retired_lifetime;
};

struct FriendlyLifetimeBucket {
    EntityId owner{};
    TowerType type = TowerType::Count;
    LifecycleMetrics stats;
};

enum class CombatDamageSource : u8 {
    Unknown, Field, Projectile, Drain, Swallow, Burst, Fluid, ScarContact,
    Count
};

struct DamageAttribution {
    EntityId owner{};
    TowerType type = TowerType::Count;
    CombatDamageSource source = CombatDamageSource::Unknown;
    /// Fluid resolves one gameplay hit, then divides its credit by deposited DPS.
    bool defer_recording = false;
};

/// Density units, after existing damage modifiers. Rejected underground damage
/// is separate from overkill. Kill credit may be fractional for mixed fluid.
struct DensityMetrics {
    f64 attempted_density = 0.0;
    f64 density_removed = 0.0;
    f64 overkill_density = 0.0;
    f64 immune_density = 0.0;
    f64 damage_kills = 0.0;
    u64 damage_calls = 0;

    void add(f64 attempted, f64 applied, f64 immune, f64 kills) {
        attempted_density += attempted;
        density_removed += applied;
        immune_density += immune;
        overkill_density += std::max(0.0, attempted - applied - immune);
        damage_kills += kills;
        ++damage_calls;
    }
};

enum class CombatSpawnPool : u8 { Cells, Projectiles, Fluid };
struct EmissionMetrics { u64 requested = 0; u64 spawned = 0; u64 denied = 0; };
struct SpawnPoolMetrics : EmissionMetrics { u64 highwater = 0; };
struct CombatPoolMetrics { SpawnPoolMetrics cells, projectiles, fluid; };
struct ProjectileOutcomeMetrics {
    u64 agent_hits = 0; ///< Applied chaff hits; piercing rounds can hit repeatedly.
    u64 projectiles_hit = 0; ///< Distinct projectiles with an observed applied hit.
    u64 wall_impacts = 0;
    u64 expired = 0; ///< Timed out or left world bounds.
    u64 retired_on_hit = 0;
};
enum class ProjectileOutcome : u8 { AgentHit, WallImpact, Expired };

/// One sample per cell that entered its behavior update alive, classified at
/// the end of that update, before body-collision resolution. Multiply counts
/// by fixed dt for 60 Hz sampled occupancy, not sub-tick phase duration.
/// Flags overlap: a shooter can chase while reloading; secretion is rest.
struct ActivityMetrics {
    u64 observed_ticks = 0;
    u64 targetless_ticks = 0;
    u64 chasing_ticks = 0;
    u64 engaged_ticks = 0;
    u64 attaching_ticks = 0;
    u64 attacking_ticks = 0;
    u64 reloading_ticks = 0;
    u64 gathering_ticks = 0;
    u64 firing_ticks = 0;
    u64 loaded_ticks = 0;
    u64 arbor_work_ticks = 0;
    u64 builder_travel_ticks = 0;
    u64 builder_rest_ticks = 0;
    u64 builder_secreting_ticks = 0;
    u64 builder_waiting_ticks = 0;
    u64 builder_paused_ticks = 0;
    u64 retired_ticks = 0;
    void add(const ActivityMetrics& other) {
        observed_ticks += other.observed_ticks; targetless_ticks += other.targetless_ticks;
        chasing_ticks += other.chasing_ticks; engaged_ticks += other.engaged_ticks;
        attaching_ticks += other.attaching_ticks; attacking_ticks += other.attacking_ticks;
        reloading_ticks += other.reloading_ticks; gathering_ticks += other.gathering_ticks;
        firing_ticks += other.firing_ticks; loaded_ticks += other.loaded_ticks;
        arbor_work_ticks += other.arbor_work_ticks; builder_travel_ticks += other.builder_travel_ticks;
        builder_rest_ticks += other.builder_rest_ticks; builder_secreting_ticks += other.builder_secreting_ticks;
        builder_waiting_ticks += other.builder_waiting_ticks; builder_paused_ticks += other.builder_paused_ticks;
        retired_ticks += other.retired_ticks;
    }
};

struct CombatMetricBucket {
    EntityId owner{};
    TowerType type = TowerType::Count;
    CombatDamageSource source = CombatDamageSource::Unknown;
    DensityMetrics totals;
    DensityMetrics by_family[kFamilyCount]{};
    EmissionMetrics emissions;
    ProjectileOutcomeMetrics projectile_outcomes;
    ActivityMetrics activity;
};

struct CombatMetricTotals {
    DensityMetrics damage;
    DensityMetrics by_family[kFamilyCount]{};
    /// Existing hostile stats report requested damage, which can exceed health.
    f64 hostile_requested_cell_damage = 0.0;
    f64 hostile_requested_structure_damage = 0.0;
    f64 hostile_actual_cell_damage = 0.0;
    u64 cells_killed_by_hostiles = 0;
    u64 toxin_fired = 0;
    u64 toxin_hits = 0;
    u64 new_latches = 0;
    u64 replications_denied_capacity = 0;
    u64 replications_denied_tick_budget = 0;
    CombatPoolMetrics pools;
    ProjectileOutcomeMetrics projectile_outcomes;
};

/// Optional observer. Storage is allocated only by enable(), outside ticks;
/// recording uses fixed-capacity hash tables and never changes gameplay state.
/// Overflow loses attribution detail, never aggregate totals, and is explicit.
class CombatMetrics {
public:
    void enable(bool enabled = true, usize bucket_capacity = 4096) {
        enabled_ = enabled;
        if (enabled) {
            capacity_ = std::max<usize>(bucket_capacity, 1);
            usize slots = 2;
            while (slots < capacity_ * 2) slots *= 2;
            lookup_.assign(slots, kEmpty);
            owners_.assign(slots, Owner{});
            friendly_lookup_.assign(slots, kEmpty);
            buckets_.clear();
            buckets_.reserve(capacity_);
            friendly_lifetimes_.clear();
            friendly_lifetimes_.reserve(capacity_);
        }
        reset();
    }
    bool enabled() const { return enabled_; }
    void reset() {
        totals_ = {};
        overflow_ = 0;
        buckets_.clear();
        enemy_lifetimes_ = {};
        friendly_lifetimes_.clear();
        activity_ = {};
        std::fill(lookup_.begin(), lookup_.end(), kEmpty);
        std::fill(friendly_lookup_.begin(), friendly_lookup_.end(), kEmpty);
        std::fill(owners_.begin(), owners_.end(), Owner{});
    }
    const CombatMetricTotals& totals() const { return totals_; }
    const std::vector<CombatMetricBucket>& buckets() const { return buckets_; }
    u64 overflow_count() const { return overflow_; }
    Tick tick() const { return tick_; }
    void set_tick(Tick tick) { tick_ = tick; }
    const std::array<LifecycleMetrics, kFamilyCount>& enemy_lifetimes() const { return enemy_lifetimes_; }
    const std::vector<FriendlyLifetimeBucket>& friendly_lifetimes() const { return friendly_lifetimes_; }
    const std::array<ActivityMetrics, kTowerTypeCount>& activity() const { return activity_; }
    void record_activity(EntityId owner, TowerType type, const ActivityMetrics& sample) {
        if (!enabled_) return;
        if (static_cast<u32>(type) < kTowerTypeCount) activity_[static_cast<u32>(type)].add(sample);
        if (auto* bucket = damage_bucket({owner, type, CombatDamageSource::Unknown})) bucket->activity.add(sample);
    }
    void enemy_spawn(u8 family, bool existing = false) {
        if (!enabled_ || family >= kFamilyCount) return;
        enter(enemy_lifetimes_[family], existing);
    }
    void enemy_first_hit(u8 family) {
        if (enabled_ && family < kFamilyCount) {
            ++enemy_lifetimes_[family].hit_alive;
            ++enemy_lifetimes_[family].first_hits;
        }
    }
    void enemy_retire(u8 family, CombatRetirement reason, Tick birth, Tick hit) {
        if (enabled_ && family < kFamilyCount) retire(enemy_lifetimes_[family], reason, birth, hit);
    }
    void friendly_spawn(EntityId owner, TowerType type, bool existing = false) {
        if (!enabled_) return;
        register_owner(owner, type);
        if (auto* bucket = friendly_bucket(owner, type)) enter(bucket->stats, existing);
    }
    void friendly_first_hit(EntityId owner, TowerType type) {
        if (enabled_) if (auto* bucket = friendly_bucket(owner, type)) {
            ++bucket->stats.hit_alive;
            ++bucket->stats.first_hits;
        }
    }
    void friendly_damage(EntityId owner, TowerType type, f32 actual) {
        if (!enabled_ || actual <= 0) return;
        totals_.hostile_actual_cell_damage += actual;
        if (auto* bucket = friendly_bucket(owner, type)) bucket->stats.damage_taken += actual;
    }
    void friendly_retire(EntityId owner, TowerType type, CombatRetirement reason, Tick birth, Tick hit) {
        if (enabled_) if (auto* bucket = friendly_bucket(owner, type)) retire(bucket->stats, reason, birth, hit);
    }
    void note_attribution_overflow() { if (enabled_) ++overflow_; }
    void record_replication_denied(bool capacity) {
        if (!enabled_) return;
        if (capacity) ++totals_.replications_denied_capacity;
        else ++totals_.replications_denied_tick_budget;
    }

    void register_owner(EntityId owner, TowerType type) {
        if (!enabled_ || !owner.valid() || type == TowerType::Count) return;
        usize slot = hash(owner.value) & (owners_.size() - 1);
        for (usize n = 0; n < owners_.size(); ++n) {
            Owner& entry = owners_[slot];
            if (!entry.used || entry.id == owner.value) {
                entry = Owner{owner.value, type, true};
                return;
            }
            slot = (slot + 1) & (owners_.size() - 1);
        }
        ++overflow_;
    }
    TowerType owner_type(EntityId owner) const {
        if (!enabled_ || !owner.valid()) return TowerType::Count;
        usize slot = hash(owner.value) & (owners_.size() - 1);
        for (usize n = 0; n < owners_.size(); ++n) {
            const Owner& entry = owners_[slot];
            if (!entry.used) break;
            if (entry.id == owner.value) return entry.type;
            slot = (slot + 1) & (owners_.size() - 1);
        }
        return TowerType::Count;
    }
    /// aggregate=false adds only source credit after an aggregate mixed hit.
    void record(DamageAttribution tag, u8 family, f64 attempted, f64 applied,
                f64 immune, f64 kills, bool aggregate = true) {
        if (!enabled_ || attempted <= 0.0) return;
        if (aggregate) {
            totals_.damage.add(attempted, applied, immune, kills);
            if (family < kFamilyCount) {
                totals_.by_family[family].add(attempted, applied, immune, kills);
                enemy_lifetimes_[family].damage_taken += applied;
            }
        }
        if (tag.defer_recording) return;
        if (auto* bucket = damage_bucket(tag)) {
            bucket->totals.add(attempted, applied, immune, kills);
            if (family < kFamilyCount) bucket->by_family[family].add(attempted, applied, immune, kills);
        }
    }
    void record_spawn(CombatSpawnPool pool, DamageAttribution tag, u64 requested, u64 spawned, usize live) {
        if (!enabled_) return;
        auto& global = pool == CombatSpawnPool::Cells ? totals_.pools.cells :
                       pool == CombatSpawnPool::Projectiles ? totals_.pools.projectiles : totals_.pools.fluid;
        global.requested += requested;
        global.spawned += spawned;
        global.denied += requested - spawned;
        global.highwater = std::max(global.highwater, static_cast<u64>(live));
        if (auto* bucket = damage_bucket(tag)) {
            bucket->emissions.requested += requested;
            bucket->emissions.spawned += spawned;
            bucket->emissions.denied += requested - spawned;
        }
    }
    void observe_pool(CombatSpawnPool pool, usize live) {
        if (!enabled_) return;
        auto& global = pool == CombatSpawnPool::Cells ? totals_.pools.cells :
                       pool == CombatSpawnPool::Projectiles ? totals_.pools.projectiles : totals_.pools.fluid;
        global.highwater = std::max(global.highwater, static_cast<u64>(live));
    }
    void record_projectile_outcome(DamageAttribution tag, ProjectileOutcome outcome, bool retires = false,
                                  bool first_hit = false) {
        if (!enabled_) return;
        const auto add = [&](ProjectileOutcomeMetrics& metrics) {
            if (outcome == ProjectileOutcome::AgentHit) {
                ++metrics.agent_hits;
                if (first_hit) ++metrics.projectiles_hit;
                if (retires) ++metrics.retired_on_hit;
            } else if (outcome == ProjectileOutcome::WallImpact) ++metrics.wall_impacts;
            else ++metrics.expired;
        };
        add(totals_.projectile_outcomes);
        if (auto* bucket = damage_bucket(tag)) add(bucket->projectile_outcomes);
    }
    void record_hostile(f32 cell_damage, f32 structure_damage, u32 killed,
                        u32 fired, u32 hits, u32 latches) {
        if (!enabled_) return;
        totals_.hostile_requested_cell_damage += cell_damage;
        totals_.hostile_requested_structure_damage += structure_damage;
        totals_.cells_killed_by_hostiles += killed;
        totals_.toxin_fired += fired;
        totals_.toxin_hits += hits;
        totals_.new_latches += latches;
    }

private:
    CombatMetricBucket* damage_bucket(DamageAttribution tag) {
        if (tag.type == TowerType::Count) tag.type = owner_type(tag.owner);
        const usize key = hash(tag.owner.value ^ (static_cast<u32>(tag.source) * 0x9e3779b9u) ^
                               (static_cast<u32>(tag.type) * 0x85ebca6bu));
        usize slot = key & (lookup_.size() - 1);
        for (usize n = 0; n < lookup_.size(); ++n) {
            u32& index = lookup_[slot];
            if (index == kEmpty) {
                if (buckets_.size() >= capacity_) { ++overflow_; return nullptr; }
                index = static_cast<u32>(buckets_.size());
                buckets_.push_back(CombatMetricBucket{tag.owner, tag.type, tag.source});
            }
            CombatMetricBucket& bucket = buckets_[index];
            if (bucket.owner.value == tag.owner.value && bucket.type == tag.type && bucket.source == tag.source) {
                return &bucket;
            }
            slot = (slot + 1) & (lookup_.size() - 1);
        }
        ++overflow_;
        return nullptr;
    }
    static constexpr u32 kEmpty = ~u32{0};
    struct Owner { u32 id = 0; TowerType type = TowerType::Count; bool used = false; };
    static void enter(LifecycleMetrics& stats, bool existing) {
        if (existing) ++stats.entered_alive;
        else ++stats.spawned;
        ++stats.alive;
    }
    void retire(LifecycleMetrics& stats, CombatRetirement reason, Tick birth, Tick hit) {
        if (stats.alive > 0) --stats.alive;
        if (hit != kUnobservedTick && stats.hit_alive > 0) --stats.hit_alive;
        switch (reason) {
        case CombatRetirement::Damage: ++stats.damage_deaths; break;
        case CombatRetirement::Leak: ++stats.leaked; break;
        case CombatRetirement::OutOfBounds: ++stats.out_of_bounds; break;
        case CombatRetirement::Consumed: ++stats.consumed; break;
        case CombatRetirement::Expired: ++stats.expired; break;
        default: ++stats.unknown; break;
        }
        if (birth != kUnobservedTick && birth <= tick_) {
            stats.retired_lifetime.add(tick_ - birth);
            if (reason == CombatRetirement::Damage) stats.damage_lifetime.add(tick_ - birth);
        }
        if (reason == CombatRetirement::Damage && hit != kUnobservedTick && hit <= tick_)
            stats.damage_ttk.add(tick_ - hit);
    }
    FriendlyLifetimeBucket* friendly_bucket(EntityId owner, TowerType type) {
        usize slot = hash(owner.value ^ (static_cast<u32>(type) * 0x85ebca6bu)) & (friendly_lookup_.size() - 1);
        for (usize n = 0; n < friendly_lookup_.size(); ++n) {
            u32& index = friendly_lookup_[slot];
            if (index == kEmpty) {
                if (friendly_lifetimes_.size() >= capacity_) { ++overflow_; return nullptr; }
                index = static_cast<u32>(friendly_lifetimes_.size());
                friendly_lifetimes_.push_back(FriendlyLifetimeBucket{owner, type});
            }
            auto& bucket = friendly_lifetimes_[index];
            if (bucket.owner == owner && bucket.type == type) return &bucket;
            slot = (slot + 1) & (friendly_lookup_.size() - 1);
        }
        ++overflow_;
        return nullptr;
    }
    static usize hash(u32 value) {
        value ^= value >> 16; value *= 0x7feb352du; value ^= value >> 15;
        return static_cast<usize>(value);
    }
    bool enabled_ = false;
    usize capacity_ = 0;
    u64 overflow_ = 0;
    Tick tick_ = 0;
    CombatMetricTotals totals_{};
    std::vector<CombatMetricBucket> buckets_;
    std::vector<u32> lookup_;
    std::vector<Owner> owners_;
    std::array<LifecycleMetrics, kFamilyCount> enemy_lifetimes_{};
    std::vector<FriendlyLifetimeBucket> friendly_lifetimes_;
    std::vector<u32> friendly_lookup_;
    std::array<ActivityMetrics, kTowerTypeCount> activity_{};
};

} // namespace immune::sim
