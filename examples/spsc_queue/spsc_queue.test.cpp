#include <foundry_runtime/atomic_spsc_queue/spsc_queue.h>
#include <foundry_runtime/locked_spsc_queue/mutex_spsc_queue.h>

//clang++ -I../../include spsc_queue.test.cpp -o main -std=c++23 -O3

#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>


#if defined(__APPLE__)

#include <pthread.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>

static inline void set_qos_high(bool aggressive = false) {
    pthread_set_qos_class_self_np(
        aggressive ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED,
        0
    );
}

static inline void set_affinity_tag(int tag) {
    thread_affinity_policy_data_t policy = { tag };
    thread_policy_set(
        mach_thread_self(),
        THREAD_AFFINITY_POLICY,
        (thread_policy_t)&policy,
        THREAD_AFFINITY_POLICY_COUNT
    );
}

#endif

static inline void setup_benchmark_thread() {
#if defined(__APPLE__)
    set_qos_high(true);
    set_affinity_tag(1);
#endif
}


struct ProducerThread { std::thread producer; };
struct ConsumerThread { std::thread consumer; };
using ThreadPair = std::tuple<ProducerThread, ConsumerThread>;

template <class QueueType>
ThreadPair dispatchThreads(QueueType& queue, std::uint64_t number) {

    ProducerThread producer{
        std::thread([&] {
            setup_benchmark_thread();

            for (uint64_t i = number; i > 0; --i) {
                while (!queue.try_enqueue(i)) {
                    std::this_thread::yield();
                }
            }
        })
    };

    ConsumerThread consumer{
        std::thread([&] {
            setup_benchmark_thread();

            uint64_t decrementor = number;
            uint64_t dequeued_value;
            while (decrementor > 0) {
                if (queue.try_dequeue(dequeued_value)) {
                    if (dequeued_value != decrementor) {
                        std::cerr << "INVALID VALUE DEQUEUED=" << dequeued_value << "\n";
                        std::terminate();
                    }                    
                    decrementor--;
                }
                else std::this_thread::yield();
            }
        })        
    };

    return {std::move(producer), std::move(consumer)};
}


template <class QueueType>
double runSim(std::uint64_t number) {
    auto queue = std::make_unique<QueueType>();

    auto start = std::chrono::steady_clock::now();
    
    ThreadPair threads = dispatchThreads(*queue, number);
    std::get<ProducerThread>(threads).producer.join();
    std::get<ConsumerThread>(threads).consumer.join();

    auto end  = std::chrono::steady_clock::now();

    return std::chrono::duration<double>(end - start).count();
}


template <std::size_t capacity, bool cache_padding, std::size_t prefetch>
double inline run_single_sim(std::uint64_t number) {
    return runSim<foundry_runtime::spsc_queue<std::uint64_t, capacity, cache_padding, prefetch>>(number);
}

template <std::size_t capacity>
void vary_parameters_per_capacity(std::uint64_t number, uint8_t num_sims) {
    static constexpr std::size_t bytes = capacity * sizeof(std::uint64_t);
    static_assert(bytes <= (64ull << 20), "capacity > 64 MB");

    auto run_sims = [&](auto ena_cache_padding, auto ena_prefetch) {
        constexpr bool cache_padding   = decltype(ena_cache_padding)::value;
        constexpr std::size_t prefetch = decltype(ena_prefetch)::value;

        double time_sum = 0;
        double minimum  = std::numeric_limits<double>::max();
        double maximum  = 0;
        double current_sim_length = 0;

        for (uint8_t sim_num = 0; sim_num < num_sims; sim_num++) {
            current_sim_length = run_single_sim<capacity, cache_padding, prefetch>(number);
            time_sum += current_sim_length;

            if (current_sim_length < minimum) minimum = current_sim_length;
            if (current_sim_length > maximum) maximum = current_sim_length;
        }

        double avg_sim_time = time_sum / num_sims;
        std::cout << "lock-free,uint64_t," << int(num_sims) << "," << capacity << "," << cache_padding << "," << prefetch << "," << avg_sim_time << "," << maximum << "," << minimum << "," << number << "\n";
    };

    using prefetchOff = std::integral_constant<std::size_t, 0>;
    using prefetchOne = std::integral_constant<std::size_t, 1>;
    using prefetchTwo = std::integral_constant<std::size_t, 2>;
    using prefetchFour = std::integral_constant<std::size_t, 4>;
    using prefetchEight= std::integral_constant<std::size_t, 8>;
    using prefetchSixteen= std::integral_constant<std::size_t, 16>;

    // note that prefetch is a uint8_t now.....
    run_sims(std::bool_constant<false>{}, prefetchOff{});
    run_sims(std::bool_constant<false>{}, prefetchOne{});
    //run_sims(std::bool_constant<false>{}, prefetchTwo{});
    run_sims(std::bool_constant<false>{}, prefetchFour{});
    //run_sims(std::bool_constant<false>{}, prefetchEight{});
    //run_sims(std::bool_constant<false>{}, prefetchSixteen{});

    run_sims(std::bool_constant<true>{},  prefetchOff{});
    run_sims(std::bool_constant<true>{},  prefetchOne{});
    //run_sims(std::bool_constant<true>{},  prefetchTwo{});
    run_sims(std::bool_constant<true>{},  prefetchFour{});
    //run_sims(std::bool_constant<true>{},  prefetchEight{});
    //run_sims(std::bool_constant<true>{},  prefetchSixteen{});
}

template <std::size_t... capacities>
void run_many_capacities(std::uint64_t number, uint8_t num_sims) {
    (vary_parameters_per_capacity<capacities>(number, num_sims), ...);
}



void run_mutex_queue(std::uint64_t number, uint8_t num_sims) {
    double time_sum = 0;
    double minimum  = std::numeric_limits<double>::max();
    double maximum  = 0;
    double current_sim_length = 0;

    for (uint8_t sim_num = 0; sim_num < num_sims; sim_num++) {
        current_sim_length = runSim<TestMutexQueue::mutex_std_queue<uint64_t>>(number);
        time_sum += current_sim_length;

        if (current_sim_length < minimum) minimum = current_sim_length;
        if (current_sim_length > maximum) maximum = current_sim_length;
    }

    double avg_sim_time = time_sum / num_sims;
    std::cout << "mutex,uint64_t," << int(num_sims) << "," << 0 << "," << 0 << "," << 0 << "," << avg_sim_time << "," << maximum << "," << minimum << "," << number << "\n";
}




int main() {

    constexpr uint64_t number   = 10'000'000;
    constexpr uint8_t  num_sims = 10;

    std::cout << "queue_type,type,numsims,capacity,cache_padding,prefetch,avg_sim_time,max_sim_time,min_sim_time,num_enq_deq\n";
    run_many_capacities<64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144/*, 524288, 1048576, 2097152, 4194304*/>(number, num_sims);
    run_mutex_queue(number, num_sims);

    return 0;
}