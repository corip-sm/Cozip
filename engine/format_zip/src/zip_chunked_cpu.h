#pragma once

#include "zip_archive_internal.h"

namespace cozip::format_zip
{
struct ChunkedCpuEntryMetrics
{
    std::string archive_path;
    std::uint64_t raw_bytes = 0;
    std::size_t chunk_size_bytes = 0;
    std::size_t chunk_count = 0;
    std::uint64_t read_ns = 0;
    std::uint64_t deflate_ns = 0;
    std::uint64_t write_ns = 0;
};

struct ChunkedCpuSchedulerMetrics
{
    std::uint64_t wall_ns = 0;
    std::size_t max_active_tasks = 0;
    std::size_t max_in_flight_chunks = 0;
    std::size_t max_in_flight_raw_bytes = 0;
    std::size_t max_in_flight_compressed_bytes = 0;
    std::size_t max_in_flight_payload_bytes = 0;
    std::size_t max_reserved_bytes = 0;
    std::size_t entries_started_before_first_completed = 0;
    std::vector<ChunkedCpuEntryMetrics> entries;
};

class ChunkedCpuCreateScheduler final
{
public:
    ~ChunkedCpuCreateScheduler();

    ChunkedCpuCreateScheduler(ChunkedCpuCreateScheduler&&) noexcept;
    ChunkedCpuCreateScheduler& operator=(ChunkedCpuCreateScheduler&&) noexcept;
    ChunkedCpuCreateScheduler(const ChunkedCpuCreateScheduler&) = delete;
    ChunkedCpuCreateScheduler& operator=(const ChunkedCpuCreateScheduler&) = delete;

    [[nodiscard]] bool Handles(const ZipEntrySource& entry) const noexcept;
    ZipOperationResult WriteEntry(std::ostream& output, ZipEntrySource& entry);
    [[nodiscard]] ChunkedCpuSchedulerMetrics Metrics() const;

private:
    class Impl;
    explicit ChunkedCpuCreateScheduler(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;

    friend ZipOperationResult CreateChunkedCpuScheduler(
        std::span<ZipEntrySource>,
        const pipeline::PipelineOptions&,
        std::size_t,
        const core::ExecutionContext&,
        std::unique_ptr<ChunkedCpuCreateScheduler>&);
};

ZipOperationResult CreateChunkedCpuScheduler(
    std::span<ZipEntrySource> entries,
    const pipeline::PipelineOptions& pipeline_options,
    std::size_t memory_budget_mb,
    const core::ExecutionContext& context,
    std::unique_ptr<ChunkedCpuCreateScheduler>& scheduler);

#if defined(COZIP_ENABLE_TEST_HOOKS)
void SetChunkCompressionFailureForTesting(std::size_t chunk_index) noexcept;
void ClearChunkCompressionFailureForTesting() noexcept;
ChunkedCpuSchedulerMetrics LastChunkedCpuSchedulerMetricsForTesting();
#endif
}
