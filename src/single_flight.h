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
#include <chrono>
#include <future>
#include <map>
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
        // One flight per key: a probe for another key running beside it does not hide it from a second caller of the same key.
        const auto found = flights_.find(key);
        if (found != flights_.end()) {
          joined = found->second.future;
        }
        else {
          mine = std::make_shared<std::promise<T>>();
          flights_[key] = flight_t { mine, mine->get_future().share() };
        }
      }
      if (joined.valid()) return joined.get();

      const auto began = std::chrono::steady_clock::now();
      const auto land = [&]() {
        const auto found = flights_.find(key);
        if (found != flights_.end() && found->second.promise == mine) flights_.erase(found);
      };
      try {
        auto value = collect();
        {
          std::lock_guard lock { mutex_ };
          // The result is as old as the moment the probe ended, not the moment it began: a six second probe is still fresh a second after it.
          const auto took = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - began).count();
          result_ = value;
          result_key_ = key;
          result_at_ = now_s + took;
          has_result_ = true;
          land();
        }
        mine->set_value(value);
        return value;
      }
      catch (...) {
        {
          std::lock_guard lock { mutex_ };
          land();
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
    struct flight_t {
      std::shared_ptr<std::promise<T>> promise;
      std::shared_future<T> future;
    };
    std::map<std::string, flight_t> flights_;
  };
}  // namespace single_flight
