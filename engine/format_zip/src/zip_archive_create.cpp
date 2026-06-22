#include "zip_archive_internal.h"
#include "zip_chunked_cpu.h"

namespace cozip::format_zip
{
namespace
{
enum class ZipCreateExecutionPath
{
    Directory,
    Prepared,
    Store,
    WholeBufferLibdeflate,
    ChunkedCpu,
    StreamDeflate,
};

struct ZipChunkPlan
{
    std::size_t index = 0;
    std::uint64_t offset = 0;
    std::size_t size = 0;
};

struct ZipCreateExecutionPlan
{
    ZipCreateExecutionPath path = ZipCreateExecutionPath::StreamDeflate;
    std::size_t chunk_size_bytes = 0;
    std::vector<ZipChunkPlan> chunks;
};

constexpr std::size_t kAdaptiveStoreSampleBytes = 2u * 1024u * 1024u;
constexpr std::size_t kChunkedCpuMinFileBytes = 256u * 1024u * 1024u;
constexpr std::size_t kChunkedCpuMinChunkCount = 8;
constexpr std::uint32_t kAdaptiveStoreMinFileBytes = 8u * 1024u * 1024u;
constexpr std::uint32_t kAdaptiveStoreThresholdX1000 = 980u;
constexpr std::uint32_t kAdaptiveStoreFastLargeFileThresholdX1000 = 830u;
constexpr std::uint32_t kAdaptiveStoreFastLargeFileBytes = 64u * 1024u * 1024u;

bool IsAdaptiveStoreCandidate(const ZipEntrySource& entry) noexcept
{
    return !entry.is_directory &&
        entry.method == codecs::ZipMethod::Deflate &&
        entry.size >= kAdaptiveStoreMinFileBytes;
}

std::uint32_t ResolveAdaptiveStoreThresholdX1000(const ZipEntrySource& entry) noexcept
{
    if (entry.compression_profile == core::CompressionProfile::Fast &&
        entry.size >= kAdaptiveStoreFastLargeFileBytes)
    {
        return kAdaptiveStoreFastLargeFileThresholdX1000;
    }

    return kAdaptiveStoreThresholdX1000;
}

bool SampleLooksIncompressible(const ZipEntrySource& entry, std::span<const std::byte> sample)
{
    if (sample.size() < (256u * 1024u))
    {
        return false;
    }

    auto compressed = codecs::CompressDeflateBuffer(sample, core::CompressionProfile::Fast);
    if (!compressed.success)
    {
        return false;
    }

    const auto threshold_x1000 = ResolveAdaptiveStoreThresholdX1000(entry);
    const auto incompressible =
        compressed.bytes.size() * 1000u >= sample.size() * threshold_x1000;
    if (ZipTimingTraceEnabled())
    {
        std::ostringstream stream;
        stream << "sample-check entry=\"" << entry.archive_path << "\""
               << " sample=" << sample.size()
               << " compressed=" << compressed.bytes.size()
               << " threshold_x1000=" << threshold_x1000
               << " result=" << (incompressible ? "store" : "deflate");
        EmitZipTimingTrace(stream.str());
    }

    return incompressible;
}

bool ShouldWholeBufferPrecompress(const ZipEntrySource& entry,
                                  const pipeline::PipelineOptions& pipeline_options,
                                  std::size_t memory_budget_mb) noexcept
{
    if (entry.is_directory || entry.method != codecs::ZipMethod::Deflate)
    {
        return false;
    }

    const auto whole_buffer_threshold =
        ResolveLibdeflateWholeBufferThreshold(pipeline_options, memory_budget_mb);
    return entry.size > 0 && entry.size <= whole_buffer_threshold;
}

const char* ToString(ZipCreateExecutionPath path) noexcept
{
    switch (path)
    {
    case ZipCreateExecutionPath::Directory:
        return "directory";
    case ZipCreateExecutionPath::Prepared:
        return "prepared";
    case ZipCreateExecutionPath::Store:
        return "store";
    case ZipCreateExecutionPath::WholeBufferLibdeflate:
        return "whole-buffer-libdeflate";
    case ZipCreateExecutionPath::ChunkedCpu:
        return "chunked-cpu";
    case ZipCreateExecutionPath::StreamDeflate:
        return "stream-deflate";
    }

    return "unknown";
}

std::vector<ZipChunkPlan> BuildChunkPlan(const ZipEntrySource& entry,
                                         std::size_t chunk_size_bytes)
{
    std::vector<ZipChunkPlan> chunks;
    if (entry.is_directory || entry.size == 0 || chunk_size_bytes == 0)
    {
        return chunks;
    }

    const auto file_size = static_cast<std::uint64_t>(entry.size);
    chunks.reserve(static_cast<std::size_t>((file_size + chunk_size_bytes - 1) / chunk_size_bytes));

    std::uint64_t offset = 0;
    std::size_t index = 0;
    while (offset < file_size)
    {
        const auto remaining = file_size - offset;
        const auto size = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(chunk_size_bytes)));
        chunks.push_back(ZipChunkPlan {
            .index = index,
            .offset = offset,
            .size = size,
        });
        offset += static_cast<std::uint64_t>(size);
        ++index;
    }

    return chunks;
}

bool ShouldUseChunkedCpu(const ZipEntrySource& entry,
                         const ZipCreateExecutionPlan& plan) noexcept
{
    const char* disabled = std::getenv("COZIP_DISABLE_CHUNKED_CPU");
    if (disabled != nullptr && disabled[0] != '\0' && disabled[0] != '0')
    {
        return false;
    }

    if (entry.is_directory || entry.method != codecs::ZipMethod::Deflate)
    {
        return false;
    }

    if (entry.compression_profile != core::CompressionProfile::Fast)
    {
        return false;
    }

    if (entry.size < kChunkedCpuMinFileBytes)
    {
        return false;
    }

    return plan.chunks.size() >= kChunkedCpuMinChunkCount;
}

