#pragma once

#include <string>

#include "cozip/core/archive_job.h"
#include "cozip/core/archive_request.h"

namespace cozip::format_zip
{
enum class ZipStatus
{
    Ok,
    Cancelled,
    InvalidJob,
    IoError,
    NotFound,
    Unsupported,
};

struct ZipOperationResult
{
    ZipStatus status = ZipStatus::Ok;
    std::string message;
};

struct ZipExecutionRequest
{
    const core::ArchiveJob* job = nullptr;
    const core::ArchiveRequest* archive_request = nullptr;
    core::ExecutionOptions execution {};
    core::ExecutionContext context {};
};

ZipExecutionRequest MakeExecutionRequest(const core::ArchiveJob& job) noexcept;
ZipExecutionRequest MakeExecutionRequest(const core::ArchiveRequest& request) noexcept;
ZipExecutionRequest MakeExecutionRequest(const core::ArchiveExecutionRequest& request) noexcept;
ZipOperationResult Execute(const core::ArchiveRequest& request);
ZipOperationResult Execute(const core::ArchiveExecutionRequest& request);
ZipOperationResult Execute(const ZipExecutionRequest& request);
ZipOperationResult Execute(const core::ArchiveJob& job);
const char* ToString(ZipStatus status) noexcept;
}
