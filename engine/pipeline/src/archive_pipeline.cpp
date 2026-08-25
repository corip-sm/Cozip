#include "cozip/pipeline/archive_pipeline.h"

#include <algorithm>
#include <limits>
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

    constexpr std::size_t kBytesPerMegabyte = 1024u * 1024u;
    const auto chunk_budget_bytes = execution.memory_budget_mb >
            std::numeric_limits<std::size_t>::max() / kBytesPerMegabyte
        ? std::numeric_limits<std::size_t>::max()
        : execution.memory_budget_mb * kBytesPerMegabyte;
    if (chunk_budget_bytes > 0)
    {
        const auto per_chunk_bytes = plan.options.chunk_size_bytes >
                std::numeric_limits<std::size_t>::max() / 2
            ? std::numeric_limits<std::size_t>::max()
            : plan.options.chunk_size_bytes * 2;
        const auto max_chunks = chunk_budget_bytes / per_chunk_bytes;
        if (max_chunks > 0)
        {
            plan.options.max_in_flight_chunks = std::max<std::size_t>(
                1,
                std::min<std::size_t>(128, static_cast<std::size_t>(max_chunks)));
        }
        else
        {
            plan.options.max_in_flight_chunks = 1;
        }
    }

    if (execution.max_in_flight_chunks > 0)
    {
        plan.options.max_in_flight_chunks =
            std::clamp<std::size_t>(execution.max_in_flight_chunks, 1, 128);
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
