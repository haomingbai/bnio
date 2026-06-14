add_executable(bupp_mini_curl mini_curl.cpp)
target_link_libraries(bupp_mini_curl PRIVATE bupp::bupp)
bupp_set_project_warnings(bupp_mini_curl)
