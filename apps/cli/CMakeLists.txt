add_executable(cozip_cli
    src/main.cpp)

target_link_libraries(cozip_cli
    PRIVATE
        cozip_engine)

target_compile_features(cozip_cli PRIVATE cxx_std_20)
