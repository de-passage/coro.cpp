#ifndef HEADER_GUARD_DEBUG_HPP
#define HEADER_GUARD_DEBUG_HPP

#include "vt100.hpp"
#include <coroutine>
#include <mutex>
#include <utility>
using namespace dpsg::vt100;

#include <sstream>
#include <unordered_map>

struct string_generator {
  struct promise_type;
  using handle_type = std::coroutine_handle<promise_type>;
  struct promise_type {
    std::string value{};
    string_generator get_return_object() noexcept {
      return string_generator{handle_type::from_promise(*this)};
    }
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    std::suspend_always yield_value(std::string value) noexcept {
      this->value = value;
      return {};
    }
    std::suspend_always return_value(std::string value) noexcept {
      this->value = std::move(value);
      return {};
    }
    void unhandled_exception() noexcept {}
  };

  handle_type handle;
  explicit string_generator(handle_type handle) : handle(handle) {}
  string_generator(string_generator &&o)
      : handle(std::exchange(o.handle, nullptr)) {}
  string_generator(const string_generator &) = delete;
  string_generator &operator=(string_generator &&o) {
    handle = std::exchange(o.handle, nullptr);
    return *this;
  }
  string_generator &operator=(const string_generator &) = delete;
  ~string_generator() {
    if (handle) {
      handle.destroy();
    }
  }

  std::string next() {
    handle.resume();
    return std::move(handle.promise().value);
  }

  bool has_next() const { return !handle.done(); }
};

namespace detail {
template <class T>
concept Streamable = requires(T t, std::ostream &out) {
  { out << t } -> std::same_as<std::ostream &>;
};

template <class T>
concept Iterable = requires(T t) {
  t.begin();
  t.begin() == t.end();
  ++t.begin();
};
} // namespace detail

template <::detail::Streamable T> decltype(auto) transform(T &&t) {
  return std::forward<T>(t);
}

static std::unordered_map<void *, std::string> address_to_string;
static int counter = 1;

inline std::string transform(bool b) { return b ? "true" : "false"; }

template <class T>
  requires(!std::is_same_v<std::decay_t<T>, char>)
std::string transform(T *ptr) {
  if (ptr == nullptr) {
    std::stringstream ss;
    ss << (cyan | bold) << "(0)" << (reset | yellow) << "nullptr" << (reset);
    return ss.str();
  }
  if (address_to_string.contains((void *)ptr)) {
    return address_to_string[(void *)ptr];
  }
  std::stringstream ss;
  ss << (cyan | bold) << "(" << std::to_string(counter++) << ")"
     << (reset | yellow) << ptr << (reset);
  address_to_string[(void *)ptr] = ss.str();
  return address_to_string[(void *)ptr];
}

inline std::string transform(string_generator gen) {
  std::string result = "";
  while (gen.has_next()) {
    result += gen.next();
  }
  return result;
}

template <std::invocable T> auto transform(T &&t) { return transform(t()); }

std::string transform(::detail::Iterable auto const &map) {
  std::stringstream s;
  s << '{';
  for (auto it = map.begin(); it != map.end(); ++it) {
    if (it != map.begin()) {
      s << ", ";
    }
    s << transform(*it);
  }
  s << '}';
  return s.str();
}

template<class T, class U>
std::string transform(const std::pair<T, U>& p) {
  std::stringstream s;
  s << '<' << transform(p.first) << ", " << transform(p.second) << '>';
  return s.str();
}

namespace debug_ {
extern std::mutex mutex_;
}
template <class... Args> void print(Args &&...args) {
  std::unique_lock lock{debug_::mutex_};
  (std::cout << ... << args) << std::flush;
}

template <class... Args> void println(Args &&...args) {
  std::unique_lock lock{debug_::mutex_};
  (std::cout << ... << transform(std::forward<decltype(args)>(args)))
      << std::endl;
}

constexpr auto gray = setf(128, 128, 128);
constexpr auto light_gray = setf(200, 200, 200);
constexpr auto purple = setf(170, 50, 170);
constexpr auto orange = setf(220, 90, 10);

#ifndef NDEBUG
#define DEBUG(...) println(reset, __VA_ARGS__, reset)
#define DEBUG_ONLY(...) __VA_ARGS__
#else
#define DEBUG(...)
#define DEBUG_ONLY(...)
#endif

#endif // HEADER_GUARD_DEBUG_HPP
