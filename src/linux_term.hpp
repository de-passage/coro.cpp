#pragma once

#include "types.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <coroutine>
#include <csignal>
#include <cstring>
#include <format>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace dpsg {

struct errno_exception : std::runtime_error {
  errno_exception() : std::runtime_error(strerror(errno)) {}

  int code{errno};
};

struct unfinished_sequence : std::runtime_error {
  template <typename T>
    requires std::is_constructible_v<std::runtime_error, T>
  unfinished_sequence(T &&param) : std::runtime_error(std::forward<T>(param)) {}
};

struct unfinished_numeric_sequence : unfinished_sequence {
  unfinished_numeric_sequence(const u16 *beg, const u16 *end, char er)
      : unfinished_sequence(
            std::format("Unfinished numeric sequence in terminal control "
                        "output (terminate with '{}')",
                        er)),
        size{end - beg}, numeric_values{(u16 *)malloc(sizeof(*beg) * size)},
        error_character{er} {
    memcpy(numeric_values, beg, size * sizeof(*beg));
  }
  ~unfinished_numeric_sequence() override { free(numeric_values); }

  unfinished_numeric_sequence(const unfinished_numeric_sequence &) = delete;
  unfinished_numeric_sequence(unfinished_numeric_sequence &&) noexcept = delete;
  unfinished_numeric_sequence &
  operator=(const unfinished_numeric_sequence &) = delete;
  unfinished_numeric_sequence &
  operator=(unfinished_numeric_sequence &&) noexcept = delete;

  ptrdiff_t size;
  u16 *numeric_values;
  char error_character;
};

struct invalid_sequence_start : unfinished_sequence {
  invalid_sequence_start(char c)
      : unfinished_sequence(std::format(
            "Invalid sequence start '{}' in terminal control output", c)) {}
};

extern "C" {
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
}

inline void raw_mode_enable(struct termios *ctx, int new_mode) {
  tcgetattr(STDIN_FILENO, ctx);
  struct termios raw = *ctx;
  raw.c_lflag &= ~(new_mode);
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

inline void raw_mode_disable(struct termios *ctx) {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, ctx);
}

// XTERM Mouse codes
// 1002 SET_BTN_EVENT_MOUSE // Buttons only, no mouse position tracking
// 1003 SET_ANY_EVENT_MOUSE // Limited to 255-32 positions, unsuited to larger
// terminals 1005 SET_EXT_MODE_MOUSE // UTF-8 encoding scheme 1006
// SET_SGR_EXT_MODE_MOUSE // Same as default mode but positions are encoded in
// ASCII, allowing for arbitrary positions

inline void enable_mouse_tracking() {
  write(STDOUT_FILENO, "\033[?1003h", 8); // enable SET_ANY_EVENT_MOUSE
  write(STDOUT_FILENO, "\033[?1006h", 8); // enable SET_SGR_EXT_MODE_MOUSE
  fsync(STDOUT_FILENO);
}

inline void disable_mouse_tracking() {
  write(STDOUT_FILENO, "\033[?1006l", 8); // disable SET_SGR_EXT_MODE_MOUSE
  write(STDOUT_FILENO, "\033[?1003l", 8); // disable SET_ANY_EVENT_MOUSE
  fsync(STDOUT_FILENO);
}

struct term_position {
  union {
    struct {
      u16 col;
      u16 row;
    };
    struct {
      u16 x;
      u16 y;
    };
  };
};
struct terminal_size {
  int col;
  int row;
};

struct event_key {
  enum class funckey_modifiers : u8 {
    Shift = 2,
    Alt = 3,
    Shift_Alt = 4,
    Control = 5,
    Shift_Control = 6,
    Alt_Control = 7,
    Shift_Alt_Control = 8
  };

  enum class modifiers : u8 {
    None = 0,
    Shift = 4,
    Alt = 8,
    Ctrl = 16,
    Special = 64,
    Key_Marker = 1 << 7,
  };

  explicit constexpr event_key() = default;
  explicit constexpr event_key(
      char value,
      event_key::modifiers mods = event_key::modifiers::None) noexcept
      : code{(u8)value}, mods{mods | event_key::modifiers::Key_Marker} {}

  u8 code;
  u8 cont[3]{};
  u8 _padding[3]{};
  modifiers mods;

