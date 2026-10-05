cmake_minimum_required(VERSION 3.24)
project(bo1_metal_port LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
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

# D3D9 SM3 bytecode -> MSL translator (ADR-005) and its corpus checker.
add_library(bo1_shaders STATIC src/shaders/sm3_msl.cpp)
target_include_directories(bo1_shaders PUBLIC src/shaders)

add_executable(sm3c tools/sm3c/main.mm)
target_link_libraries(sm3c PRIVATE bo1_shaders "-framework Metal" "-framework Foundation")
