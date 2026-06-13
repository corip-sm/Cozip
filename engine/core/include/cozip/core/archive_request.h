#pragma once

#include <string>
#include <vector>

#include "cozip/core/archive_job.h"
#include "cozip/core/execution_context.h"

namespace cozip::storage
{
class IRandomAccessReader;
}

namespace cozip::core
{
enum class Operation : std::uint8_t
{
    Create,
    Extract,
    List,
    Test,
};

enum class ArchiveSourceKind : std::uint8_t
{
    Path,
    ReaderFile,
};

struct ArchiveSource
{
    ArchiveSourceKind kind = ArchiveSourceKind::Path;
    std::string path;
    bool recursive = true;
    std::string archive_path;
    storage::IRandomAccessReader* reader = nullptr;
};

struct ArchiveRequest
{
    Operation operation = Operation::Create;
    ArchiveFormat format = ArchiveFormat::Zip;
    CompressionProfile profile = CompressionProfile::Fast;
    ExecutionOptions execution {};
    std::vector<ArchiveSource> inputs;
    std::string output_path;
};

struct ArchiveExecutionRequest
{
    ArchiveRequest archive {};
    ExecutionContext context {};
};

Operation ToOperation(JobType type) noexcept;
JobType ToJobType(Operation operation) noexcept;
ArchiveRequest MakeArchiveRequest(const ArchiveJob& job) noexcept;
ArchiveExecutionRequest MakeArchiveExecutionRequest(const ArchiveJob& job) noexcept;
ArchiveExecutionRequest MakeArchiveExecutionRequest(const ArchiveRequest& request) noexcept;
ArchiveJob MakeArchiveJob(const ArchiveRequest& request) noexcept;
bool IsValid(const ArchiveRequest& request) noexcept;
bool IsValid(const ArchiveExecutionRequest& request) noexcept;
}
