// Catch2 provides main() via Catch2WithMain. This file exists so the test
// target has a stable anchor for any future global fixtures.
#include <catch2/catch_test_macros.hpp>

TEST_CASE("test harness runs", "[meta]") {
    REQUIRE(true);
}