std::size_t ResolveChunkedCpuChunkSize(const ZipEntrySource& entry,
                                       const pipeline::PipelineOptions& pipeline_options) noexcept
{
    return ResolveZipStreamChunkSize(entry, pipeline_options);
}

ZipCreateExecutionPlan BuildExecutionPlan(const ZipEntrySource& entry,
                                          const pipeline::PipelineOptions& pipeline_options,
                                          std::size_t memory_budget_mb) noexcept
{
    ZipCreateExecutionPlan plan {};
    plan.chunk_size_bytes = ResolveChunkedCpuChunkSize(entry, pipeline_options);
    plan.chunks = BuildChunkPlan(entry, plan.chunk_size_bytes);

    if (entry.is_directory)
    {
        plan.path = ZipCreateExecutionPath::Directory;
        return plan;
    }

    if (!entry.prepared_data.empty())
    {
        plan.path = ZipCreateExecutionPath::Prepared;
        return plan;
    }

    if (entry.method == codecs::ZipMethod::Store)
    {
        plan.path = ZipCreateExecutionPath::Store;
        return plan;
    }

    if (ShouldUseChunkedCpu(entry, plan))
    {
        plan.path = ZipCreateExecutionPath::ChunkedCpu;
        return plan;
    }

    if (ShouldWholeBufferPrecompress(entry, pipeline_options, memory_budget_mb))
    {
        plan.path = ZipCreateExecutionPath::WholeBufferLibdeflate;
        return plan;
    }

    plan.path = ZipCreateExecutionPath::StreamDeflate;
    return plan;
}

void TraceExecutionPlan(const ZipEntrySource& entry,
                        const ZipCreateExecutionPlan& plan,
                        std::size_t memory_budget_mb)
{
    if (!ZipTimingTraceEnabled())
    {
        return;
    }

    std::ostringstream stream;
    stream << "entry-plan entry=\"" << entry.archive_path << "\""
           << " size=" << entry.size
           << " method=" << static_cast<int>(entry.method)
           << " path=" << ToString(plan.path)
           << " chunk=" << plan.chunk_size_bytes
           << " chunks=" << plan.chunks.size()
           << " memory_mb=" << memory_budget_mb;
    if (!plan.chunks.empty())
    {
        stream << " first_chunk=" << plan.chunks.front().size;
        stream << " last_chunk=" << plan.chunks.back().size;
    }
    EmitZipTimingTrace(stream.str());
}

ZipOperationResult MaybePreferStoreFromSample(ZipEntrySource& entry)
{
    if (entry.adaptive_store_evaluated)
    {
        return {ZipStatus::Ok, {}};
    }

    entry.adaptive_store_evaluated = true;

    if (!IsAdaptiveStoreCandidate(entry) || entry.storage_factory == nullptr)
    {
        return {ZipStatus::Ok, {}};
    }

    std::vector<std::byte> sample_bytes;
    auto sample_result = LoadFileSample(
        *entry.storage_factory,
        entry.source_path,
        kAdaptiveStoreSampleBytes,
        entry.mapping_mode,
        entry.source_reader,
        sample_bytes);
    if (sample_result.status != ZipStatus::Ok)
    {
        return sample_result;
    }

    if (SampleLooksIncompressible(entry, sample_bytes))
    {
        entry.method = codecs::ZipMethod::Store;
        entry.prepared_backend = codecs::DeflateBackend::None;
        entry.prepared_data.clear();
        entry.general_purpose_flag = 0u;
    }

    return {ZipStatus::Ok, {}};
}

pipeline::PipelineOptions TuneCreatePipelineOptions(const std::vector<ZipEntrySource>& entries,
                                                    pipeline::PipelineOptions options) noexcept
{
    std::uint64_t total_input_bytes = 0;
    std::uint64_t largest_file_bytes = 0;
    for (const auto& entry : entries)
    {
        if (entry.is_directory)
        {
            continue;
        }

        total_input_bytes += entry.size;
        largest_file_bytes = std::max<std::uint64_t>(largest_file_bytes, entry.size);
    }

    if (total_input_bytes >= (32ull * 1024ull * 1024ull * 1024ull) ||
        largest_file_bytes >= (4ull * 1024ull * 1024ull * 1024ull))
    {
        options.chunk_size_bytes = std::max<std::size_t>(options.chunk_size_bytes, 16u * 1024u * 1024u);
        options.max_in_flight_chunks = std::min<std::size_t>(options.max_in_flight_chunks, 24);
    }
    else if (total_input_bytes >= (8ull * 1024ull * 1024ull * 1024ull) ||
             largest_file_bytes >= (1024ull * 1024ull * 1024ull))
    {
        options.chunk_size_bytes = std::max<std::size_t>(options.chunk_size_bytes, 8u * 1024u * 1024u);
        options.max_in_flight_chunks = std::min<std::size_t>(options.max_in_flight_chunks, 32);
    }
    else if (total_input_bytes >= (2ull * 1024ull * 1024ull * 1024ull))
    {
        options.chunk_size_bytes = std::max<std::size_t>(options.chunk_size_bytes, 4u * 1024u * 1024u);
    }

    return options;
}
}

