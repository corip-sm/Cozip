set(COZIP_MINIZ_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/miniz)
set(COZIP_MINIZ_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/third_party/miniz)

file(MAKE_DIRECTORY ${COZIP_MINIZ_BUILD_DIR})

foreach(COZIP_MINIZ_FILE IN ITEMS
    miniz.c
    miniz.h
    miniz_common.h
    miniz_tdef.c
    miniz_tdef.h
    miniz_tinfl.c
    miniz_tinfl.h
    miniz_zip.c
    miniz_zip.h)
    configure_file(
        ${COZIP_MINIZ_SOURCE_DIR}/${COZIP_MINIZ_FILE}
        ${COZIP_MINIZ_BUILD_DIR}/${COZIP_MINIZ_FILE}
        COPYONLY)
endforeach()

file(WRITE ${COZIP_MINIZ_BUILD_DIR}/miniz_export.h
"#pragma once\n\n#ifndef MINIZ_EXPORT\n#define MINIZ_EXPORT\n#endif\n")

add_library(cozip_engine
    ../third_party/libdeflate-src/lib/adler32.c
    ../third_party/libdeflate-src/lib/crc32.c
    ../third_party/libdeflate-src/lib/deflate_compress.c
    ../third_party/libdeflate-src/lib/deflate_decompress.c
    ../third_party/libdeflate-src/lib/gzip_compress.c
    ../third_party/libdeflate-src/lib/gzip_decompress.c
    ../third_party/libdeflate-src/lib/utils.c
    ../third_party/libdeflate-src/lib/zlib_compress.c
    ../third_party/libdeflate-src/lib/zlib_decompress.c
    ../third_party/libdeflate-src/lib/x86/cpu_features.c
    ../third_party/libdeflate-src/lib/arm/cpu_features.c
    ${COZIP_MINIZ_BUILD_DIR}/miniz.c
    ${COZIP_MINIZ_BUILD_DIR}/miniz_tdef.c
    ${COZIP_MINIZ_BUILD_DIR}/miniz_tinfl.c
    ${COZIP_MINIZ_BUILD_DIR}/miniz_zip.c
    codecs/src/deflate_codec.cpp
    codecs/src/zip_method.cpp
    core/src/version.cpp
    core/src/archive_job.cpp
    core/src/archive_request.cpp
    pipeline/src/archive_pipeline.cpp
    pipeline/src/buffer_pool.cpp
    pipeline/src/pipeline_options.cpp
    platform/src/filesystem_random_access.cpp
    platform/src/filesystem_storage_factory.cpp
    platform/src/mapped_file.cpp
    format_zip/src/zip_archive_api.cpp
    format_zip/src/zip_archive_common.cpp
    format_zip/src/zip_archive_crypto.cpp
    format_zip/src/zip_archive_create.cpp
    format_zip/src/zip_archive_read_extract.cpp
    format_zip/src/zip_archive_read_shared.cpp
    format_zip/src/zip_archive_read_test.cpp)

target_include_directories(cozip_engine
    PUBLIC
        ${COZIP_MINIZ_BUILD_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libdeflate-src
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libdeflate-src/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/codecs/include
        ${CMAKE_CURRENT_SOURCE_DIR}/core/include
        ${CMAKE_CURRENT_SOURCE_DIR}/storage/include
        ${CMAKE_CURRENT_SOURCE_DIR}/pipeline/include
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/include
        ${CMAKE_CURRENT_SOURCE_DIR}/format_zip/include)

target_compile_features(cozip_engine PUBLIC cxx_std_20)
