#pragma once

#include "cozip/storage/storage_factory.h"

namespace cozip::platform
{
class FilesystemStorageFactory final : public storage::IStorageFactory
{
public:
    [[nodiscard]] std::unique_ptr<storage::IRandomAccessReader> OpenReader(
        const std::filesystem::path& path,
        core::MappingMode mapping_mode,
        std::string& error_message) override;

    [[nodiscard]] std::unique_ptr<storage::IRandomAccessWriter> OpenWriter(
        const std::filesystem::path& path,
        std::string& error_message) override;
};
}
