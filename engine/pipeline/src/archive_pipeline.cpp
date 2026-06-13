#include "cozip/pipeline/archive_pipeline.h"

#include <algorithm>
#include <sstream>
#include <thread>

namespace cozip::pipeline
{
PipelinePlan BuildPipelinePlan(const core::ArchiveJob& job)
{
    const auto execution = core::ResolveExecutionOptions(job);
    PipelinePlan plan {};
    plan.options = DefaultPipelineOptions();
    plan.input_count = job.inputs.size();
    plan.memory_budget_mb = execution.memory_budget_mb;

    if (execution.worker_count > 0)
    {
        plan.options.compressor_threads = std::max<std::size_t>(1, execution.worker_count);
    }

    if (execution.chunk_size_bytes > 0)
    {
        plan.options.chunk_size_bytes = execution.chunk_size_bytes;
    }

    const auto chunk_budget_bytes = execution.memory_budget_mb * 1024ull * 1024ull;
    if (chunk_budget_bytes > 0)
    {
        const auto max_chunks = chunk_budget_bytes / plan.options.chunk_size_bytes;
        if (max_chunks > 0)
        {
            plan.options.max_in_flight_chunks = std::max<std::size_t>(
                4,
                std::min<std::size_t>(128, static_cast<std::size_t>(max_chunks)));
        }
    }

    std::ostringstream stream;
    stream << "pipeline chunk=" << plan.options.chunk_size_bytes
           << " inflight=" << plan.options.max_in_flight_chunks
           << " reader_threads=" << plan.options.reader_threads
           << " compressor_threads=" << plan.options.compressor_threads
           << " writer_threads=" << plan.options.writer_threads;
    plan.summary = stream.str();

    return plan;
}
}
