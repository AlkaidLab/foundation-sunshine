/**
 * @file src/ai/http_stream.cpp
 * @brief Admission control for blocking upstream SSE requests.
 */
#include "http_stream.h"
#include <atomic>
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <semaphore>

namespace ai::http {
  namespace {
    struct StreamExecutor {
      std::counting_semaphore<4> slots { 4 };
      std::atomic_bool stopping { false };
      std::mutex admission;
      std::once_flag shutdown;
      // Declared last so workers finish before the semaphore is destroyed.
      boost::asio::thread_pool workers { 4 };
    };

    StreamExecutor &
    executor() {
      static StreamExecutor instance;
      return instance;
    }
  }  // namespace

  bool
  submit_stream(std::function<void()> task) {
    auto &pool = executor();
    std::lock_guard admission(pool.admission);
    if (pool.stopping || !pool.slots.try_acquire()) return false;
    try {
      boost::asio::post(pool.workers, [&pool, task = std::move(task)] {
        struct Release {
          StreamExecutor &pool;
          ~Release() { pool.slots.release(); }
        } release { pool };
        try {
          task();
        }
        catch (...) {
          // Never unwind an executor worker.
        }
      });
    }
    catch (...) {
      pool.slots.release();
      throw;
    }
    return true;
  }

  bool
  stream_shutdown_requested() {
    return executor().stopping;
  }

  void
  shutdown_streams() {
    auto &pool = executor();
    std::call_once(pool.shutdown, [&pool] {
      {
        std::lock_guard admission(pool.admission);
        pool.stopping = true;
      }
      pool.workers.join();
    });
  }
}  // namespace ai::http
