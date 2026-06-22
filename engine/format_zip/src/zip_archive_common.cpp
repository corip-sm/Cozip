#include "zip_archive_internal.h"

namespace cozip::format_zip
{
std::string NormalizeArchivePath(const fs::path& path)
{
    auto normalized = path.generic_string();
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    return normalized;
}

fs::path Utf8Path(const std::string& value)
{
    return fs::u8path(value);
}

std::string LowercaseAscii(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

bool IsSamePath(const fs::path& left, const fs::path& right)
{
    std::error_code left_error;
    std::error_code right_error;
    const auto normalized_left = fs::weakly_canonical(left, left_error);
    const auto normalized_right = fs::weakly_canonical(right, right_error);

    if (!left_error && !right_error)
    {
        return normalized_left == normalized_right;
    }

    return left.lexically_normal() == right.lexically_normal();
}

bool FitsInUint16(std::size_t value) noexcept
{
    return value <= static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max());
}

bool FitsInUint32(std::uintmax_t value) noexcept
{
    return value <= static_cast<std::uintmax_t>(std::numeric_limits<std::uint32_t>::max());
}

bool ShouldPreferStoreForZipEntry(const ZipEntrySource& entry) noexcept
{
    if (entry.is_directory || entry.size < 4u * 1024u * 1024u)
    {
        return false;
    }

    const auto extension_path =
        entry.source_path.empty() ? fs::path(entry.archive_path) : entry.source_path;
    const auto extension = LowercaseAscii(extension_path.extension().string());
    return extension == ".zip" ||
        extension == ".7z" ||
        extension == ".rar" ||
        extension == ".gz" ||
        extension == ".bz2" ||
        extension == ".xz" ||
        extension == ".zst" ||
        extension == ".jpg" ||
        extension == ".jpeg" ||
        extension == ".png" ||
        extension == ".webp" ||
        extension == ".mp3" ||
        extension == ".mp4" ||
        extension == ".mkv" ||
        extension == ".avi";
}

bool ShouldStorePreparedDeflateResult(std::size_t input_size, std::size_t compressed_size) noexcept
{
    return compressed_size >= input_size;
}

void WriteU16(std::ostream& stream, std::uint16_t value)
{
    const char bytes[] {
        static_cast<char>(value & 0xFFu),
        static_cast<char>((value >> 8) & 0xFFu),
    };
    stream.write(bytes, sizeof(bytes));
}

void WriteU32(std::ostream& stream, std::uint32_t value)
{
    const char bytes[] {
        static_cast<char>(value & 0xFFu),
        static_cast<char>((value >> 8) & 0xFFu),
        static_cast<char>((value >> 16) & 0xFFu),
        static_cast<char>((value >> 24) & 0xFFu),
    };
    stream.write(bytes, sizeof(bytes));
}

void WriteU64(std::ostream& stream, std::uint64_t value)
{
    const char bytes[] {
        static_cast<char>(value & 0xFFu),
        static_cast<char>((value >> 8) & 0xFFu),
        static_cast<char>((value >> 16) & 0xFFu),
        static_cast<char>((value >> 24) & 0xFFu),
        static_cast<char>((value >> 32) & 0xFFu),
        static_cast<char>((value >> 40) & 0xFFu),
        static_cast<char>((value >> 48) & 0xFFu),
        static_cast<char>((value >> 56) & 0xFFu),
    };
    stream.write(bytes, sizeof(bytes));
}

std::uint16_t ReadU16(std::span<const std::byte> data, std::size_t offset)
{
    return static_cast<std::uint16_t>(
        static_cast<std::uint8_t>(data[offset]) |
        (static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[offset + 1])) << 8));
}

std::uint32_t ReadU32(std::span<const std::byte> data, std::size_t offset)
{
    return static_cast<std::uint32_t>(
        static_cast<std::uint8_t>(data[offset]) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 1])) << 8) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 2])) << 16) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 3])) << 24));
}

