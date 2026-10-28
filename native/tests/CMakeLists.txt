get_filename_component(QL_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# Protocol core, linked statically
add_executable(questlink_tests
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
)
target_link_libraries(questlink_tests PRIVATE questlink_core)
target_compile_definitions(questlink_tests PRIVATE QL_REPO_ROOT="${QL_REPO_ROOT}")
add_test(NAME questlink_tests COMMAND questlink_tests)

# The shared library through its C API only
add_executable(questlink_capi_tests test_main.cpp test_capi.cpp ../src/json.cpp ../src/xrsp.cpp)
target_include_directories(questlink_capi_tests PRIVATE ../src ../cli)
target_link_libraries(questlink_capi_tests PRIVATE questlink)
target_compile_definitions(questlink_capi_tests PRIVATE QL_REPO_ROOT="${QL_REPO_ROOT}")
add_test(NAME questlink_capi_tests COMMAND questlink_capi_tests)

# Session API runtime over a fake Quest and a fake texture encoder
add_executable(questlink_runtime_tests test_main.cpp test_runtime.cpp $<TARGET_OBJECTS:questlink_api>)
target_link_libraries(questlink_runtime_tests PRIVATE questlink_api questlink_core)
target_compile_definitions(questlink_runtime_tests PRIVATE QL_REPO_ROOT="${QL_REPO_ROOT}")
add_test(NAME questlink_runtime_tests COMMAND questlink_runtime_tests)

# Session API runtime recovering from every kind of disconnect, against a reactive fake headset
add_executable(questlink_recovery_tests test_main.cpp test_recovery.cpp $<TARGET_OBJECTS:questlink_api>)
target_link_libraries(questlink_recovery_tests PRIVATE questlink_api questlink_core)
target_compile_definitions(questlink_recovery_tests PRIVATE QL_REPO_ROOT="${QL_REPO_ROOT}")
add_test(NAME questlink_recovery_tests COMMAND questlink_recovery_tests)

# macOS: the real Metal + VideoToolbox texture encoder behind the Session API, fake Quest
if(APPLE)
  add_executable(questlink_metal_tests test_main.cpp test_metal.mm ../src/texture_encoder_metal.mm
    $<TARGET_OBJECTS:questlink_api>)
  set_source_files_properties(test_metal.mm ../src/texture_encoder_metal.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(questlink_metal_tests PRIVATE questlink_api questlink_core questlink_encoders
    "-framework Metal" "-framework Foundation" "-framework IOSurface")
  target_compile_definitions(questlink_metal_tests PRIVATE QL_REPO_ROOT="${QL_REPO_ROOT}")
  add_test(NAME questlink_metal_tests COMMAND questlink_metal_tests)
  add_test(NAME questlink_metal_hevc_tests COMMAND questlink_metal_tests)
  set_tests_properties(questlink_metal_hevc_tests PROPERTIES ENVIRONMENT QL_CODEC=hevc)

  # VideoToolbox HEVC / H.264 strips: encode, decode each strip again, slice cap
  add_executable(questlink_vt_tests test_main.cpp test_vt.cpp)
  target_link_libraries(questlink_vt_tests PRIVATE questlink_core questlink_encoders)
  target_compile_definitions(questlink_vt_tests PRIVATE QL_REPO_ROOT="${QL_REPO_ROOT}")
  add_test(NAME questlink_vt_tests COMMAND questlink_vt_tests)
endif()
