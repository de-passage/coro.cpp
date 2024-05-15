#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stack>
#include <thread>
#include <cassert>
#include <vector>
#define DPSG_COMPILE_DEBUG_HPP
#include "common.hpp"
#include "debug.hpp"

#include <coroutine>

template <class T>
concept HandleLike = requires(T obj) {
  obj.resume();
  { obj.done() } -> std::same_as<bool>;
};

template <HandleLike H> struct basic_worker {
  using handle_type = H;
  using handle_container = std::stack<handle_type>;

private:
  static thread_local size_t tl_id_;
  size_t id_{0};

  mutable std::mutex mut_{};
  std::condition_variable cv_;
  handle_container handles_;
  std::jthread th_{};

  void run_(std::stop_token token, size_t id) {
    basic_worker::tl_id_ = id;
    DEBUG(orange, "Starting worker ", basic_worker::tl_id_);
    while (!token.stop_requested()) {
      std::unique_lock lock{mut_};
      while (!handles_.empty()) {
        handle_type h = handles_.top();
        handles_.pop();
        lock.unlock();
        assert(!h.done());
        h.resume();
        if (token.stop_requested()) {
          return;
        }
        lock.lock();
      }
      DEBUG(orange, "Worker ", basic_worker::tl_id_, " waiting");
      cv_.wait(lock);
      DEBUG(orange, "Worker ", basic_worker::tl_id_,
            " checking for wait condition");
    };
    DEBUG(orange, "Stopping worker ", basic_worker::tl_id_);
  }

public:
  static size_t this_worker() { return basic_worker::tl_id_; }

  basic_worker(size_t id) : id_{id} {
    th_ = std::jthread{[this, id](std::stop_token token) { run_(token, id); }};
  }

  basic_worker(const basic_worker &other) = delete;
  basic_worker(basic_worker &&other) noexcept = delete;
  basic_worker &operator=(const basic_worker &other) = delete;
  basic_worker &operator=(basic_worker &&other) noexcept = delete;

  void run(std::stop_token token) { run_(token, 0); }

  void push(handle_type h) {
    DEBUG(orange, "Pushing on stack of worker ", this->id_);
    std::unique_lock lock{mut_};
    handles_.push(h);
    DEBUG("Notifying worker ", this->id_);
    cv_.notify_one();
  }

  void stop() {
    DEBUG(orange, "Requiring worker ", this->id_, " to stop");
    std::unique_lock lock{mut_};
    handles_ = handle_container{};
    DEBUG(orange, "Notifying worker ", this->id_);
    th_.request_stop();
    cv_.notify_one();
  }

  bool stopped() {
    return !th_.joinable();
  }

  bool empty() const {
  std::unique_lock lock{mut_};
    return handles_.size() == 0;
  }
};
template <HandleLike H> thread_local size_t basic_worker<H>::tl_id_{0};

using worker = basic_worker<std::coroutine_handle<>>;

void print_w(auto &&...params) {
  static auto color = []() -> std::unordered_map<int, color_termcode> {
    return {{0, red}, {1, green}, {2, yellow}, {3, cyan}, {4, magenta}};
  }();
  println(bold | color[worker::this_worker()] | dpsg::vt100::reverse,
          "From worker ", worker::this_worker(), ':', reset, ' ', FWD(params)...);
}
template<HandleLike H> struct basic_scheduler {
  using worker_id = std::size_t;
  using handle_t = H;
  virtual ~basic_scheduler() = 0;
  virtual void schedule(size_t n, handle_t h) = 0;
};

template<HandleLike H> basic_scheduler<H>::~basic_scheduler() = default;

template <HandleLike H> struct basic_scheduler_impl : basic_scheduler<H> {
  using typename basic_scheduler<H>::worker_id;
  using typename basic_scheduler<H>::handle_t;

  using ptr = std::unique_ptr<basic_worker<handle_t>>;

  struct resume_source {
    basic_scheduler_impl *scheduler;
    handle_t handle;
    worker_id worker;

    void resume() { scheduler->schedule(worker, handle); }
  };

  std::vector<ptr> workers_;
  basic_scheduler_impl(size_t n) : workers_{} {
    workers_.reserve(n);
    for (size_t i = 1; i <= n; ++i) {
      workers_.emplace_back(std::make_unique<typename ptr::element_type>(i));
    }
  }

   basic_scheduler_impl(const basic_scheduler_impl& other) = delete;
   basic_scheduler_impl(basic_scheduler_impl&& other) noexcept = delete;
   basic_scheduler_impl& operator=(const basic_scheduler_impl& other) = delete;
   basic_scheduler_impl& operator=(basic_scheduler_impl&& other) noexcept = delete;

  void schedule(size_t worker, handle_t to_resume) override {
    DEBUG_ONLY(auto c = cyan | ::dpsg::vt100::reverse);
    DEBUG(c, "Scheduling ", reset, to_resume.address(), c, " on worker ", worker);
    assert(worker <= workers_.size());
    assert(workers_[worker - 1] != nullptr);
    workers_[worker - 1]->push(to_resume);
  }

  ~basic_scheduler_impl() {
    for (auto &worker : workers_) {
      worker->stop();
    }
    while (!std::ranges::all_of(workers_, [](auto& w)  { return w->stopped(); })) { /* busy loop */ }
  }

  bool empty() {
    for (auto& worker : workers_) {
      if (!worker->empty()) {
        return false;
      }
    }
    return true;
  }
};
using scheduler = basic_scheduler<std::coroutine_handle<>>;
using threadpool = basic_scheduler_impl<std::coroutine_handle<>>;

