cmake_minimum_required(VERSION 3.20)
project(questbridge_encoders LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /utf-8 /permissive-)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra)
endif()

# Encoder backends behind encoder.hpp. Windows: Media Foundation by GPU vendor, OpenH264 (Cisco's
# DLL, loaded at run time; BSD-2 headers in openh264/include) as the software fallback.
# macOS: VideoToolbox. select/strips: platform-neutral selection and strip layout.
add_library(questbridge_encoders STATIC h264.cpp select.cpp strips.cpp)
target_include_directories(questbridge_encoders PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
  target_sources(questbridge_encoders PRIVATE create_windows.cpp mf/mf_encoder.cpp mf/d3d11_input.cpp
    openh264/openh264_encoder.cpp)
  target_include_directories(questbridge_encoders PRIVATE openh264/include)
  target_link_libraries(questbridge_encoders PUBLIC mfplat mfuuid mf d3d11 ole32 strmiids)
elseif(APPLE)
  target_sources(questbridge_encoders PRIVATE vt/vt_encoder.cpp)
  target_link_libraries(questbridge_encoders PUBLIC "-framework VideoToolbox" "-framework CoreMedia"
    "-framework CoreVideo" "-framework CoreFoundation")
endif()

add_executable(qbenc qbenc/qbenc.cpp)
if(APPLE)
  add_executable(vtprobe vt/vtprobe.cpp)
  target_link_libraries(vtprobe PRIVATE questbridge_encoders)
endif()
target_link_libraries(qbenc PRIVATE questbridge_encoders)
