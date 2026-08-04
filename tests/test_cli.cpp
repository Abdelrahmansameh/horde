// The CLI surface is a frozen contract: every later sub-agent's verification
// commands go through it. These tests are the guard on that.
#include "app/Cli.h"
#include "app/Modes.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace immune;
using namespace immune::app;

namespace {
Options parse(std::vector<const char*> args) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("immune"));
    for (const char* a : args) argv.push_back(const_cast<char*>(a));
    return parse_args(static_cast<int>(argv.size()), argv.data());
}
} // namespace

TEST_CASE("no arguments means interactive play", "[app][cli]") {
    const auto o = parse({});
    REQUIRE(o.mode == Mode::Play);
    REQUIRE(o.error.empty());
}

TEST_CASE("cli bench parses scenario and ticks", "[app][cli][bench]") {
    const auto o = parse({"--bench", "chaff10k", "--ticks", "600"});
    REQUIRE(o.mode == Mode::Bench);
    REQUIRE(o.scenario == "chaff10k");
    REQUIRE(o.ticks == 600);
}

TEST_CASE("cli bench without a scenario is an error", "[app][cli]") {
    const auto o = parse({"--bench"});
    REQUIRE(o.mode == Mode::Invalid);
    REQUIRE(!o.error.empty());
}

TEST_CASE("cli sim-test parses a script path", "[app][cli][simtest]") {
    const auto o = parse({"--sim-test", "tests/scripts/smoke.json"});
    REQUIRE(o.mode == Mode::SimTest);
    REQUIRE(o.script_path == "tests/scripts/smoke.json");
}

TEST_CASE("cli screenshot parses level, tick, and out", "[app][cli][screenshot]") {
    const auto o = parse({"--screenshot", "capillary", "--tick", "300", "--out", "a.png"});
    REQUIRE(o.mode == Mode::Screenshot);
    REQUIRE(o.level == "capillary");
    REQUIRE(o.ticks == 300);
    REQUIRE(o.out_path == "a.png");
}

TEST_CASE("cli screenshot defaults to tick 0", "[app][cli][screenshot]") {
    const auto o = parse({"--screenshot", "capillary", "--out", "a.png"});
    REQUIRE(o.ticks == 0);
}

TEST_CASE("cli seed is parsed and defaults are stable", "[app][cli][determinism]") {
    const auto a = parse({"--seed", "12345"});
    REQUIRE(a.seed == 12345);
    const auto b = parse({});
    const auto c = parse({});
    REQUIRE(b.seed == c.seed);   // default seed must be fixed, not random
}

TEST_CASE("unknown arguments are rejected, not ignored", "[app][cli]") {
    const auto o = parse({"--not-a-flag"});
    REQUIRE(o.mode == Mode::Invalid);
    REQUIRE(o.error.find("--not-a-flag") != std::string::npos);
}

TEST_CASE("cli rejects a non-numeric ticks value", "[app][cli]") {
    const auto o = parse({"--bench", "empty", "--ticks", "lots"});
    REQUIRE(o.mode == Mode::Invalid);
}

TEST_CASE("the gate bench scenarios exist", "[app][bench]") {
    REQUIRE(find_bench_scenario("empty") != nullptr);
    REQUIRE(find_bench_scenario("chaff10k") != nullptr);
    REQUIRE(find_bench_scenario("chaff10k")->chaff_count == 10000);
    REQUIRE(find_bench_scenario("named200") != nullptr);
    REQUIRE(find_bench_scenario("nope") == nullptr);
    REQUIRE(bench_scenarios().size() >= 5);
}
