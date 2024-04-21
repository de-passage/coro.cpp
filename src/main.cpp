#include "basic_type.hpp"
#include "concepts.hpp"
#include <array>
#include <cassert>
#include <coroutine>
#include <iostream>
#include <queue>
#include <semaphore>
#include <sstream>
#include <thread>
#include <vector>

#include "debug.hpp"
#include "vt100.hpp"
using namespace dpsg::vt100;

struct trivial_executor {
  using handle_type = std::coroutine_handle<>;
  using task_type = std::pair<handle_type, handle_type>;
  void schedule(std::convertible_to<task_type> auto &&task) {
      DEBUG(bold|green, "Running task: ", task.first.address());
    task.first.resume();
    if constexpr (!std::is_same_v<decltype(task.second), std::nullptr_t>) {
      if (task.second != nullptr) {
      DEBUG(bold|green, "Running 2nd task: ", task.second.address());
        task.second.resume();
      }
    }
  }

  void execute() {}
};
static_assert(Executor<trivial_executor>);

struct suspend_if {
  bool should_suspend;

  bool await_ready() { return !should_suspend; }
  void await_suspend([[maybe_unused]] std::coroutine_handle<> coro) {}
  void await_resume() {}
};

template <class T, Executor E> struct schedulable_task;

template <class T, Executor E>
struct schedulable_task_promise
    : basic_promise<T, schedulable_task<T, E>, schedulable_task_promise<T, E>> {
  using executor_type = E;
  using promise_type =
      basic_promise<T, schedulable_task<T, E>, schedulable_task_promise<T, E>>;
  using handle_type = promise_type::handle_type;
  bool has_executor() const noexcept { return executor_ != nullptr; }
  executor_type *executor() const noexcept { return executor_; }
  void set_executor(executor_type *executor) noexcept { executor_ = executor; }
  template <Resumable R> void schedule(R &&task) noexcept {
    assert(executor_ != nullptr);
    executor_->schedule(std::pair{std::forward<R>(task), nullptr});
  }
  template <Resumable R, Resumable S>
  void schedule(R &&task, S &&to_resume) noexcept {
    assert(executor_ != nullptr);
    executor_->schedule(
        std::pair{std::forward<R>(task), std::forward<S>(to_resume)});
  }

  auto initial_suspend() noexcept { return suspend_if{!has_executor()}; }

private:
  E *executor_;
};

template <class T, Executor E>
struct schedulable_task : trivial_task<T, schedulable_task_promise<T, E>> {
  using promise_type = schedulable_task_promise<T, E>;
  using base = trivial_task<T, promise_type>;

  schedulable_task(std::coroutine_handle<promise_type> coro) : base(coro) {
      DEBUG(bold | red, "creating schedulable_task with handle ", this->get_handle().address());
  }
  ~schedulable_task() {
    DEBUG(
        bold | red, "destroying schedulable_task. Has handle: ", reset | yellow,
        this->get_handle() != nullptr ? this->get_handle().address() : nullptr);
  }

  void resume() {
    DEBUG(blue | bold, "resuming schedulable_task with handle ", reset | yellow,
          this->get_handle().address());
    this->get_handle().resume();
  }

  struct awaitable {
    std::coroutine_handle<>
        stopped_coro; // The coroutine in which co_await is called
    std::coroutine_handle<promise_type>
        source_coro; // The coroutine that is being waited on

    awaitable(std::coroutine_handle<promise_type> source_coro)
        : stopped_coro{nullptr}, source_coro{source_coro} {}

    bool await_ready() {
      DEBUG(gray, "await_ready called (source_coro: ", source_coro.address(),
            gray, ") -> ", yellow, source_coro.promise().has_executor());
      return source_coro.promise().has_executor();
    }

    template <class U> void await_suspend(std::coroutine_handle<U> caller) {
      stopped_coro = caller;
      DEBUG(gray, "await_suspend called (source_coro: ", source_coro.address(),
            gray, ") with stopped_coro: ", caller.address());
      if (caller.promise().has_executor()) {
        source_coro.promise().set_executor(caller.promise().executor());
        source_coro.promise().schedule(source_coro, caller);
      }
    }

    T await_resume() {
      auto result = std::move(source_coro.promise().result());
      DEBUG(gray, "await_resume called returning ", orange, result, gray,
            " from (source_coro: ", source_coro.address(), gray, ")");
      return std::move(result);
    }
  };
  static_assert(Awaitable<awaitable>);

  awaitable operator co_await() const noexcept {
    DEBUG(bold | blue, "co_await called by ", reset | yellow,
          this->get_handle().address());
    return awaitable{this->get_handle()};
  }