  friend constexpr modifiers operator&(modifiers left,
                                       modifiers right) noexcept {
    return static_cast<modifiers>(static_cast<u8>(left) &
                                  static_cast<u8>(right));
  }
  friend constexpr modifiers operator|(modifiers left,
                                       modifiers right) noexcept {
    return static_cast<modifiers>(static_cast<u8>(left) |
                                  static_cast<u8>(right));
  }
  friend constexpr event_key operator|(event_key left,
                                       modifiers right) noexcept {
    left.mods = left.mods | right;
    return left;
  }
};

struct event_mouse {
  enum class buttons {
    Left = 0,
    Middle = 1,
    Right = 2,
    Move = 3,
    WheelUp = 65,
    WheelDown = 66,
  };
  enum class modifiers : u8 {
    None = 0,
    Button1 = 0,
    Button2 = 1,
    Button3 = 2,
    Unused = 3, // Extended mode doesn't use this
    Shift = 4,
    Alt = 8,
    Ctrl = 16,
    Release = 32,
    Move = 35, // Release + Release (Unused) == drag
    WheelUp = 64,
    WheelDown = 65,
    Key_Marker = 1 << 7,
  };

  explicit constexpr event_mouse() = default;
  explicit constexpr event_mouse(modifiers magic, term_position pos)
      : x{pos.col}, y{pos.row}, mods(magic) {}
  explicit constexpr event_mouse(buttons button, term_position pos)
      : x{pos.col}, y{pos.row}, mods((modifiers)button) {}

  u16 x;
  u16 y;
  u8 _padding[3];
  modifiers mods;

  [[nodiscard]] constexpr buttons button() const { return (buttons)(mods ^ modifiers::Release); }

  friend constexpr modifiers operator&(modifiers left,
                                       modifiers right) noexcept {
    return static_cast<modifiers>(static_cast<u8>(left) &
                                  static_cast<u8>(right));
  }
  friend constexpr modifiers operator|(modifiers left,
                                       modifiers right) noexcept {
    return static_cast<modifiers>(static_cast<u8>(left) |
                                  static_cast<u8>(right));
  }
  friend constexpr modifiers operator^(modifiers left,
                                       modifiers right) noexcept {
    return static_cast<modifiers>(static_cast<u8>(left) ^
                                  static_cast<u8>(right));
  }
};

struct event {

  explicit constexpr event() : _cheat_{0} {}
  constexpr event(event_key key) : key{key} {}
  constexpr event(event_mouse mouse) : mouse{mouse} {}
  union {
    event_key key;
    event_mouse mouse;
    struct {
      char data[7];
      u8 mods;
    } raw;
    u64 _cheat_;
  };

  constexpr static inline u8 MASK_TYPE_BIT = 0x80;
  constexpr static inline u8 MODS_INDEX = 7;

  [[nodiscard]] bool is_key_event() const {
    return ((u8)event_mouse::modifiers::Key_Marker & raw.mods) != 0;
  }

  [[nodiscard]] bool is_mouse_event() const { return !is_key_event(); }

  [[nodiscard]] bool alt_pressed() const {
    return ((u8)event_mouse::modifiers::Alt & raw.mods) != 0;
  }

  [[nodiscard]] bool ctrl_pressed() const {
    return ((u8)event_mouse::modifiers::Ctrl & raw.mods) != 0;
  }

  [[nodiscard]] bool shift_pressed() const {
    return ((u8)event_mouse::modifiers::Shift & raw.mods) != 0;
  }

  friend constexpr bool operator==(event left, event right) {
    if (std::is_constant_evaluated()) {
      if (left.is_key_event()) {
        return right.is_key_event() && left.key.code == right.key.code &&
               left.key.mods == right.key.mods;
      }
      return false;
    }
    return left._cheat_ == right._cheat_;
  }
  constexpr static inline event_key arrow_up{'A',
                                             event_key::modifiers::Special};
  constexpr static inline event_key arrow_down{
      event_key{'B', event_key::modifiers::Special}};
  constexpr static inline event_key arrow_right{
      event_key{'C', event_key::modifiers::Special}};
  constexpr static inline event_key arrow_left{
      event_key{'D', event_key::modifiers::Special}};
};
static_assert(sizeof(event) == 8);