ZipOperationResult CompressSmallFileWithLibdeflate(std::ostream& output,
                                                   ZipEntrySource& entry)
{
    ScopedZipEntryTimer timer(entry);

    const auto load_started_at = std::chrono::steady_clock::now();
    WholeFileInput input;
    auto load_result = LoadWholeFileInput(
        *entry.storage_factory,
        entry.source_path,
        entry.mapping_mode,
        entry.source_reader,
        input);
    if (load_result.status != ZipStatus::Ok)
    {
        return load_result;
    }
    timer.AddPhase("load", std::chrono::steady_clock::now() - load_started_at);

    const auto deflate_started_at = std::chrono::steady_clock::now();
    auto compressed = codecs::CompressDeflateBuffer(
        input.bytes,
        entry.compression_profile);
    if (!compressed.success)
    {
        return MakeError(ZipStatus::IoError, compressed.error_message + ": " + entry.source_path.string());
    }
    timer.AddPhase("deflate", std::chrono::steady_clock::now() - deflate_started_at);

    if (compressed.has_crc32)
    {
        entry.crc32 = compressed.crc32;
        timer.AddPhase("crc", std::chrono::steady_clock::duration::zero());
    }
    else
    {
        const auto crc_started_at = std::chrono::steady_clock::now();
        Crc32 crc;
        if (!input.bytes.empty())
        {
            crc.Update(input.bytes.data(), input.bytes.size());
        }
        entry.crc32 = crc.Finalize();
        timer.AddPhase("crc", std::chrono::steady_clock::now() - crc_started_at);
    }

    const auto write_started_at = std::chrono::steady_clock::now();
    output.write(
        reinterpret_cast<const char*>(compressed.bytes.data()),
        static_cast<std::streamsize>(compressed.bytes.size()));
    if (!output)
    {
        return MakeError(ZipStatus::IoError, "failed to write libdeflate output");
    }
    timer.AddPhase("write", std::chrono::steady_clock::now() - write_started_at);

    entry.size = static_cast<std::uint32_t>(input.bytes.size());
    entry.compressed_size = static_cast<std::uint32_t>(compressed.bytes.size());
    entry.prepared_backend = compressed.backend;
    timer.Finish();
    return {ZipStatus::Ok, {}};
}

ZipOperationResult PrepareSmallEntryWithLibdeflate(ZipEntrySource& entry)
{
    ScopedZipEntryTimer timer(entry);

    const auto load_started_at = std::chrono::steady_clock::now();
    WholeFileInput input;
    auto load_result = LoadWholeFileInput(
        *entry.storage_factory,
        entry.source_path,
        entry.mapping_mode,
        entry.source_reader,
        input);
    if (load_result.status != ZipStatus::Ok)
    {
        return load_result;
    }
    timer.AddPhase("load", std::chrono::steady_clock::now() - load_started_at);

    const auto deflate_started_at = std::chrono::steady_clock::now();
    auto compressed = codecs::CompressDeflateBuffer(
        input.bytes,
        entry.compression_profile);
    if (!compressed.success)
    {
        return MakeError(ZipStatus::IoError, compressed.error_message + ": " + entry.source_path.string());
    }
    timer.AddPhase("deflate", std::chrono::steady_clock::now() - deflate_started_at);

    if (compressed.has_crc32)
    {
        entry.crc32 = compressed.crc32;
        timer.AddPhase("crc", std::chrono::steady_clock::duration::zero());
    }
    else
    {
        const auto crc_started_at = std::chrono::steady_clock::now();
        Crc32 crc;
        if (!input.bytes.empty())
        {
            crc.Update(input.bytes.data(), input.bytes.size());
        }
        entry.crc32 = crc.Finalize();
        timer.AddPhase("crc", std::chrono::steady_clock::now() - crc_started_at);
    }

    entry.size = static_cast<std::uint32_t>(input.bytes.size());
    if (ShouldStorePreparedDeflateResult(input.bytes.size(), compressed.bytes.size()))
    {
        entry.method = codecs::ZipMethod::Store;
        entry.compressed_size = entry.size;
        entry.prepared_backend = codecs::DeflateBackend::None;
        entry.prepared_data.assign(input.bytes.begin(), input.bytes.end());
        timer.AddPhase("store_fallback", std::chrono::steady_clock::duration::zero());
    }
    else
    {
        entry.compressed_size = static_cast<std::uint32_t>(compressed.bytes.size());
        entry.prepared_backend = compressed.backend;
        entry.prepared_data = std::move(compressed.bytes);
    }
    entry.general_purpose_flag = 0u;
    timer.AddPhase("stage", std::chrono::steady_clock::duration::zero());
    timer.Finish();
    return {ZipStatus::Ok, {}};
}

ZipOperationResult PrepareEncryptedEntry(ZipEntrySource& entry,
                                         const core::ExecutionOptions& execution)
{
    if (entry.is_directory)
    {
        entry.crc32 = 0;
        entry.size = 0;
        entry.compressed_size = 0;
        entry.general_purpose_flag = 0u;
        entry.prepared_data.clear();
        return {ZipStatus::Ok, {}};
    }

    auto sample_result = MaybePreferStoreFromSample(entry);
    if (sample_result.status != ZipStatus::Ok)
    {
        return sample_result;
    }

    WholeFileInput input;
    auto load_result = LoadWholeFileInput(
        *entry.storage_factory,
        entry.source_path,
        entry.mapping_mode,
        entry.source_reader,
        input);
    if (load_result.status != ZipStatus::Ok)
    {
        return load_result;
    }

    Crc32 crc;
    if (!input.bytes.empty())
    {
        crc.Update(input.bytes.data(), input.bytes.size());
    }

    entry.crc32 = crc.Finalize();
    entry.size = static_cast<std::uint32_t>(input.bytes.size());
    entry.prepared_backend = codecs::DeflateBackend::None;

    std::vector<std::byte> compressed_bytes;
    if (entry.method == codecs::ZipMethod::Store)
    {
        compressed_bytes.assign(input.bytes.begin(), input.bytes.end());
    }
    else
    {
        auto compressed = codecs::CompressDeflateBuffer(input.bytes, entry.compression_profile);
        if (!compressed.success)
        {
            return MakeError(ZipStatus::IoError, compressed.error_message + ": " + entry.source_label);
        }

        if (ShouldStorePreparedDeflateResult(input.bytes.size(), compressed.bytes.size()))
        {
            entry.method = codecs::ZipMethod::Store;
            compressed_bytes.assign(input.bytes.begin(), input.bytes.end());
        }
        else
        {
            entry.prepared_backend = compressed.backend;
            compressed_bytes = std::move(compressed.bytes);
        }
    }

    auto encrypt_result = EncryptZipEntryPayload(compressed_bytes, entry, execution, entry.prepared_data);
    if (encrypt_result.status != ZipStatus::Ok)
    {
        return encrypt_result;
    }

    entry.compressed_size = static_cast<std::uint32_t>(entry.prepared_data.size());
    entry.general_purpose_flag = kEncryptedFlag;
    return {ZipStatus::Ok, {}};
}

