#pragma once

#include <queue>
#include <mutex>

namespace TestMutexQueue {

template <class T>
class mutex_std_queue {
public:
    mutex_std_queue()                                  = default;
    mutex_std_queue(const mutex_std_queue&)            = delete;
    mutex_std_queue& operator=(const mutex_std_queue&) = delete;

    ~mutex_std_queue() = default;

    bool try_enqueue(const T& in_data) {
        std::unique_lock<std::mutex> lock(mut);
        queue.push(in_data);
        return true;
    }

    bool try_dequeue(T& out_data) { 
        std::unique_lock<std::mutex> lock(mut);
        if (queue.empty()) return false;

        out_data = queue.front();
        queue.pop();
        return true;
    }

private:
    std::mutex mut;
    std::queue<T> queue;
};

};