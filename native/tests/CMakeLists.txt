get_filename_component(QL_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# Protocol core, linked statically
add_executable(questlink_tests
  test_main.cpp
  test_xrsp.cpp
  test_capnp.cpp
  test_session.cpp
  test_frame.cpp
  test_pose.cpp
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