namespace detail {
struct resumable_impl_ {
  void set_scheduler(scheduler *sc) { this->scheduler_ = sc; }
  void resume_on(size_t worker_id) {
    assert(this->scheduler_ != nullptr);
    this->scheduler_->schedule(
        worker_id, std::coroutine_handle<resumable_impl_>::from_promise(*this));
  }

  void next(size_t worker_id, std::coroutine_handle<> next) {
    this->next_.handle = next;
    this->next_.worker_id = worker_id;
  }

protected:
  struct next_info_t {
    std::coroutine_handle<> handle{nullptr};
    size_t worker_id{0};
  } next_;

  scheduler *scheduler_{nullptr};
  void resume_() {
    if (this->scheduler_ != nullptr && next_.handle != nullptr) {
      this->scheduler_->schedule(next_.worker_id, next_.handle);
    }
  }
};

template <class T> struct return_impl_ : resumable_impl_ {
  T value;

  void return_value(std::convertible_to<T> auto &&v) {
    value = FWD(v);
    this->resume_();
  }
};

template <> struct return_impl_<void> : resumable_impl_ {
  void return_void() { this->resume_(); }
};
template <class T>
concept SchedulingAwaitable =
    requires(T obj, scheduler *sc) { obj.set_scheduler(sc); };
} // namespace detail

template <class T> struct chainable_promise : ::detail::return_impl_<T> {
  using return_type = T;

  std::suspend_always initial_suspend() noexcept { return {}; }
  std::suspend_always final_suspend() noexcept { return {}; }
  auto get_return_object() noexcept {
    return std::coroutine_handle<chainable_promise>::from_promise(*this);
  }

  void unhandled_exception() noexcept { std::terminate(); }

  decltype(auto) await_transform(::detail::SchedulingAwaitable auto &&sa) {
    sa.set_scheduler(this->scheduler_);
    return FWD(sa);
  }
};

namespace detail {

template <class T> struct task_ret_base_impl_ {
  using result_type = T;
  using promise_type = chainable_promise<result_type>;
  using handle_type = std::coroutine_handle<promise_type>;

  task_ret_base_impl_(handle_type h) noexcept : handle_{h} {}
  task_ret_base_impl_(const task_ret_base_impl_ &other) = delete;
  task_ret_base_impl_ &operator=(const task_ret_base_impl_ &other) = delete;
  task_ret_base_impl_(task_ret_base_impl_ &&other) noexcept
      : handle_{std::exchange(other.handle_, nullptr)}, name_{std::move(other.name_)} {}

  task_ret_base_impl_ &operator=(task_ret_base_impl_ &&other) noexcept {
    handle_ = std::exchange(other.handle_, nullptr);
    name_ = std::move(other.name_);
    return *this;
  }

protected:
  handle_type handle_;
  std::string name_{"<anonymous>"};
};

template <class T> struct task_ret_impl_ : task_ret_base_impl_<T> {
  T &&result() && {
    return const_cast<task_ret_impl_<T>*>(this)->template get_<T&&>();
  }
  const T &&result() const && {
    return const_cast<task_ret_impl_<T>*>(this)->template get_<const T&&>();
  }
  T &result() & {
    return const_cast<task_ret_impl_<T>*>(this)->template get_<T&>();
  }
  const T &result() const & {
    return const_cast<task_ret_impl_<T>*>(this)->template get_<const T&>();
  }
private:
  template<class U>
  decltype(auto) get_() {
    assert(this->handle_ != nullptr);
    assert(this->handle_.done());
    return static_cast<U>(this->handle_.promise().value);
  }
};

template <> struct task_ret_impl_<void> : task_ret_base_impl_<void> {};
} // namespace detail

