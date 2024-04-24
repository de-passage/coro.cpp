cmake_minimum_required(VERSION 3.17)

project(coro)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

macro(add_runnable_example name)
  set(SOURCES ${ARGN})
  list(LENGTH SOURCES SF_LEN)
  if (SF_LEN GREATER 0)
    list(TRANSFORM SOURCES PREPEND ${SOURCE_DIR}/)
  else()
    set(SOURCES ${SOURCE_DIR}/${name}.cpp)
  endif()

  add_executable(${name} ${SOURCES})

  target_include_directories(${name} PRIVATE ${SOURCE_DIR})

  add_custom_target(run-${name}
    COMMAND ${CMAKE_BINARY_DIR}/$<CONFIG>/${name}
      DEPENDS ${name}
      WORKING_DIRECTORY ${CMAKE_PROJECT_DIR}
      USES_TERMINAL
  )

  target_compile_options(coro PRIVATE -Wall -Wextra -Werror)
endmacro()

add_runnable_example(coro main.cpp)
add_runnable_example(fd_coroutines)
add_runnable_example(parallel_demo)
target_link_libraries(parallel_demo PRIVATE pthread)
add_runnable_example(animation)

