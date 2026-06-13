#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cozip/core/execution_options.h"

namespace cozip::core
{
enum class JobType : std::uint8_t
{
    CreateArchive,
    ExtractArchive,
    ListArchive,
    TestArchive,
};

enum class CompressionProfile : std::uint8_t
{
    Store,
    Fast,
    Balanced,
    Small,
    Maximum,
};

enum class ArchiveFormat : std::uint8_t
{
    Zip,
};

struct ArchiveInput
{
    std::string path;
    bool recursive = true;
};

struct ArchiveJob
{
    JobType type = JobType::CreateArchive;
    ArchiveFormat format = ArchiveFormat::Zip;
    CompressionProfile profile = CompressionProfile::Fast;
    ExecutionOptions execution {};
    std::vector<ArchiveInput> inputs;
    std::string output_path;
};

bool IsValid(const ArchiveJob& job) noexcept;
ExecutionOptions ResolveExecutionOptions(const ArchiveJob& job) noexcept;
const char* ToString(ArchiveFormat format) noexcept;
}
