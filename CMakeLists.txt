cmake_minimum_required(VERSION 3.24)
project(bo1_metal_port LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

# Trace replay harness (P3). The slot tables come from the proxy's header: single source of truth.
add_executable(replay
  tests/replay/main.cpp
  tests/replay/trace.cpp
  tests/replay/null_backend.cpp)
target_include_directories(replay PRIVATE tests/replay tools/d3d9_proxy)
