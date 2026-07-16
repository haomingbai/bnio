# CPack configuration for bupp component-based packaging.
# Included by the top-level CMakeLists.txt only when BUPP_PACKAGE=ON.

# ---- Component definitions ----
set(CPACK_COMPONENT_RUNTIME_DISPLAY_NAME "Runtime library")
set(CPACK_COMPONENT_RUNTIME_DESCRIPTION "Shared library files for bupp")

set(CPACK_COMPONENT_DEVELOPMENT_DISPLAY_NAME "Development files")
set(CPACK_COMPONENT_DEVELOPMENT_DESCRIPTION
  "Headers, CMake config, and pkg-config for bupp")
set(CPACK_COMPONENT_DEVELOPMENT_DEPENDS runtime)

# ---- Common metadata ----
set(CPACK_PACKAGE_NAME "bupp")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "Haoming Bai")
set(CPACK_PACKAGE_CONTACT "haomingbai@users.noreply.github.com")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/haomingbai/bupp")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A small native asynchronous I/O library")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${PROJECT_SOURCE_DIR}/README.md")

# Enable component-based packaging for both generators
set(CPACK_COMPONENTS_ALL runtime development)
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_RPM_COMPONENT_INSTALL ON)

# ---- DEB generator ----
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

# DEB: runtime component -> libbupp0
set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME "libbupp0")
set(CPACK_DEBIAN_RUNTIME_PACKAGE_SECTION "libs")
set(CPACK_DEBIAN_RUNTIME_FILE_NAME
  "libbupp0_${CPACK_PACKAGE_VERSION}_${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}.deb")

# DEB: development component -> libbupp-dev
set(CPACK_DEBIAN_DEVELOPMENT_PACKAGE_NAME "libbupp-dev")
set(CPACK_DEBIAN_DEVELOPMENT_PACKAGE_SECTION "libdevel")
set(CPACK_DEBIAN_DEVELOPMENT_PACKAGE_DEPENDS
  "libbupp0 (= ${CPACK_PACKAGE_VERSION}), bexec-dev")
set(CPACK_DEBIAN_DEVELOPMENT_FILE_NAME
  "libbupp-dev_${CPACK_PACKAGE_VERSION}_${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}.deb")

# ---- RPM generator ----
set(CPACK_RPM_PACKAGE_LICENSE "MIT")

# RPM: runtime component -> bupp
set(CPACK_RPM_RUNTIME_PACKAGE_NAME "bupp")
set(CPACK_RPM_RUNTIME_FILE_NAME
  "bupp-${CPACK_PACKAGE_VERSION}-${CPACK_RPM_PACKAGE_RELEASE}.${CPACK_RPM_PACKAGE_ARCHITECTURE}.rpm")

# RPM: development component -> bupp-devel
set(CPACK_RPM_DEVELOPMENT_PACKAGE_NAME "bupp-devel")
set(CPACK_RPM_DEVELOPMENT_PACKAGE_REQUIRES
  "bupp = ${CPACK_PACKAGE_VERSION}, bexec-devel")
set(CPACK_RPM_DEVELOPMENT_FILE_NAME
  "bupp-devel-${CPACK_PACKAGE_VERSION}-${CPACK_RPM_PACKAGE_RELEASE}.${CPACK_RPM_PACKAGE_ARCHITECTURE}.rpm")

include(CPack)