std::uint64_t ReadU64(std::span<const std::byte> data, std::size_t offset)
{
    return static_cast<std::uint64_t>(
        static_cast<std::uint8_t>(data[offset])) |
        (static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[offset + 1])) << 8) |
        (static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[offset + 2])) << 16) |
        (static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[offset + 3])) << 24) |
        (static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[offset + 4])) << 32) |
        (static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[offset + 5])) << 40) |
        (static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[offset + 6])) << 48) |
        (static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[offset + 7])) << 56);
}

bool EntryNeedsZip64CentralDirectoryOffset(const ZipEntrySource& entry) noexcept
{
    return !FitsInUint32(entry.local_header_offset);
}

std::vector<std::byte> BuildCentralDirectoryZip64ExtraField(const ZipEntrySource& entry)
{
    std::vector<std::byte> bytes;

    if (!EntryNeedsZip64CentralDirectoryOffset(entry))
    {
        return bytes;
    }

    bytes.reserve(2 + 2 + 8);
    const auto append_u16 = [&](std::uint16_t value) {
        bytes.push_back(static_cast<std::byte>(value & 0xFFu));
        bytes.push_back(static_cast<std::byte>((value >> 8) & 0xFFu));
    };
    const auto append_u64 = [&](std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8)
        {
            bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
        }
    };

    append_u16(kZip64ExtraFieldHeaderId);
    append_u16(8);
    append_u64(entry.local_header_offset);
    return bytes;
}

ZipOperationResult MakeError(ZipStatus status, std::string message)
{
    return {status, std::move(message)};
}

ZipOperationResult MakeCancelled(std::string message)
{
    return {ZipStatus::Cancelled, std::move(message)};
}

bool IsCancellationRequested(const core::ExecutionContext& context) noexcept
{
    return context.cancel != nullptr && context.cancel->IsCancellationRequested();
}

void ReportProgress(const core::ExecutionContext& context, core::ProgressEvent event)
{
    if (context.progress != nullptr)
    {
        context.progress->OnProgress(event);
    }
}

void ReportDiagnostic(const core::ExecutionContext& context,
                      core::DiagnosticSeverity severity,
                      std::string message,
                      std::string path)
{
    if (context.environment != nullptr && context.environment->logger != nullptr)
    {
        context.environment->logger->Log(severity, message);
    }

    if (context.diagnostics != nullptr)
    {
        context.diagnostics->OnDiagnostic({severity, std::move(path), std::move(message)});
    }
}

std::string DescribeJobOperation(core::JobType type)
{
    switch (type)
    {
    case core::JobType::CreateArchive:
        return "zip create";
    case core::JobType::ExtractArchive:
        return "zip extract";
    case core::JobType::ListArchive:
        return "zip list";
    case core::JobType::TestArchive:
        return "zip test";
    }

    return "zip execute";
}

storage::IStorageFactory& DefaultStorageFactory()
{
    static platform::FilesystemStorageFactory factory;
    return factory;
}

storage::IStorageFactory& ResolveStorageFactory(const core::ExecutionContext& context)
{
    if (context.environment != nullptr && context.environment->storage_factory != nullptr)
    {
        return *context.environment->storage_factory;
    }

    return DefaultStorageFactory();
}

bool OpenRandomAccessReader(storage::IStorageFactory& factory,
                            const fs::path& path,
                            core::MappingMode mapping_mode,
                            std::unique_ptr<storage::IRandomAccessReader>& reader,
                            std::string& error_message)
{
    reader = factory.OpenReader(path, mapping_mode, error_message);
    return static_cast<bool>(reader);
}

bool OpenRandomAccessWriter(storage::IStorageFactory& factory,
                            const fs::path& path,
                            std::unique_ptr<storage::IRandomAccessWriter>& writer,
                            std::string& error_message)
{
    writer = factory.OpenWriter(path, error_message);
    return static_cast<bool>(writer);
}

