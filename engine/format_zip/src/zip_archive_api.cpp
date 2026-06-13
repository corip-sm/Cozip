#include "zip_archive_internal.h"

namespace cozip::format_zip
{
ZipExecutionRequest MakeExecutionRequest(const core::ArchiveJob& job) noexcept
{
    return {
        .job = &job,
        .archive_request = nullptr,
        .execution = core::ResolveExecutionOptions(job),
        .context = {},
    };
}

ZipExecutionRequest MakeExecutionRequest(const core::ArchiveRequest& request) noexcept
{
    static thread_local core::ArchiveJob adapted_job {};
    adapted_job = core::MakeArchiveJob(request);
    auto execution_request = MakeExecutionRequest(adapted_job);
    execution_request.archive_request = &request;
    return execution_request;
}

ZipExecutionRequest MakeExecutionRequest(const core::ArchiveExecutionRequest& request) noexcept
{
    auto execution_request = MakeExecutionRequest(request.archive);
    execution_request.context = request.context;
    return execution_request;
}

ZipOperationResult Execute(const ZipExecutionRequest& request)
{
    if (request.job == nullptr)
    {
        return {ZipStatus::InvalidJob, "zip execution request is missing archive job"};
    }

    auto job = *request.job;
    const auto* archive_request = request.archive_request;
    job.execution = request.execution;
    const auto& context = request.context;

    if ((archive_request == nullptr && !core::IsValid(job)) ||
        (archive_request != nullptr && !core::IsValid(*archive_request)))
    {
        return {ZipStatus::InvalidJob, archive_request == nullptr ? "invalid archive job" : "invalid archive request"};
    }

    if (IsCancellationRequested(context))
    {
        return MakeCancelled(DescribeJobOperation(job.type) + " cancelled before start");
    }

    ReportProgress(
        context,
        {
            .phase = core::ProgressPhase::Started,
            .completed_items = 0,
            .total_items = archive_request != nullptr ? archive_request->inputs.size() : job.inputs.size(),
            .message = DescribeJobOperation(job.type) + " started",
        });

    ZipOperationResult result {ZipStatus::Unsupported, "unsupported zip operation"};
    switch (job.type)
    {
    case core::JobType::CreateArchive:
        result = CreateZipArchive(job, archive_request, context);
        break;
    case core::JobType::ExtractArchive:
        result = ExtractArchive(job, context);
        break;
    case core::JobType::ListArchive:
        result = ListArchive(job, context);
        break;
    case core::JobType::TestArchive:
        result = TestArchive(job, context);
        break;
    }

    if (result.status == ZipStatus::Ok)
    {
        ReportProgress(
            context,
            {
                .phase = core::ProgressPhase::Completed,
                .completed_items = archive_request != nullptr ? archive_request->inputs.size() : job.inputs.size(),
                .total_items = archive_request != nullptr ? archive_request->inputs.size() : job.inputs.size(),
                .message = DescribeJobOperation(job.type) + " completed",
            });
    }
    else if (result.status != ZipStatus::Cancelled)
    {
        ReportDiagnostic(context, core::DiagnosticSeverity::Error, result.message);
    }

    return result;
}

ZipOperationResult Execute(const core::ArchiveRequest& request)
{
    if (!core::IsValid(request))
    {
        return {ZipStatus::InvalidJob, "invalid archive request"};
    }

    if (request.format != core::ArchiveFormat::Zip)
    {
        return {ZipStatus::Unsupported, "unsupported archive format for zip executor"};
    }

    return Execute(MakeExecutionRequest(request));
}

ZipOperationResult Execute(const core::ArchiveExecutionRequest& request)
{
    if (!core::IsValid(request))
    {
        return {ZipStatus::InvalidJob, "invalid archive execution request"};
    }

    if (request.archive.format != core::ArchiveFormat::Zip)
    {
        return {ZipStatus::Unsupported, "unsupported archive format for zip executor"};
    }

    return Execute(MakeExecutionRequest(request));
}

ZipOperationResult Execute(const core::ArchiveJob& job)
{
    return Execute(MakeExecutionRequest(job));
}

const char* ToString(ZipStatus status) noexcept
{
    switch (status)
    {
    case ZipStatus::Ok:
        return "ok";
    case ZipStatus::Cancelled:
        return "cancelled";
    case ZipStatus::InvalidJob:
        return "invalid_job";
    case ZipStatus::IoError:
        return "io_error";
    case ZipStatus::NotFound:
        return "not_found";
    case ZipStatus::Unsupported:
        return "unsupported";
    }

    return "unknown";
}
}
