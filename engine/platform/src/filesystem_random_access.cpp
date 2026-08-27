#include "cozip/platform/filesystem_random_access.h"

#include <algorithm>
#include <system_error>

namespace cozip::platform
{
namespace
{
std::string PathToString(const std::filesystem::path& path)
{
    return path.generic_string();
}
}

FilesystemRandomAccessReader::FilesystemRandomAccessReader(core::MappingMode mapping_mode)
    : mapping_mode_(mapping_mode)
{
}

bool FilesystemRandomAccessReader::Open(const std::filesystem::path& path, std::string& error_message)
{
    Close();

    std::error_code size_error;
    size_ = std::filesystem::file_size(path, size_error);
    if (size_error)
    {
        error_message = "failed to stat input file: " + PathToString(path);
        return false;
    }

    stream_.open(path, std::ios::binary);
    if (!stream_)
    {
        error_message = "failed to open file for random read: " + PathToString(path);
        Close();
        return false;
    }

    path_ = path;
    if (mapping_mode_ == core::MappingMode::RequireOn && !EnsureMappedReader(error_message))
    {
        Close();
        return false;
    }

    return true;
}

void FilesystemRandomAccessReader::Close() noexcept
{
    {
        std::lock_guard lock(stream_mutex_);
        if (stream_.is_open())
        {
            stream_.close();
        }
    }

    mapped_reader_.Close();
    mapped_view_ = {};
    path_.clear();
    size_ = 0;
}

bool FilesystemRandomAccessReader::IsOpen() const noexcept
{
    return !path_.empty();
}

std::uint64_t FilesystemRandomAccessReader::Size() const noexcept
{
    return size_;
}

storage::StorageCapabilities FilesystemRandomAccessReader::Capabilities() const noexcept
{
    storage::StorageCapabilities capabilities {};
    capabilities.supports_random_read = true;
    capabilities.supports_mapping = MappingAllowed();
    capabilities.supports_parallel_reads = true;
    capabilities.preferred_window_bytes = 8u * 1024u * 1024u;
    capabilities.preferred_alignment_bytes = capabilities.supports_mapping ? 64u * 1024u : 0u;
    capabilities.max_efficient_read_bytes = 8u * 1024u * 1024u;
    return capabilities;
}

bool FilesystemRandomAccessReader::Read(std::uint64_t offset,
                                        std::span<std::byte> output,
                                        std::size_t& bytes_read,
                                        std::string& error_message)
{
    bytes_read = 0;

    if (!IsOpen())
    {
        error_message = "random access reader is not open";
        return false;
    }

    if (offset > size_)
    {
        error_message = "read offset is outside file";
        return false;
    }

    const auto clamped_size = static_cast<std::size_t>(std::min<std::uint64_t>(output.size(), size_ - offset));
    if (clamped_size == 0)
    {
        return true;
    }

    std::lock_guard lock(stream_mutex_);
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream_)
    {
        error_message = "failed to seek file for random read: " + PathToString(path_);
        return false;
    }

    stream_.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(clamped_size));
    const auto read_count = stream_.gcount();
    if (read_count < 0)
    {
        error_message = "failed to read file bytes: " + PathToString(path_);
        return false;
    }

    bytes_read = static_cast<std::size_t>(read_count);
    if (bytes_read != clamped_size)
    {
        error_message = "short read while reading file bytes: " + PathToString(path_);
        return false;
    }

    return true;
}

bool FilesystemRandomAccessReader::TryMapWindow(std::uint64_t offset,
                                                std::size_t length,
                                                storage::MappedReadWindow& window,
                                                std::string& error_message)
{
    window = {};

    if (!IsOpen() || !MappingAllowed() || !ShouldAttemptMapping(length))
    {
        return false;
    }

    if (!EnsureMappedReader(error_message))
    {
        if (mapping_mode_ == core::MappingMode::RequireOn)
        {
            return false;
        }

        error_message.clear();
        return false;
    }

    if (!mapped_reader_.MapWindow(offset, length, mapped_view_, error_message))
    {
        if (mapping_mode_ == core::MappingMode::RequireOn)
        {
            return false;
        }

        error_message.clear();
        return false;
    }

    window.offset = mapped_view_.offset;
    window.bytes = mapped_view_.bytes;
    return true;
}