namespace detail {

constexpr static inline std::initializer_list<int> HANDLED_SIGNALS = {
    SIGINT, SIGSEGV, SIGTERM, SIGILL, SIGFPE, SIGABRT};
constexpr static inline auto MAX_SIGNAL =
    HANDLED_SIGNALS.size() + 2; // SIGCONT & SIGTSTP
constexpr static inline auto INDEX_HANDLER_SIGTSTP = MAX_SIGNAL - 1;
constexpr static inline auto INDEX_HANDLER_SIGCONT = INDEX_HANDLER_SIGTSTP - 1;
constexpr size_t index_of(int signal) {
  size_t i = 0;
  for (auto sig : HANDLED_SIGNALS) {
    if (sig == signal) {
      return i;
    }
    ++i;
  }
  return -1; // Just don't let it happen
}
extern struct sigaction new_sa[MAX_SIGNAL], old_sa[MAX_SIGNAL];
extern struct termios orig_termios;
extern bool require_mouse;
} // namespace detail

template <int Mode> struct raw_mode_context_basic {

  raw_mode_context_basic() noexcept {
    raw_mode_enable(&detail::orig_termios, Mode);
    register_signal_handlers();
  }

private:
  static void set_handler(int signal, void (*func)(int), size_t index) {
    memset(&detail::new_sa[index], 0, sizeof(detail::new_sa[index]));
    detail::new_sa[index].sa_handler = func;
    sigaction(signal, &detail::new_sa[index], &detail::old_sa[index]);
  }

  static void restore_old_and_raise(int sig, size_t idx) {
    sigaction(sig, &detail::old_sa[idx], nullptr);
    raise(sig);
  }

  void register_signal_handlers() {
    for (int signal : detail::HANDLED_SIGNALS) {
      set_handler(signal, &raw_mode_context_basic::handle_fatal_signal,
                  detail::index_of(signal));
    }
    set_handler(SIGTSTP, &raw_mode_context_basic::handle_sigtstp,
                detail::INDEX_HANDLER_SIGTSTP);
  }

  // Called on interuption from the outside (Ctrl-Z)
  static void handle_sigtstp(int sig) {
    set_handler(SIGCONT, &raw_mode_context_basic::handle_sigcont,
                detail::INDEX_HANDLER_SIGCONT);
    raw_mode_disable(&detail::orig_termios);
    if (detail::require_mouse) {
      ::dpsg::disable_mouse_tracking();
    }
    restore_old_and_raise(sig, detail::INDEX_HANDLER_SIGCONT);
  }

  // Called on continue from the outside (fg/bg)
  static void handle_sigcont(int sig) {
    set_handler(SIGTSTP, &raw_mode_context_basic::handle_sigtstp,
                detail::INDEX_HANDLER_SIGTSTP);
    raw_mode_enable(&detail::orig_termios, Mode);
    if (detail::require_mouse) {
      ::dpsg::enable_mouse_tracking();
    }
    restore_old_and_raise(sig, detail::INDEX_HANDLER_SIGTSTP);
  }

  static void handle_fatal_signal(int sig) {
    raw_mode_disable(&detail::orig_termios);
    if (detail::require_mouse) {
      ::dpsg::disable_mouse_tracking();
    }

    restore_old_and_raise(sig, detail::index_of(sig));
  }

public:
  raw_mode_context_basic(const raw_mode_context_basic &) = delete;
  raw_mode_context_basic &operator=(const raw_mode_context_basic &) = delete;
  raw_mode_context_basic(raw_mode_context_basic &&) = delete;
  raw_mode_context_basic &operator=(raw_mode_context_basic &&) = delete;

  ~raw_mode_context_basic() noexcept {
    raw_mode_disable(&detail::orig_termios);
  }

  struct enable_mouse_t {
    enable_mouse_t() noexcept {
      ::dpsg::enable_mouse_tracking();
      detail::require_mouse = true;
    }
    ~enable_mouse_t() noexcept {
      ::dpsg::disable_mouse_tracking();
      detail::require_mouse = false;
    }
    enable_mouse_t(const enable_mouse_t &) noexcept = delete;
    enable_mouse_t(enable_mouse_t &&) noexcept = delete;
    const enable_mouse_t &operator=(const enable_mouse_t &) noexcept = delete;
    const enable_mouse_t &operator=(enable_mouse_t &&) noexcept = delete;
  };

  [[nodiscard]] enable_mouse_t enable_mouse_tracking() { return {}; }

  [[nodiscard]] struct term_position cursor_position() const {
    (void)this; // This is intentionally not static
    constexpr static term_position invalid_pos = {(u16)0xFFFFFFFF,
                                                  (u16)0xFFFFFFFF};
    struct term_position p = {.col = 0, .row = 0};
    char buf[32];
    ssize_t i = 0; // Number of known characters in the buffer
    write(STDOUT_FILENO, "\033[6n", 4);

    enum class state : char {
      start,
      esc,
      bracket,
      semicolon,
      end
    } state = state::start;
    for (;;) {
      const auto e = read(STDIN_FILENO, buf + i, sizeof(buf) - i);
      if (e < 0) {
        // There was an error
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          // But we can recover
          continue;
        } // We can't recover, return an invalid position
        p = invalid_pos;
        goto abort;

      } else if (e == 0) [[unlikely]] {
        // Not enough characters to form a valid response
        p = invalid_pos;
        goto abort;
      } else [[likely]] {
        // We need to extract the response from the buffer
        // The response is of the form \033[<row>;<col>R
        char *const end = buf + i + e;
        char *begin = buf + i;
        if (state == state::start) [[likely]] {
          begin = std::find(begin, end, '\033');
          if (begin == end) {
            // No response yet
            continue;
          }
          state = state::esc;
          if (++begin == end) [[unlikely]] {
            continue;
          }
        }

        // consume the bracket, if not present, return an invalid position
        if (state == state::esc) [[likely]] {
          if (*begin != '[') [[unlikely]] {
            // Not a valid response
            p = invalid_pos;
            goto abort;
          }
          state = state::bracket;
          if (++begin == end) [[unlikely]] {
            continue;
          }
        }

        // consume the row, if not present, return an invalid position
        if (state == state::bracket) [[likely]] {
          for (; begin != end; ++begin) {
            if (isdigit(*begin) != 0) {
              p.row = p.row * 10 + (*begin - '0');
            } else if (*begin == ';') {
              begin++;
              state = state::semicolon;
              break;
            } else [[unlikely]] {
              // Not a valid response
              p = invalid_pos;
              goto abort;
            }
          }
        }

        // consume the column, if not present, return an invalid position
        if (state == state::semicolon) [[likely]] {
          for (; begin != end; ++begin) {
            if (isdigit(*begin) != 0) {
              p.col = p.col * 10 + (*begin - '0');
            } else if (*begin == 'R') {
              state = state::end;
              goto abort;
            } else [[unlikely]] {
              // Not a valid response
              p = invalid_pos;
              goto abort;
            }
          }
        }
      }
    }
  abort:
    return p;
  }


  template<class T>
  struct input_generator {
    struct promise_type;
    using handle_t = std::coroutine_handle<promise_type>;

    input_generator(handle_t handle) : handle_{handle} {}
    input_generator(const input_generator &) = delete;
    input_generator &operator=(const input_generator &) = delete;
    input_generator(input_generator &&other) noexcept
        : handle_{std::exchange(other.handle_, nullptr)} {}
    input_generator &operator=(input_generator &&other) noexcept {
      handle_ = std::exchange(other.handle_, nullptr);
      return *this;
    }
    ~input_generator() {
      if (handle_) {
        handle_.destroy();
      }
    }

    T operator()() {
      assert(handle_ && "Generator was likely moved from");
      assert(!handle_.done() && "Generator is done");
      handle_.resume();
      return handle_.promise().value;
    }

    operator bool() noexcept {
      return handle_ && !handle_.done();
    }

    struct promise_type {
      T value;
      std::exception_ptr except_;
      auto initial_suspend() noexcept { return std::suspend_always{}; }
      auto final_suspend() noexcept { return std::suspend_always{}; }
      auto get_return_object() noexcept {
        return input_generator{handle_t::from_promise(*this)};
      }
      std::suspend_always yield_value(T v) noexcept { value = v; return {}; }
      void return_void() noexcept {}
      void unhandled_exception() noexcept { except_ = std::current_exception(); }
    };


    private:
    handle_t handle_;
  };

  template <size_t BufSize = 32, int Timeout = 0>
  input_generator<char> input_stream() {
    (void)this;
    pollfd fds;
    fds.fd = STDIN_FILENO;
    fds.events = POLLIN;

    for (;;) {
      auto poll_result = poll(&fds, 1, Timeout);
      if (poll_result == -1) {
        if (errno == EINTR) {
          continue;
        }
        throw errno_exception{};
      }

      if (poll_result == 0) {
        continue;
      }

      int last = 0;
      int current = 0;
      char buffer[BufSize];
      last = read(STDIN_FILENO, buffer, sizeof(buffer));
      if (last == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        continue;
      }

      current = 0;
      while (current < last) {
        char c = buffer[current++];
        co_yield (char) c;
      }
    }

    throw errno_exception{};
  }

