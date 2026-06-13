#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace cozip::storage
{
struct StorageCapabilities
{
    bool supports_random_read = false;
    bool supports_random_write = false;
    bool supports_mapping = false;
    bool supports_parallel_reads = false;
    bool supports_parallel_writes = false;
    bool requires_exclusive_access = false;
    bool durable_flush = false;
    std::size_t preferred_window_bytes = 0;
    std::size_t preferred_alignment_bytes = 0;
    std::size_t max_efficient_read_bytes = 0;
};

struct MappedReadWindow
{
    std::uint64_t offset = 0;
    std::span<const std::byte> bytes {};
};

class IRandomAccessReader
{
public:
    virtual ~IRandomAccessReader() = default;

    [[nodiscard]] virtual std::uint64_t Size() const noexcept = 0;
    [[nodiscard]] virtual StorageCapabilities Capabilities() const noexcept = 0;

    virtual bool Read(std::uint64_t offset,
                      std::span<std::byte> output,
                      std::size_t& bytes_read,
                      std::string& error_message) = 0;

    virtual bool TryMapWindow(std::uint64_t offset,
                              std::size_t length,
                              MappedReadWindow& window,
                              std::string& error_message) = 0;
};

class IRandomAccessWriter
{
public:
    virtual ~IRandomAccessWriter() = default;

    [[nodiscard]] virtual std::uint64_t Size() const noexcept = 0;
    [[nodiscard]] virtual StorageCapabilities Capabilities() const noexcept = 0;

    virtual bool Write(std::uint64_t offset,
                       std::span<const std::byte> data,
                       std::string& error_message) = 0;

    virtual bool Resize(std::uint64_t size, std::string& error_message) = 0;
    virtual bool Flush(std::string& error_message) = 0;
};
}