bool ShouldTryMapReader(const storage::IRandomAccessReader& reader,
                        core::MappingMode mapping_mode,
                        std::uint64_t source_size,
                        std::size_t requested_window_bytes,
                        std::uintmax_t auto_threshold_bytes) noexcept
{
    const auto capabilities = reader.Capabilities();
    if (!capabilities.supports_mapping || mapping_mode == core::MappingMode::ForceOff)
    {
        return false;
    }

    if (mapping_mode == core::MappingMode::PreferOn || mapping_mode == core::MappingMode::RequireOn)
    {
        return true;
    }

    return source_size >= auto_threshold_bytes || requested_window_bytes >= (1u * 1024u * 1024u);
}

codecs::ZipMethod SelectZipMethod(const core::ArchiveJob& job) noexcept
{
    switch (job.profile)
    {
    case core::CompressionProfile::Store:
        return codecs::ZipMethod::Store;
    case core::CompressionProfile::Fast:
    case core::CompressionProfile::Balanced:
    case core::CompressionProfile::Small:
    case core::CompressionProfile::Maximum:
        return codecs::ZipMethod::Deflate;
    }

    return codecs::ZipMethod::Store;
}

bool IsSupportedWriterMethod(codecs::ZipMethod method) noexcept
{
    return method == codecs::ZipMethod::Store || method == codecs::ZipMethod::Deflate;
}

bool IsSupportedReaderMethod(std::uint16_t method) noexcept
{
    return method == static_cast<std::uint16_t>(codecs::ZipMethod::Store) ||
        method == static_cast<std::uint16_t>(codecs::ZipMethod::Deflate);
}

const char* ZipMethodName(std::uint16_t method) noexcept
{
    switch (static_cast<codecs::ZipMethod>(method))
    {
    case codecs::ZipMethod::Store:
        return codecs::ToString(codecs::ZipMethod::Store);
    case codecs::ZipMethod::Deflate:
        return codecs::ToString(codecs::ZipMethod::Deflate);
    }

    return "unknown";
}

std::string EntrySourceLabel(const ZipEntrySource& entry)
{
    if (!entry.source_label.empty())
    {
        return entry.source_label;
    }

    if (!entry.source_path.empty())
    {
        return entry.source_path.string();
    }

    return entry.archive_path;
}

bool AddDirectoryEntry(const fs::path& directory_path,
                       const fs::path& archive_relative_path,
                       std::vector<ZipEntrySource>& entries)
{
    auto archive_name = NormalizeArchivePath(archive_relative_path);
    if (archive_name.empty())
    {
        return true;
    }

    if (!archive_name.ends_with('/'))
    {
        archive_name.push_back('/');
    }

    entries.push_back({
        directory_path,
        archive_name,
        directory_path.generic_string(),
        0u,
        0u,
        0u,
        0u,
        codecs::ZipMethod::Store,
        0u,
        codecs::DeflateBackend::None,
        core::CompressionProfile::Balanced,
        {},
        true,
        false,
        core::MappingMode::Auto,
        nullptr,
        nullptr});
    return true;
}

ZipOperationResult CollectRegularFile(const fs::path& file_path,
                                      const fs::path& archive_relative_path,
                                      std::vector<ZipEntrySource>& entries)
{
    std::error_code size_error;
    const auto file_size = fs::file_size(file_path, size_error);
    if (size_error)
    {
        return MakeError(ZipStatus::IoError, "failed to read file size: " + file_path.string());
    }

    if (!FitsInUint32(file_size))
    {
        return MakeError(ZipStatus::Unsupported, "zip64 not implemented for file: " + file_path.string());
    }

    auto archive_name = NormalizeArchivePath(archive_relative_path);
    if (archive_name.empty())
    {
        archive_name = file_path.filename().generic_string();
    }

    entries.push_back({
        file_path,
        archive_name,
        file_path.generic_string(),
        0u,
        static_cast<std::uint32_t>(file_size),
        0u,
        0u,
        codecs::ZipMethod::Store,
        0u,
        codecs::DeflateBackend::None,
        core::CompressionProfile::Balanced,
        {},
        false,
        false,
        core::MappingMode::Auto,
        nullptr,
        nullptr});
    return {ZipStatus::Ok, {}};
}

