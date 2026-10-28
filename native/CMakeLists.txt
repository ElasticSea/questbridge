cmake_minimum_required(VERSION 3.20)
project(questlink LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
# Library, CLI and tests side by side so the DLL / dylib is found at run time
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
  add_compile_options(/W4 /utf-8 /permissive-)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra)
endif()

# Protocol core: no I/O, no dependencies. Shared by the library and the tests.
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

# USB backend: WinUSB on Windows (adb keeps its interface), libusb-1.0 elsewhere
if(WIN32)
  set(QL_USB_SOURCES src/transport_winusb.cpp)
  set(QL_USB_LIBS winusb setupapi)
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(LIBUSB IMPORTED_TARGET libusb-1.0)
  endif()
  if(LIBUSB_FOUND)
    set(QL_USB_SOURCES src/transport_libusb.cpp)
    set(QL_USB_LIBS PkgConfig::LIBUSB)
  else()
    message(WARNING "libusb-1.0 not found: building without USB (brew install libusb pkg-config)")
    set(QL_USB_SOURCES src/transport_none.cpp)
  endif()
endif()

# The C API: questlink.dll / libquestlink.dylib
add_library(questlink SHARED src/questlink.cpp ${QL_USB_SOURCES})
target_compile_definitions(questlink PRIVATE QL_BUILD_SHARED)
target_include_directories(questlink PUBLIC include)
target_link_libraries(questlink PRIVATE questlink_core ${QL_USB_LIBS})

# CLI on the C API only (plus the JSON reader for replay clips)
add_executable(qlhost cli/qlhost.cpp src/json.cpp)
target_include_directories(qlhost PRIVATE include src)
target_link_libraries(qlhost PRIVATE questlink)

enable_testing()
add_subdirectory(tests)
