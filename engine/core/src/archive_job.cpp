#include "cozip/core/archive_job.h"

namespace cozip::core
{
bool IsValid(const ArchiveJob& job) noexcept
{
    if (job.type == JobType::CreateArchive)
    {
        return !job.inputs.empty() && !job.output_path.empty();
    }

    if (job.type == JobType::ExtractArchive)
    {
        return !job.inputs.empty();
    }

    return true;
}

ExecutionOptions ResolveExecutionOptions(const ArchiveJob& job) noexcept
{
    return job.execution;
}

const char* ToString(ArchiveFormat format) noexcept
{
    switch (format)
    {
    case ArchiveFormat::Zip:
        return "zip";
    }

    return "unknown";
}

}