ZipOperationResult CollectReaderFile(const core::ArchiveSource& input,
                                     std::vector<ZipEntrySource>& entries)
{
    if (input.reader == nullptr)
    {
        return MakeError(ZipStatus::InvalidJob, "reader-backed archive input is missing reader");
    }

    if (!FitsInUint32(input.reader->Size()))
    {
        return MakeError(ZipStatus::Unsupported, "zip64 not implemented for reader-backed input: " + input.archive_path);
    }

    auto archive_name = NormalizeArchivePath(input.archive_path);
    if (archive_name.empty())
    {
        return MakeError(ZipStatus::InvalidJob, "reader-backed archive input is missing archive path");
    }

    entries.push_back({
        {},
        archive_name,
        archive_name,
        0u,
        static_cast<std::uint32_t>(input.reader->Size()),
        0u,
        0u,
        codecs::ZipMethod::Store,
        0u,
        codecs::DeflateBackend::None,
        core::CompressionProfile::Balanced,
        {},
        false,
        false,
        core::MappingMode::Auto,
        nullptr,
        input.reader});
    return {ZipStatus::Ok, {}};
}

ZipOperationResult CollectInputEntries(const core::ArchiveInput& input,
                                       const fs::path& output_path,
                                       std::vector<ZipEntrySource>& entries)
{
    const fs::path source_path = Utf8Path(input.path);
    if (!fs::exists(source_path))
    {
        return MakeError(ZipStatus::NotFound, "input not found: " + input.path);
    }

    const auto root_name = source_path.filename().empty() ? source_path.stem() : source_path.filename();

    if (fs::is_regular_file(source_path))
    {
        return CollectRegularFile(source_path, root_name, entries);
    }

    if (!fs::is_directory(source_path))
    {
        return MakeError(ZipStatus::Unsupported, "unsupported input type: " + input.path);
    }

    AddDirectoryEntry(source_path, root_name, entries);

    if (!input.recursive)
    {
        return {ZipStatus::Ok, {}};
    }

    for (const auto& entry : fs::recursive_directory_iterator(source_path))
    {
        const auto relative = fs::relative(entry.path(), source_path);
        const auto archive_relative = root_name / relative;

        if (IsSamePath(entry.path(), output_path))
        {
            continue;
        }

        if (entry.is_directory())
        {
            AddDirectoryEntry(entry.path(), archive_relative, entries);
            continue;
        }

        if (entry.is_regular_file())
        {
            const auto result = CollectRegularFile(entry.path(), archive_relative, entries);
            if (result.status != ZipStatus::Ok)
            {
                return result;
            }
        }
    }

    return {ZipStatus::Ok, {}};
}

