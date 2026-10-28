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
  src/audio.cpp
  src/capnp.cpp
  src/frame.cpp
  src/pose.cpp
  src/hands.cpp
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

# H.264 encoders (Media Foundation on Windows; VideoToolbox later)
add_subdirectory(encoders)

# Texture capture + encode for ql_submit_frame_texture
if(WIN32)
  set(QL_TEXTURE_SOURCES src/texture_encoder_d3d11.cpp)
  set(QL_TEXTURE_LIBS questlink_encoders d3d11)
else()
  set(QL_TEXTURE_SOURCES src/texture_encoder_none.cpp)
endif()

# C API implementation, shared by the library and the runtime tests (which bring fake USB
# and encoder backends)
add_library(questlink_api OBJECT src/questlink.cpp src/link_runtime.cpp)
target_compile_definitions(questlink_api PUBLIC QL_BUILD_SHARED)
target_link_libraries(questlink_api PUBLIC questlink_core)

# The C API: questlink.dll / libquestlink.dylib
add_library(questlink SHARED $<TARGET_OBJECTS:questlink_api> ${QL_USB_SOURCES} ${QL_TEXTURE_SOURCES})
target_compile_definitions(questlink PRIVATE QL_BUILD_SHARED)
target_include_directories(questlink PUBLIC include)
target_link_libraries(questlink PRIVATE questlink_core ${QL_USB_LIBS} ${QL_TEXTURE_LIBS})

# CLI on the C API only (plus the JSON reader for replay clips)
add_executable(qlhost cli/qlhost.cpp src/json.cpp)
target_include_directories(qlhost PRIVATE include src)
target_link_libraries(qlhost PRIVATE questlink)

enable_testing()
add_subdirectory(tests)
