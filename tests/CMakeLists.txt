include("${PROJECT_SOURCE_DIR}/cmake/googletest.cmake")
bupp_resolve_googletest_dependency()
include(GoogleTest)

function(bupp_add_gtest target test_prefix)
  target_link_libraries(${target} PRIVATE GTest::gtest_main)
  bupp_set_project_warnings(${target})
  gtest_discover_tests(
    ${target}
    TEST_PREFIX "${test_prefix}."
    DISCOVERY_MODE PRE_TEST
    ${ARGN})
endfunction()

add_subdirectory(async_io)

if(BUPP_PLATFORM_LINUX)
  add_subdirectory(base/linux)
endif()

if(BUPP_PLATFORM_LINUX OR BUPP_PLATFORM_BSD)
  add_subdirectory(io_context)
endif()

if(BUPP_PLATFORM_BSD)
  add_subdirectory(base/bsd)
endif()
