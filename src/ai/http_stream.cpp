/**
 * @file src/ai/http_stream.cpp
 * @brief Admission control for blocking upstream SSE requests.
 */
#include "http_stream.h"
#include <atomic>
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>

namespace ai::http {
  namespace {
    unsigned
    validated(unsigned capacity) {
      if (!capacity || capacity > 16) throw std::invalid_argument("Invalid AI stream capacity");
      return capacity;
    }
  }  // namespace
  struct StreamExecutor::Impl {
    const unsigned capacity;
    std::atomic_uint active { 0 };
    std::atomic_bool stopping { false };
    std::mutex admission;
    std::once_flag shutdown;
    // Last so workers finish before the state they use is destroyed.
    boost::asio::thread_pool workers;
    explicit Impl(unsigned count): capacity(validated(count)), workers(capacity) {}
  };
  StreamExecutor::StreamExecutor(unsigned capacity): impl(std::make_unique<Impl>(capacity)) {}
  StreamExecutor::~StreamExecutor() { stop(); }
  bool
  StreamExecutor::submit(std::function<void()> task) {
    auto &pool = *impl;
    std::lock_guard lock(pool.admission);
    if (pool.stopping || pool.active >= pool.capacity) return false;
    ++pool.active;
    try {
      boost::asio::post(pool.workers, [&pool, task = std::move(task)] {
        struct Release {
          Impl &pool;
          ~Release() { --pool.active; }
        } release { pool };
        try {
          task();
        }
        catch (...) { /* Never unwind an executor worker. */
        }
      });
    }
    catch (...) {
      --pool.active;
      throw;
    }
    return true;
  }
  bool
  StreamExecutor::stopping() const { return impl->stopping; }
  unsigned
  StreamExecutor::active() const { return impl->active; }
  void
  StreamExecutor::stop() {
    auto &pool = *impl;
    std::call_once(pool.shutdown, [&pool] {
      {
        std::lock_guard lock(pool.admission);
        pool.stopping = true;
      }
      pool.workers.join();
    });
  }
  StreamExecutor &
  production_executor() {
    static StreamExecutor pool;
    return pool;
  }
  void
  shutdown_streams() { production_executor().stop(); }
}  // namespace ai::http
