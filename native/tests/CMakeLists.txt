add_executable(questlink_tests
  test_main.cpp
  test_xrsp.cpp
  test_capnp.cpp
  test_session.cpp
)
target_link_libraries(questlink_tests PRIVATE questlink_core)
get_filename_component(QL_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
target_compile_definitions(questlink_tests PRIVATE QL_REPO_ROOT="${QL_REPO_ROOT}")

add_test(NAME questlink_tests COMMAND questlink_tests)
