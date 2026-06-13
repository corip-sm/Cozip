#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace cozip::platform
{
struct MappedView
{
    std::uint64_t offset = 0;
    std::span<const std::byte> bytes {};
};

struct WritableMappedView
{
    std::uint64_t offset = 0;
    std::span<std::byte> bytes {};
};

class MappedFileReader
{
public:
    MappedFileReader();
    ~MappedFileReader();

    MappedFileReader(const MappedFileReader&) = delete;
    MappedFileReader& operator=(const MappedFileReader&) = delete;
    MappedFileReader(MappedFileReader&&) noexcept;
    MappedFileReader& operator=(MappedFileReader&&) noexcept;

    bool Open(const std::filesystem::path& path, std::string& error_message);
    void Close() noexcept;

    [[nodiscard]] bool IsOpen() const noexcept;
    [[nodiscard]] std::uint64_t Size() const noexcept;

    bool MapWindow(std::uint64_t offset,
                   std::size_t length,
                   MappedView& view,
                   std::string& error_message);

private:
    struct Impl;

    std::filesystem::path path_;
    std::uint64_t size_ = 0;
    std::unique_ptr<Impl> impl_;
    std::vector<std::byte> scratch_;
};

class MappedFileWriter
{
public:
    MappedFileWriter();
    ~MappedFileWriter();

    MappedFileWriter(const MappedFileWriter&) = delete;
    MappedFileWriter& operator=(const MappedFileWriter&) = delete;
    MappedFileWriter(MappedFileWriter&&) noexcept;
    MappedFileWriter& operator=(MappedFileWriter&&) noexcept;

    bool Open(const std::filesystem::path& path,
              std::uint64_t size,
              std::string& error_message);
    void Close() noexcept;

    [[nodiscard]] bool IsOpen() const noexcept;
    [[nodiscard]] std::uint64_t Size() const noexcept;

    bool MapWindow(std::uint64_t offset,
                   std::size_t length,
                   WritableMappedView& view,
                   std::string& error_message);

private:
    struct Impl;

    std::filesystem::path path_;
    std::uint64_t size_ = 0;
    std::unique_ptr<Impl> impl_;
};
}
