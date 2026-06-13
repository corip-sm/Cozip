#include "cozip/codecs/deflate_codec.h"

#include "libdeflate.h"

#include <array>

namespace cozip::codecs
{
namespace
{
int ToLibdeflateLevel(core::CompressionProfile profile) noexcept
{
    switch (profile)
    {
    case core::CompressionProfile::Fast:
        return 1;
    case core::CompressionProfile::Balanced:
        return 1;
    case core::CompressionProfile::Small:
        return 8;
    case core::CompressionProfile::Maximum:
        return 9;
    case core::CompressionProfile::Store:
        return 0;
    }

    return 1;
}
class LibdeflateCompressorCache
{
public:
    ~LibdeflateCompressorCache()
    {
        for (auto*& compressor : compressors_)
        {
            if (compressor != nullptr)
            {
                libdeflate_free_compressor(compressor);
                compressor = nullptr;
            }
        }
    }

    libdeflate_compressor* Acquire(int level)
    {
        if (level < 1 || level >= static_cast<int>(compressors_.size()))
        {
            return nullptr;
        }

        auto*& compressor = compressors_[static_cast<std::size_t>(level)];
        if (compressor == nullptr)
        {
            compressor = libdeflate_alloc_compressor(level);
        }

        return compressor;
    }

private:
    std::array<libdeflate_compressor*, 13> compressors_ {};
};

libdeflate_compressor* AcquireThreadLocalCompressor(core::CompressionProfile profile)
{
    thread_local LibdeflateCompressorCache cache;
    return cache.Acquire(ToLibdeflateLevel(profile));
}

libdeflate_decompressor* AcquireThreadLocalDecompressor()
{
    thread_local libdeflate_decompressor* decompressor = libdeflate_alloc_decompressor();
    return decompressor;
}
}

DeflateBufferResult CompressDeflateBufferLibdeflate(
    std::span<const std::byte> input,
    core::CompressionProfile profile)
{
    DeflateBufferResult result {};
    auto* compressor = AcquireThreadLocalCompressor(profile);
    if (compressor == nullptr)
    {
        result.error_message = "failed to allocate libdeflate compressor";
        return result;
    }

    const auto bound = libdeflate_deflate_compress_bound(compressor, input.size());
    result.bytes.resize(bound);

    const auto compressed_size = libdeflate_deflate_compress(
        compressor,
        input.data(),
        input.size(),
        result.bytes.data(),
        bound);

    if (compressed_size == 0)
    {
        result.bytes.clear();
        result.error_message = "libdeflate compression did not fit bound buffer";
        return result;
    }

    result.bytes.resize(compressed_size);
    result.success = true;
    result.backend = DeflateBackend::Libdeflate;
    result.has_crc32 = true;
    result.crc32 = libdeflate_crc32(0, input.data(), input.size());
    return result;
}

DeflateBufferResult CompressDeflateBuffer(
    std::span<const std::byte> input,
    core::CompressionProfile profile)
{
    return CompressDeflateBufferLibdeflate(input, profile);
}

DeflateBufferResult DecompressDeflateBufferLibdeflate(
    std::span<const std::byte> input,
    std::size_t expected_output_size)
{
    DeflateBufferResult result {};
    auto* decompressor = AcquireThreadLocalDecompressor();
    if (decompressor == nullptr)
    {
        result.error_message = "failed to allocate libdeflate decompressor";
        return result;
    }

    result.bytes.resize(expected_output_size);
    const auto status = libdeflate_deflate_decompress(
        decompressor,
        input.data(),
        input.size(),
        result.bytes.data(),
        result.bytes.size(),
        nullptr);

    if (status != LIBDEFLATE_SUCCESS)
    {
        result.bytes.clear();
        result.error_message = "libdeflate decompression failed";
        return result;
    }

    result.success = true;
    result.backend = DeflateBackend::Libdeflate;
    return result;
}

bool DecompressDeflateToBufferLibdeflate(
    std::span<const std::byte> input,
    std::span<std::byte> output)
{
    auto* decompressor = AcquireThreadLocalDecompressor();
    if (decompressor == nullptr)
    {
        return false;
    }

    const auto status = libdeflate_deflate_decompress(
        decompressor,
        input.data(),
        input.size(),
        output.data(),
        output.size(),
        nullptr);
    return status == LIBDEFLATE_SUCCESS;
}

DeflateBufferResult DecompressDeflateBuffer(
    std::span<const std::byte> input,
    std::size_t expected_output_size)
{
    return DecompressDeflateBufferLibdeflate(input, expected_output_size);
}

std::uint32_t UpdateCrc32(
    std::uint32_t crc,
    std::span<const std::byte> input) noexcept
{
    return libdeflate_crc32(crc, input.data(), input.size());
}

}
