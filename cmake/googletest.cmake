include_guard(GLOBAL)

set(BUPP_GOOGLETEST_PROVIDER
    "AUTO"
    CACHE STRING "How to resolve GoogleTest: AUTO, FIND_PACKAGE, or FETCH")
set_property(CACHE BUPP_GOOGLETEST_PROVIDER PROPERTY STRINGS AUTO FIND_PACKAGE
                                                        FETCH)
set(BUPP_GOOGLETEST_GIT_REPOSITORY
    "https://github.com/google/googletest.git"
    CACHE STRING "Git repository used by the GoogleTest FETCH provider")
set(BUPP_GOOGLETEST_GIT_TAG
    "v1.17.0"
    CACHE STRING "Git ref used by the GoogleTest FETCH provider")

function(bupp_resolve_googletest_dependency)
  if(TARGET GTest::gtest_main)
    message(STATUS "Using the existing GTest::gtest_main target")
    return()
  endif()

  string(TOUPPER "${BUPP_GOOGLETEST_PROVIDER}"
                 _bupp_googletest_provider)
  set(_bupp_googletest_providers AUTO FIND_PACKAGE FETCH)
  if(NOT _bupp_googletest_provider IN_LIST _bupp_googletest_providers)
    message(
      FATAL_ERROR
        "BUPP_GOOGLETEST_PROVIDER must be AUTO, FIND_PACKAGE, or FETCH")
  endif()

  if(_bupp_googletest_provider STREQUAL "AUTO")
    find_package(GTest CONFIG QUIET)
    if(NOT TARGET GTest::gtest_main)
      find_package(GTest QUIET)
    endif()

    if(TARGET GTest::gtest_main)
      set(_bupp_googletest_provider FIND_PACKAGE)
    else()
      set(_bupp_googletest_provider FETCH)
    endif()
  endif()

  if(_bupp_googletest_provider STREQUAL "FIND_PACKAGE")
    if(NOT TARGET GTest::gtest_main)
      find_package(GTest CONFIG QUIET)
    endif()
    if(NOT TARGET GTest::gtest_main)
      find_package(GTest REQUIRED)
    endif()
    message(STATUS "Resolved GoogleTest with find_package")
  elseif(_bupp_googletest_provider STREQUAL "FETCH")
    # GoogleTest is a test-only implementation detail of bupp. Do not build
    # GoogleMock or add GoogleTest to bupp's installation surface.
    set(BUILD_GMOCK OFF CACHE BOOL "Build GoogleMock" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "Install GoogleTest" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "Use the shared MSVC runtime" FORCE)

    include(FetchContent)
    FetchContent_Declare(
      googletest
      GIT_REPOSITORY "${BUPP_GOOGLETEST_GIT_REPOSITORY}"
      GIT_TAG "${BUPP_GOOGLETEST_GIT_TAG}"
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(googletest)
    message(
      STATUS
        "Fetched GoogleTest ${BUPP_GOOGLETEST_GIT_TAG} from ${BUPP_GOOGLETEST_GIT_REPOSITORY}"
    )
  endif()

  if(NOT TARGET GTest::gtest_main)
    message(FATAL_ERROR
            "GoogleTest was resolved, but GTest::gtest_main was not found")
  endif()
endfunction()
