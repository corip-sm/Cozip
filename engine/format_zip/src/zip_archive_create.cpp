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
    ChunkedCpu,
};

struct ZipCreateExecutionPlan
{
    ZipCreateExecutionPath path = ZipCreateExecutionPath::ChunkedCpu;
    std::size_t chunk_size_bytes = 0;
    std::size_t chunk_count = 0;
};

constexpr std::size_t kAdaptiveStoreSampleBytes = 2u * 1024u * 1024u;
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
    case ZipCreateExecutionPath::ChunkedCpu:
        return "chunked-cpu";
    }

    return "unknown";
}

std::size_t ResolveChunkedCpuChunkSize(const ZipEntrySource& entry,
                                       const pipeline::PipelineOptions& pipeline_options) noexcept
{
    return ResolveZipStreamChunkSize(entry, pipeline_options);
}

ZipCreateExecutionPlan BuildExecutionPlan(const ZipEntrySource& entry,
                                          const pipeline::PipelineOptions& pipeline_options) noexcept
{
    ZipCreateExecutionPlan plan {};
    plan.chunk_size_bytes = ResolveChunkedCpuChunkSize(entry, pipeline_options);
    plan.chunk_count = entry.size == 0 || plan.chunk_size_bytes == 0
        ? 0
        : entry.size / plan.chunk_size_bytes +
            (entry.size % plan.chunk_size_bytes != 0 ? 1u : 0u);

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

    if (entry.method == codecs::ZipMethod::Deflate)
    {
        plan.path = ZipCreateExecutionPath::ChunkedCpu;
        return plan;
    }

    plan.path = ZipCreateExecutionPath::ChunkedCpu;
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
           << " chunks=" << plan.chunk_count
           << " memory_mb=" << memory_budget_mb;
    if (plan.chunk_count > 0)
    {
        stream << " first_chunk=" << std::min<std::size_t>(entry.size, plan.chunk_size_bytes);
        stream << " last_chunk=" <<
            (entry.size - (plan.chunk_count - 1) * plan.chunk_size_bytes);
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

ZipOperationResult StreamEntryData(std::ostream& output,
                                   ZipEntrySource& entry,
                                   const pipeline::PipelineOptions& pipeline_options,
                                   std::size_t memory_budget_mb,
                                   const core::ExecutionContext& context)
{
    const auto execution_plan =
        BuildExecutionPlan(entry, pipeline_options);
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

    const auto chunk_size = execution_plan.chunk_size_bytes;
    if (execution_plan.path == ZipCreateExecutionPath::Store)
    {
        RandomAccessReaderIStream input(*reader);
        std::vector<std::byte> input_buffer(chunk_size);
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

    if (execution_plan.path == ZipCreateExecutionPath::ChunkedCpu)
    {
        auto result = ExecuteChunkedCpuEntry(
            output,
            entry,
            *reader,
            pipeline_options,
            execution_plan.chunk_size_bytes,
            context);
        return result;
    }

    return MakeError(ZipStatus::Unsupported, "unsupported zip create execution path");
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
            auto sample_result = MaybePreferStoreFromSample(entry);
            if (sample_result.status != ZipStatus::Ok)
            {
                return sample_result;
            }
        }

        auto write_result = WriteLocalFileHeader(output, entry);
        if (write_result.status != ZipStatus::Ok)
        {
            return write_result;
        }

        write_result = StreamEntryData(
            output,
            entry,
            pipeline_plan.options,
            execution.memory_budget_mb,
            context);
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
