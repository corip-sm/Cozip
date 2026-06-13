cmake_minimum_required(VERSION 3.24)

project(Cozip
    VERSION 0.1.0
    DESCRIPTION "High-performance archive utility"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(COZIP_BUILD_TESTS "Build Cozip tests" ON)

add_subdirectory(engine)
add_subdirectory(apps/cli)

if(COZIP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