bool FilesystemRandomAccessReader::MappingAllowed() const noexcept
{
    return mapping_mode_ != core::MappingMode::ForceOff;
}

bool FilesystemRandomAccessReader::ShouldAttemptMapping(std::size_t length) const noexcept
{
    switch (mapping_mode_)
    {
    case core::MappingMode::ForceOff:
        return false;
    case core::MappingMode::PreferOn:
    case core::MappingMode::RequireOn:
        return true;
    case core::MappingMode::Auto:
        return size_ >= (8u * 1024u * 1024u) || length >= (1u * 1024u * 1024u);
    }

    return false;
}

bool FilesystemRandomAccessReader::EnsureMappedReader(std::string& error_message)
{
    if (mapped_reader_.IsOpen())
    {
        return true;
    }

    return mapped_reader_.Open(path_, error_message);
}

bool FilesystemRandomAccessWriter::Open(const std::filesystem::path& path, std::string& error_message)
{
    Close();
    path_ = path;
    size_ = 0;
    current_offset_ = 0;
    return ReopenStream(error_message);
}

void FilesystemRandomAccessWriter::Close() noexcept
{
    std::lock_guard lock(stream_mutex_);
    if (stream_.is_open())
    {
        stream_.flush();
        stream_.close();
    }

    path_.clear();
    size_ = 0;
    current_offset_ = 0;
}

bool FilesystemRandomAccessWriter::IsOpen() const noexcept
{
    return !path_.empty();
}

std::uint64_t FilesystemRandomAccessWriter::Size() const noexcept
{
    return size_;
}

storage::StorageCapabilities FilesystemRandomAccessWriter::Capabilities() const noexcept
{
    storage::StorageCapabilities capabilities {};
    capabilities.supports_random_write = true;
    capabilities.requires_exclusive_access = true;
    capabilities.durable_flush = true;
    return capabilities;
}

bool FilesystemRandomAccessWriter::Write(std::uint64_t offset,
                                         std::span<const std::byte> data,
                                         std::string& error_message)
{
    if (!IsOpen())
    {
        error_message = "random access writer is not open";
        return false;
    }

    std::lock_guard lock(stream_mutex_);
    if (offset != current_offset_)
    {
        stream_.clear();
        stream_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!stream_)
        {
            error_message = "failed to seek file for random write: " + PathToString(path_);
            return false;
        }

        current_offset_ = offset;
    }

    stream_.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!stream_)
    {
        error_message = "failed to write file bytes: " + PathToString(path_);
        return false;
    }

    current_offset_ = offset + data.size();
    size_ = std::max<std::uint64_t>(size_, offset + data.size());
    return true;
}

bool FilesystemRandomAccessWriter::Resize(std::uint64_t size, std::string& error_message)
{
    if (!IsOpen())
    {
        error_message = "random access writer is not open";
        return false;
    }

    {
        std::lock_guard lock(stream_mutex_);
        if (stream_.is_open())
        {
            stream_.flush();
            stream_.close();
        }
    }

    std::error_code resize_error;
    std::filesystem::resize_file(path_, size, resize_error);
    if (resize_error)
    {
        error_message = "failed to resize output file: " + PathToString(path_);
        return false;
    }

    size_ = size;
    current_offset_ = 0;
    return ReopenStream(error_message);
}

bool FilesystemRandomAccessWriter::Flush(std::string& error_message)
{
    if (!IsOpen())
    {
        error_message = "random access writer is not open";
        return false;
    }

    std::lock_guard lock(stream_mutex_);
    stream_.flush();
    if (!stream_)
    {
        error_message = "failed to flush output file: " + PathToString(path_);
        return false;
    }

    return true;
}

bool FilesystemRandomAccessWriter::ReopenStream(std::string& error_message)
{
    std::lock_guard lock(stream_mutex_);
    stream_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream_)
    {
        stream_.clear();
        std::ofstream create(path_, std::ios::binary | std::ios::trunc);
        if (!create)
        {
            error_message = "failed to create output file: " + PathToString(path_);
            return false;
        }
        create.close();

        stream_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
        if (!stream_)
        {
            error_message = "failed to reopen output file: " + PathToString(path_);
            return false;
        }
    }

    current_offset_ = 0;
    return true;
}
}