private:
  constexpr static inline u8 UPPER_BOUND_CTRL_CHARACTERS =
      32; // 32 first values represent ctrl+<char>. 0 is ctrl+` for some reason

  static void from_character(char c, event_key::modifiers mod, event &out) {
    if (c < UPPER_BOUND_CTRL_CHARACTERS) { // CTRL+<char> is sent as (<char> -
                                           // 'A' + 1)
      out = event{
          event_key{(char)(c + 'a' - 1), mod | event_key::modifiers::Ctrl}};
      //} else if ((c & 0b11000000) != 0) { // unicode continuation
      // TODO : implement unicode characters
      // out = event{event::build_key, (char)c, mod};
    } else {
      out = event{event_key{(char)c, mod}};
    }
  };

  static void parse_mouse(const u16 *numbers, event_mouse::modifiers mods,
                          event &ev) {
    auto magic = (event_mouse::modifiers)numbers[0];
    auto x = numbers[1];
    auto y = numbers[2];
    ev = event_mouse{mods | magic, {.x = x, .y = y}};
  }

  static event parse_function_key(char c, event base, u16 modifiers) {
    base.key.code = c;
    switch ((event_key::funckey_modifiers)modifiers) {
    case event_key::funckey_modifiers::Alt:
      base.key.mods = base.key.mods | event_key::modifiers::Alt;
      break;
    case event_key::funckey_modifiers::Shift:
      base.key.mods = base.key.mods | event_key::modifiers::Shift;
      break;
    case event_key::funckey_modifiers::Shift_Alt:
      base.key.mods = base.key.mods | event_key::modifiers::Shift;
      base.key.mods = base.key.mods | event_key::modifiers::Alt;
      break;
    case event_key::funckey_modifiers::Control:
      base.key.mods = base.key.mods | event_key::modifiers::Ctrl;
      break;
    case event_key::funckey_modifiers::Shift_Control:
      base.key.mods = base.key.mods | event_key::modifiers::Shift;
      base.key.mods = base.key.mods | event_key::modifiers::Ctrl;
      break;
    case event_key::funckey_modifiers::Alt_Control:
      base.key.mods = base.key.mods | event_key::modifiers::Alt;
      base.key.mods = base.key.mods | event_key::modifiers::Ctrl;
      break;
    case event_key::funckey_modifiers::Shift_Alt_Control:
      base.key.mods = base.key.mods | event_key::modifiers::Alt;
      base.key.mods = base.key.mods | event_key::modifiers::Ctrl;
      base.key.mods = base.key.mods | event_key::modifiers::Shift;
      break;
    }
    return base;
  }

