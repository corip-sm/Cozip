#include "cozip/pipeline/pipeline_options.h"

#include <thread>

namespace cozip::pipeline
{
PipelineOptions DefaultPipelineOptions() noexcept
{
    PipelineOptions options {};
    const auto hardware_threads = std::thread::hardware_concurrency();
    const auto available_threads = hardware_threads == 0 ? 4u : hardware_threads;

    options.compressor_threads = available_threads > 2 ? available_threads - 2 : 1;
    return options;
}
}
