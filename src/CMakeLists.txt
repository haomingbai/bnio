add_library(bnio)
add_library(bnio::bnio ALIAS bnio)

add_subdirectory(async_io)

target_compile_features(bnio PUBLIC cxx_std_20)
target_link_libraries(
  bnio PUBLIC $<BUILD_INTERFACE:bexec::bexec>
              $<INSTALL_INTERFACE:bexec::bexec>)

if(BNIO_PLATFORM_LINUX)
  add_subdirectory(base/linux)
  target_link_libraries(bnio PUBLIC PkgConfig::LIBURING
                                    OpenSSL::SSL
                                    OpenSSL::Crypto)
else()
  target_compile_definitions(
    bnio
    PUBLIC BNIO_DISABLE_ASYNC_IO_LINUX=1
           BNIO_DISABLE_BASE_LINUX=1
           BNIO_DISABLE_IO_CONTEXT_LINUX=1)
endif()

if(BNIO_PLATFORM_BSD)
  add_subdirectory(base/bsd)
  target_link_libraries(bnio PUBLIC OpenSSL::SSL OpenSSL::Crypto)
else()
  target_compile_definitions(bnio PUBLIC BNIO_DISABLE_BASE_BSD=1)
endif()

if(BNIO_PLATFORM_LINUX OR BNIO_PLATFORM_BSD)
  target_sources(
    bnio
    PRIVATE io_context.cpp
            io_context_queue.cpp
            io_context_timer.cpp
            io_context_timer_state.cpp
            ssl.cpp
            tcp.cpp
            udp.cpp)
endif()

target_include_directories(
  bnio
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

get_target_property(BNIO_TARGET_TYPE bnio TYPE)
if(BNIO_TARGET_TYPE STREQUAL "SHARED_LIBRARY")
  target_compile_definitions(bnio PUBLIC BNIO_SHARED_LIBRARY)
  target_compile_definitions(bnio PRIVATE BNIO_BUILDING_LIBRARY)
  set_target_properties(
    bnio
    PROPERTIES VERSION "${PROJECT_VERSION}"
               SOVERSION "${PROJECT_VERSION_MAJOR}"
               CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN YES)
endif()

target_sources(bnio PRIVATE ${BNIO_PUBLIC_HEADERS})

bnio_set_project_warnings(bnio)

if(BNIO_INSTALL)
  install(
    TARGETS bnio
    EXPORT bnioTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT development
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT runtime
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime)
  install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/bnio"
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
          COMPONENT development)
  install(
    EXPORT bnioTargets
    FILE bnioTargets.cmake
    NAMESPACE bnio::
    DESTINATION "${BNIO_INSTALL_CMAKEDIR}"
    COMPONENT development)
endif()