template <class T> struct task : ::detail::task_ret_impl_<T> {
  using base = ::detail::task_ret_impl_<T>;
  using typename base::handle_type;

  explicit(false) task(handle_type p) : base{p} {}
  task(const task &other) = delete;
  task(task &&other) = default;
  task &operator=(const task &other) = delete;
  task &operator=(task &&other) noexcept = default;

  void scheduler(scheduler *sc) { this->handle_.promise().set_scheduler(sc); }

  task &&name(std::string name) && {
    this->name_ = std::move(name);
    return std::move(*this);
  }
  task &name(std::string name) & {
    this->name_ = std::move(name);
    return *this;
  }
  const std::string &name() const { return this->name_; }

  void then(size_t worker_id, std::coroutine_handle<> to_resume) {
    this->handle_.promise().next(worker_id, to_resume);
  }

  void resume_on(size_t worker_id) {
    this->handle_.promise().resume_on(worker_id);
  }

  bool start() {
    assert(this->handle_ != nullptr);
    assert(!this->handle_.done());
    this->handle_.resume();
    return this->handle_.done();
  }

  bool done() {
    return this->handle_.done();
  }

  handle_type handle() const { return this->handle_; }
};

template <class T>
  requires(!template_instance_of<T, task>)
struct run_on_worker_t {
  size_t worker_id;
  struct task<T> task;

  auto await_ready() {
    return false;
  }

  void await_suspend(std::coroutine_handle<> to_resume) {
    task.then(worker::this_worker(), to_resume);
    task.resume_on(worker_id);
  }

  decltype(auto) await_resume() noexcept {
    if constexpr (!std::is_same_v<T, void>) {
      return std::move(this->task.result());
    }
  }

  void set_scheduler(scheduler *sc) { this->task.scheduler(sc); }
};

template <class T>
  requires(!template_instance_of<T, task>)
run_on_worker_t<T> on_worker(size_t worker_id, task<T> tsk) {
  return run_on_worker_t<T>{
      .worker_id = worker_id,
      .task = std::move(tsk),
  };
}

template <class F>
  requires std::invocable<F> &&
           template_instance_of<std::invoke_result_t<F>, ::task>
run_on_worker_t<typename std::invoke_result_t<F>::result_type>
on_worker(size_t worker_id, F &&f) {
  return on_worker(worker_id, FWD(f)());
}


task<std::string> bar() {
  print_w(green | bold, "BAR(start)");
  co_await on_worker(2, []() -> task<void> {
    print_w(yellow | bold, "BAR.lambda.1");
    co_await on_worker(4, []() -> task<void> {
      print_w(orange | bold, "BAR.lambda.1.nested.1");
      co_return;
    });
    co_await on_worker(1, []() -> task<void> {
      print_w(orange | bold, "BAR.lambda.1.nested.2");
      co_return;
    });
  });
  print_w(green|bold, "BAR(middle)");
  co_await on_worker(2, []() -> task<void> {
    print_w(yellow | bold, "BAR.lambda.2");
    co_await on_worker(3, []() -> task<void> {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      print_w(orange | bold, "BAR.lambda.2.nested.1");
      co_return;
    });
    co_await on_worker(1, []() -> task<void> {
      print_w(orange | bold, "BAR.lambda.2.nested.2");
      co_return;
    });
  });
  print_w(green|bold, "BAR(end)");
  co_return "Hello coroutine!";
}

task<void> foo(std::atomic<int>& l) {
  print_w(bold | purple, "FOO(start)");
  int r = co_await on_worker(2, []() -> task<int> {
    using namespace std::chrono_literals;
    print_w(bold | cyan, "Running lambda");
    co_return 42;
  });
  print_w(bold | purple, "Got ", r);
  print_w(bold | purple, "Baring ", co_await on_worker(1, bar().name("bar")));
  print_w(bold | purple, "FOO(end)");
  l++;
}

void add_to_sc(scheduler& scheduler, task<void>& t, int n, const std::string& name) {
  DEBUG(red|bold, "Building: ", name);
  t.scheduler(&scheduler);
  t.name(name);
  scheduler.schedule(n, t.handle());
}


struct immediate : scheduler {
  immediate(int /*discard*/) {}

  void schedule(size_t /*unused*/, std::coroutine_handle<> h) override {
    h.resume();
  }
};

int main() {
  std::atomic<int> l{};
  auto i = foo(l).name("main");
  auto j = foo(l).name("secondary");
  immediate scheduler{4};
  i.scheduler(&scheduler);
  j.scheduler(&scheduler);
  scheduler.schedule(1, i.handle());
  scheduler.schedule(3, j.handle());
  std::vector<task<void>> tasks;
  for (int i = 0; i < 10; ++i) {
    tasks.emplace_back(foo(l));
    add_to_sc(scheduler, tasks.back(), (i % 4) + 1, "secondary." + std::to_string(i));
  }
  using namespace std::chrono_literals;

  while (l.load() != 12) {}
  println(magenta, "Ending");
}
