add_executable(cozip_unit_tests
    unit/src/archive_job_tests.cpp)

target_link_libraries(cozip_unit_tests
    PRIVATE
        cozip_engine)

add_test(NAME cozip_unit_tests COMMAND cozip_unit_tests)

add_executable(cozip_zip_executor_tests
    unit/src/zip_executor_tests.cpp)

target_link_libraries(cozip_zip_executor_tests
    PRIVATE
        cozip_engine)

target_include_directories(cozip_zip_executor_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/format_zip/src)

target_compile_definitions(cozip_zip_executor_tests
    PRIVATE
        COZIP_ENABLE_TEST_HOOKS)

add_test(NAME cozip_zip_executor_tests COMMAND cozip_zip_executor_tests)

add_executable(cozip_zip_compat_tests
    integration/src/zip_dotnet_compat_tests.cpp)

target_link_libraries(cozip_zip_compat_tests
    PRIVATE
        cozip_engine)

target_compile_definitions(cozip_zip_compat_tests
    PRIVATE
        COZIP_TESTS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME cozip_zip_compat_tests COMMAND cozip_zip_compat_tests)

add_executable(cozip_zip_external_compat_tests
    integration/src/zip_external_compat_tests.cpp)

target_link_libraries(cozip_zip_external_compat_tests
    PRIVATE
        cozip_engine)

target_compile_definitions(cozip_zip_external_compat_tests
    PRIVATE
        COZIP_TESTS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME cozip_zip_external_compat_tests COMMAND cozip_zip_external_compat_tests)