ZipOperationResult CollectEntries(const core::ArchiveJob& job,
                                  const core::ExecutionContext& context,
                                  const core::ArchiveRequest* archive_request,
                                  std::vector<ZipEntrySource>& entries)
{
    const auto execution = core::ResolveExecutionOptions(job);
    auto& storage_factory = ResolveStorageFactory(context);
    const auto output_path = fs::absolute(Utf8Path(job.output_path));
    if (archive_request != nullptr)
    {
        for (const auto& input : archive_request->inputs)
        {
            ZipOperationResult result {ZipStatus::Unsupported, {}};
            switch (input.kind)
            {
            case core::ArchiveSourceKind::Path:
                result = CollectInputEntries({input.path, input.recursive}, output_path, entries);
                break;
            case core::ArchiveSourceKind::ReaderFile:
                result = CollectReaderFile(input, entries);
                break;
            }

            if (result.status != ZipStatus::Ok)
            {
                return result;
            }
        }
    }
    else for (const auto& input : job.inputs)
    {
        const auto result = CollectInputEntries(input, output_path, entries);
        if (result.status != ZipStatus::Ok)
        {
            return result;
        }
    }

    if (entries.empty())
    {
        return MakeError(ZipStatus::InvalidJob, "no input files found");
    }

    const auto method = SelectZipMethod(job);
    for (auto& entry : entries)
    {
        entry.method = entry.is_directory ? codecs::ZipMethod::Store : method;
        entry.compression_profile = job.profile;
        entry.mapping_mode = execution.mapping_mode;
        entry.storage_factory = &storage_factory;
        if (!entry.is_directory && entry.source_reader == nullptr)
        {
            entry.source_label = entry.source_path.generic_string();
        }
        if (entry.method == codecs::ZipMethod::Deflate && ShouldPreferStoreForZipEntry(entry))
        {
            entry.method = codecs::ZipMethod::Store;
        }
        entry.general_purpose_flag = entry.is_directory ? 0u : kDataDescriptorFlag;
    }

    return {ZipStatus::Ok, {}};
}

int ToMinizLevel(codecs::ZipMethod method, core::CompressionProfile profile) noexcept
{
    if (method == codecs::ZipMethod::Store)
    {
        return 0;
    }

    switch (profile)
    {
    case core::CompressionProfile::Fast:
        return 1;
    case core::CompressionProfile::Balanced:
        return 1;
    case core::CompressionProfile::Small:
        return 8;
    case core::CompressionProfile::Maximum:
        return 9;
    case core::CompressionProfile::Store:
        return 0;
    }

    return 1;
}

std::size_t ResolveLibdeflateWholeBufferThreshold(const pipeline::PipelineOptions& pipeline_options,
                                                  std::size_t memory_budget_mb) noexcept
{
    const auto base_threshold = std::max<std::size_t>(pipeline_options.chunk_size_bytes, 1u * 1024u * 1024u);
    auto threshold = std::min<std::size_t>(8u * 1024u * 1024u, base_threshold * 8u);

    if (memory_budget_mb >= 4096u)
    {
        threshold = std::max<std::size_t>(threshold, 1024u * 1024u * 1024u);
    }
    else if (memory_budget_mb >= 2048u)
    {
        threshold = std::max<std::size_t>(threshold, 1600u * 1024u * 1024u);
    }
    else if (memory_budget_mb >= 1024u)
    {
        threshold = std::max<std::size_t>(threshold, 256u * 1024u * 1024u);
    }

    if (memory_budget_mb > 0 && memory_budget_mb < 512u)
    {
        threshold = std::min<std::size_t>(threshold, 2u * 1024u * 1024u);
    }

    return threshold;
}

std::size_t ResolveZipStreamChunkSize(const ZipEntrySource& entry,
                                      const pipeline::PipelineOptions& pipeline_options) noexcept
{
    auto chunk_size = std::max<std::size_t>(64 * 1024, pipeline_options.chunk_size_bytes);

    if (entry.size >= 1024u * 1024u * 1024u)
    {
        chunk_size = std::max<std::size_t>(chunk_size, 32u * 1024u * 1024u);
    }
    else if (entry.size >= 256u * 1024u * 1024u)
    {
        chunk_size = std::max<std::size_t>(chunk_size, 16u * 1024u * 1024u);
    }
    else if (entry.size >= 64u * 1024u * 1024u)
    {
        chunk_size = std::max<std::size_t>(chunk_size, 8u * 1024u * 1024u);
    }
    else if (entry.size >= 16u * 1024u * 1024u)
    {
        chunk_size = std::max<std::size_t>(chunk_size, 4u * 1024u * 1024u);
    }

    return chunk_size;
}