  void set_executor(E *executor) {
    /* DEBUG(bold | blue, "set_executor called on ", reset | yellow,
          this->get_handle().address(), bold | blue, " with ", reset | yellow,
          executor); */
    this->get_handle().promise().set_executor(executor);
    this->get_handle().promise().schedule(this->release_handle());
  }

  bool executable() const noexcept {
    return this->base::get_handle().promise().has_executor();
  }
};

struct delayed_executor {
  using handle_type = std::coroutine_handle<>;
  using task_type = std::pair<handle_type, handle_type>;
  using task_queue = std::queue<task_type>;
  using pending_task_stack = std::vector<handle_type>;

  task_queue tasks;
  pending_task_stack execution_stack;

  void schedule(std::convertible_to<task_type> auto &&task) {
    DEBUG(bold | green, "scheduling task ", task.first.address(), bold | green,
          " with second ", [&] {
            if constexpr (!std::is_same_v<decltype(task.second),
                                          std::nullptr_t>) {
              return (void *)task.second.address();
            } else {
              return (void *)nullptr;
            }
          });
    tasks.push(task);
  }

  void execute() {
#ifndef NDEBUG
    auto show_execution_stack = [&]() noexcept -> string_generator {
      if (execution_stack.empty()) {
        co_return "empty";
      }
      co_yield "(";
      for (auto &t : execution_stack) {
        co_yield transform(t.address());
        std::stringstream ss;
        ss << (bold | green) << ", ";
        co_yield ss.str();
      }
      co_return ")";
    };
#endif
    while (!tasks.empty() || !execution_stack.empty()) {
      while (!tasks.empty()) {
        auto task = tasks.front();
        tasks.pop();
        DEBUG(bold | green, "execution task is ", show_execution_stack());
        if (task.second != nullptr) {
          DEBUG(bold | green, "stacking task ", task.second.address());
          execution_stack.push_back(task.second);
        }
        DEBUG(bold | green, "launching task ", task.first.address());
        task.first.resume();
      }
      DEBUG(bold | green, "Done with launch round, execution task is ",
            show_execution_stack());
      if (!execution_stack.empty()) {
        auto task = execution_stack.back();
        execution_stack.pop_back();
        DEBUG(bold | green, "resuming task ", task.address());
        task.resume();
      }
    }
  }
};

struct thread_executor {
  using handle_type = std::coroutine_handle<>;
  using task_type = std::pair<handle_type, handle_type>;
  using task_queue = std::queue<task_type>;
  constexpr static inline size_t MAX_CONCURRENT_TASKS = 4;

  std::array<std::thread, MAX_CONCURRENT_TASKS> threads;

  task_queue tasks;

  std::counting_semaphore<MAX_CONCURRENT_TASKS> semaphore{MAX_CONCURRENT_TASKS};

  template <Resumable T> void schedule(T &&task) { tasks.push(task); }

  void execute() {
    while (!tasks.empty()) {
      semaphore.acquire();
      auto task = tasks.front();
      tasks.pop();
      for (auto &thread : threads) {
        if (!thread.joinable()) {
          thread = std::thread([task = std::move(task), this] {
            task.first.resume();
            semaphore.release();
          });
          break;
        }
      }
    }
  }
};

using used_executor = trivial_executor;

template <class T> using task = schedulable_task<T, used_executor>;

task<int> id(auto color, int value) {
  std::cout << color << "yielding " << value << reset << std::endl;
  co_return value;
}

task<int> hello_world(auto color) {
  std::cout << color << "Hello, World!\n" << reset;
  co_return co_await id(color, 1) + co_await id(color, 10);
}

task<void> whatever(auto color) {
  std::cout << color << "Calling hello_world" << reset << std::endl;
  int result = co_await hello_world(color);
  std::cout << color << "Calling id(100)" << reset << std::endl;
  int result2 = co_await id(color, 100);
  std::cout << color << "Result: " << result2 + result << reset << '\n';
}

int main() {
  used_executor executor;
  std::cout << reverse << green << "Starting 1" << reset << std::endl;
  whatever(reverse|blue).set_executor(&executor);
  std::cout << reverse << green << "Starting 2" << reset << std::endl;
  whatever(reverse|yellow).set_executor(&executor);
  std::cout << reverse << green << "Starting 3" << reset << std::endl;
  whatever(reverse|purple).set_executor(&executor);
  std::cout << reverse << green << "Executing" << reset << std::endl;
  executor.execute();
  std::cout << reverse << green << "Done" << reset << std::endl;
}
