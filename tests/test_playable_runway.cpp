#include "game/balance/PlayableRunway.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace immune;
using immune::game::balance::detail::playable_runway;

TEST_CASE("playable runway preserves fully in-bounds goals and early holds", "[balance][controller][playable-runway]") {
    const Rect bounds{{0,0},{100,100}};
    const std::vector<Vec2> path{{0,20},{30,20},{30,80}};
    CHECK(playable_runway({10,20},1,path,bounds,80)==80);
    // Retain the caller's existing distance exactly when no exit occurs.
    const f32 original=80.000015f;
    CHECK(playable_runway({10,20},1,path,bounds,original)==original);
    const std::vector<Vec2> offscreen{{0,50},{250,50}};
    CHECK(playable_runway({20,50},1,offscreen,bounds,230)==Catch::Approx(80));
    CHECK(playable_runway({80,50},1,offscreen,bounds,170)==Catch::Approx(20));
}

TEST_CASE("playable runway stops at the first exit through bends and re-entry", "[balance][controller][playable-runway]") {
    const Rect bounds{{0,0},{100,100}};
    const std::vector<Vec2> reentry{{20,20},{80,20},{80,140},{20,140},{20,50}};
    CHECK(playable_runway({50,20},1,reentry,bounds,300)==Catch::Approx(110));
    const std::vector<Vec2> diagonal{{80,80},{140,110},{80,50}};
    CHECK(playable_runway({80,80},1,diagonal,bounds,150)==Catch::Approx(std::sqrt(500.0f)));
    const std::vector<Vec2> touch{{20,20},{100,20},{100,50},{20,50}};
    CHECK(playable_runway({80,20},1,touch,bounds,130)==130);
    const std::vector<Vec2> outward{{0,20},{100,20},{140,20}};
    CHECK(playable_runway({100,20},1,outward,bounds,40)==0);
}

TEST_CASE("playable runway handles degenerate segments and outside projections", "[balance][controller][playable-runway]") {
    const Rect bounds{{0,0},{100,100}};
    const std::vector<Vec2> duplicates{{20,50},{20,50},{140,50}};
    CHECK(playable_runway({20,50},1,duplicates,bounds,120)==Catch::Approx(80));
    const std::vector<Vec2> incoming{{-20,50},{20,50},{80,50}};
    CHECK(playable_runway({-10,50},1,incoming,bounds,90)==0);
    CHECK(playable_runway({20,50},0,incoming,bounds,60)==0);
    CHECK(playable_runway({20,50},1,{},bounds,60)==0);
    CHECK(playable_runway({20,50},1,{{20,50}},bounds,60)==0);
}
