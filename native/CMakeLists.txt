cmake_minimum_required(VERSION 3.20)
# macOS: arm64 (Apple silicon); 13+ for VideoToolbox CBR
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS")
project(questbridge LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJCXX)
endif()

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
add_library(questbridge_core STATIC
  src/xrsp.cpp
  src/json.cpp
  src/audio.cpp
  src/capnp.cpp
  src/frame.cpp
  src/pose.cpp
  src/hands.cpp
  src/controllers.cpp
  src/session.cpp
  src/fake_quest.cpp
  src/templates_data.cpp
  src/ovr_service.cpp
  src/setup_check.cpp
)
target_include_directories(questbridge_core PUBLIC include src)

# USB backend: WinUSB on Windows (adb keeps its interface), libusb-1.0 elsewhere
if(WIN32)
  # OVRService helper: SCM query + an elevated PowerShell started through ShellExecuteEx
  set(QB_USB_SOURCES src/transport_winusb.cpp src/ovr_service_win.cpp)
  set(QB_USB_LIBS winusb setupapi advapi32 shell32 ole32)
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(LIBUSB IMPORTED_TARGET libusb-1.0)
  endif()
  if(LIBUSB_FOUND)
    set(QB_USB_SOURCES src/transport_libusb.cpp)
    set(QB_USB_LIBS PkgConfig::LIBUSB)
    # macOS: link libusb statically so libquestbridge.dylib runs on Macs without Homebrew
    find_library(LIBUSB_STATIC NAMES libusb-1.0.a HINTS ${LIBUSB_LIBRARY_DIRS} NO_DEFAULT_PATH)
    if(APPLE AND LIBUSB_STATIC)
      add_library(qb_libusb INTERFACE)
      target_include_directories(qb_libusb INTERFACE ${LIBUSB_INCLUDE_DIRS})
      target_link_libraries(qb_libusb INTERFACE ${LIBUSB_STATIC} objc "-framework IOKit"
        "-framework CoreFoundation" "-framework Security")
      set(QB_USB_LIBS qb_libusb)
    endif()
  else()
    message(WARNING "libusb-1.0 not found: building without USB (brew install libusb pkg-config)")
    set(QB_USB_SOURCES src/transport_none.cpp)
  endif()
endif()

# H.264 encoders (Media Foundation on Windows; VideoToolbox on macOS)
add_subdirectory(encoders)

# Texture capture + encode for qb_submit_frame_texture
if(WIN32)
  set(QB_TEXTURE_SOURCES src/texture_encoder_d3d11.cpp)
  set(QB_TEXTURE_LIBS questbridge_encoders d3d11)
elseif(APPLE)
  # Metal render passes into IOSurface strips + VideoToolbox; UnityPluginLoad lives here too
  set(QB_TEXTURE_SOURCES src/texture_encoder_metal.mm)
  set_source_files_properties(src/texture_encoder_metal.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  set(QB_TEXTURE_LIBS questbridge_encoders "-framework Metal" "-framework Foundation" "-framework IOSurface")
else()
  set(QB_TEXTURE_SOURCES src/texture_encoder_none.cpp)
endif()

# C API implementation, shared by the library and the runtime tests (which bring fake USB
# and encoder backends)
add_library(questbridge_api OBJECT src/questbridge.cpp src/link_runtime.cpp)
target_compile_definitions(questbridge_api PUBLIC QB_BUILD_SHARED)
target_link_libraries(questbridge_api PUBLIC questbridge_core)

# The C API: questbridge.dll / libquestbridge.dylib
add_library(questbridge SHARED $<TARGET_OBJECTS:questbridge_api> ${QB_USB_SOURCES} ${QB_TEXTURE_SOURCES}
  src/setup_probe.cpp)
target_compile_definitions(questbridge PRIVATE QB_BUILD_SHARED)
target_include_directories(questbridge PUBLIC include)
target_link_libraries(questbridge PRIVATE questbridge_core ${QB_USB_LIBS} ${QB_TEXTURE_LIBS})

# CLI on the C API only (plus the JSON reader for replay clips)
add_executable(qbhost cli/qbhost.cpp src/json.cpp)
target_include_directories(qbhost PRIVATE include src)
target_link_libraries(qbhost PRIVATE questbridge)

enable_testing()
add_subdirectory(tests)