ZipOperationResult PrecompressSmallEntries(std::vector<ZipEntrySource>& entries,
                                           const pipeline::PipelineOptions& pipeline_options,
                                           std::size_t memory_budget_mb)
{
    constexpr std::size_t kMinParallelCandidateCount = 2;
    constexpr std::size_t kMinParallelInputBytes = 8 * 1024 * 1024;
    constexpr std::size_t kPrecompressReserveBytes = 512ull * 1024ull * 1024ull;
    constexpr std::size_t kMinSinglePrecompressBytes = 256ull * 1024ull * 1024ull;

    std::vector<std::size_t> candidate_indexes;
    std::size_t prepared_input_bytes = 0;
    const auto memory_budget_bytes = memory_budget_mb * 1024ull * 1024ull;
    const auto precompress_budget =
        memory_budget_bytes == 0 ? 0 :
        memory_budget_bytes > kPrecompressReserveBytes
            ? std::max<std::size_t>(memory_budget_bytes / 2, memory_budget_bytes - kPrecompressReserveBytes)
            : memory_budget_bytes / 2;
    const auto whole_buffer_threshold =
        ResolveLibdeflateWholeBufferThreshold(pipeline_options, memory_budget_mb);

    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        auto& entry = entries[index];
        if (entry.is_directory || entry.method != codecs::ZipMethod::Deflate)
        {
            continue;
        }

        auto sample_result = MaybePreferStoreFromSample(entry);
        if (sample_result.status != ZipStatus::Ok)
        {
            return sample_result;
        }

        if (entry.method != codecs::ZipMethod::Deflate)
        {
            continue;
        }

        ZipCreateExecutionPlan candidate_plan {};
        candidate_plan.chunk_size_bytes = ResolveZipStreamChunkSize(entry, pipeline_options);
        candidate_plan.chunks = BuildChunkPlan(entry, candidate_plan.chunk_size_bytes);
        if (ShouldUseChunkedCpu(entry, candidate_plan))
        {
            continue;
        }

        if (entry.size == 0 || entry.size > whole_buffer_threshold)
        {
            continue;
        }

        if (precompress_budget > 0 &&
            prepared_input_bytes + entry.size > precompress_budget &&
            !candidate_indexes.empty())
        {
            continue;
        }

        candidate_indexes.push_back(index);
        prepared_input_bytes += entry.size;
    }

    if (candidate_indexes.empty())
    {
        return {ZipStatus::Ok, {}};
    }

    const bool allow_single_large_candidate =
        candidate_indexes.size() == 1 &&
        prepared_input_bytes >= kMinSinglePrecompressBytes;

    if ((!allow_single_large_candidate &&
         candidate_indexes.size() < kMinParallelCandidateCount) ||
        prepared_input_bytes < kMinParallelInputBytes)
    {
        return {ZipStatus::Ok, {}};
    }

    const auto worker_count = std::max<std::size_t>(
        1,
        std::min<std::size_t>(pipeline_options.compressor_threads, candidate_indexes.size()));
    std::atomic<std::size_t> next_index = 0;
    std::mutex error_mutex;
    bool failed = false;
    ZipOperationResult failure = {ZipStatus::Ok, {}};
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    for (std::size_t worker = 0; worker < worker_count; ++worker)
    {
        workers.emplace_back([&] {
            while (true)
            {
                if (failed)
                {
                    return;
                }

                const auto candidate_position = next_index.fetch_add(1);
                if (candidate_position >= candidate_indexes.size())
                {
                    return;
                }

                auto& entry = entries[candidate_indexes[candidate_position]];
                const auto result = PrepareSmallEntryWithLibdeflate(entry);
                if (result.status != ZipStatus::Ok)
                {
                    std::lock_guard lock(error_mutex);
                    if (!failed)
                    {
                        failed = true;
                        failure = result;
                    }
                    return;
                }
            }
        });
    }

    for (auto& worker : workers)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }

    if (failed)
    {
        return failure;
    }

    return {ZipStatus::Ok, {}};
}

