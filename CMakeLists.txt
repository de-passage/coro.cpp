cmake_minimum_required(VERSION 3.17)

project(coro)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

set(SOURCES
    main.cpp
)
list(TRANSFORM SOURCES PREPEND ${SOURCE_DIR}/)

add_executable(coro ${SOURCES})

target_include_directories(coro PRIVATE ${SOURCE_DIR})

add_custom_target(run
  COMMAND ${CMAKE_BINARY_DIR}/$<CONFIG>/coro
    DEPENDS coro
    WORKING_DIRECTORY ${CMAKE_PROJECT_DIR}
    USES_TERMINAL
)

target_compile_options(coro PRIVATE -Wall -Wextra -Werror -Wpedantic)
