include_guard(GLOBAL)

function(bupp_resolve_wrk_dependency)
  if(NOT BUPP_FETCH_WRK)
    return()
  endif()

  # Already resolved?
  if(TARGET wrk)
    return()
  endif()

  # ---- OpenSSL prefix (wrk needs OpenSSL; bupp already requires it) ----
  find_package(OpenSSL REQUIRED)
  get_filename_component(_bupp_openssl_prefix "${OPENSSL_INCLUDE_DIR}" DIRECTORY)

  # ---- LuaJIT (required, use system package) ----
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(_LUAJIT luajit IMPORTED_TARGET)
  if(NOT _LUAJIT_FOUND)
    message(FATAL_ERROR
      "BUPP_FETCH_WRK=ON requires LuaJIT development headers.\n"
      "  Fedora:   sudo dnf install luajit-devel\n"
      "  Debian:   sudo apt install libluajit-dev")
  endif()

  if(_LUAJIT_PREFIX)
    set(_bupp_wrk_luajit_flag "WITH_LUAJIT=${_LUAJIT_PREFIX}")
  else()
    get_filename_component(_lj_prefix "${_LUAJIT_INCLUDE_DIRS}" DIRECTORY)
    set(_bupp_wrk_luajit_flag "WITH_LUAJIT=${_lj_prefix}")
  endif()
  message(STATUS "wrk: using system LuaJIT at ${_LUAJIT_INCLUDE_DIRS}")

  # ---- ExternalProject: fetch and build wrk ----
  include(ExternalProject)

  set(_bupp_wrk_install_dir "${CMAKE_BINARY_DIR}/wrk-install")

  ExternalProject_Add(wrk_ext
    GIT_REPOSITORY  "https://github.com/wg/wrk.git"
    GIT_TAG         "4.2.0"
    GIT_SHALLOW     TRUE
    PREFIX          "${CMAKE_BINARY_DIR}/_deps/wrk-ext"

    CONFIGURE_COMMAND ""

    BUILD_COMMAND
      $(MAKE) -j
      "WITH_OPENSSL=${_bupp_openssl_prefix}"
      ${_bupp_wrk_luajit_flag}

    BUILD_IN_SOURCE TRUE

    INSTALL_COMMAND
      ${CMAKE_COMMAND} -E make_directory "${_bupp_wrk_install_dir}/bin"
      COMMAND
        ${CMAKE_COMMAND} -E copy_if_different
        <SOURCE_DIR>/wrk
        "${_bupp_wrk_install_dir}/bin/wrk"

    BUILD_BYPRODUCTS "${_bupp_wrk_install_dir}/bin/wrk"
  )

  # Convenience target — building "wrk" triggers the ExternalProject build
  add_custom_target(wrk ALL
    DEPENDS wrk_ext
  )

  # Expose the binary path so scripts can find it
  set(WRK_BINARY
      "${_bupp_wrk_install_dir}/bin/wrk"
      CACHE FILEPATH "Path to the wrk benchmarking tool binary")

  message(STATUS "wrk: will be built at ${WRK_BINARY}")
endfunction()

if(BUPP_FETCH_WRK)
  bupp_resolve_wrk_dependency()
endif()