ZipOperationResult StreamDeflateEntry(std::istream& input,
                                      std::ostream& output,
                                      ZipEntrySource& entry,
                                      const pipeline::PipelineOptions& pipeline_options)
{
    ScopedZipEntryTimer timer(entry);
    struct RawChunk
    {
        pipeline::BufferBlockPtr buffer;
        std::size_t size = 0;
        bool is_last = false;
    };

    struct CompressedChunk
    {
        pipeline::BufferBlockPtr buffer;
        std::size_t size = 0;
    };

    struct DeflatePipelineState
    {
        std::mutex mutex;
        std::exception_ptr error;
        std::uint32_t crc32 = 0;
        std::uint32_t uncompressed_size = 0;
        std::uint32_t compressed_size = 0;
        std::atomic<std::uint64_t> read_ns {0};
        std::atomic<std::uint64_t> crc_ns {0};
        std::atomic<std::uint64_t> deflate_ns {0};
        std::atomic<std::uint64_t> write_ns {0};
    };

    auto capture_error = [](DeflatePipelineState& state) {
        std::lock_guard lock(state.mutex);
        if (!state.error)
        {
            state.error = std::current_exception();
        }
    };

    const auto chunk_size = ResolveZipStreamChunkSize(entry, pipeline_options);
    const auto compressed_block_size =
        static_cast<std::size_t>(mz_compressBound(static_cast<mz_ulong>(chunk_size)));
    const auto queue_capacity = std::max<std::size_t>(
        2,
        std::min<std::size_t>(64, std::max<std::size_t>(4, pipeline_options.max_in_flight_chunks / 2)));

    pipeline::BufferPool raw_pool(chunk_size, queue_capacity + 1);
    pipeline::BufferPool compressed_pool(compressed_block_size, queue_capacity + 1);
    pipeline::BoundedQueue<RawChunk> raw_queue(queue_capacity);
    pipeline::BoundedQueue<CompressedChunk> compressed_queue(queue_capacity);
    DeflatePipelineState state {};

    std::thread reader([&] {
        try
        {
            while (true)
            {
                auto block = raw_pool.Acquire();
                const auto read_started_at = std::chrono::steady_clock::now();
                input.read(reinterpret_cast<char*>(block->bytes.data()), static_cast<std::streamsize>(chunk_size));
                const auto bytes_read = input.gcount();
                state.read_ns.fetch_add(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - read_started_at)
                        .count()),
                    std::memory_order_relaxed);
                if (bytes_read < 0)
                {
                    throw std::runtime_error("failed to read input file: " + entry.source_path.string());
                }

                const auto is_last = input.eof();
                if (bytes_read == 0 && !is_last)
                {
                    continue;
                }

                if (!raw_queue.Push({std::move(block), static_cast<std::size_t>(bytes_read), is_last}))
                {
                    return;
                }

                if (is_last)
                {
                    break;
                }
            }
        }
        catch (...)
        {
            capture_error(state);
        }

        raw_queue.Close();
    });

    std::thread compressor([&] {
        mz_stream stream {};
        if (mz_deflateInit2(
                &stream,
                ToMinizLevel(entry.method, entry.compression_profile),
                MZ_DEFLATED,
                -MZ_DEFAULT_WINDOW_BITS,
                9,
                MZ_DEFAULT_STRATEGY) != MZ_OK)
        {
            std::lock_guard lock(state.mutex);
            state.error = std::make_exception_ptr(
                std::runtime_error("failed to initialize deflate compressor"));
            compressed_queue.Close();
            return;
        }

        Crc32 crc;
        std::uint64_t total_input_bytes = 0;
        std::uint64_t total_output_bytes = 0;

        try
        {
            while (const auto raw_chunk = raw_queue.Pop())
            {
                if (raw_chunk->size > 0)
                {
                    const auto crc_started_at = std::chrono::steady_clock::now();
                    crc.Update(raw_chunk->buffer->bytes.data(), raw_chunk->size);
                    state.crc_ns.fetch_add(static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - crc_started_at)
                            .count()),
                        std::memory_order_relaxed);
                    total_input_bytes += raw_chunk->size;
                    if (!FitsInUint32(total_input_bytes))
                    {
                        throw std::runtime_error("zip64 is not implemented for file: " + entry.source_path.string());
                    }
                }

                stream.next_in = reinterpret_cast<const unsigned char*>(raw_chunk->buffer->bytes.data());
                stream.avail_in = static_cast<mz_uint>(raw_chunk->size);
                const auto flush = raw_chunk->is_last ? MZ_FINISH : MZ_NO_FLUSH;

                int deflate_status = MZ_OK;
                do
                {
                    auto output_block = compressed_pool.Acquire();
                    stream.next_out = reinterpret_cast<unsigned char*>(output_block->bytes.data());
                    stream.avail_out = static_cast<mz_uint>(output_block->bytes.size());

                    const auto deflate_started_at = std::chrono::steady_clock::now();
                    deflate_status = mz_deflate(&stream, flush);
                    state.deflate_ns.fetch_add(static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - deflate_started_at)
                            .count()),
                        std::memory_order_relaxed);
                    if (deflate_status != MZ_OK && deflate_status != MZ_STREAM_END)
                    {
                        compressed_pool.Release(std::move(output_block));
                        throw std::runtime_error("deflate compression failed for: " + entry.source_path.string());
                    }

                    const auto produced = output_block->bytes.size() - stream.avail_out;
                    if (produced > 0)
                    {
                        total_output_bytes += produced;
                        if (!FitsInUint32(total_output_bytes))
                        {
                            compressed_pool.Release(std::move(output_block));
                            throw std::runtime_error(
                                "zip64 is not implemented for file: " + entry.source_path.string());
                        }

                        if (!compressed_queue.Push({std::move(output_block), produced}))
                        {
                            compressed_pool.Release(std::move(output_block));
                            throw std::runtime_error("compression output queue closed unexpectedly");
                        }
                    }
                    else
                    {
                        compressed_pool.Release(std::move(output_block));
                    }
                } while (
                    stream.avail_in > 0 ||
                    stream.avail_out == 0 ||
                    (flush == MZ_FINISH && deflate_status != MZ_STREAM_END));

                raw_pool.Release(raw_chunk->buffer);
            }

            state.crc32 = crc.Finalize();
            state.uncompressed_size = static_cast<std::uint32_t>(total_input_bytes);
            state.compressed_size = static_cast<std::uint32_t>(total_output_bytes);
        }
        catch (...)
        {
            capture_error(state);
        }

        mz_deflateEnd(&stream);
        compressed_queue.Close();
    });

    try
    {
        while (const auto compressed_chunk = compressed_queue.Pop())
        {
            const auto write_started_at = std::chrono::steady_clock::now();
            output.write(
                reinterpret_cast<const char*>(compressed_chunk->buffer->bytes.data()),
                static_cast<std::streamsize>(compressed_chunk->size));
            state.write_ns.fetch_add(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - write_started_at)
                    .count()),
                std::memory_order_relaxed);
            compressed_pool.Release(compressed_chunk->buffer);
            if (!output)
            {
                throw std::runtime_error("failed to write deflate output");
            }
        }
    }
    catch (...)
    {
        capture_error(state);
        raw_queue.Close();
        compressed_queue.Close();
    }

    if (reader.joinable())
    {
        reader.join();
    }
    if (compressor.joinable())
    {
        compressor.join();
    }

    if (state.error)
    {
        try
        {
            std::rethrow_exception(state.error);
        }
        catch (const std::runtime_error& error)
        {
            return MakeError(ZipStatus::IoError, error.what());
        }
        catch (...)
        {
            return MakeError(ZipStatus::IoError, "deflate pipeline failed");
        }
    }

    entry.crc32 = state.crc32;
    entry.size = state.uncompressed_size;
    entry.compressed_size = state.compressed_size;
    timer.AddPhase("read", std::chrono::nanoseconds(state.read_ns.load(std::memory_order_relaxed)));
    timer.AddPhase("crc", std::chrono::nanoseconds(state.crc_ns.load(std::memory_order_relaxed)));
    timer.AddPhase("deflate", std::chrono::nanoseconds(state.deflate_ns.load(std::memory_order_relaxed)));
    timer.AddPhase("write", std::chrono::nanoseconds(state.write_ns.load(std::memory_order_relaxed)));
    timer.Finish();
    return {ZipStatus::Ok, {}};
}

