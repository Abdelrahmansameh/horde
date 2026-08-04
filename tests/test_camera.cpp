#include "render/Camera.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace immune;
using namespace immune::render;
using Catch::Approx;

TEST_CASE("screen_to_world inverts world_to_screen", "[render][camera]") {
    Camera cam;
    cam.set_viewport(1600, 900);
    cam.set_view_height(90.0f);
    cam.set_center(Vec2{128.0f, 72.0f});
    cam.set_tilt_degrees(20.0f);

    for (const Vec2 world : {Vec2{128.0f, 72.0f}, Vec2{100.0f, 50.0f}, Vec2{200.0f, 90.0f}}) {
        const Vec2 screen = cam.world_to_screen(world);
        const Vec2 back = cam.screen_to_world(screen);
        REQUIRE(back.x == Approx(world.x).margin(0.01));
        REQUIRE(back.y == Approx(world.y).margin(0.01));
    }
}

TEST_CASE("world centre maps to screen centre", "[render][camera]") {
    Camera cam;
    cam.set_viewport(1600, 900);
    cam.set_center(Vec2{50.0f, 25.0f});
    const Vec2 s = cam.world_to_screen(Vec2{50.0f, 25.0f});
    REQUIRE(s.x == Approx(800.0f).margin(0.01));
    REQUIRE(s.y == Approx(450.0f).margin(0.01));
}

TEST_CASE("tilt is clamped to a sane range", "[render][camera]") {
    Camera cam;
    cam.set_tilt_degrees(-10.0f);
    REQUIRE(cam.tilt_degrees() == 0.0f);
    cam.set_tilt_degrees(90.0f);
    REQUIRE(cam.tilt_degrees() == 45.0f);
}

TEST_CASE("tilt foreshortens world Y", "[render][camera]") {
    Camera flat, tilted;
    for (Camera* c : {&flat, &tilted}) {
        c->set_viewport(1600, 900);
        c->set_view_height(90.0f);
        c->set_center(Vec2{0.0f, 0.0f});
    }
    flat.set_tilt_degrees(0.0f);
    tilted.set_tilt_degrees(30.0f);

    // The same world offset covers fewer screen pixels when tilted.
    const f32 flat_dy = std::abs(flat.world_to_screen(Vec2{0, 10}).y - flat.world_to_screen(Vec2{0, 0}).y);
    const f32 tilt_dy = std::abs(tilted.world_to_screen(Vec2{0, 10}).y - tilted.world_to_screen(Vec2{0, 0}).y);
    REQUIRE(tilt_dy < flat_dy);
}

TEST_CASE("clamp_to_bounds keeps the view inside the level", "[render][camera]") {
    Camera cam;
    cam.set_viewport(1600, 900);
    cam.set_view_height(40.0f);
    cam.set_bounds(Rect{Vec2{0.0f, 0.0f}, Vec2{256.0f, 144.0f}});
    cam.set_center(Vec2{-500.0f, -500.0f});
    cam.clamp_to_bounds();
    const Rect vis = cam.visible_bounds();
    REQUIRE(vis.min.x >= -0.01f);
    REQUIRE(vis.min.y >= -0.01f);
}

TEST_CASE("visible_bounds grows with view height", "[render][camera]") {
    Camera cam;
    cam.set_viewport(1600, 900);
    cam.set_center(Vec2{0.0f, 0.0f});
    cam.set_view_height(50.0f);
    const Vec2 small = cam.visible_bounds().size();
    cam.set_view_height(100.0f);
    const Vec2 big = cam.visible_bounds().size();
    REQUIRE(big.x > small.x);
    REQUIRE(big.y > small.y);
}
