# CPack configuration for bnio component-based packaging.
# Included by the top-level CMakeLists.txt only when BNIO_PACKAGE=ON.

# ---- Component definitions ----
set(CPACK_COMPONENT_RUNTIME_DISPLAY_NAME "Runtime library")
set(CPACK_COMPONENT_RUNTIME_DESCRIPTION "Shared library files for bnio")

set(CPACK_COMPONENT_DEVELOPMENT_DISPLAY_NAME "Development files")
set(CPACK_COMPONENT_DEVELOPMENT_DESCRIPTION
  "Headers, CMake config, and pkg-config for bnio")
set(CPACK_COMPONENT_DEVELOPMENT_DEPENDS runtime)

# ---- Common metadata ----
set(CPACK_PACKAGE_NAME "bnio")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "Haoming Bai")
set(CPACK_PACKAGE_CONTACT "haomingbai@users.noreply.github.com")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/haomingbai/bnio")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A small native asynchronous I/O library")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${PROJECT_SOURCE_DIR}/README.md")

# Enable component-based packaging for both generators
set(CPACK_COMPONENTS_ALL runtime development)
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_RPM_COMPONENT_INSTALL ON)

# ---- DEB generator ----
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

# DEB: runtime component -> libbnio0
set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME "libbnio0")
set(CPACK_DEBIAN_RUNTIME_PACKAGE_SECTION "libs")

# DEB: development component -> libbnio-dev
set(CPACK_DEBIAN_DEVELOPMENT_PACKAGE_NAME "libbnio-dev")
set(CPACK_DEBIAN_DEVELOPMENT_PACKAGE_SECTION "libdevel")
set(CPACK_DEBIAN_DEVELOPMENT_PACKAGE_DEPENDS
  "libbnio0 (= ${CPACK_PACKAGE_VERSION}), bexec-dev")

# ---- RPM generator ----
set(CPACK_RPM_PACKAGE_LICENSE "MIT")

# RPM: runtime component -> bnio
# NOTE: Do not set CPACK_RPM_RUNTIME_FILE_NAME manually. The release and
# architecture variables are only available at cpack runtime, not during
# CMake configure, so a hand-crafted name produces "bnio-0.0.3-..rpm".
# Letting CPack auto-generate yields "bnio-<ver>-<rel>.<arch>.rpm".
set(CPACK_RPM_RUNTIME_PACKAGE_NAME "bnio")

# RPM: development component -> bnio-devel
set(CPACK_RPM_DEVELOPMENT_PACKAGE_NAME "bnio-devel")
set(CPACK_RPM_DEVELOPMENT_PACKAGE_REQUIRES
  "bnio = ${CPACK_PACKAGE_VERSION}, bexec-devel")

include(CPack)