ZipOperationResult WriteLocalFileHeader(std::ostream& output, const ZipEntrySource& entry)
{
    if (!FitsInUint16(entry.archive_path.size()))
    {
        return MakeError(ZipStatus::Unsupported, "path too long for zip entry: " + entry.archive_path);
    }

    const bool uses_data_descriptor = (entry.general_purpose_flag & kDataDescriptorFlag) != 0u;

    WriteU32(output, kLocalFileHeaderSignature);
    WriteU16(output, kVersionNeeded);
    WriteU16(output, entry.general_purpose_flag);
    WriteU16(output, static_cast<std::uint16_t>(entry.method));
    WriteU16(output, kDosTime);
    WriteU16(output, kDosDate);
    WriteU32(output, uses_data_descriptor ? 0u : entry.crc32);
    WriteU32(output, uses_data_descriptor ? 0u : entry.compressed_size);
    WriteU32(output, uses_data_descriptor ? 0u : entry.size);
    WriteU16(output, static_cast<std::uint16_t>(entry.archive_path.size()));
    WriteU16(output, 0);
    output.write(entry.archive_path.data(), static_cast<std::streamsize>(entry.archive_path.size()));

    if (!output)
    {
        return MakeError(ZipStatus::IoError, "failed to write local header");
    }

    return {ZipStatus::Ok, {}};
}

ZipOperationResult WriteDataDescriptor(std::ostream& output, const ZipEntrySource& entry)
{
    if ((entry.general_purpose_flag & kDataDescriptorFlag) == 0u)
    {
        return {ZipStatus::Ok, {}};
    }

    WriteU32(output, kDataDescriptorSignature);
    WriteU32(output, entry.crc32);
    WriteU32(output, entry.compressed_size);
    WriteU32(output, entry.size);

    if (!output)
    {
        return MakeError(ZipStatus::IoError, "failed to write data descriptor");
    }

    return {ZipStatus::Ok, {}};
}

ZipOperationResult StreamStoreEntry(std::istream& input,
                                    std::ostream& output,
                                    std::vector<std::byte>& input_buffer,
                                    ZipEntrySource& entry)
{
    Crc32 crc;
    std::uint64_t total_bytes = 0;

    while (input)
    {
        input.read(reinterpret_cast<char*>(input_buffer.data()), static_cast<std::streamsize>(input_buffer.size()));
        const auto bytes_read = input.gcount();
        if (bytes_read < 0)
        {
            return MakeError(ZipStatus::IoError, "failed to read input file: " + entry.source_path.string());
        }

        if (bytes_read == 0)
        {
            break;
        }

        crc.Update(input_buffer.data(), static_cast<std::size_t>(bytes_read));
        output.write(reinterpret_cast<const char*>(input_buffer.data()), bytes_read);
        if (!output)
        {
            return MakeError(ZipStatus::IoError, "failed to write stored file data");
        }

        total_bytes += static_cast<std::uint64_t>(bytes_read);
        if (!FitsInUint32(total_bytes))
        {
            return MakeError(ZipStatus::Unsupported, "zip64 is not implemented for file: " + entry.source_path.string());
        }
    }

    entry.crc32 = crc.Finalize();
    entry.size = static_cast<std::uint32_t>(total_bytes);
    entry.compressed_size = entry.size;
    return {ZipStatus::Ok, {}};
}

ZipOperationResult ReadAllBytes(storage::IRandomAccessReader& reader,
                                std::uint64_t offset,
                                std::vector<std::byte>& bytes);

ZipOperationResult LoadFileBytes(storage::IStorageFactory& storage_factory,
                                 const fs::path& source_path,
                                 std::vector<std::byte>& bytes)
{
    std::unique_ptr<storage::IRandomAccessReader> reader;
    std::string open_error;
    if (!OpenRandomAccessReader(storage_factory, source_path, core::MappingMode::ForceOff, reader, open_error))
    {
        return MakeError(ZipStatus::IoError, open_error);
    }

    if (!FitsInUint32(reader->Size()))
    {
        return MakeError(ZipStatus::Unsupported, "input file is too large: " + source_path.string());
    }

    return ReadAllBytes(*reader, 0, bytes);
}

