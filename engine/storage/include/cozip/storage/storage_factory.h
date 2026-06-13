#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "cozip/core/execution_options.h"
#include "cozip/storage/random_access.h"

namespace cozip::storage
{
class IStorageFactory
{
public:
    virtual ~IStorageFactory() = default;

    [[nodiscard]] virtual std::unique_ptr<IRandomAccessReader> OpenReader(
        const std::filesystem::path& path,
        core::MappingMode mapping_mode,
        std::string& error_message) = 0;

    [[nodiscard]] virtual std::unique_ptr<IRandomAccessWriter> OpenWriter(
        const std::filesystem::path& path,
        std::string& error_message) = 0;
};
}
