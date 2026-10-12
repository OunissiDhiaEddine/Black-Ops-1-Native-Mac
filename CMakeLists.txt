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

# D3D9 SM3 bytecode -> MSL translator (ADR-005) and its corpus checker.
add_library(bo1_shaders STATIC src/shaders/sm3_msl.cpp)
target_include_directories(bo1_shaders PUBLIC src/shaders)

add_executable(sm3c tools/sm3c/main.mm)
target_link_libraries(sm3c PRIVATE bo1_shaders "-framework Metal" "-framework Foundation")

# SDL3 (ADR-006): window, input and the CAMetalLayer the renderer presents to. Homebrew: `brew install sdl3`.
list(APPEND CMAKE_PREFIX_PATH /opt/homebrew)
find_package(SDL3 REQUIRED CONFIG)

# Renderer (ADR-004): D3D9-shaped call records in (src/rhi), Metal out (src/metal). Shared by the replay
# tool and the native runtime's d3d9 shim. The slot tables are src/rhi/d3d9_methods.h: single source of truth.
add_library(bo1_metal STATIC src/metal/metal_renderer.mm)
target_include_directories(bo1_metal PUBLIC src/rhi)
set_source_files_properties(src/metal/metal_renderer.mm tools/sm3c/main.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_link_libraries(bo1_metal PUBLIC bo1_shaders SDL3::SDL3 "-framework Metal" "-framework Foundation"
                      "-framework CoreGraphics" "-framework ImageIO" "-framework QuartzCore")

# Trace replay harness (P3).
add_executable(replay
  tests/replay/main.cpp
  tests/replay/trace.cpp
  tests/replay/null_backend.cpp)
target_include_directories(replay PRIVATE tests/replay)
target_link_libraries(replay PRIVATE bo1_metal)

# SDL3 -> Win32 window messages (T-052) and its unit test.
add_library(bo1_window STATIC src/platform/window/win_messages.cpp src/platform/window/window_service.cpp
            src/platform/window/boot_splash.mm)
set_source_files_properties(src/platform/window/boot_splash.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_include_directories(bo1_window PUBLIC src/platform/window)
target_link_libraries(bo1_window PUBLIC SDL3::SDL3 "-framework AppKit" "-framework QuartzCore")
add_executable(test_boot_splash tests/window/test_boot_splash.cpp)
target_link_libraries(test_boot_splash PRIVATE bo1_window)
add_executable(test_win_messages tests/window/test_win_messages.cpp)
target_link_libraries(test_win_messages PRIVATE bo1_window)
add_executable(test_window_service tests/window/test_window_service.cpp)
target_link_libraries(test_window_service PRIVATE bo1_window)
