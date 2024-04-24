#include <coroutine>
#include <exception>
#include <thread>
#include <utility>
#include <vector>

#include "basic_type.hpp"
#define DPSG_COMPILE_LINUX_TERM
#include "linux_term.hpp"
#include "vt100.hpp"

using namespace dpsg;
using namespace dpsg::vt100;

void print(char c) { write(STDOUT_FILENO, &c, 1); }

void print(std::string_view sv) { write(STDOUT_FILENO, sv.data(), sv.size()); }
template <size_t N> void print(const char (&arr)[N]) {
  write(STDOUT_FILENO, arr, N - 1);
}

constexpr static inline suspend_if do_suspend{true};
constexpr static inline suspend_if do_not_suspend{false};

struct skip_frames {
  int value;
};

struct pause {
  int pause;
};

template <class T> struct immediate {
  T c;
  explicit(false) immediate(T c) : c{c} {}
};

struct animation {

  struct promise_type;
  using handle_t = std::coroutine_handle<promise_type>;
  handle_t handle_;

  dpsg::term_position anchor_{};

  struct promise_type {
    enum class status { running, stopped, failed };

    int skip_frames = 0;
    int pause = 0;

    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }

    suspend_if yield_value(char c) {
      if (skip_frames > 0) {
        skip_frames--;
        return do_not_suspend;
      }
      print(c);
      return do_suspend;
    }
    template <class C> std::suspend_never yield_value(const immediate<C> &im) {
      print(im.c);
      return {};
    }

    animation get_return_object() {
      return animation{handle_t::from_promise(*this)};
    }

    std::suspend_never await_transform(struct skip_frames sk) {
      skip_frames += sk.value;
      return {};
    }

    std::suspend_never await_transform(struct pause sk) {
      pause += sk.pause;
      return {};
    }

    template <class T>
    std::suspend_never await_transform(struct immediate<T> sk) {
      print(sk.c);
      return {};
    }

    void unhandled_exception() noexcept {
      try {
        std::rethrow_exception(std::current_exception());
      } catch (std::exception &e) {
        std::clog << "Error!!! " << e.what() << std::endl;
      }
    }
  };

  animation(handle_t h) : handle_{h} {}
  animation(const animation &other) = delete;
  animation(animation &&other) noexcept
      : handle_{std::exchange(other.handle_, nullptr)}, anchor_{other.anchor_} {
  }
  animation &operator=(const animation &other) = delete;
  animation &operator=(animation &&other) noexcept {
    this->handle_ = std::exchange(other.handle_, nullptr);
    this->anchor_ = other.anchor_;
    return *this;
  }
  ~animation() noexcept {}

  void run() {
    if (handle_.done())
      return;
    std::cout << set_cursor(anchor_.x, anchor_.y) << std::flush;
    handle_.resume();
  }

  animation &anchor(u16 x, u16 y) & {
    this->anchor_ = {
        .x = x,
        .y = y,
    };
    return *this;
  }
  animation &&anchor(u16 x, u16 y) && {
    return static_cast<animation &&>(anchor(x, y));
  }

  animation const &skip_frames(int x = 1) const & {
    handle_.promise().skip_frames = x;
    return *this;
  }

  animation &&skip_frames(int x = 1) && {
    return const_cast<animation &&>(skip_frames(x));
  }
};

animation with_skip() {
  int iter = 0;
  while (true) {
    auto skip = skip_frames{(iter % 10) > 5 ? 1 : 0};
    co_await skip;
    co_yield '<';
    co_yield '^';
    co_await skip;
    co_yield '>';
    co_yield 'v';

    iter++;
  }
}

animation spinner() {
  while (true) {
    co_yield '|';
    co_yield '/';
    co_yield '-';
    co_yield '\\';
  }
}

animation bar() {
  const auto width = 100;

  for (int i = 0; i < width; ++i) {
    co_yield immediate{'['};
    int j = 0;
    for (j = 0; j < i; ++j) {
      co_yield immediate{'#'};
    }
    for (; j < width; ++j) {
      co_yield immediate{' '};
    }
    co_yield ']';
  }
}

int main() {
  std::vector<animation> animations;
  animations.emplace_back(spinner().anchor(4, 4));
  animations.emplace_back(spinner().anchor(4, 5).skip_frames(2));
  animations.emplace_back(bar().anchor(6, 1));
  animations.emplace_back(with_skip().anchor(4, 6));

  while (true) {
    std::cout << clear << hide_cursor << std::flush;
    for (auto &anim : animations) {
      anim.run();
    }
    using namespace std::chrono_literals;
    std::this_thread::sleep_for(160ms);
  }
}
