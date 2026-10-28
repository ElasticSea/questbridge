cmake_minimum_required(VERSION 3.20)
project(questlink_encoders LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /utf-8 /permissive-)
else()
  add_compile_options(-Wall -Wextra)
endif()

# Encoder backends behind encoder.hpp. Windows: Media Foundation. macOS: VideoToolbox.
add_library(questlink_encoders STATIC h264.cpp)
target_include_directories(questlink_encoders PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
  target_sources(questlink_encoders PRIVATE mf/mf_encoder.cpp)
  target_link_libraries(questlink_encoders PUBLIC mfplat mfuuid mf d3d11 ole32 strmiids)
elseif(APPLE)
  target_sources(questlink_encoders PRIVATE vt/vt_encoder.cpp)
  target_link_libraries(questlink_encoders PUBLIC "-framework VideoToolbox" "-framework CoreMedia"
    "-framework CoreVideo" "-framework CoreFoundation")
endif()

add_executable(qlenc qlenc/qlenc.cpp)
target_link_libraries(qlenc PRIVATE questlink_encoders)
