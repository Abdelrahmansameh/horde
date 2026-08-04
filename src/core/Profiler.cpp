#include "core/Profiler.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace immune {
namespace {

f64 percentile(const std::vector<f64>& sorted, f64 q) {
    if (sorted.empty()) return 0.0;
    if (sorted.size() == 1) return sorted[0];
    const f64 pos = q * static_cast<f64>(sorted.size() - 1);
    const usize lo = static_cast<usize>(pos);
    const usize hi = std::min(lo + 1, sorted.size() - 1);
    const f64 frac = pos - static_cast<f64>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

} // namespace

void Profiler::reserve(usize expected_samples) {
    for (const char* key : {prof_key::kChaffUpdate, prof_key::kSpatialHash,
                            prof_key::kEcsTick, prof_key::kRenderSubmit,
                            prof_key::kFrameTotal}) {
        samples_[key].reserve(expected_samples);
    }
}

void Profiler::record(const char* key, f64 ms) {
    samples_[key].push_back(ms);
}

void Profiler::clear() {
    for (auto& [key, v] : samples_) v.clear();
}

std::unordered_map<std::string, TimingStats> Profiler::summarize() const {
    std::unordered_map<std::string, TimingStats> out;
    for (const auto& [key, raw] : samples_) {
        TimingStats s;
        s.samples = raw.size();
        if (!raw.empty()) {
            std::vector<f64> sorted = raw;
            std::sort(sorted.begin(), sorted.end());
            f64 sum = 0.0;
            for (f64 v : sorted) sum += v;
            s.avg_ms = sum / static_cast<f64>(sorted.size());
            s.p50_ms = percentile(sorted, 0.50);
            s.p99_ms = percentile(sorted, 0.99);
            s.min_ms = sorted.front();
            s.max_ms = sorted.back();
        }
        out.emplace(key, s);
    }
    return out;
}

std::string Profiler::to_json(const std::string& scenario, u64 ticks, u64 seed,
                              u64 chaff_count, u64 named_count) const {
    const auto stats = summarize();

    // Emit the canonical keys first and in a fixed order, then any extras
    // sorted, so the document is byte-stable across runs.
    std::vector<std::string> order = {
        prof_key::kChaffUpdate, prof_key::kSpatialHash, prof_key::kEcsTick,
        prof_key::kRenderSubmit, prof_key::kFrameTotal};
    std::map<std::string, const TimingStats*> extras;
    for (const auto& [k, v] : stats) {
        if (std::find(order.begin(), order.end(), k) == order.end()) extras[k] = &v;
    }

    std::string json;
    json.reserve(2048);
    char buf[512];

    json += "{\n";
    std::snprintf(buf, sizeof(buf), "  \"scenario\": \"%s\",\n", scenario.c_str());
    json += buf;
    std::snprintf(buf, sizeof(buf), "  \"ticks\": %llu,\n", static_cast<unsigned long long>(ticks));
    json += buf;
    std::snprintf(buf, sizeof(buf), "  \"seed\": %llu,\n", static_cast<unsigned long long>(seed));
    json += buf;
    std::snprintf(buf, sizeof(buf),
                  "  \"agents\": { \"chaff\": %llu, \"named\": %llu },\n",
                  static_cast<unsigned long long>(chaff_count),
                  static_cast<unsigned long long>(named_count));
    json += buf;
    json += "  \"timings_ms\": {\n";

    auto emit = [&](const std::string& key, const TimingStats& s, bool last) {
        std::snprintf(buf, sizeof(buf),
                      "    \"%s\": { \"avg\": %.6f, \"p50\": %.6f, \"p99\": %.6f, "
                      "\"min\": %.6f, \"max\": %.6f, \"samples\": %llu }%s\n",
                      key.c_str(), s.avg_ms, s.p50_ms, s.p99_ms, s.min_ms, s.max_ms,
                      static_cast<unsigned long long>(s.samples), last ? "" : ",");
        json += buf;
    };

    const TimingStats zero{};
    const usize total = order.size() + extras.size();
    usize i = 0;
    for (const auto& key : order) {
        auto it = stats.find(key);
        emit(key, it != stats.end() ? it->second : zero, ++i == total);
    }
    for (const auto& [k, s] : extras) emit(k, *s, ++i == total);

    json += "  }\n}\n";
    return json;
}

} // namespace immune
