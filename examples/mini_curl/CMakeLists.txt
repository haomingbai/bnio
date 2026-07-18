add_executable(bnio_mini_curl mini_curl.cpp request.cpp)
target_link_libraries(bnio_mini_curl PRIVATE bnio::bnio)
bnio_set_project_warnings(bnio_mini_curl)
