#include "cozip/platform/filesystem_storage_factory.h"

#include "cozip/platform/filesystem_random_access.h"

namespace cozip::platform
{
std::unique_ptr<storage::IRandomAccessReader> FilesystemStorageFactory::OpenReader(
    const std::filesystem::path& path,
    core::MappingMode mapping_mode,
    std::string& error_message)
{
    auto reader = std::make_unique<FilesystemRandomAccessReader>(mapping_mode);
    if (!reader->Open(path, error_message))
    {
        return {};
    }

    return reader;
}

std::unique_ptr<storage::IRandomAccessWriter> FilesystemStorageFactory::OpenWriter(
    const std::filesystem::path& path,
    std::string& error_message)
{
    auto writer = std::make_unique<FilesystemRandomAccessWriter>();
    if (!writer->Open(path, error_message))
    {
        return {};
    }

    return writer;
}
}