public:
  template <size_t BufSize = 32, int Timeout = 0>
  input_generator<event> event_stream() {
    (void)this;
    pollfd fds;
    fds.fd = STDIN_FILENO;
    fds.events = POLLIN;

    enum class parse_state {
      expecting_first,
      expecting_control_character,
      expecting_control_sequence,
      parsing_number
    };

    for (;;) {
      auto poll_result = poll(&fds, 1, Timeout);
      if (poll_result == -1) {
        if (errno == EINTR) {
          continue;
        }
        throw errno_exception{};
      }

      if (poll_result == 0) {
        continue;
      }

      int last = 0;
      char buffer[BufSize];
      last = read(STDIN_FILENO, buffer, sizeof(buffer));
      if (last == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        continue;
      }

      parse_state state{parse_state::expecting_first};
      int current = 0;
      char control_character = 0;
      u16 num_parameters[4] = {0};
      u16 *current_param = num_parameters;
      event result;
      const auto reset = [&] {
        state = parse_state::expecting_first;
        do {
          *current_param = 0;
        } while ((current_param != num_parameters) && (current_param--, true));
        result = event{};
      };
      while (current < last) {
        char c = buffer[current++];

        switch (state) {
        case parse_state::expecting_first: {
          if (c == '\033') { // Control character
            state = parse_state::expecting_control_character;
          } else {
            from_character(c, event_key::modifiers::None, result);
            co_yield result;
            reset();
          }
          break;
        }
        case parse_state::expecting_control_character: {
          if (c == '[') { // Control !
            state = parse_state::expecting_control_sequence;
            control_character = c;
          } else {
            from_character(c, event_key::modifiers::Alt, result);
            co_yield result;
            reset();
          }
          break;
        }
        case parse_state::expecting_control_sequence: {
          if (isdigit(c)) {
            state = parse_state::parsing_number;
            *current_param = c - '0';
          } else {
            switch (c) {
            case '<': { // mouse sequence
              state = parse_state::parsing_number;
              break;
            }
            case 'A': {
              co_yield event::arrow_up;
              reset();
              break;
            }
            case 'B': {
              co_yield event::arrow_down;
              reset();
              break;
            }
            case 'C': {
              co_yield event::arrow_right;
              reset();
              break;
            }
            case 'D': {
              co_yield event::arrow_left;
              reset();
              break;
            }
            default: {
              throw invalid_sequence_start(c);
            }
            }
          }
          break;
        }
        case parse_state::parsing_number: {
          if (isdigit(c)) {
            *current_param = (*current_param * 10) + (c - '0');
          } else {
            switch (c) {
            case ';': {
              current_param++;
              assert(current_param < num_parameters + 4 &&
                     "More than 4 numeric characters in terminal control "
                     "sequence!");
              break;
            }
            case 'm': {
              assert(current_param == num_parameters + 2 &&
                     "Mouse events require exactly 3 values");
              parse_mouse(num_parameters, event_mouse::modifiers::Release,
                          result);
              co_yield result;
              reset();
              break;
            }
            case 'M': {
              assert(current_param == num_parameters + 2 &&
                     "Mouse events require exactly 3 values");
              parse_mouse(num_parameters, event_mouse::modifiers::None, result);
              co_yield result;
              reset();
              break;
            }
            case 'A': {
              assert(num_parameters[0] == 1 &&
                     current_param == num_parameters + 1 &&
                     "Unknown sequence for arrow key!");
              result =
                  parse_function_key(c, event::arrow_up, num_parameters[1]);
              co_yield result;
              reset();
              break;
            }
            case 'B': {
              assert(num_parameters[0] == 1 &&
                     current_param == num_parameters + 1 &&
                     "Unknown sequence for arrow key!");
              result =
                  parse_function_key(c, event::arrow_down, num_parameters[1]);
              co_yield result;
              reset();
              break;
            }
            case 'C': {
              assert(num_parameters[0] == 1 &&
                     current_param == num_parameters + 1 &&
                     "Unknown sequence for arrow key!");
              result =
                  parse_function_key(c, event::arrow_right, num_parameters[1]);
              co_yield result;
              reset();
              break;
            }
            case 'D': {
              assert(num_parameters[0] == 1 &&
                     current_param == num_parameters + 1 &&
                     "Unknown sequence for arrow key!");
              result =
                  parse_function_key(c, event::arrow_left, num_parameters[1]);
              co_yield result;
              reset();
              break;
            }
            default: {
              throw unfinished_numeric_sequence(num_parameters,
                                                current_param + 1, c);
            }
            }
            break;
          }
        }
        }
      }
      if (state ==
          parse_state::expecting_control_sequence) { // The control char
                                                     // is actually an
                                                     // Alt+<ctrl char>
        from_character(control_character, event_key::modifiers::Alt, result);
        co_yield result;
      }
    }

    throw errno_exception{};
  }
};

using raw_mode_context = raw_mode_context_basic<ISIG | ECHO | ICANON>;
using cbreak_mode_context = raw_mode_context_basic<ECHO | ICANON>;

#ifdef DPSG_COMPILE_LINUX_TERM
struct termios detail::orig_termios {};
bool detail::require_mouse{};
struct sigaction detail::new_sa[MAX_SIGNAL]{}, detail::old_sa[MAX_SIGNAL]{};
#endif

template <std::invocable<raw_mode_context &> F>
std::invoke_result_t<F, raw_mode_context &> with_raw_mode(F &&f) {
  raw_mode_context ctx;
  return f(ctx);
}

template <std::invocable<cbreak_mode_context &> F>
std::invoke_result_t<F, cbreak_mode_context &> with_raw_mode(F &&f) {
  cbreak_mode_context ctx;
  return f(ctx);
}

inline terminal_size get_terminal_size() {
  struct winsize w;
  char *col = getenv("COLUMNS");
  char *row = getenv("LINES");
  if (col != nullptr && row != nullptr) {
    return {std::atoi(col), std::atoi(row)};
  }

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1) {
    return {-1, -1};
  }
  return {w.ws_col, w.ws_row};
}

} // namespace dpsg
