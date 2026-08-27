#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "cozip/core/execution_options.h"
#include "cozip/platform/mapped_file.h"
#include "cozip/storage/random_access.h"
#include "cozip/storage/storage_factory.h"

namespace cozip::platform
{
class FilesystemRandomAccessReader final : public storage::IRandomAccessReader
{
public:
    explicit FilesystemRandomAccessReader(core::MappingMode mapping_mode = core::MappingMode::Auto);

    bool Open(const std::filesystem::path& path, std::string& error_message);
    void Close() noexcept;
    [[nodiscard]] bool IsOpen() const noexcept;

    [[nodiscard]] std::uint64_t Size() const noexcept override;
    [[nodiscard]] storage::StorageCapabilities Capabilities() const noexcept override;

    bool Read(std::uint64_t offset,
              std::span<std::byte> output,
              std::size_t& bytes_read,
              std::string& error_message) override;

    bool TryMapWindow(std::uint64_t offset,
                      std::size_t length,
                      storage::MappedReadWindow& window,
                      std::string& error_message) override;

private:
    [[nodiscard]] bool MappingAllowed() const noexcept;
    [[nodiscard]] bool ShouldAttemptMapping(std::size_t length) const noexcept;
    bool EnsureMappedReader(std::string& error_message);

    std::filesystem::path path_;
    core::MappingMode mapping_mode_ = core::MappingMode::Auto;
    std::uint64_t size_ = 0;
    mutable std::mutex stream_mutex_;
    std::fstream stream_;
    MappedFileReader mapped_reader_;
    MappedView mapped_view_ {};
};

class FilesystemRandomAccessWriter final : public storage::IRandomAccessWriter
{
public:
    FilesystemRandomAccessWriter() = default;

    bool Open(const std::filesystem::path& path, std::string& error_message);
    void Close() noexcept;
    [[nodiscard]] bool IsOpen() const noexcept;

    [[nodiscard]] std::uint64_t Size() const noexcept override;
    [[nodiscard]] storage::StorageCapabilities Capabilities() const noexcept override;

    bool Write(std::uint64_t offset,
               std::span<const std::byte> data,
               std::string& error_message) override;

    bool Resize(std::uint64_t size, std::string& error_message) override;
    bool Flush(std::string& error_message) override;

private:
    bool ReopenStream(std::string& error_message);

    std::filesystem::path path_;
    std::uint64_t size_ = 0;
    std::uint64_t current_offset_ = 0;
    mutable std::mutex stream_mutex_;
    std::fstream stream_;
};
}
