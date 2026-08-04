#include "core/Arena.h"

#include <catch2/catch_test_macros.hpp>

using namespace immune;

TEST_CASE("arena bump-allocates and resets", "[core][arena]") {
    Arena arena(1024);
    REQUIRE(arena.capacity() == 1024);
    REQUIRE(arena.used() == 0);

    f32* a = arena.alloc_array<f32>(16);
    REQUIRE(a != nullptr);
    REQUIRE(arena.used() >= 16 * sizeof(f32));

    const usize after_first = arena.used();
    f32* b = arena.alloc_array<f32>(16);
    REQUIRE(b != nullptr);
    REQUIRE(b != a);
    REQUIRE(arena.used() > after_first);

    arena.reset();
    REQUIRE(arena.used() == 0);
    REQUIRE(arena.capacity() == 1024);
    REQUIRE(arena.peak() >= after_first);
}

TEST_CASE("arena returns nullptr instead of growing", "[core][arena]") {
    Arena arena(64);
    REQUIRE(arena.alloc_array<u8>(64) != nullptr);
    REQUIRE(arena.alloc_array<u8>(1) == nullptr);
}

TEST_CASE("arena honours alignment", "[core][arena]") {
    Arena arena(4096);
    (void)arena.alloc_array<u8>(1);  // knock the offset off-alignment
    auto* p = arena.allocate(32, 64);
    REQUIRE(p != nullptr);
    REQUIRE(reinterpret_cast<std::uintptr_t>(p) % 64 == 0);
}

TEST_CASE("arena scope restores the bump pointer", "[core][arena]") {
    Arena arena(1024);
    (void)arena.alloc_array<u32>(4);
    const usize outer = arena.used();
    {
        Arena::Scope scope(arena);
        (void)arena.alloc_array<u32>(32);
        REQUIRE(arena.used() > outer);
    }
    REQUIRE(arena.used() == outer);
}

TEST_CASE("arena alloc constructs the object", "[core][arena]") {
    struct Pod { int x; float y; };
    Arena arena(256);
    Pod* p = arena.alloc<Pod>(Pod{7, 1.5f});
    REQUIRE(p != nullptr);
    REQUIRE(p->x == 7);
    REQUIRE(p->y == 1.5f);
}
