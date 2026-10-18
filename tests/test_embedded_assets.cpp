#include "platform/EmbeddedAssets.h"
#include "platform/FileIO.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>

using namespace immune;

TEST_CASE("embedded asset paths are reserved and read only", "[platform][assets]") {
    const std::string missing = "immune-embedded:/assets/not-a-shipped-file.bin";
    REQUIRE(platform::is_embedded_path(missing));
    REQUIRE_FALSE(platform::is_embedded_path("assets/config/sim.json"));
    REQUIRE_FALSE(platform::file_exists(missing));
    REQUIRE_FALSE(platform::read_text_file(missing));
    REQUIRE_FALSE(platform::read_binary_file(missing));
    REQUIRE_FALSE(platform::write_text_file(missing, "must not be written"));
    REQUIRE_FALSE(platform::write_binary_file(missing, "x", 1));
    REQUIRE_FALSE(platform::ensure_directory("immune-embedded:/new-directory"));
    REQUIRE_FALSE(platform::read_text_file("immune-embedded:/assets/../assets/config/sim.json"));
    REQUIRE_FALSE(platform::read_text_file("immune-embedded:assets/config/sim.json"));
    REQUIRE(platform::list_files("immune-embedded:/assets/missing", ".json").empty());
}

TEST_CASE("explicit external files remain readable and writable", "[platform][assets]") {
    const auto directory = std::filesystem::temp_directory_path() / "immune_embedded_asset_test";
    const std::string path = (directory / "external.bin").string();
    const std::vector<u8> contents{0, 1, 127, 128, 255, 0};
    REQUIRE(platform::ensure_directory(directory.string()));
    REQUIRE(platform::write_binary_file(path, contents.data(), contents.size()));
    REQUIRE(platform::file_exists(path));
    REQUIRE(platform::directory_exists(directory.string()));
    REQUIRE(platform::read_binary_file(path) == contents);
    const auto text = platform::read_text_file(path);
    REQUIRE(text);
    REQUIRE(text->size() == contents.size());
    REQUIRE((*text)[0] == '\0');
    REQUIRE(static_cast<u8>((*text)[4]) == 255);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(directory, ec);
}

#ifdef IMMUNE_EMBEDDED_ASSETS
TEST_CASE("standalone assets load text and binary resources from memory", "[platform][assets][embedded]") {
    REQUIRE(platform::asset_root() == platform::kEmbeddedAssetRoot);
    const std::string font_path = platform::asset_path("fonts/Fredoka-Medium.ttf");
    REQUIRE(font_path == "immune-embedded:/assets/fonts/Fredoka-Medium.ttf");
    const auto font = platform::read_binary_file(font_path);
    REQUIRE(font);
    REQUIRE(font->size() > 10000);
    REQUIRE((*font)[0] == 0);
    REQUIRE((*font)[1] == 1);
    REQUIRE((*font)[2] == 0);
    REQUIRE((*font)[3] == 0);
    REQUIRE(platform::embedded_asset_data(font_path)->size() == font->size());
    const auto license = platform::read_text_file(platform::asset_path("fonts/OFL.txt"));
    REQUIRE(license);
    REQUIRE(license->find("SIL OPEN FONT LICENSE") != std::string::npos);
    const auto config = platform::read_text_file(platform::asset_path("config/sim.json"));
    REQUIRE(config);
    REQUIRE_FALSE(config->empty());
    REQUIRE(platform::read_text_file("immune-embedded:\\assets\\config\\sim.json") == config);

    // An explicitly chosen external file can share a name with a shipped asset
    // without replacing the resource used by normal gameplay.
    const auto external = std::filesystem::temp_directory_path() /
                          "immune_embedded_external_config" / "sim.json";
    REQUIRE(platform::write_text_file(external.string(), "external user config"));
    REQUIRE(platform::read_text_file(external.string()) == "external user config");
    REQUIRE(platform::read_text_file(platform::asset_path("config/sim.json")) == config);
    std::error_code ec;
    std::filesystem::remove(external, ec);
    std::filesystem::remove(external.parent_path(), ec);
}

TEST_CASE("standalone listings discover only immediate shipped files", "[platform][assets][embedded]") {
    REQUIRE(platform::directory_exists(std::string(platform::kEmbeddedAssetRoot)));
    REQUIRE(platform::directory_exists(platform::asset_path("levels")));
    REQUIRE_FALSE(platform::file_exists(platform::asset_path("levels")));
    REQUIRE_FALSE(platform::directory_exists(platform::asset_path("config/sim.json")));
    const auto levels = platform::list_files(platform::asset_path("levels"), ".json");
    REQUIRE(levels.size() >= 10);
    REQUIRE(std::is_sorted(levels.begin(), levels.end()));
    for (const auto& path : levels) {
        REQUIRE(path.starts_with("immune-embedded:/assets/levels/"));
        REQUIRE(path.ends_with(".json"));
        REQUIRE(platform::file_exists(path));
    }
    const auto all_levels = platform::list_files(platform::asset_path("levels"), "");
    REQUIRE(all_levels.size() == levels.size()); // No .bak files in the release.
    const auto icons = platform::list_files(platform::asset_path("ui/icons"), ".svg");
    REQUIRE(icons.size() >= 20);
    REQUIRE(platform::list_files(platform::asset_path("ui"), ".svg").empty());
    REQUIRE_FALSE(platform::file_exists(platform::asset_path("levels/level_1.json.bak")));
}
#else
TEST_CASE("loose development builds keep normal filesystem asset paths", "[platform][assets]") {
    REQUIRE_FALSE(platform::is_embedded_path(platform::asset_path("config/sim.json")));
    REQUIRE(platform::file_exists(platform::asset_path("config/sim.json")));
    REQUIRE_FALSE(platform::embedded_asset_data("immune-embedded:/assets/config/sim.json"));
}
#endif
