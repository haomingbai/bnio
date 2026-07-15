add_library(bupp)
add_library(bupp::bupp ALIAS bupp)

add_subdirectory(async_io)

target_compile_features(bupp PUBLIC cxx_std_20)
target_link_libraries(
  bupp PUBLIC $<BUILD_INTERFACE:bexec::bexec>
              $<INSTALL_INTERFACE:bexec::bexec>)

if(BUPP_PLATFORM_LINUX)
  add_subdirectory(base/linux)
  target_link_libraries(bupp PUBLIC PkgConfig::LIBURING
                                    OpenSSL::SSL
                                    OpenSSL::Crypto)
  target_sources(
    bupp
    PRIVATE linux/io_context.cpp
            linux/io_context_queue.cpp
            linux/io_context_timer.cpp
            linux/io_context_timer_driver.cpp
            linux/io_context_timer_state.cpp
            ssl.cpp
            tcp.cpp
            udp.cpp)
else()
  target_compile_definitions(
    bupp
    PUBLIC BUPP_DISABLE_ASYNC_IO_LINUX=1
           BUPP_DISABLE_BASE_LINUX=1
           BUPP_DISABLE_IO_CONTEXT_LINUX=1)
endif()

if(BUPP_PLATFORM_BSD)
  add_subdirectory(base/bsd)
  target_link_libraries(bupp PUBLIC OpenSSL::SSL OpenSSL::Crypto)
  target_sources(
    bupp
    PRIVATE bsd/io_context.cpp
            bsd/io_context_queue.cpp
            bsd/io_context_timer.cpp
            bsd/io_context_timer_driver.cpp
            bsd/io_context_timer_state.cpp
            ssl.cpp
            tcp.cpp
            udp.cpp)
else()
  target_compile_definitions(bupp PUBLIC BUPP_DISABLE_BASE_BSD=1)
endif()

target_include_directories(
  bupp
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

get_target_property(BUPP_TARGET_TYPE bupp TYPE)
if(BUPP_TARGET_TYPE STREQUAL "SHARED_LIBRARY")
  target_compile_definitions(bupp PUBLIC BUPP_SHARED_LIBRARY)
  target_compile_definitions(bupp PRIVATE BUPP_BUILDING_LIBRARY)
  set_target_properties(
    bupp
    PROPERTIES VERSION "${PROJECT_VERSION}"
               SOVERSION "${PROJECT_VERSION_MAJOR}"
               CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN YES)
endif()

target_sources(bupp PRIVATE ${BUPP_PUBLIC_HEADERS})

bupp_set_project_warnings(bupp)

if(BUPP_INSTALL)
  install(
    TARGETS bupp
    EXPORT buppTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/bupp"
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(
    EXPORT buppTargets
    FILE buppTargets.cmake
    NAMESPACE bupp::
    DESTINATION "${BUPP_INSTALL_CMAKEDIR}")
endif()
