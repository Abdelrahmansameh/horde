// core/Arena.h — bump/linear allocator. FROZEN CONTRACT.
//
// Hot-path rule (docs/CONVENTIONS.md): no malloc/new during a sim tick.
// Per-tick scratch memory comes from a frame Arena that is reset (not freed)
// at the top of every tick, making allocation a pointer bump.
//
// Arena never runs destructors. Only use it for trivially destructible types.
#pragma once

#include "core/Types.h"

#include <cassert>
#include <new>
#include <type_traits>

namespace immune {

class Arena {
public:
    Arena() = default;
    explicit Arena(usize capacity_bytes) { reserve(capacity_bytes); }
    ~Arena();

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&& other) noexcept;
    Arena& operator=(Arena&& other) noexcept;

    /// Allocates the backing block. Existing contents are discarded.
    void reserve(usize capacity_bytes);

    /// Raw allocation. Returns nullptr if the arena would overflow — callers in
    /// the hot path must size the arena so this never happens, and assert on it.
    void* allocate(usize bytes, usize alignment = alignof(std::max_align_t));

    /// Typed uninitialised array allocation. T must be trivially destructible.
    template <typename T>
    T* alloc_array(usize count) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "Arena never runs destructors; T must be trivially destructible");
        void* p = allocate(sizeof(T) * count, alignof(T));
        return static_cast<T*>(p);
    }

    /// Typed single-object allocation with in-place construction.
    template <typename T, typename... Args>
    T* alloc(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "Arena never runs destructors; T must be trivially destructible");
        void* p = allocate(sizeof(T), alignof(T));
        return p ? new (p) T(static_cast<Args&&>(args)...) : nullptr;
    }

    /// Frees everything in O(1). Memory stays reserved.
    void reset() { offset_ = 0; }

    usize used() const { return offset_; }
    usize capacity() const { return capacity_; }
    usize remaining() const { return capacity_ - offset_; }
    /// High-water mark since the last reserve(); used to right-size arenas.
    usize peak() const { return peak_; }

    /// RAII save/restore of the bump pointer for nested scratch scopes.
    class Scope {
    public:
        explicit Scope(Arena& a) : arena_(a), mark_(a.offset_) {}
        ~Scope() { arena_.offset_ = mark_; }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        Arena& arena_;
        usize mark_;
    };

private:
    u8* base_ = nullptr;
    usize capacity_ = 0;
    usize offset_ = 0;
    usize peak_ = 0;
};

} // namespace immune
