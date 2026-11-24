cmake_minimum_required(VERSION 3.20)
project(realmem VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
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

FetchContent_MakeAvailable(ftxui cli11 tomlplusplus)

add_library(realmem_core STATIC
  src/config.cpp
  src/model.cpp
  src/process.cpp
  src/render.cpp
)

target_include_directories(realmem_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(realmem_core PUBLIC
  ftxui::dom
  ftxui::screen
  CLI11::CLI11
  tomlplusplus::tomlplusplus
)

target_compile_options(realmem_core PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

add_executable(realmem src/main.cpp)
target_link_libraries(realmem PRIVATE realmem_core)
target_compile_options(realmem PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
  add_executable(realmem_tests tests/test_main.cpp)
  target_link_libraries(realmem_tests PRIVATE realmem_core)
  target_compile_options(realmem_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME realmem_tests COMMAND realmem_tests)
endif()
