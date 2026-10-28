get_filename_component(QB_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# Protocol core, linked statically
add_executable(questbridge_tests
  test_main.cpp
  test_xrsp.cpp
  test_capnp.cpp
  test_session.cpp
  test_device.cpp
  test_frame.cpp
  test_pose.cpp
  test_audio.cpp
  test_hands.cpp
  test_controllers.cpp
  test_setup.cpp
)
if(WIN32)
  target_sources(questbridge_tests PRIVATE test_ovr_win.cpp)
endif()
target_link_libraries(questbridge_tests PRIVATE questbridge_core)
target_compile_definitions(questbridge_tests PRIVATE QB_REPO_ROOT="${QB_REPO_ROOT}")
add_test(NAME questbridge_tests COMMAND questbridge_tests)

# Encoder selection and strip layout over fake encoders (no GPU or codec needed)
add_executable(questbridge_encoder_tests test_main.cpp test_encoder_select.cpp)
target_link_libraries(questbridge_encoder_tests PRIVATE questbridge_encoders questbridge_core)
target_compile_definitions(questbridge_encoder_tests PRIVATE QB_REPO_ROOT="${QB_REPO_ROOT}")
add_test(NAME questbridge_encoder_tests COMMAND questbridge_encoder_tests)

# The shared library through its C API only
add_executable(questbridge_capi_tests test_main.cpp test_capi.cpp ../src/json.cpp ../src/xrsp.cpp)
target_include_directories(questbridge_capi_tests PRIVATE ../src ../cli)
target_link_libraries(questbridge_capi_tests PRIVATE questbridge)
target_compile_definitions(questbridge_capi_tests PRIVATE QB_REPO_ROOT="${QB_REPO_ROOT}")
add_test(NAME questbridge_capi_tests COMMAND questbridge_capi_tests)

# Session API runtime over a fake Quest and a fake texture encoder
add_executable(questbridge_runtime_tests test_main.cpp test_runtime.cpp $<TARGET_OBJECTS:questbridge_api>)
target_link_libraries(questbridge_runtime_tests PRIVATE questbridge_api questbridge_core)
target_compile_definitions(questbridge_runtime_tests PRIVATE QB_REPO_ROOT="${QB_REPO_ROOT}")
add_test(NAME questbridge_runtime_tests COMMAND questbridge_runtime_tests)

# Session API runtime recovering from every kind of disconnect, against a reactive fake headset
add_executable(questbridge_recovery_tests test_main.cpp test_recovery.cpp $<TARGET_OBJECTS:questbridge_api>)
target_link_libraries(questbridge_recovery_tests PRIVATE questbridge_api questbridge_core)
target_compile_definitions(questbridge_recovery_tests PRIVATE QB_REPO_ROOT="${QB_REPO_ROOT}")
add_test(NAME questbridge_recovery_tests COMMAND questbridge_recovery_tests)

# macOS: the real Metal + VideoToolbox texture encoder behind the Session API, fake Quest
if(APPLE)
  add_executable(questbridge_metal_tests test_main.cpp test_metal.mm ../src/texture_encoder_metal.mm
    $<TARGET_OBJECTS:questbridge_api>)
  set_source_files_properties(test_metal.mm ../src/texture_encoder_metal.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(questbridge_metal_tests PRIVATE questbridge_api questbridge_core questbridge_encoders
    "-framework Metal" "-framework Foundation" "-framework IOSurface")
  target_compile_definitions(questbridge_metal_tests PRIVATE QB_REPO_ROOT="${QB_REPO_ROOT}")
  add_test(NAME questbridge_metal_tests COMMAND questbridge_metal_tests)
  set_tests_properties(questbridge_metal_tests PROPERTIES ENVIRONMENT QB_CODEC=h264)
  add_test(NAME questbridge_metal_hevc_tests COMMAND questbridge_metal_tests)  # auto = HEVC on macOS

  # VideoToolbox HEVC / H.264 strips: encode, decode each strip again, slice cap
  add_executable(questbridge_vt_tests test_main.cpp test_vt.cpp)
  target_link_libraries(questbridge_vt_tests PRIVATE questbridge_core questbridge_encoders)
  target_compile_definitions(questbridge_vt_tests PRIVATE QB_REPO_ROOT="${QB_REPO_ROOT}")
  add_test(NAME questbridge_vt_tests COMMAND questbridge_vt_tests)
endif()
