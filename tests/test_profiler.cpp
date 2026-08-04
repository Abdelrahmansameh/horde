#include "core/Profiler.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

using namespace immune;

TEST_CASE("profiler summarizes avg/p50/p99", "[core][profiler]") {
    Profiler p;
    for (int i = 1; i <= 100; ++i) p.record(prof_key::kChaffUpdate, static_cast<f64>(i));

    const auto stats = p.summarize();
    const auto it = stats.find(prof_key::kChaffUpdate);
    REQUIRE(it != stats.end());
    REQUIRE(it->second.samples == 100);
    REQUIRE(it->second.min_ms == 1.0);
    REQUIRE(it->second.max_ms == 100.0);
    REQUIRE(it->second.avg_ms > 50.0);
    REQUIRE(it->second.avg_ms < 51.0);
    REQUIRE(it->second.p50_ms > 50.0);
    REQUIRE(it->second.p50_ms < 51.5);
    REQUIRE(it->second.p99_ms > 98.0);
}

TEST_CASE("bench JSON has every canonical key and parses", "[core][profiler][bench]") {
    Profiler p;
    p.record(prof_key::kChaffUpdate, 1.0);
    p.record(prof_key::kSpatialHash, 0.5);
    p.record(prof_key::kEcsTick, 0.25);
    p.record(prof_key::kRenderSubmit, 0.75);
    p.record(prof_key::kFrameTotal, 2.5);

    const std::string text = p.to_json("unit_test", 600, 42, 10000, 200);
    const auto j = nlohmann::json::parse(text);

    REQUIRE(j["scenario"] == "unit_test");
    REQUIRE(j["ticks"] == 600);
    REQUIRE(j["seed"] == 42);
    REQUIRE(j["agents"]["chaff"] == 10000);
    REQUIRE(j["agents"]["named"] == 200);

    for (const char* key : {prof_key::kChaffUpdate, prof_key::kSpatialHash,
                            prof_key::kEcsTick, prof_key::kRenderSubmit,
                            prof_key::kFrameTotal}) {
        REQUIRE(j["timings_ms"].contains(key));
        REQUIRE(j["timings_ms"][key].contains("avg"));
        REQUIRE(j["timings_ms"][key].contains("p50"));
        REQUIRE(j["timings_ms"][key].contains("p99"));
    }
}

TEST_CASE("bench JSON emits missing keys as zero rather than omitting them",
          "[core][profiler][bench]") {
    Profiler p;
    const auto j = nlohmann::json::parse(p.to_json("empty", 0, 0, 0, 0));
    REQUIRE(j["timings_ms"][prof_key::kChaffUpdate]["samples"] == 0);
    REQUIRE(j["timings_ms"][prof_key::kFrameTotal]["avg"] == 0.0);
}

TEST_CASE("clear empties the samples", "[core][profiler]") {
    Profiler p;
    p.record(prof_key::kEcsTick, 5.0);
    p.clear();
    REQUIRE(p.summarize()[prof_key::kEcsTick].samples == 0);
}