ZipOperationResult ReadAllBytes(storage::IRandomAccessReader& reader,
                                std::uint64_t offset,
                                std::vector<std::byte>& bytes)
{
    if (offset > reader.Size())
    {
        return MakeError(ZipStatus::IoError, "read offset is outside source");
    }

    const auto remaining_size = static_cast<std::size_t>(reader.Size() - offset);
    bytes.resize(remaining_size);
    if (bytes.empty())
    {
        return {ZipStatus::Ok, {}};
    }

    std::size_t bytes_read = 0;
    std::string error_message;
    if (!reader.Read(offset, std::span<std::byte>(bytes), bytes_read, error_message))
    {
        return MakeError(ZipStatus::IoError, error_message);
    }

    if (bytes_read != bytes.size())
    {
        return MakeError(ZipStatus::IoError, "short read while loading input bytes");
    }

    return {ZipStatus::Ok, {}};
}

ZipOperationResult LoadWholeFileInput(storage::IStorageFactory& storage_factory,
                                      const fs::path& source_path,
                                      core::MappingMode mapping_mode,
                                      storage::IRandomAccessReader* source_reader,
                                      WholeFileInput& input)
{
    input = {};
    if (source_reader != nullptr)
    {
        input.bytes = {};
    }
    else
    {
        std::string open_error;
        if (!OpenRandomAccessReader(storage_factory, source_path, mapping_mode, input.reader, open_error))
        {
            return MakeError(ZipStatus::IoError, open_error);
        }
    }

    auto& reader = source_reader != nullptr ? *source_reader : *input.reader;
    const auto file_size = reader.Size();

    if (!FitsInUint32(file_size))
    {
        return MakeError(ZipStatus::Unsupported, "input file is too large: " + source_path.string());
    }

    constexpr std::uintmax_t kMappedWholeFileThreshold = 8u * 1024u * 1024u;
    const bool should_try_map_whole_file = ShouldTryMapReader(
        reader,
        mapping_mode,
        file_size,
        static_cast<std::size_t>(file_size),
        kMappedWholeFileThreshold);
    if (should_try_map_whole_file)
    {
        std::string map_error;
        if (reader.TryMapWindow(
                0,
                static_cast<std::size_t>(file_size),
                input.mapped_window,
                map_error))
        {
            input.bytes = input.mapped_window.bytes;
            return {ZipStatus::Ok, {}};
        }

        if (!map_error.empty())
        {
            return MakeError(ZipStatus::IoError, map_error);
        }

        input.owned_bytes.clear();
        auto read_result = ReadAllBytes(reader, 0, input.owned_bytes);
        if (read_result.status != ZipStatus::Ok)
        {
            return read_result;
        }

        input.bytes = input.owned_bytes;
        return {ZipStatus::Ok, {}};
    }

    auto read_result = ReadAllBytes(reader, 0, input.owned_bytes);
    if (read_result.status != ZipStatus::Ok)
    {
        return read_result;
    }

    input.bytes = input.owned_bytes;
    return {ZipStatus::Ok, {}};
}

