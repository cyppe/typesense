// https://github.com/jhasse/ThreadPool

#pragma once

#include <functional>
#include <future>
#include <queue>
#include <atomic>
#include <chrono>
#include "logger.h"

struct ThreadPoolMetricsSnapshot {
    uint64_t queued_tasks = 0;
    uint64_t active_workers = 0;
    uint64_t cumulative_enqueued = 0;
    uint64_t cumulative_executed = 0;
    uint64_t last_wait_ms = 0;
    uint64_t max_wait_ms = 0;
    uint64_t max_queued_tasks = 0;
    uint64_t worker_count = 0;
};

class ThreadPool {
public:
    explicit ThreadPool(size_t);
    template<class F, class... Args>
    decltype(auto) enqueue(F&& f, Args&&... args);
    void log_exhaustion();
    void shutdown();
    ThreadPoolMetricsSnapshot get_metrics_snapshot() const;
private:
    struct queued_task_t {
        std::packaged_task<void()> task;
        uint64_t enqueue_ts_us;
    };

    // need to keep track of threads so we can join them
    std::vector< std::thread > workers;
    // the task queue
    std::queue<queued_task_t> tasks;

    // synchronization
    std::mutex queue_mutex;
    std::condition_variable condition;
    std::condition_variable condition_producers;
    bool stop;

    std::atomic<uint64_t> queued_tasks_current{0};
    std::atomic<uint64_t> active_workers{0};
    std::atomic<uint64_t> cumulative_enqueued{0};
    std::atomic<uint64_t> cumulative_executed{0};
    std::atomic<uint64_t> last_wait_ms{0};
    std::atomic<uint64_t> max_wait_ms{0};
    std::atomic<uint64_t> max_queued_tasks{0};
};

// the constructor just launches some amount of workers
inline ThreadPool::ThreadPool(size_t threads)
        :   stop(false)
{
    for(size_t i = 0;i<threads;++i)
        workers.emplace_back(
                [this]
                {
                    for(;;)
                    {
                        queued_task_t queued_task;
                        {
                            std::unique_lock<std::mutex> lock(this->queue_mutex);
                            this->condition.wait(lock,
                                                 [this]{ return this->stop || !this->tasks.empty(); });
                            if(this->stop) {
                                return;
                            }

                            if(this->tasks.empty()) {
                                continue;
                            }
                            queued_task = std::move(this->tasks.front());
                            this->tasks.pop();
                            queued_tasks_current.fetch_sub(1, std::memory_order_relaxed);
                            if (tasks.empty()) {
                                condition_producers.notify_one(); // notify the destructor that the queue is empty
                            }
                        }

                        const uint64_t now_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count());
                        const auto wait_ms = now_us >= queued_task.enqueue_ts_us
                            ? (now_us - queued_task.enqueue_ts_us) / 1000
                            : 0;
                        last_wait_ms.store(wait_ms, std::memory_order_relaxed);
                        auto prev_max_wait = max_wait_ms.load(std::memory_order_relaxed);
                        while (wait_ms > prev_max_wait &&
                               !max_wait_ms.compare_exchange_weak(prev_max_wait, wait_ms, std::memory_order_relaxed)) {
                        }

                        active_workers.fetch_add(1, std::memory_order_relaxed);
                        queued_task.task();
                        active_workers.fetch_sub(1, std::memory_order_relaxed);
                        cumulative_executed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
        );
}

// add new work item to the pool
template<class F, class... Args>
decltype(auto) ThreadPool::enqueue(F&& f, Args&&... args)
{
    using return_type = std::invoke_result_t<F, Args...>;

    std::packaged_task<return_type()> task(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    std::future<return_type> res = task.get_future();
    {
        std::unique_lock<std::mutex> lock(queue_mutex);

        // don't allow enqueueing after stopping the pool
        if(!stop) {
            const uint64_t enqueue_ts_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            tasks.emplace(queued_task_t{std::move(task), enqueue_ts_us});
            cumulative_enqueued.fetch_add(1, std::memory_order_relaxed);
            const auto queued = queued_tasks_current.fetch_add(1, std::memory_order_relaxed) + 1;
            auto prev_max_queued = max_queued_tasks.load(std::memory_order_relaxed);
            while (queued > prev_max_queued &&
                   !max_queued_tasks.compare_exchange_weak(prev_max_queued, queued, std::memory_order_relaxed)) {
            }
        }
    }
    condition.notify_one();
    return res;
}

inline void ThreadPool::shutdown() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        condition_producers.wait(lock, [this] { return tasks.empty(); });
        stop = true;
    }
    condition.notify_all();
    for (std::thread& worker : workers) {
        worker.join();
    }
}

inline void ThreadPool::log_exhaustion() {
    std::unique_lock<std::mutex> lock(queue_mutex);
    if(tasks.size() >= workers.size()) {
        TS_LOG(WARNING) << "Threadpool exhaustion detected, task_queue_len: "
                     << tasks.size() << ", thread_pool_len: " << workers.size();
    }
}

inline ThreadPoolMetricsSnapshot ThreadPool::get_metrics_snapshot() const {
    ThreadPoolMetricsSnapshot snapshot;
    snapshot.queued_tasks = queued_tasks_current.load(std::memory_order_relaxed);
    snapshot.active_workers = active_workers.load(std::memory_order_relaxed);
    snapshot.cumulative_enqueued = cumulative_enqueued.load(std::memory_order_relaxed);
    snapshot.cumulative_executed = cumulative_executed.load(std::memory_order_relaxed);
    snapshot.last_wait_ms = last_wait_ms.load(std::memory_order_relaxed);
    snapshot.max_wait_ms = max_wait_ms.load(std::memory_order_relaxed);
    snapshot.max_queued_tasks = max_queued_tasks.load(std::memory_order_relaxed);
    snapshot.worker_count = workers.size();
    return snapshot;
}
