add_executable(bnio_file_cat file_cat.cpp)
target_link_libraries(bnio_file_cat PRIVATE bnio::bnio)
bnio_set_project_warnings(bnio_file_cat)