bool TryWriteMappedStoreEntry(storage::IRandomAccessReader& reader,
                              std::ostream& output,
                              ZipEntrySource& entry,
                              std::string& error_message)
{
    if (!FitsInUint32(reader.Size()))
    {
        error_message = "zip64 is not implemented for file: " + entry.source_path.string();
        return false;
    }

    const auto source_size = static_cast<std::size_t>(reader.Size());
    const bool should_try_map = ShouldTryMapReader(
        reader,
        entry.mapping_mode,
        reader.Size(),
        source_size,
        8u * 1024u * 1024u);
    if (!should_try_map)
    {
        return false;
    }

    storage::MappedReadWindow mapped_window {};
    if (!reader.TryMapWindow(0, source_size, mapped_window, error_message))
    {
        return false;
    }

    Crc32 crc;
    if (!mapped_window.bytes.empty())
    {
        crc.Update(mapped_window.bytes.data(), mapped_window.bytes.size());
        output.write(
            reinterpret_cast<const char*>(mapped_window.bytes.data()),
            static_cast<std::streamsize>(mapped_window.bytes.size()));
        if (!output)
        {
            error_message = "failed to write stored file data";
            return false;
        }
    }

    entry.crc32 = crc.Finalize();
    entry.size = static_cast<std::uint32_t>(mapped_window.bytes.size());
    entry.compressed_size = entry.size;
    return true;
}

ZipOperationResult ExecutePreparedEntry(std::ostream& output, ZipEntrySource& entry)
{
    output.write(
        reinterpret_cast<const char*>(entry.prepared_data.data()),
        static_cast<std::streamsize>(entry.prepared_data.size()));
    if (!output)
    {
        return MakeError(ZipStatus::IoError, "failed to write prepared deflate data");
    }

    return {ZipStatus::Ok, {}};
}

ZipOperationResult ExecuteStoredEntry(std::ostream& output,
                                      ZipEntrySource& entry,
                                      storage::IRandomAccessReader& reader,
                                      std::istream& input,
                                      std::vector<std::byte>& input_buffer)
{
    std::string map_error;
    if (TryWriteMappedStoreEntry(reader, output, entry, map_error))
    {
        return {ZipStatus::Ok, {}};
    }
    if (!map_error.empty())
    {
        return MakeError(ZipStatus::IoError, map_error);
    }

    return StreamStoreEntry(input, output, input_buffer, entry);
}

ZipOperationResult ExecuteWholeBufferLibdeflateEntry(std::ostream& output,
                                                     ZipEntrySource& entry)
{
    auto prepare_result = PrepareSmallEntryWithLibdeflate(entry);
    if (prepare_result.status != ZipStatus::Ok)
    {
        return prepare_result;
    }

    return ExecutePreparedEntry(output, entry);
}

ZipOperationResult StreamEntryData(std::ostream& output,
                                   ZipEntrySource& entry,
                                   const pipeline::PipelineOptions& pipeline_options,
                                   std::size_t memory_budget_mb)
{
    const auto execution_plan =
        BuildExecutionPlan(entry, pipeline_options, memory_budget_mb);
    TraceExecutionPlan(entry, execution_plan, memory_budget_mb);

    if (execution_plan.path == ZipCreateExecutionPath::Directory)
    {
        entry.crc32 = 0;
        entry.size = 0;
        entry.compressed_size = 0;
        return {ZipStatus::Ok, {}};
    }

    if (execution_plan.path == ZipCreateExecutionPath::Prepared)
    {
        return ExecutePreparedEntry(output, entry);
    }

    std::unique_ptr<storage::IRandomAccessReader> opened_reader;
    storage::IRandomAccessReader* reader = entry.source_reader;
    if (reader == nullptr)
    {
        std::string open_error;
        if (!OpenRandomAccessReader(
                *entry.storage_factory,
                entry.source_path,
                entry.mapping_mode,
                opened_reader,
                open_error))
        {
            return MakeError(ZipStatus::IoError, open_error);
        }
        reader = opened_reader.get();
    }

    RandomAccessReaderIStream input(*reader);

    const auto chunk_size = execution_plan.chunk_size_bytes;
    std::vector<std::byte> input_buffer(chunk_size);
    if (execution_plan.path == ZipCreateExecutionPath::Store)
    {
        auto result = ExecuteStoredEntry(output, entry, *reader, input, input_buffer);
        if (result.status == ZipStatus::Ok && input.Failed())
        {
            return MakeError(ZipStatus::IoError, input.ErrorMessage());
        }
        return result;
    }

    auto sample_result = MaybePreferStoreFromSample(entry);
    if (sample_result.status != ZipStatus::Ok)
    {
        return sample_result;
    }

    if (execution_plan.path == ZipCreateExecutionPath::WholeBufferLibdeflate)
    {
        return ExecuteWholeBufferLibdeflateEntry(output, entry);
    }

    if (execution_plan.path == ZipCreateExecutionPath::ChunkedCpu)
    {
        auto result = ExecuteChunkedCpuEntry(
            output,
            entry,
            pipeline_options,
            execution_plan.chunk_size_bytes);
        if (result.status == ZipStatus::Ok && input.Failed())
        {
            return MakeError(ZipStatus::IoError, input.ErrorMessage());
        }
        return result;
    }

    auto result = StreamDeflateEntry(
        input,
        output,
        entry,
        pipeline_options);
    if (result.status == ZipStatus::Ok && input.Failed())
    {
        return MakeError(ZipStatus::IoError, input.ErrorMessage());
    }

    return result;
}