ZipOperationResult LoadFileSample(storage::IStorageFactory& storage_factory,
                                  const fs::path& source_path,
                                  std::size_t sample_size,
                                  core::MappingMode mapping_mode,
                                  storage::IRandomAccessReader* source_reader,
                                  std::vector<std::byte>& bytes)
{
    bytes.clear();
    std::unique_ptr<storage::IRandomAccessReader> opened_reader;
    if (source_reader == nullptr)
    {
        std::string open_error;
        if (!OpenRandomAccessReader(storage_factory, source_path, mapping_mode, opened_reader, open_error))
        {
            return MakeError(ZipStatus::IoError, open_error);
        }
    }

    auto& reader = source_reader != nullptr ? *source_reader : *opened_reader;
    const auto file_size = reader.Size();

    const auto clamped_size = static_cast<std::size_t>(
        std::min<std::uint64_t>(file_size, static_cast<std::uint64_t>(sample_size)));
    if (clamped_size == 0)
    {
        return {ZipStatus::Ok, {}};
    }

    constexpr std::uintmax_t kMappedSampleThreshold = 8u * 1024u * 1024u;
    const bool should_try_map_sample = ShouldTryMapReader(
        reader,
        mapping_mode,
        file_size,
        clamped_size,
        kMappedSampleThreshold);
    if (should_try_map_sample)
    {
        if (file_size > static_cast<std::uint64_t>(clamped_size) * 2u)
        {
            const auto head_size = clamped_size / 2;
            const auto tail_size = clamped_size - head_size;
            const auto tail_offset = static_cast<std::size_t>(file_size - static_cast<std::uint64_t>(tail_size));

            storage::MappedReadWindow head_view {};
            std::string map_error;
            if (!reader.TryMapWindow(0, head_size, head_view, map_error))
            {
                if (!map_error.empty())
                {
                    return MakeError(ZipStatus::IoError, map_error);
                }

                bytes.resize(clamped_size);
                std::size_t bytes_read = 0;
                if (!reader.Read(0, std::span<std::byte>(bytes.data(), head_size), bytes_read, map_error))
                {
                    return MakeError(ZipStatus::IoError, map_error);
                }
                if (bytes_read != head_size)
                {
                    return MakeError(ZipStatus::IoError, "short read while loading file sample head");
                }

                if (!reader.Read(
                        tail_offset,
                        std::span<std::byte>(bytes.data() + static_cast<std::ptrdiff_t>(head_size), tail_size),
                        bytes_read,
                        map_error))
                {
                    return MakeError(ZipStatus::IoError, map_error);
                }
                if (bytes_read != tail_size)
                {
                    return MakeError(ZipStatus::IoError, "short read while loading file sample tail");
                }

                return {ZipStatus::Ok, {}};
            }

            storage::MappedReadWindow tail_view {};
            if (!reader.TryMapWindow(tail_offset, tail_size, tail_view, map_error))
            {
                return MakeError(ZipStatus::IoError, map_error);
            }

            bytes.resize(clamped_size);
            if (head_size > 0)
            {
                std::copy(head_view.bytes.begin(), head_view.bytes.end(), bytes.begin());
            }
            if (tail_size > 0)
            {
                std::copy(tail_view.bytes.begin(), tail_view.bytes.end(), bytes.begin() + static_cast<std::ptrdiff_t>(head_size));
            }

            return {ZipStatus::Ok, {}};
        }

        storage::MappedReadWindow mapped_view {};
        std::string map_error;
        if (!reader.TryMapWindow(0, clamped_size, mapped_view, map_error))
        {
            if (!map_error.empty())
            {
                return MakeError(ZipStatus::IoError, map_error);
            }

            bytes.resize(clamped_size);
            std::size_t bytes_read = 0;
            if (!reader.Read(0, std::span<std::byte>(bytes), bytes_read, map_error))
            {
                return MakeError(ZipStatus::IoError, map_error);
            }
            if (bytes_read != bytes.size())
            {
                return MakeError(ZipStatus::IoError, "short read while loading file sample");
            }
            return {ZipStatus::Ok, {}};
        }

        bytes.assign(mapped_view.bytes.begin(), mapped_view.bytes.end());
        return {ZipStatus::Ok, {}};
    }

    bytes.resize(clamped_size);
    std::size_t bytes_read = 0;
    std::string error_message;
    if (!reader.Read(0, std::span<std::byte>(bytes), bytes_read, error_message))
    {
        return MakeError(ZipStatus::IoError, error_message);
    }
    if (bytes_read != bytes.size())
    {
        return MakeError(ZipStatus::IoError, "short read while loading file sample");
    }

    return {ZipStatus::Ok, {}};
}

}
