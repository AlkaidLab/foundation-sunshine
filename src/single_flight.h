/**
 * @file src/single_flight.h
 * @brief One slow probe at a time per key, shared by every caller that asks while it runs.
 *
 * The network probes take seconds (a router that does not answer UPnP costs about six). They used to hold a mutex while they ran, so every request that came
 * in behind one started its own full probe afterwards: four refreshes at once (the home page and the network page together do that) answered after 8, 16, 24
 * and 32 seconds, and each of them held a worker of the web server for the whole time. A caller that asks while the same probe is running now waits for that
 * run and gets its answer, and a refresh right after a finished run is answered by that run.
 */
#pragma once

#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <string>

namespace single_flight {
  template<class T>
  class cell_t {
  public:
    /**
     * @param key        what the result depends on; a different key is a different probe
     * @param refresh    the caller wants a new run
     * @param now_s      the current time in seconds
     * @param ttl_s      a result younger than this answers a caller that does not ask for a refresh
     * @param reuse_s    a result younger than this answers a refresh too (it is what the refresh would find)
     * @param collect    the probe; runs on the calling thread, without any lock held
     */
    template<class Collect>
    T
    get(const std::string &key, bool refresh, std::int64_t now_s, std::int64_t ttl_s, std::int64_t reuse_s, Collect &&collect) {
      std::shared_future<T> joined;
      std::shared_ptr<std::promise<T>> mine;
      {
        std::lock_guard lock { mutex_ };
        if (has_result_ && key == result_key_) {
          const auto age = now_s - result_at_;
          if (age < reuse_s || (!refresh && age < ttl_s)) return result_;
        }
        if (flight_ && flight_key_ == key) {
          joined = flight_future_;
        }
        else {
          mine = std::make_shared<std::promise<T>>();
          flight_key_ = key;
          flight_future_ = mine->get_future().share();
          flight_ = mine;
        }
      }
      if (joined.valid()) return joined.get();

      try {
        auto value = collect();
        {
          std::lock_guard lock { mutex_ };
          result_ = value;
          result_key_ = key;
          result_at_ = now_s;
          has_result_ = true;
          if (flight_ == mine) flight_.reset();
        }
        mine->set_value(value);
        return value;
      }
      catch (...) {
        {
          std::lock_guard lock { mutex_ };
          if (flight_ == mine) flight_.reset();
        }
        mine->set_exception(std::current_exception());
        throw;
      }
    }

  private:
    std::mutex mutex_;
    T result_ {};
    std::string result_key_;
    std::int64_t result_at_ = 0;
    bool has_result_ = false;
    std::shared_ptr<std::promise<T>> flight_;
    std::string flight_key_;
    std::shared_future<T> flight_future_;
  };
}  // namespace single_flight