ZipOperationResult WriteCentralDirectory(std::ostream& output, const std::vector<ZipEntrySource>& entries)
{
    for (const auto& entry : entries)
    {
        if (!FitsInUint16(entry.archive_path.size()))
        {
            return MakeError(ZipStatus::Unsupported, "path too long for zip entry: " + entry.archive_path);
        }

        const auto zip64_extra = BuildCentralDirectoryZip64ExtraField(entry);
        const auto version_needed = zip64_extra.empty() ? kVersionNeeded : kVersionZip64Needed;
        WriteU32(output, kCentralDirectoryHeaderSignature);
        WriteU16(output, kVersionMadeBy);
        WriteU16(output, version_needed);
        WriteU16(output, entry.general_purpose_flag);
        WriteU16(output, static_cast<std::uint16_t>(entry.method));
        WriteU16(output, kDosTime);
        WriteU16(output, kDosDate);
        WriteU32(output, entry.crc32);
        WriteU32(output, entry.compressed_size);
        WriteU32(output, entry.size);
        WriteU16(output, static_cast<std::uint16_t>(entry.archive_path.size()));
        WriteU16(output, static_cast<std::uint16_t>(zip64_extra.size()));
        WriteU16(output, 0);
        WriteU16(output, 0);
        WriteU16(output, 0);
        WriteU32(output, entry.is_directory ? kDirectoryExternalAttributes : 0u);
        WriteU32(
            output,
            zip64_extra.empty() ? static_cast<std::uint32_t>(entry.local_header_offset) : kZip64Sentinel32);
        output.write(entry.archive_path.data(), static_cast<std::streamsize>(entry.archive_path.size()));
        if (!zip64_extra.empty())
        {
            output.write(
                reinterpret_cast<const char*>(zip64_extra.data()),
                static_cast<std::streamsize>(zip64_extra.size()));
        }

        if (!output)
        {
            return MakeError(ZipStatus::IoError, "failed to write central directory");
        }
    }

    return {ZipStatus::Ok, {}};
}

ZipOperationResult WriteEndOfCentralDirectory(std::ostream& output,
                                              std::size_t entry_count,
                                              std::uint64_t central_directory_size,
                                              std::uint64_t central_directory_offset)
{
    const auto needs_zip64 = !FitsInUint16(entry_count) ||
        !FitsInUint32(central_directory_size) ||
        !FitsInUint32(central_directory_offset);

    if (needs_zip64)
    {
        const auto zip64_eocd_offset = output.tellp();
        if (zip64_eocd_offset < 0)
        {
            return MakeError(ZipStatus::IoError, "failed to determine zip64 central directory offset");
        }

        WriteU32(output, kZip64EndOfCentralDirectorySignature);
        WriteU64(output, 44);
        WriteU16(output, kVersionZip64Needed);
        WriteU16(output, kVersionZip64Needed);
        WriteU32(output, 0);
        WriteU32(output, 0);
        WriteU64(output, entry_count);
        WriteU64(output, entry_count);
        WriteU64(output, central_directory_size);
        WriteU64(output, central_directory_offset);

        WriteU32(output, kZip64EndOfCentralDirectoryLocatorSignature);
        WriteU32(output, 0);
        WriteU64(output, static_cast<std::uint64_t>(zip64_eocd_offset));
        WriteU32(output, 1);
    }

    WriteU32(output, kEndOfCentralDirectorySignature);
    WriteU16(output, 0);
    WriteU16(output, 0);
    WriteU16(output, FitsInUint16(entry_count) ? static_cast<std::uint16_t>(entry_count) : kZip64Sentinel16);
    WriteU16(output, FitsInUint16(entry_count) ? static_cast<std::uint16_t>(entry_count) : kZip64Sentinel16);
    WriteU32(output, FitsInUint32(central_directory_size) ? static_cast<std::uint32_t>(central_directory_size) : kZip64Sentinel32);
    WriteU32(output, FitsInUint32(central_directory_offset) ? static_cast<std::uint32_t>(central_directory_offset) : kZip64Sentinel32);
    WriteU16(output, 0);

    if (!output)
    {
        return MakeError(ZipStatus::IoError, "failed to finalize zip archive");
    }

    return {ZipStatus::Ok, {}};
}

