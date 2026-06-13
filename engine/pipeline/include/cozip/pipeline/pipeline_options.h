#pragma once

#include <cstddef>

namespace cozip::pipeline
{
struct PipelineOptions
{
    std::size_t chunk_size_bytes = 1024 * 1024;
    std::size_t max_in_flight_chunks = 32;
    std::size_t reader_threads = 1;
    std::size_t compressor_threads = 0;
    std::size_t writer_threads = 1;
};

PipelineOptions DefaultPipelineOptions() noexcept;
}
