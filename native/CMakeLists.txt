cmake_minimum_required(VERSION 3.20)
project(questlink LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  add_compile_options(/W4 /utf-8 /permissive-)
else()
  add_compile_options(-Wall -Wextra)
endif()

# Protocol core: no I/O, no dependencies. Shared by the library, the CLI and the tests.
add_library(questlink_core STATIC
  src/xrsp.cpp
  src/json.cpp
  src/capnp.cpp
  src/frame.cpp
  src/pose.cpp
  src/session.cpp
  src/templates_data.cpp
)
target_include_directories(questlink_core PUBLIC include src)

enable_testing()
add_subdirectory(tests)
