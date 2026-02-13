#pragma once 

#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <new>

#if defined(__cpp_lib_hardware_interference_size)
    static constexpr std::size_t cacheline_size = std::hardware_destructive_interference_size;
#elif defined(__APPLE__) && defined(__aarch64__)
    static constexpr std::size_t cacheline_size = 128;
#else   
    static constexpr std::size_t cacheline_size = 64;
#endif

namespace foundry_runtime {

static inline void sw_prefetch_read(const void* p) noexcept {
    __builtin_prefetch(p, 0, 3);
}

static inline void sw_prefetch_write(const void* p) noexcept {
    __builtin_prefetch(p, 1, 3);
}


template <class T, size_t capacity, bool enable_cacheline_padding, bool enable_prefetch>
class spsc_queue {
    static_assert(capacity >= 2);
    static_assert(std::is_trivially_copyable_v<T>, "Trivially Copyable T NOT Provided...");    

    struct alignas(cacheline_size) PaddedLine {
        std::atomic<std::size_t> r_w_index{0};
        char pad[cacheline_size - sizeof(std::atomic<std::size_t>)]{}; // basically hogs the rest of the cacheline so that read and write indeces never share the same line
    };

    struct UnpaddedLine {
        std::atomic<std::size_t> r_w_index{0};
    };

    static_assert(sizeof(PaddedLine) == cacheline_size);

    using IndexType = std::conditional_t<
        enable_cacheline_padding,
        PaddedLine,
        UnpaddedLine
    >;

public:
    spsc_queue()                             = default;
    spsc_queue(const spsc_queue&)            = delete;
    spsc_queue& operator=(const spsc_queue&) = delete;

    ~spsc_queue() = default;

    bool try_enqueue(const T& in_data) {
        auto current_write_loc = write_next.r_w_index.load(std::memory_order_relaxed);
        auto next_loc          = increment(current_write_loc);

        if (next_loc == read_next.r_w_index.load(std::memory_order_acquire)) return false;

        if constexpr (enable_prefetch) sw_prefetch_write(&queue[current_write_loc]);
        queue[current_write_loc] = in_data;

        write_next.r_w_index.store(next_loc, std::memory_order_release);
        
        return true;
    }

    bool try_dequeue(T& out_data) {
        auto current_read_loc = read_next.r_w_index.load(std::memory_order_relaxed);

        if (current_read_loc == write_next.r_w_index.load(std::memory_order_acquire)) return false;

        if constexpr (enable_prefetch) sw_prefetch_read(&queue[current_read_loc]);
        out_data = queue[current_read_loc];

        read_next.r_w_index.store(increment(current_read_loc), std::memory_order_release);
        
        return true;
    }

private:
    static constexpr std::size_t increment(std::size_t i) noexcept {
        return (i + 1 == capacity) ? 0 : (i + 1);
    }


    IndexType write_next{}; 
    IndexType read_next{}; 

    T queue[capacity];
};

};