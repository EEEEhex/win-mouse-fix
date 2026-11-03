#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <type_traits>

namespace wmf::host {
#pragma warning(push)
#pragma warning(disable: 4324) // Intentional cache-line padding separates producer and consumer cursors.
template<class T, std::size_t Capacity> class SpscRing {
    static_assert(Capacity > 1 && std::is_trivially_copyable_v<T>);
public:
    SpscRing() : entries_(std::make_unique<std::array<T, Capacity>>()) {}
    bool Push(const T& value) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) == Capacity) return false;
        (*entries_)[head % Capacity] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }
    bool Pop(T& value) noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        value = (*entries_)[tail % Capacity];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }
private:
    std::unique_ptr<std::array<T, Capacity>> entries_;
    alignas(64) std::atomic<std::size_t> head_{};
    alignas(64) std::atomic<std::size_t> tail_{};
};
#pragma warning(pop)
}
