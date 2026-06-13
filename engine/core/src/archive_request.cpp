#include "cozip/core/archive_request.h"

namespace cozip::core
{
Operation ToOperation(JobType type) noexcept
{
    switch (type)
    {
    case JobType::CreateArchive:
        return Operation::Create;
    case JobType::ExtractArchive:
        return Operation::Extract;
    case JobType::ListArchive:
        return Operation::List;
    case JobType::TestArchive:
        return Operation::Test;
    }

    return Operation::Create;
}

JobType ToJobType(Operation operation) noexcept
{
    switch (operation)
    {
    case Operation::Create:
        return JobType::CreateArchive;
    case Operation::Extract:
        return JobType::ExtractArchive;
    case Operation::List:
        return JobType::ListArchive;
    case Operation::Test:
        return JobType::TestArchive;
    }

    return JobType::CreateArchive;
}

ArchiveRequest MakeArchiveRequest(const ArchiveJob& job) noexcept
{
    ArchiveRequest request {};
    request.operation = ToOperation(job.type);
    request.format = job.format;
    request.profile = job.profile;
    request.execution = ResolveExecutionOptions(job);
    request.output_path = job.output_path;
    request.inputs.reserve(job.inputs.size());
    for (const auto& input : job.inputs)
    {
        request.inputs.push_back({
            .kind = ArchiveSourceKind::Path,
            .path = input.path,
            .recursive = input.recursive,
            .archive_path = {},
            .reader = nullptr,
        });
    }
    return request;
}

ArchiveExecutionRequest MakeArchiveExecutionRequest(const ArchiveJob& job) noexcept
{
    ArchiveExecutionRequest request {};
    request.archive = MakeArchiveRequest(job);
    return request;
}

ArchiveExecutionRequest MakeArchiveExecutionRequest(const ArchiveRequest& request) noexcept
{
    ArchiveExecutionRequest execution_request {};
    execution_request.archive = request;
    return execution_request;
}

ArchiveJob MakeArchiveJob(const ArchiveRequest& request) noexcept
{
    ArchiveJob job {};
    job.type = ToJobType(request.operation);
    job.format = request.format;
    job.profile = request.profile;
    job.execution = request.execution;
    job.output_path = request.output_path;
    job.inputs.reserve(request.inputs.size());
    for (const auto& input : request.inputs)
    {
        if (input.kind != ArchiveSourceKind::Path)
        {
            continue;
        }

        job.inputs.push_back({input.path, input.recursive});
    }
    return job;
}

bool IsValid(const ArchiveRequest& request) noexcept
{
    if (request.inputs.empty())
    {
        return false;
    }

    for (const auto& input : request.inputs)
    {
        switch (input.kind)
        {
        case ArchiveSourceKind::Path:
            if (input.path.empty())
            {
                return false;
            }
            break;
        case ArchiveSourceKind::ReaderFile:
            if (input.reader == nullptr || input.archive_path.empty() || input.recursive)
            {
                return false;
            }
            break;
        }
    }

    const auto path_only_request = [&]() {
        ArchiveRequest filtered = request;
        filtered.inputs.clear();
        for (const auto& input : request.inputs)
        {
            if (input.kind == ArchiveSourceKind::Path)
            {
                filtered.inputs.push_back(input);
            }
        }
        return filtered;
    }();

    if (!path_only_request.inputs.empty())
    {
        return IsValid(MakeArchiveJob(path_only_request));
    }

    if (request.operation == Operation::Create)
    {
        return !request.output_path.empty();
    }

    return request.inputs.size() == 1;
}

bool IsValid(const ArchiveExecutionRequest& request) noexcept
{
    return IsValid(request.archive);
}
}
