#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "cozip/core/archive_job.h"

namespace cozip::codecs
{
enum class DeflateBackend
{
    None,
    Libdeflate,
    Miniz,
};

struct DeflateBufferResult
{
    bool success = false;
    DeflateBackend backend = DeflateBackend::None;
    std::string error_message;
    bool has_crc32 = false;
    std::uint32_t crc32 = 0;
    std::vector<std::byte> bytes;
};

struct DeflateStreamResult
{
    bool success = false;
    DeflateBackend backend = DeflateBackend::None;
    std::string error_message;
    std::size_t compressed_size = 0;
    std::uint32_t crc32 = 0;
};

DeflateBufferResult CompressDeflateBufferLibdeflate(
    std::span<const std::byte> input,
    core::CompressionProfile profile);

DeflateBufferResult CompressDeflateBuffer(
    std::span<const std::byte> input,
    core::CompressionProfile profile);

DeflateBufferResult DecompressDeflateBufferLibdeflate(
    std::span<const std::byte> input,
    std::size_t expected_output_size);

bool DecompressDeflateToBufferLibdeflate(
    std::span<const std::byte> input,
    std::span<std::byte> output);

DeflateBufferResult DecompressDeflateBuffer(
    std::span<const std::byte> input,
    std::size_t expected_output_size);

std::uint32_t UpdateCrc32(
    std::uint32_t crc,
    std::span<const std::byte> input) noexcept;

}
