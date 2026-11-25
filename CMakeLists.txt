cmake_minimum_required(VERSION 3.20)
project(simon VERSION 1.1.1 LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Guard against older cmake_minimum_required() inside third-party dependencies on CMake 4+.
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

include(FetchContent)

set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(CLI11_INSTALL OFF CACHE BOOL "" FORCE)

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
  GIT_TAG        v7.0.3
  GIT_SHALLOW    TRUE
)

FetchContent_Declare(
  cli11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
  GIT_TAG        v2.7.2
  GIT_SHALLOW    TRUE
)

FetchContent_Declare(
  tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
  GIT_TAG        v3.5.1
  GIT_SHALLOW    TRUE
)

FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(ftxui cli11 tomlplusplus nlohmann_json)

# Embed skills/simon/SKILL.md directly into the binary for `simon --skill`.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/skills/simon/SKILL.md" SIMON_SKILL_CONTENT)
set_property(
  DIRECTORY
  APPEND
  PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/skills/simon/SKILL.md"
)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/skill_doc.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/skill_doc.hpp"
  @ONLY
)

add_library(simon_core STATIC
  src/config/config.cpp
  src/domain/delta.cpp
  src/domain/grouping.cpp
  src/domain/query.cpp
  src/domain/tree.cpp
  src/platform/monitor.cpp
  src/platform/macos/cpu_sampler.cpp
  src/platform/macos/disk_sampler.cpp
  src/platform/macos/gpu_sampler.cpp
  src/platform/macos/ioreport_sampler.cpp
  src/platform/macos/memory_sampler.cpp
  src/platform/macos/metal_info.mm
  src/platform/macos/process_sampler.cpp
  src/render/format_utils.cpp
  src/render/json_renderer.cpp
  src/render/plain_renderer.cpp
  src/render/renderer.cpp
  src/render/table_renderer.cpp
  src/tui/tui_app.cpp
  src/tui/tui_state.cpp
)

target_include_directories(simon_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_link_libraries(simon_core PUBLIC
  ftxui::component
  ftxui::dom
  ftxui::screen
  CLI11::CLI11
  tomlplusplus::tomlplusplus
  nlohmann_json::nlohmann_json
  IOReport
  "-framework IOKit"
  "-framework CoreFoundation"
  "-framework Foundation"
  "-framework Metal"
)

target_compile_options(simon_core PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

# IOReportIterate takes an Apple block (^).
set_source_files_properties(
  src/platform/macos/ioreport_sampler.cpp
  PROPERTIES COMPILE_OPTIONS "-fblocks;-Wno-gnu-statement-expression"
)

add_executable(simon src/main.cpp)
target_link_libraries(simon PRIVATE simon_core)
target_compile_options(simon PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
  add_executable(simon_tests tests/test_main.cpp)
  target_link_libraries(simon_tests PRIVATE simon_core)
  target_compile_options(simon_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME simon_tests COMMAND simon_tests)
endif()
