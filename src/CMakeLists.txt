add_library(bupp)
add_library(bupp::bupp ALIAS bupp)

add_subdirectory(async_io)
add_subdirectory(base/linux)

target_compile_features(bupp PUBLIC cxx_std_20)
target_link_libraries(bupp PUBLIC PkgConfig::LIBURING
                                  OpenSSL::SSL
                                  OpenSSL::Crypto
                                  $<BUILD_INTERFACE:bexec::bexec>)

target_sources(bupp PRIVATE linux/io_context.cpp ssl.cpp tcp.cpp)

target_include_directories(
  bupp
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

get_target_property(BUPP_TARGET_TYPE bupp TYPE)
if(BUPP_TARGET_TYPE STREQUAL "SHARED_LIBRARY")
  target_compile_definitions(bupp PUBLIC BUPP_SHARED_LIBRARY)
  target_compile_definitions(bupp PRIVATE BUPP_BUILDING_LIBRARY)
endif()

target_sources(
  bupp
  PUBLIC FILE_SET
         HEADERS
         BASE_DIRS
         ${PROJECT_SOURCE_DIR}/include
         FILES
         ${BUPP_PUBLIC_HEADERS})

bupp_set_project_warnings(bupp)

install(
  TARGETS bupp
  EXPORT buppTargets
  FILE_SET HEADERS DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(
  EXPORT buppTargets
  NAMESPACE bupp::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bupp)
