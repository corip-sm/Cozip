#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cozip/codecs/zip_method.h"

namespace cozip::pipeline
{
struct BufferBlock
{
    std::vector<std::byte> bytes;
};

using BufferBlockPtr = std::shared_ptr<BufferBlock>;

struct FileTask
{
    std::uint64_t file_id = 0;
    std::string source_path;
    std::string archive_path;
    std::uint64_t expected_size = 0;
    codecs::ZipMethod method = codecs::ZipMethod::Store;
    bool is_directory = false;
};

struct Chunk
{
    std::uint64_t file_id = 0;
    std::uint32_t chunk_index = 0;
    std::size_t uncompressed_size = 0;
    std::size_t compressed_size = 0;
    std::uint32_t crc32_partial = 0;
    bool is_last = false;
    BufferBlockPtr uncompressed;
    BufferBlockPtr compressed;
};

struct FileResult
{
    std::uint64_t file_id = 0;
    std::string archive_path;
    std::uint64_t uncompressed_size = 0;
    std::uint64_t compressed_size = 0;
    std::uint32_t crc32 = 0;
    std::uint64_t local_header_offset = 0;
    codecs::ZipMethod method = codecs::ZipMethod::Store;
    bool is_directory = false;
};
}
