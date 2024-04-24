#include <chrono>
#include <numeric>
#include <random>
#include <thread>
#include <utility>

#include "debug.hpp"
#include "threaded_task.hpp"
#include "vt100.hpp"
using namespace dpsg::vt100;

// Definition of global declared in  debug.hpp
std::mutex debug_::mutex_{};

int random_integer() {
  static std::mt19937 eng;
  static std::uniform_int_distribution distrib{500, 1500};
  return distrib(eng);
}
int long_computation(auto color, int value) {
  auto delay = random_integer();
  println(color, "Entering id(", value, "), delay: ", delay, "ms");
  std::this_thread::sleep_for(std::chrono::milliseconds{delay});
  println(color, "Yielding ", value);
  return value;
}

int sum(const std::vector<int> &v) {
  DEBUG(white | bold, "Vector of size: ", v.size());
  return std::accumulate(v.begin(), v.end(), 0);
}

int very_long_computation(auto color) {
  auto compute = [color](int i) {
    return [color, i] { return long_computation(color, i); };
  };
  auto p = parallel{
      compute(1),
      compute(10),
      compute(100),
  };
  p.join();
  return sum(p.results());
}

struct coro {
  struct promise_type;
  using handle_t = std::coroutine_handle<promise_type>;

  struct promise_type {
    std::binary_semaphore signal_done_{0};
    int result_{0};

    coro get_return_object() { return {handle_t::from_promise(*this)}; }

    auto initial_suspend() noexcept { return std::suspend_never{}; }
    auto final_suspend() noexcept { return std::suspend_always{}; }

    void unhandled_exception() noexcept {}

    void return_value(int result) noexcept {
      DEBUG(orange | reverse, "Coroutine is exiting with result: ", result);
      result_ = result;
      signal_done_.release();
    }
  };

  coro(handle_t handl) : handle_{handl} {}
  coro(const coro &other) = delete;
  coro(coro &&other) noexcept
      : handle_{std::exchange(other.handle_, nullptr)} {}
  coro &operator=(const coro &other) = delete;
  coro &operator=(coro &&other) noexcept {
    handle_ = std::exchange(other.handle_, nullptr);
    return *this;
  }

  int result() {
    assert(handle_);
    DEBUG(orange, "Trying to extract result.");
    handle_.promise().signal_done_.acquire();
    return handle_.promise().result_;
  }

  ~coro() noexcept {
    if (handle_) {
      handle_.destroy();
    }
  }

private:
  handle_t handle_;
};

coro awaitable_computation(auto color) {
  auto compute = [color](int i) {
    return [color, i] { return long_computation(color, i); };
  };
  auto p = parallel{
      compute(1),
      compute(10),
      compute(100),
  };
  DEBUG(bold | color | underline, "co_awaiting");
  co_await p;
  DEBUG(bold | color | underline, "done");
  co_return sum(p.results());
}

coro awaitable_computation_tmp(auto color) {
  auto compute = [color](int i) {
    return [color, i] { return long_computation(color, i); };
  };
  auto s1 = sum(co_await parallel{
      compute(2),
      compute(20),
      compute(200),
      compute(2000),
  });

  co_return sum(co_await parallel {
    compute(1),
    compute(10),
    compute(100),
    compute(1000),
  }) + s1;
}



int main() {
  coro c1 = awaitable_computation(red);
  coro c2 = awaitable_computation(yellow);
  coro c3 = awaitable_computation(green);
  coro c4 = awaitable_computation_tmp(magenta);
  coro c5 = awaitable_computation_tmp(blue);
  coro c6 = awaitable_computation_tmp(cyan);

  println(red|reverse, c1.result(), reset);
  println(yellow|reverse, c2.result(), reset);
  println(green|reverse, c3.result(), reset);
  println(magenta|reverse, c4.result(), reset);
  println(blue|reverse, c5.result(), reset);
  println(cyan|reverse, c6.result(), reset);
}
