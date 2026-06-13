#pragma once

#include <string>

#include "cozip/core/archive_job.h"
#include "cozip/pipeline/pipeline_options.h"

namespace cozip::pipeline
{
struct PipelinePlan
{
    PipelineOptions options;
    std::size_t input_count = 0;
    std::size_t memory_budget_mb = 0;
    std::string summary;
};

PipelinePlan BuildPipelinePlan(const core::ArchiveJob& job);
}
