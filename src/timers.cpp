#define DPSG_COMPILE_DEBUG_HPP
#include "fd_timers.hpp"

#include "vt100.hpp"

std::chrono::steady_clock::time_point start;
timer tick(auto color, int times, Duration auto delay) {
  while (times-- > 0) {
    co_await delay;
    std::cout << color << times << "! (after " << delay << "): \t" << std::chrono::duration_cast<std::chrono::milliseconds>(decltype(start)::clock::now() - start) << reset << std::endl;
  }
}

timer hello_world(auto color, Duration auto delay) {
  co_await tick(color, 5, delay);
  auto delay_s = std::chrono::duration_cast<std::chrono::milliseconds>(decltype(start)::clock::now() - start);
  std::cout << (color | dpsg::vt100::reverse) << "Hello World!: \t"<< delay_s  << reset
            << std::endl;
}

auto stop(timer_scheduler &timer_scheduler, auto d = std::chrono::seconds{1}) -> timer {
  using namespace std::chrono_literals;
  println(cyan | bold, "Stop after 5s: \t", std::chrono::duration_cast<std::chrono::milliseconds>(decltype(start)::clock::now() - start) );
  co_await d;
  auto delay_s = std::chrono::duration_cast<std::chrono::milliseconds>(decltype(start)::clock::now() - start);
  println(cyan | bold, "Stop now!: \t", delay_s);
  timer_scheduler.stop();
};

int main() {
  using namespace std::chrono_literals;
  using dpsg::vt100::reverse;
  fd_scheduler_single_consumer fd_scheduler;
  timer_scheduler timer_scheduler;
  println(reverse | green, "START", reset);
  fd_scheduler.add(timer_scheduler.manage_timers());
   start = decltype(start)::clock::now();

  println(reverse | green, "ADD TIMER 1", reset);
  timer_scheduler.add(hello_world(blue, 200ms));

  println(reverse | green, "ADD TIMER 2", reset);
  timer_scheduler.add(hello_world(red, 1s));

  println(reverse | green, "ADD TIMER 3", reset);
  timer_scheduler.add(hello_world(yellow, 500ms));
  timer_scheduler.add(stop(timer_scheduler, 5s));

  println(reverse | green, "RUN", reset);

  fd_scheduler.run();
}
