#include "app/Cli.h"
#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace immune::app;
namespace {
Options balance_args(std::initializer_list<const char*> arguments) {
    std::vector<char*> args{const_cast<char*>("immune")};
    for (const auto argument : arguments) args.push_back(const_cast<char*>(argument));
    return parse_args(static_cast<int>(args.size()),args.data());
}
}

TEST_CASE("balance entry points pin serial execution and explicit output intent", "[balance][cli]") {
    const auto run = balance_args({"--balance-run","experiment.json","--out","report.json"});
    REQUIRE(run.error.empty());
    CHECK(run.mode == Mode::BalanceRun);
    CHECK(run.balance_path == "experiment.json");
    CHECK(run.threads == 1);
    CHECK(run.output_explicit);
    const auto replay = balance_args({"--balance-replay","report.json","--tick","123","--out","frame.png"});
    REQUIRE(replay.error.empty());
    CHECK(replay.mode == Mode::BalanceReplay);
    CHECK(replay.ticks == 123);
    CHECK(replay.ticks_explicit);
    CHECK(replay.threads == 1);
    const auto catalog = balance_args({"--balance-catalog"});
    REQUIRE(catalog.error.empty());
    CHECK(catalog.mode == Mode::BalanceCatalog);
}

TEST_CASE("player recordings reject incompatible execution modes and mutations", "[balance][cli]") {
    const auto play = balance_args({"--record-balance","scratch/recordings"});
    REQUIRE(play.error.empty());
    CHECK(play.mode == Mode::Play);
    CHECK(play.record_balance_dir == "scratch/recordings");
    CHECK(play.threads == 1);
    for (const auto args : {balance_args({"--record-balance","records","--threads","4"}),
                            balance_args({"--balance-run","spec.json","--threads","4"}),
                            balance_args({"--record-balance","records","--exec","meta reset"}),
                            balance_args({"--record-balance","records","--screenshot","gym"})}) {
        CHECK(args.mode == Mode::Invalid);
        CHECK_FALSE(args.error.empty());
    }
}
