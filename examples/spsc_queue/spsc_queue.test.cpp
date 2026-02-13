#include <foundry_runtime/spsc_queue/spsc_queue.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>



struct ProducerThread { std::thread producer; };
struct ConsumerThread { std::thread consumer; };
using ThreadPair = std::tuple<ProducerThread, ConsumerThread>;

template <class QueueType>
ThreadPair dispatchThreads(QueueType& queue, std::uint64_t number) {

    ProducerThread producer{
        std::thread([&] {
            for (uint64_t i = 0; i < number; ++i) {
                while (!queue.try_enqueue(i)) {
                    std::this_thread::yield();
                }
            }
        })
    };

    ConsumerThread consumer{
        std::thread([&] {
            uint64_t decrementor = number;
            uint64_t dequeued_value;
            while (decrementor > 0) {
                if (queue.try_dequeue(dequeued_value)) decrementor--;
                else std::this_thread::yield();
            }
        })        
    };

    return {std::move(producer), std::move(consumer)};
}


template <class QueueType>
double runSim(std::uint64_t number) {
    QueueType queue;

    auto start = std::chrono::steady_clock::now();
    
    ThreadPair threads = dispatchThreads(queue, number);
    std::get<ProducerThread>(threads).producer.join();
    std::get<ConsumerThread>(threads).consumer.join();

    auto end  = std::chrono::steady_clock::now();

    return std::chrono::duration<double>(end - start).count();
}

int main() {

    constexpr uint64_t number   = 5'000'000;
    constexpr uint8_t  num_sims = 10;

    std::vector<double> sim_times;
    sim_times.reserve(num_sims);
    
    for (uint8_t i = 0; i < num_sims; i++) {
        sim_times.emplace_back(runSim<foundry_runtime::spsc_queue<std::uint64_t, 128, true, false>>(number));
    }

    double cumulative_time = 0;
    for (const double entry : sim_times) {
        cumulative_time += entry;
    }

    std::cout << "Num Sims=" << int(num_sims) << "\n";
    std::cout << "Average Sim Time=" << (cumulative_time / num_sims) << "\n";
    std::cout << "Num Entries=" << int(number) << "\n";

    return 0;
}

/*
Benchmarking

Initial SPSC Run of
    runSim<foundry_runtime::spsc_queue, 128>(number)
    Num Sims=10
    Average Sim Time=0.0494914
    Num Entries=5000000

With CacheLine Padding
    runSim<foundry_runtime::spsc_queue, 128>(number)
    Num Sims=10
    Average Sim Time=0.0284818
    Num Entries=5000000

With Prefetch
    runSim<foundry_runtime::spsc_queue, 128>(number)
    Num Sims=10
    Average Sim Time=0.063968
    Num Entries=5000000

With Prefetch and Padding
    runSim<foundry_runtime::spsc_queue, 128>(number)
    Num Sims=10
    Average Sim Time=0.0449923
    Num Entries=5000000
With Cached Read/Writes + Line Padding
    runSim<foundry_runtime::spsc_queue, 128>(number)
    Num Sims=10
    Average Sim Time=0.0180902
    Num Entries=5000000
With forced power of two Array Size
    runSim<foundry_runtime::spsc_queue, 128>(number)
    Num Sims=10
    Average Sim Time=0.0170691
    Num Entries=5000000

// PREFETCH SEEMS TO HELP WHEN I DRASTICALLY INCREASE BUFFER SIZE
*/