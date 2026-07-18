include("${PROJECT_SOURCE_DIR}/cmake/googletest.cmake")
bnio_resolve_googletest_dependency()
include(GoogleTest)

function(bnio_add_gtest target test_prefix)
  target_link_libraries(${target} PRIVATE GTest::gtest_main)
  bnio_set_project_warnings(${target})
  gtest_discover_tests(
    ${target}
    TEST_PREFIX "${test_prefix}."
    DISCOVERY_MODE PRE_TEST
    ${ARGN})
endfunction()

add_subdirectory(basic)
add_subdirectory(integration)
add_subdirectory(stress)
