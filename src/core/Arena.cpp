#include "core/Arena.h"

#include <cstdint>
#include <cstdlib>
#include <utility>

namespace immune {
namespace {
constexpr std::uintptr_t align_up(std::uintptr_t v, usize a) {
    return (v + (a - 1)) & ~(static_cast<std::uintptr_t>(a) - 1);
}
} // namespace

Arena::~Arena() {
    if (base_) std::free(base_);
}

Arena::Arena(Arena&& other) noexcept
    : base_(other.base_), capacity_(other.capacity_), offset_(other.offset_), peak_(other.peak_) {
    other.base_ = nullptr;
    other.capacity_ = other.offset_ = other.peak_ = 0;
}

Arena& Arena::operator=(Arena&& other) noexcept {
    if (this != &other) {
        if (base_) std::free(base_);
        base_ = other.base_;
        capacity_ = other.capacity_;
        offset_ = other.offset_;
        peak_ = other.peak_;
        other.base_ = nullptr;
        other.capacity_ = other.offset_ = other.peak_ = 0;
    }
    return *this;
}

void Arena::reserve(usize capacity_bytes) {
    if (base_) std::free(base_);
    base_ = static_cast<u8*>(std::malloc(capacity_bytes));
    capacity_ = base_ ? capacity_bytes : 0;
    offset_ = 0;
    peak_ = 0;
}

void* Arena::allocate(usize bytes, usize alignment) {
    if (alignment == 0) alignment = 1;
    if (!base_) return nullptr;
    // Align the ABSOLUTE address, not the offset: malloc only guarantees
    // max_align_t, so an aligned offset off an unaligned base is still unaligned.
    const auto base_addr = reinterpret_cast<std::uintptr_t>(base_);
    const usize aligned =
        static_cast<usize>(align_up(base_addr + offset_, alignment) - base_addr);
    if (aligned + bytes > capacity_) return nullptr;
    void* p = base_ + aligned;
    offset_ = aligned + bytes;
    if (offset_ > peak_) peak_ = offset_;
    return p;
}

} // namespace immune