ZipOperationResult CreateZipArchiveToWriter(storage::IRandomAccessWriter& writer,
                                            const core::ArchiveJob& job,
                                            const core::ArchiveRequest* archive_request,
                                            const core::ExecutionContext& context)
{
    std::vector<ZipEntrySource> entries;
    const auto execution = core::ResolveExecutionOptions(job);
    auto encryption_result = ValidateZipEncryptionOptions(execution);
    if (encryption_result.status != ZipStatus::Ok)
    {
        return encryption_result;
    }

    auto collect_result = CollectEntries(job, context, archive_request, entries);
    if (collect_result.status != ZipStatus::Ok)
    {
        return collect_result;
    }

    if (!entries.empty() && !IsSupportedWriterMethod(SelectZipMethod(job)))
    {
        return MakeError(
            ZipStatus::Unsupported,
            std::string("zip writer method not implemented yet: ") +
                codecs::ToString(SelectZipMethod(job)));
    }
    RandomAccessWriterOStream output(writer);

    auto pipeline_plan = pipeline::BuildPipelinePlan(job);
    pipeline_plan.options = TuneCreatePipelineOptions(entries, pipeline_plan.options);
    const auto encryption_mode = ResolveZipEncryptionMode(execution);
    if (encryption_mode == core::EncryptionMode::None)
    {
        bool should_precompress = true;
        for (const auto& entry : entries)
        {
            const auto execution_plan =
                BuildExecutionPlan(entry, pipeline_plan.options, execution.memory_budget_mb);
            if (execution_plan.path == ZipCreateExecutionPath::ChunkedCpu)
            {
                should_precompress = false;
                break;
            }
        }
        if (should_precompress)
        {
            const auto prepare_result = PrecompressSmallEntries(
                entries,
                pipeline_plan.options,
                execution.memory_budget_mb);
            if (prepare_result.status != ZipStatus::Ok)
            {
                return prepare_result;
            }
        }
    }

    ReportProgress(
        context,
        {
            .phase = core::ProgressPhase::ProcessingItems,
            .completed_items = 0,
            .total_items = entries.size(),
            .message = "zip create prepared entries",
        });

    std::size_t completed_entries = 0;
    for (auto& entry : entries)
    {
        if (IsCancellationRequested(context))
        {
            return MakeCancelled("zip create cancelled");
        }

        const auto position = output.tellp();
        if (position < 0)
        {
            return MakeError(ZipStatus::IoError, "failed to determine output offset");
        }

        entry.local_header_offset = static_cast<std::uint64_t>(position);

        if (encryption_mode != core::EncryptionMode::None)
        {
            auto prepare_result = PrepareEncryptedEntry(entry, execution);
            if (prepare_result.status != ZipStatus::Ok)
            {
                return prepare_result;
            }
        }
        else if (entry.prepared_data.empty())
        {
            const auto execution_plan =
                BuildExecutionPlan(entry, pipeline_plan.options, execution.memory_budget_mb);
            if (execution_plan.path == ZipCreateExecutionPath::WholeBufferLibdeflate)
            {
                auto prepare_result = PrepareSmallEntryWithLibdeflate(entry);
                if (prepare_result.status != ZipStatus::Ok)
                {
                    return prepare_result;
                }
            }
        }

        auto write_result = WriteLocalFileHeader(output, entry);
        if (write_result.status != ZipStatus::Ok)
        {
            return write_result;
        }

        write_result = StreamEntryData(output, entry, pipeline_plan.options, execution.memory_budget_mb);
        if (write_result.status != ZipStatus::Ok)
        {
            return write_result;
        }

        if (encryption_mode != core::EncryptionMode::None)
        {
            entry.prepared_data.clear();
        }

        write_result = WriteDataDescriptor(output, entry);
        if (write_result.status != ZipStatus::Ok)
        {
            return write_result;
        }

        ++completed_entries;
        ReportProgress(
            context,
            {
                .phase = core::ProgressPhase::WritingOutput,
                .completed_items = completed_entries,
                .total_items = entries.size(),
                .completed_bytes = entry.size,
                .current_path = entry.archive_path,
                .message = "zip create wrote entry",
            });
    }

    const auto central_directory_offset_stream = output.tellp();
    if (central_directory_offset_stream < 0)
    {
        return MakeError(ZipStatus::IoError, "failed to determine central directory offset");
    }

    const auto central_directory_offset = static_cast<std::uint64_t>(central_directory_offset_stream);

    auto write_result = WriteCentralDirectory(output, entries);
    if (write_result.status != ZipStatus::Ok)
    {
        return write_result;
    }

    const auto central_directory_end_stream = output.tellp();
    if (central_directory_end_stream < 0)
    {
        return MakeError(ZipStatus::IoError, "failed to determine central directory end");
    }

    const auto central_directory_end = static_cast<std::uint64_t>(central_directory_end_stream);
    const auto central_directory_size = central_directory_end - central_directory_offset;

    write_result = WriteEndOfCentralDirectory(
        output,
        entries.size(),
        central_directory_size,
        central_directory_offset);

    if (write_result.status != ZipStatus::Ok)
    {
        return write_result;
    }

    output.flush();
    if (!output)
    {
        return MakeError(ZipStatus::IoError, "failed to flush zip archive output");
    }

    std::string flush_error;
    if (!writer.Flush(flush_error))
    {
        return MakeError(ZipStatus::IoError, flush_error);
    }

    return {
        ZipStatus::Ok,
        "created zip archive entries=" + std::to_string(entries.size()) + " output=" + job.output_path +
            " " + pipeline_plan.summary};
}

ZipOperationResult CreateZipArchive(const core::ArchiveJob& job,
                                    const core::ArchiveRequest* archive_request,
                                    const core::ExecutionContext& context)
{
    std::unique_ptr<storage::IRandomAccessWriter> writer;
    std::string error_message;
    if (!OpenRandomAccessWriter(ResolveStorageFactory(context), Utf8Path(job.output_path), writer, error_message))
    {
        return MakeError(ZipStatus::IoError, error_message);
    }

    return CreateZipArchiveToWriter(*writer, job, archive_request, context);
}

}
