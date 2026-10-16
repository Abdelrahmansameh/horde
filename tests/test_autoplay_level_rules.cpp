#include "platform/FileIO.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace {
// Config application changes process-wide mechanics tables. Exercise the CLI
// in a child so this integration check cannot change later unit-test tuning.
int run_autoplay_cli(const std::filesystem::path& level, const std::filesystem::path& report,
                     unsigned ticks) {
#ifdef _WIN32
    const auto executable = std::filesystem::path(immune::platform::executable_dir()) / "immune.exe";
#else
    const auto executable = std::filesystem::path(immune::platform::executable_dir()) / "immune";
#endif
    std::vector<std::string> args{executable.string(), "--autoplay", "--level", level.string(),
        "--report", report.string(), "--profile", "single-type:neutrophil", "--max-ticks",
        std::to_string(ticks), "--threads", "1", "--quiet"};
#ifdef _WIN32
    std::string command;
    for (const auto& arg : args) command += "\"" + arg + "\" ";
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessA(executable.string().c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) return -1;
    WaitForSingleObject(child.hProcess, INFINITE);
    DWORD result = 1;
    GetExitCodeProcess(child.hProcess, &result);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return static_cast<int>(result);
#else
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t child;
    if (posix_spawn(&child, executable.string().c_str(), nullptr, nullptr, argv.data(), environ) != 0)
        return -1;
    int status = 0;
    if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
#endif
}
}

TEST_CASE("autoplay respects authored starting ATP and cell restrictions",
          "[app][autoplay][level]") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = std::filesystem::temp_directory_path();
    const auto level = dir / ("immune-level-rules-" + std::to_string(nonce) + ".json");
    const auto report = dir / ("immune-level-rules-report-" + std::to_string(nonce) + ".json");
    struct Cleanup {
        std::filesystem::path level, report;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove(level, ignored);
            std::filesystem::remove(report, ignored);
        }
    } cleanup{level, report};
    REQUIRE(immune::platform::write_text_file(level.string(), R"JSON({
      "schema":2,"name":"authored_economy_fixture","region":"capillary",
      "world":{"min":[0,0],"max":[120,80],"cell_size":1},
      "economy":{"starting_atp":777},"allowed_towers":["fibroblast"],
      "vessels":[{"id":"main","points":[{"p":[4,40],"w":30},{"p":[116,40],"w":30}]}],
      "spawn_points":[{"id":"p0","pos":[4,40],"radius":3,"lane_id":"main"}],
      "objectives":[{"id":"organ","pos":[116,40],"half_extents":[4,15]}],
      "placement_zones":[{"min":[12,25],"max":[108,55]}],
      "waves":[{"name":"w1","prep_time":20,"spawns":[{"family":"virus","count":10,"duration":2,"spawn_point_id":"p0"}]}]
    })JSON"));
    REQUIRE(run_autoplay_cli(level, report, 1) == 0);
    const auto text = immune::platform::read_text_file(report.string());
    REQUIRE(text.has_value());
    const auto result = nlohmann::json::parse(*text);
    REQUIRE(result.at("totals").at("final_atp") == 777);
    REQUIRE(result.at("totals").at("atp_earned") == 777);
    REQUIRE(result.at("cells").empty());

    // The first purchase decision is at tick 30. Exercise it and then prove
    // this same fixture purchases when the requested cell is permitted.
    REQUIRE(run_autoplay_cli(level, report, 60) == 0);
    auto after_decisions = nlohmann::json::parse(*immune::platform::read_text_file(report.string()));
    REQUIRE(after_decisions.at("cells").empty());
    auto fixture = nlohmann::json::parse(*immune::platform::read_text_file(level.string()));
    fixture["allowed_towers"] = {"neutrophil"};
    REQUIRE(immune::platform::write_text_file(level.string(), fixture.dump()));
    REQUIRE(run_autoplay_cli(level, report, 60) == 0);
    after_decisions = nlohmann::json::parse(*immune::platform::read_text_file(report.string()));
    REQUIRE_FALSE(after_decisions.at("cells").empty());
    REQUIRE(after_decisions.at("totals").at("atp_spent") > 0);
}
