include_guard(GLOBAL)

set(BUPP_BEXEC_SOURCE_DIR
    ""
    CACHE PATH
          "Path to a local bexec source checkout. If empty, fetch from GitHub.")
set(BUPP_BEXEC_GIT_REPOSITORY
    "https://github.com/haomingbai/bexec.git"
    CACHE STRING "Git repository used when BUPP_BEXEC_SOURCE_DIR is empty.")
set(BUPP_BEXEC_GIT_TAG
    "main"
    CACHE STRING "Git ref used when fetching bexec from GitHub.")

function(bupp_resolve_bexec_dependency)
  if(TARGET bexec::bexec)
    return()
  endif()

  if(TARGET bexec)
    add_library(bexec::bexec ALIAS bexec)
    return()
  endif()

  set(BEXEC_BUILD_TESTS
      OFF
      CACHE BOOL "Build bexec tests" FORCE)
  set(BEXEC_BUILD_EXAMPLES
      OFF
      CACHE BOOL "Build bexec examples" FORCE)

  if(BUPP_BEXEC_SOURCE_DIR)
    get_filename_component(_bupp_bexec_source_dir "${BUPP_BEXEC_SOURCE_DIR}"
                           ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")

    if(NOT EXISTS "${_bupp_bexec_source_dir}/CMakeLists.txt")
      message(FATAL_ERROR
              "BUPP_BEXEC_SOURCE_DIR must point to a bexec source tree.")
    endif()

    add_subdirectory("${_bupp_bexec_source_dir}"
                     "${PROJECT_BINARY_DIR}/_deps/bexec-build"
                     EXCLUDE_FROM_ALL)
  else()
    include(FetchContent)
    FetchContent_Declare(
      bexec
      GIT_REPOSITORY "${BUPP_BEXEC_GIT_REPOSITORY}"
      GIT_TAG "${BUPP_BEXEC_GIT_TAG}"
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(bexec)
  endif()

  if(NOT TARGET bexec::bexec)
    if(TARGET bexec)
      add_library(bexec::bexec ALIAS bexec)
    else()
      message(FATAL_ERROR
              "bexec was resolved, but target bexec::bexec was not found.")
    endif()
  endif()
endfunction()
