#include "zip_archive_internal.h"

namespace cozip::format_zip
{
ZipOperationResult LoadArchiveInput(const fs::path& archive_path,
                                    storage::IStorageFactory& storage_factory,
                                    core::MappingMode mapping_mode,
                                    ArchiveInput& input)
{
    input = {};
    std::string open_error;
    if (!OpenRandomAccessReader(storage_factory, archive_path, mapping_mode, input.reader, open_error))
    {
        if (open_error.empty())
        {
            return MakeError(ZipStatus::NotFound, "archive not found: " + archive_path.string());
        }

        return MakeError(ZipStatus::IoError, open_error);
    }

    const auto archive_size = input.reader->Size();

    constexpr std::uintmax_t kMappedArchiveThreshold = 128u * 1024u * 1024u;
    const bool should_try_map_archive =
        mapping_mode != core::MappingMode::ForceOff &&
        (archive_size >= kMappedArchiveThreshold || mapping_mode == core::MappingMode::PreferOn ||
         mapping_mode == core::MappingMode::RequireOn);
    if (should_try_map_archive)
    {
        std::string map_error;
        if (input.reader->TryMapWindow(
                0,
                static_cast<std::size_t>(archive_size),
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
    }

    auto read_result = ReadAllBytes(*input.reader, 0, input.owned_bytes);
    if (read_result.status != ZipStatus::Ok)
    {
        return read_result;
    }

    input.bytes = input.owned_bytes;
    return {ZipStatus::Ok, {}};
}

ZipOperationResult ParseCentralDirectory(std::span<const std::byte> bytes,
                                         std::vector<ZipCentralDirectoryEntry>& entries)
{
    constexpr std::size_t minimum_eocd_size = 22;
    constexpr std::size_t maximum_eocd_comment = 65535;
    constexpr std::size_t zip64_locator_size = 20;
    constexpr std::size_t zip64_eocd_minimum_size = 56;

    if (bytes.size() < minimum_eocd_size)
    {
        return MakeError(ZipStatus::InvalidJob, "archive is too small to be a valid zip file");
    }

    const auto search_start = bytes.size() > (minimum_eocd_size + maximum_eocd_comment)
        ? bytes.size() - (minimum_eocd_size + maximum_eocd_comment)
        : 0;

    std::size_t eocd_offset = bytes.size();
    for (std::size_t offset = bytes.size() - minimum_eocd_size + 1; offset-- > search_start;)
    {
        if (ReadU32(bytes, offset) == kEndOfCentralDirectorySignature)
        {
            eocd_offset = offset;
            break;
        }
    }

    if (eocd_offset == bytes.size())
    {
        return MakeError(ZipStatus::InvalidJob, "end of central directory record not found");
    }

    std::uint64_t entry_count = ReadU16(bytes, eocd_offset + 10);
    std::uint64_t central_directory_size = ReadU32(bytes, eocd_offset + 12);
    std::uint64_t central_directory_offset = ReadU32(bytes, eocd_offset + 16);

    const auto needs_zip64 = entry_count == kZip64Sentinel16 ||
        central_directory_size == kZip64Sentinel32 ||
        central_directory_offset == kZip64Sentinel32;

    if (needs_zip64)
    {
        if (eocd_offset < zip64_locator_size)
        {
            return MakeError(ZipStatus::InvalidJob, "zip64 locator is missing");
        }

        const auto locator_offset = eocd_offset - zip64_locator_size;
        if (ReadU32(bytes, locator_offset) != kZip64EndOfCentralDirectoryLocatorSignature)
        {
            return MakeError(ZipStatus::InvalidJob, "zip64 locator signature not found");
        }

        const auto zip64_eocd_offset = ReadU64(bytes, locator_offset + 8);
        if (zip64_eocd_offset > bytes.size() ||
            zip64_eocd_offset + zip64_eocd_minimum_size > bytes.size())
        {
            return MakeError(ZipStatus::InvalidJob, "zip64 end of central directory points outside the archive");
        }

        const auto zip64_offset = static_cast<std::size_t>(zip64_eocd_offset);
        if (ReadU32(bytes, zip64_offset) != kZip64EndOfCentralDirectorySignature)
        {
            return MakeError(ZipStatus::InvalidJob, "zip64 end of central directory signature not found");
        }

        entry_count = ReadU64(bytes, zip64_offset + 32);
        central_directory_size = ReadU64(bytes, zip64_offset + 40);
        central_directory_offset = ReadU64(bytes, zip64_offset + 48);
    }

    if (central_directory_offset > bytes.size() ||
        central_directory_offset + central_directory_size > bytes.size())
    {
        return MakeError(ZipStatus::InvalidJob, "central directory points outside the archive");
    }

    std::size_t cursor = static_cast<std::size_t>(central_directory_offset);
    entries.clear();
    if (entry_count > 0)
    {
        entries.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(
            entry_count,
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))));
    }

    for (std::uint64_t index = 0; index < entry_count; ++index)
    {
        constexpr std::size_t central_directory_fixed_size = 46;
        if (cursor + central_directory_fixed_size > bytes.size())
        {
            return MakeError(ZipStatus::InvalidJob, "truncated central directory entry");
        }

        if (ReadU32(bytes, cursor) != kCentralDirectoryHeaderSignature)
        {
            return MakeError(ZipStatus::InvalidJob, "invalid central directory signature");
        }

        const auto general_purpose_flag = ReadU16(bytes, cursor + 8);
        const auto compression_method = ReadU16(bytes, cursor + 10);
        const auto crc32 = ReadU32(bytes, cursor + 16);
        auto compressed_size = static_cast<std::uint64_t>(ReadU32(bytes, cursor + 20));
        auto uncompressed_size = static_cast<std::uint64_t>(ReadU32(bytes, cursor + 24));
        const auto file_name_length = ReadU16(bytes, cursor + 28);
        const auto extra_field_length = ReadU16(bytes, cursor + 30);
        const auto file_comment_length = ReadU16(bytes, cursor + 32);
        const auto external_attributes = ReadU32(bytes, cursor + 38);
        auto local_header_offset = static_cast<std::uint64_t>(ReadU32(bytes, cursor + 42));

        const auto record_size = static_cast<std::size_t>(central_directory_fixed_size) +
            file_name_length + extra_field_length + file_comment_length;

        if (cursor + record_size > bytes.size())
        {
            return MakeError(ZipStatus::InvalidJob, "central directory entry exceeds archive size");
        }

        std::string name;
        if (file_name_length > 0)
        {
            name.resize(file_name_length);
            for (std::size_t name_index = 0; name_index < file_name_length; ++name_index)
            {
                name[name_index] = static_cast<char>(
                    static_cast<std::uint8_t>(bytes[cursor + central_directory_fixed_size + name_index]));
            }
        }

        if (extra_field_length > 0)
        {
            std::size_t extra_cursor = cursor + central_directory_fixed_size + file_name_length;
            const auto extra_end = extra_cursor + extra_field_length;
            while (extra_cursor + 4 <= extra_end)
            {
                const auto header_id = ReadU16(bytes, extra_cursor);
                const auto data_size = ReadU16(bytes, extra_cursor + 2);
                extra_cursor += 4;

                if (extra_cursor + data_size > extra_end)
                {
                    return MakeError(ZipStatus::InvalidJob, "zip extra field exceeds entry size");
                }

                if (header_id == kZip64ExtraFieldHeaderId)
                {
                    std::size_t zip64_cursor = extra_cursor;
                    const auto zip64_end = extra_cursor + data_size;

                    if (uncompressed_size == kZip64Sentinel32)
                    {
                        if (zip64_cursor + 8 > zip64_end)
                        {
                            return MakeError(ZipStatus::InvalidJob, "zip64 extra field is truncated");
                        }
                        uncompressed_size = ReadU64(bytes, zip64_cursor);
                        zip64_cursor += 8;
                    }

                    if (compressed_size == kZip64Sentinel32)
                    {
                        if (zip64_cursor + 8 > zip64_end)
                        {
                            return MakeError(ZipStatus::InvalidJob, "zip64 extra field is truncated");
                        }
                        compressed_size = ReadU64(bytes, zip64_cursor);
                        zip64_cursor += 8;
                    }

                    if (local_header_offset == kZip64Sentinel32)
                    {
                        if (zip64_cursor + 8 > zip64_end)
                        {
                            return MakeError(ZipStatus::InvalidJob, "zip64 extra field is truncated");
                        }
                        local_header_offset = ReadU64(bytes, zip64_cursor);
                        zip64_cursor += 8;
                    }
                }

                extra_cursor += data_size;
            }
        }

        const auto is_directory = !name.empty() && name.back() == '/';
        const auto dos_directory_flag = (external_attributes & kDirectoryExternalAttributes) != 0;

        entries.push_back({
            std::move(name),
            general_purpose_flag,
            compression_method,
            crc32,
            compressed_size,
            uncompressed_size,
            local_header_offset,
            is_directory || dos_directory_flag});

        cursor += record_size;
    }

    return {ZipStatus::Ok, {}};
}

ZipOperationResult ListArchive(const core::ArchiveJob& job, const core::ExecutionContext& context)
{
    const fs::path archive_path = Utf8Path(job.inputs.front().path);
    ArchiveInput archive;
    auto load_result = LoadArchiveInput(
        archive_path,
        ResolveStorageFactory(context),
        core::ResolveExecutionOptions(job).mapping_mode,
        archive);
    if (load_result.status != ZipStatus::Ok)
    {
        return load_result;
    }

    std::vector<ZipCentralDirectoryEntry> entries;
    auto parse_result = ParseCentralDirectory(archive.bytes, entries);
    if (parse_result.status != ZipStatus::Ok)
    {
        return parse_result;
    }

    ReportProgress(
        context,
        {
            .phase = core::ProgressPhase::ProcessingItems,
            .completed_items = entries.size(),
            .total_items = entries.size(),
            .current_path = archive_path.generic_string(),
            .message = "zip list parsed central directory",
        });

    std::ostringstream stream;
    stream << "zip entries=" << entries.size() << '\n';
    for (const auto& entry : entries)
    {
        if (IsCancellationRequested(context))
        {
            return MakeCancelled("zip list cancelled");
        }

        stream << (entry.is_directory ? 'd' : 'f') << ' '
               << std::setw(10) << entry.uncompressed_size << ' '
               << std::setw(10) << entry.compressed_size << ' '
               << "method=" << ZipMethodName(entry.compression_method)
               << '(' << entry.compression_method << ')' << ' '
               << "encrypted=" << (IsZipEntryEncrypted(entry) ? "yes" : "no") << ' '
               << "crc=0x" << std::hex << std::setw(8) << std::setfill('0') << entry.crc32
               << std::dec << std::setfill(' ') << ' '
               << entry.name << '\n';
    }

    return {ZipStatus::Ok, stream.str()};
}

bool HasPathTraversal(std::string_view path)
{
    if (path.empty())
    {
        return true;
    }

    if (path.starts_with('/') || path.starts_with('\\'))
    {
        return true;
    }

    if (path.size() >= 2 && path[1] == ':')
    {
        return true;
    }

    fs::path parsed(path);
    for (const auto& part : parsed)
    {
        if (part == "..")
        {
            return true;
        }
    }

    return false;
}

ZipOperationResult ResolveExtractionRoot(const core::ArchiveJob& job, fs::path& output_root)
{
    const fs::path archive_path = Utf8Path(job.inputs.front().path);
    if (!job.output_path.empty())
    {
        output_root = Utf8Path(job.output_path);
        return {ZipStatus::Ok, {}};
    }

    auto default_root = archive_path.stem();
    if (default_root.empty())
    {
        default_root = "cozip_extract";
    }

    output_root = archive_path.parent_path() / default_root;
    return {ZipStatus::Ok, {}};
}

ZipOperationResult ReadLocalFileDataOffset(std::span<const std::byte> bytes,
                                           const ZipCentralDirectoryEntry& entry,
                                           std::uint64_t& data_offset,
                                           std::uint16_t* local_general_purpose_flag = nullptr,
                                           std::uint16_t* password_verifier = nullptr)
{
    constexpr std::size_t local_header_fixed_size = 30;

    if (static_cast<std::uint64_t>(entry.local_header_offset) + local_header_fixed_size > bytes.size())
    {
        return MakeError(ZipStatus::InvalidJob, "local file header points outside the archive");
    }

    const auto offset = static_cast<std::size_t>(entry.local_header_offset);
    if (ReadU32(bytes, offset) != kLocalFileHeaderSignature)
    {
        return MakeError(ZipStatus::InvalidJob, "invalid local file header signature");
    }

    const auto general_purpose_flag = ReadU16(bytes, offset + 6);
    const auto compression_method = ReadU16(bytes, offset + 8);
    const auto file_name_length = ReadU16(bytes, offset + 26);
    const auto extra_field_length = ReadU16(bytes, offset + 28);

    if (compression_method != entry.compression_method)
    {
        return MakeError(ZipStatus::InvalidJob, "local header compression method mismatch");
    }

    if (local_general_purpose_flag != nullptr)
    {
        *local_general_purpose_flag = general_purpose_flag;
    }

    if (password_verifier != nullptr)
    {
        if ((general_purpose_flag & kDataDescriptorFlag) != 0u)
        {
            *password_verifier = ReadU16(bytes, offset + 10);
        }
        else
        {
            *password_verifier = static_cast<std::uint16_t>((entry.crc32 >> 16) & 0xffffu);
        }
    }

    data_offset = static_cast<std::uint64_t>(offset + local_header_fixed_size + file_name_length + extra_field_length);
    if (data_offset + entry.compressed_size > bytes.size())
    {
        return MakeError(ZipStatus::InvalidJob, "file data exceeds archive size");
    }

    return {ZipStatus::Ok, {}};
}

ZipOperationResult DecompressEntryData(std::span<const std::byte> bytes,
                                       const ZipCentralDirectoryEntry& entry,
                                       const core::ExecutionOptions& execution,
                                       std::vector<std::byte>& uncompressed_bytes)
{
    std::uint64_t data_offset = 0;
    std::uint16_t password_verifier = 0;
    auto offset_result = ReadLocalFileDataOffset(bytes, entry, data_offset, nullptr, &password_verifier);
    if (offset_result.status != ZipStatus::Ok)
    {
        return offset_result;
    }

    const auto compressed_size = static_cast<std::size_t>(entry.compressed_size);
    const auto* compressed_data = bytes.data() + static_cast<std::size_t>(data_offset);
    std::span<const std::byte> compressed_bytes {compressed_data, compressed_size};
    std::vector<std::byte> decrypted_compressed_bytes;
    if (IsZipEntryEncrypted(entry))
    {
        auto decrypt_result = DecryptZipEntryPayload(
            compressed_bytes,
            entry,
            password_verifier,
            execution,
            decrypted_compressed_bytes);
        if (decrypt_result.status != ZipStatus::Ok)
        {
            return decrypt_result;
        }
        compressed_bytes = decrypted_compressed_bytes;
    }

    if (entry.compression_method == static_cast<std::uint16_t>(codecs::ZipMethod::Store))
    {
        if (compressed_bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        {
            return MakeError(ZipStatus::Unsupported, "stored entry is too large to extract: " + entry.name);
        }
        uncompressed_bytes.resize(compressed_bytes.size());
        if (!compressed_bytes.empty())
        {
            std::copy_n(compressed_bytes.data(), compressed_bytes.size(), uncompressed_bytes.data());
        }
        return {ZipStatus::Ok, {}};
    }

    if (entry.compression_method == static_cast<std::uint16_t>(codecs::ZipMethod::Deflate))
    {
        if (compressed_bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::size_t>::max()) ||
            entry.uncompressed_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        {
            return MakeError(ZipStatus::Unsupported, "deflate entry is too large to extract: " + entry.name);
        }

        auto libdeflate_result = codecs::DecompressDeflateBuffer(
            compressed_bytes,
            static_cast<std::size_t>(entry.uncompressed_size));
        if (libdeflate_result.success)
        {
            uncompressed_bytes = std::move(libdeflate_result.bytes);
            return {ZipStatus::Ok, {}};
        }

        uncompressed_bytes.resize(static_cast<std::size_t>(entry.uncompressed_size));

        mz_stream stream {};
        stream.next_in = reinterpret_cast<const unsigned char*>(compressed_bytes.data());
        stream.avail_in = static_cast<mz_uint>(compressed_bytes.size());
        stream.next_out = reinterpret_cast<unsigned char*>(uncompressed_bytes.data());
        stream.avail_out = static_cast<mz_uint>(entry.uncompressed_size);

        if (mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK)
        {
            return MakeError(ZipStatus::IoError, "failed to initialize inflate decompressor");
        }

        const auto inflate_status = mz_inflate(&stream, MZ_FINISH);
        if (inflate_status != MZ_STREAM_END)
        {
            mz_inflateEnd(&stream);
            return MakeError(ZipStatus::InvalidJob, "deflate decompression failed while reading: " + entry.name);
        }

        const auto end_status = mz_inflateEnd(&stream);
        if (end_status != MZ_OK)
        {
            return MakeError(ZipStatus::IoError, "failed to finalize inflate decompressor");
        }

        if (stream.total_out != entry.uncompressed_size)
        {
            return MakeError(ZipStatus::InvalidJob, "uncompressed size mismatch while reading: " + entry.name);
        }

        return {ZipStatus::Ok, {}};
    }

    return MakeError(
        ZipStatus::Unsupported,
        std::string("zip decompression method is not implemented yet: ") +
            ZipMethodName(entry.compression_method) + " entry=" + entry.name);
}

bool ShouldUseStreamingDeflateExtract(const ZipCentralDirectoryEntry& entry) noexcept
{
    return entry.compression_method == static_cast<std::uint16_t>(codecs::ZipMethod::Deflate) &&
           entry.uncompressed_size >= (64ull * 1024ull * 1024ull) &&
           entry.compressed_size <= static_cast<std::uint64_t>(std::numeric_limits<mz_uint>::max());
}

std::uint64_t ResolveMappedLibdeflateExtractLimitBytes(std::size_t memory_budget_mb) noexcept
{
    const auto memory_budget_bytes = static_cast<std::uint64_t>(memory_budget_mb) * 1024ull * 1024ull;
    if (memory_budget_bytes == 0)
    {
        return 0;
    }

    if (memory_budget_mb >= 4096)
    {
        return std::min<std::uint64_t>(3072ull * 1024ull * 1024ull, memory_budget_bytes - (256ull * 1024ull * 1024ull));
    }

    if (memory_budget_mb >= 2048)
    {
        return std::min<std::uint64_t>(1792ull * 1024ull * 1024ull, memory_budget_bytes - (128ull * 1024ull * 1024ull));
    }

    return std::min<std::uint64_t>(
        1536ull * 1024ull * 1024ull,
        memory_budget_bytes / 2);
}

bool ShouldUseMappedLibdeflateExtract(const ZipCentralDirectoryEntry& entry,
                                      std::size_t memory_budget_mb) noexcept
{
    if (entry.compression_method != static_cast<std::uint16_t>(codecs::ZipMethod::Deflate))
    {
        return false;
    }

    if (entry.uncompressed_size < (32ull * 1024ull * 1024ull))
    {
        return false;
    }

    if (entry.uncompressed_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
    {
        return false;
    }

    const auto mapped_limit = ResolveMappedLibdeflateExtractLimitBytes(memory_budget_mb);
    return mapped_limit >= entry.uncompressed_size;
}

bool ZipExtractTraceEnabled() noexcept
{
    static const bool enabled = [] {
        const char* value = std::getenv("COZIP_EXTRACT_TRACE");
        return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

std::size_t ResolveMappedStoreCopyWorkers(std::size_t bytes) noexcept
{
    static_cast<void>(bytes);
    return 1;
}

std::size_t ResolveStreamingInflateOutputChunkSize() noexcept
{
    constexpr std::size_t kDefaultChunkBytes = 1u * 1024u * 1024u;
    constexpr std::size_t kMinChunkBytes = 256u * 1024u;
    constexpr std::size_t kMaxChunkBytes = 16u * 1024u * 1024u;

    const char* value = std::getenv("COZIP_STREAM_CHUNK_KB");
    if (value == nullptr || *value == '\0')
    {
        return kDefaultChunkBytes;
    }

    const auto parsed_kb = std::strtoull(value, nullptr, 10);
    if (parsed_kb == 0)
    {
        return kDefaultChunkBytes;
    }

    const auto requested_bytes = static_cast<std::size_t>(parsed_kb * 1024ull);
    return std::clamp(requested_bytes, kMinChunkBytes, kMaxChunkBytes);
}

void TraceZipExtractEntry(const ZipCentralDirectoryEntry& entry,
                          const char* mode,
                          std::chrono::steady_clock::duration elapsed,
                          std::string_view detail = {})
{
    if (!ZipExtractTraceEnabled())
    {
        return;
    }

    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    std::cerr << "[cozip][extract] entry=" << entry.name
              << " mode=" << mode
              << " compressed=" << entry.compressed_size
              << " uncompressed=" << entry.uncompressed_size
              << " elapsed_ms=" << elapsed_ms;
    if (!detail.empty())
    {
        std::cerr << ' ' << detail;
    }
    std::cerr << '\n';
}

ZipOperationResult WriteBufferToWriter(storage::IRandomAccessWriter& writer,
                                       std::span<const std::byte> bytes)
{
    std::string error_message;
    if (!writer.Resize(bytes.size(), error_message))
    {
        return MakeError(ZipStatus::IoError, error_message);
    }

    if (!bytes.empty() && !writer.Write(0, bytes, error_message))
    {
        return MakeError(ZipStatus::IoError, error_message);
    }

    if (!writer.Flush(error_message))
    {
        return MakeError(ZipStatus::IoError, error_message);
    }

    return {ZipStatus::Ok, {}};
}

#if defined(_WIN32)
bool PrepareSequentialOutputFile(HANDLE handle,
                                 std::uint64_t size,
                                 const fs::path& destination_path,
                                 std::string& error_message) noexcept
{
    LARGE_INTEGER target_size {};
    target_size.QuadPart = static_cast<LONGLONG>(size);
    LARGE_INTEGER rewind {};
    rewind.QuadPart = 0;
    if (!SetFilePointerEx(handle, target_size, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(handle) ||
        !SetFilePointerEx(handle, rewind, nullptr, FILE_BEGIN))
    {
        error_message = "failed to size extracted file: " + destination_path.string();
        return false;
    }

    return true;
}
#endif

std::size_t ResolveMappedStoreMaxBytes() noexcept
{
    return 0;
}

ZipOperationResult WriteStoredEntryToFile(const fs::path& destination_path,
                                          std::span<const std::byte> bytes,
                                          std::uint32_t expected_crc32)
{
#if defined(_WIN32)
    constexpr std::size_t kMappedStoreThresholdBytes = 64u * 1024u * 1024u;
    const auto kMappedStoreMaxBytes = ResolveMappedStoreMaxBytes();
    constexpr std::size_t kSequentialWriteChunkBytes = 64u * 1024u * 1024u;
    const auto started_at = std::chrono::steady_clock::now();

    if (bytes.size() >= kMappedStoreThresholdBytes && bytes.size() <= kMappedStoreMaxBytes)
    {
        platform::MappedFileWriter writer;
        std::string error_message;
        if (!writer.Open(destination_path, bytes.size(), error_message))
        {
            return MakeError(ZipStatus::IoError, error_message);
        }

        platform::WritableMappedView mapped_view {};
        if (!writer.MapWindow(0, bytes.size(), mapped_view, error_message))
        {
            return MakeError(ZipStatus::IoError, error_message);
        }

        const auto copy_workers = ResolveMappedStoreCopyWorkers(bytes.size());
        Crc32 crc;
        std::size_t offset = 0;
        while (offset < bytes.size())
        {
            const auto remaining = bytes.size() - offset;
            const auto chunk_size = std::min<std::size_t>(remaining, kSequentialWriteChunkBytes);
            const auto chunk = bytes.subspan(offset, chunk_size);
            crc.Update(chunk.data(), chunk.size());
            std::memcpy(mapped_view.bytes.data() + offset, chunk.data(), chunk.size());
            offset += chunk_size;
        }

        if (crc.Finalize() != expected_crc32)
        {
            return MakeError(ZipStatus::InvalidJob, "crc mismatch while extracting: " + destination_path.string());
        }

        if (ZipExtractTraceEnabled())
        {
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started_at)
                    .count();
            std::cerr << "[cozip][extract] store-entry path=mapped-write"
                      << " bytes=" << bytes.size()
                      << " copy_workers=" << copy_workers
                      << " elapsed_ms=" << elapsed_ms
                      << " file=" << destination_path.generic_string()
                      << '\n';
        }

        return {ZipStatus::Ok, {}};
    }

    HANDLE handle = CreateFileW(
        destination_path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return MakeError(ZipStatus::IoError, "failed to create extracted file: " + destination_path.string());
    }

    std::string error_message;
    if (!PrepareSequentialOutputFile(handle, bytes.size(), destination_path, error_message))
    {
        CloseHandle(handle);
        return MakeError(ZipStatus::IoError, error_message);
    }

    Crc32 crc;
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const auto remaining = bytes.size() - offset;
        const auto chunk_size = std::min<std::size_t>(remaining, kSequentialWriteChunkBytes);
        const auto chunk = bytes.subspan(offset, chunk_size);
        crc.Update(chunk.data(), chunk.size());

        DWORD written = 0;
        if (!WriteFile(
                handle,
                reinterpret_cast<const char*>(chunk.data()),
                static_cast<DWORD>(chunk.size()),
                &written,
                nullptr) ||
            written != chunk.size())
        {
            CloseHandle(handle);
            return MakeError(ZipStatus::IoError, "failed to write extracted file: " + destination_path.string());
        }

        offset += chunk_size;
    }

    CloseHandle(handle);

    if (crc.Finalize() != expected_crc32)
    {
        return MakeError(ZipStatus::InvalidJob, "crc mismatch while extracting: " + destination_path.string());
    }

    if (ZipExtractTraceEnabled())
    {
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started_at)
                .count();
        std::cerr << "[cozip][extract] store-entry path=sequential-write"
                  << " bytes=" << bytes.size()
                  << " elapsed_ms=" << elapsed_ms
                  << " file=" << destination_path.generic_string()
                  << '\n';
    }

    return {ZipStatus::Ok, {}};
#else
    Crc32 crc;
    if (!bytes.empty())
    {
        crc.Update(bytes.data(), bytes.size());
    }
    if (crc.Finalize() != expected_crc32)
    {
        return MakeError(ZipStatus::InvalidJob, "crc mismatch while extracting: " + destination_path.string());
    }

    std::ofstream output(destination_path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return MakeError(ZipStatus::IoError, "failed to create extracted file: " + destination_path.string());
    }

    if (!bytes.empty())
    {
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!output)
        {
            return MakeError(ZipStatus::IoError, "failed to write extracted file: " + destination_path.string());
        }
    }

    return {ZipStatus::Ok, {}};
#endif
}

#if defined(_WIN32)
ZipOperationResult WriteLargeBufferToFileSequential(const fs::path& destination_path,
                                                    std::span<const std::byte> bytes)
{
    constexpr std::size_t kSequentialWriteThreshold = 8u * 1024u * 1024u;
    constexpr std::size_t kSequentialWriteChunkBytes = 64u * 1024u * 1024u;

    if (bytes.size() < kSequentialWriteThreshold)
    {
        return {ZipStatus::Unsupported, {}};
    }

    HANDLE handle = CreateFileW(
        destination_path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return MakeError(ZipStatus::IoError, "failed to create extracted file: " + destination_path.string());
    }

    std::string error_message;
    if (!PrepareSequentialOutputFile(handle, bytes.size(), destination_path, error_message))
    {
        CloseHandle(handle);
        return MakeError(ZipStatus::IoError, error_message);
    }

    const auto close_handle = [&]() {
        CloseHandle(handle);
    };

    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const auto remaining = bytes.size() - offset;
        const auto chunk_size = std::min<std::size_t>(remaining, kSequentialWriteChunkBytes);
        DWORD written = 0;
        const auto* chunk = reinterpret_cast<const char*>(bytes.data() + offset);
        if (!WriteFile(handle, chunk, static_cast<DWORD>(chunk_size), &written, nullptr) ||
            written != chunk_size)
        {
            close_handle();
            return MakeError(ZipStatus::IoError, "failed to write extracted file: " + destination_path.string());
        }

        offset += chunk_size;
    }

    close_handle();
    return {ZipStatus::Ok, {}};
}
#endif

ZipOperationResult WriteBufferToFile(storage::IStorageFactory& storage_factory,
                                     const fs::path& destination_path,
                                     std::span<const std::byte> bytes)
{
#if defined(_WIN32)
    auto sequential_result = WriteLargeBufferToFileSequential(destination_path, bytes);
    if (sequential_result.status != ZipStatus::Unsupported)
    {
        return sequential_result;
    }
#endif

    std::unique_ptr<storage::IRandomAccessWriter> writer;
    std::string error_message;
    if (!OpenRandomAccessWriter(storage_factory, destination_path, writer, error_message))
    {
        return MakeError(ZipStatus::IoError, error_message);
    }

    return WriteBufferToWriter(*writer, bytes);
}

ZipOperationResult StreamDeflateEntryToFile(std::span<const std::byte> bytes,
                                            const ZipCentralDirectoryEntry& entry,
                                            const fs::path& destination_path,
                                            const std::function<void(std::uint64_t)>& progress)
{
    const auto started_at = std::chrono::steady_clock::now();

    std::uint64_t data_offset = 0;
    auto offset_result = ReadLocalFileDataOffset(bytes, entry, data_offset);
    if (offset_result.status != ZipStatus::Ok)
    {
        return offset_result;
    }

#if defined(_WIN32)
    HANDLE handle = CreateFileW(
        destination_path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return MakeError(ZipStatus::IoError, "failed to create extracted file: " + destination_path.string());
    }

    LARGE_INTEGER initial_size {};
    initial_size.QuadPart = static_cast<LONGLONG>(entry.uncompressed_size);
    if (!SetFilePointerEx(handle, initial_size, nullptr, FILE_BEGIN) || !SetEndOfFile(handle) ||
        !SetFilePointerEx(handle, LARGE_INTEGER {}, nullptr, FILE_BEGIN))
    {
        CloseHandle(handle);
        return MakeError(ZipStatus::IoError, "failed to size extracted file: " + destination_path.string());
    }
#else
    std::ofstream output(destination_path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return MakeError(ZipStatus::IoError, "failed to create extracted file: " + destination_path.string());
    }
#endif

    const auto inflate_output_chunk_bytes = ResolveStreamingInflateOutputChunkSize();
    const auto* compressed_data =
        reinterpret_cast<const unsigned char*>(bytes.data() + static_cast<std::size_t>(data_offset));

    std::vector<unsigned char> output_buffer(inflate_output_chunk_bytes);
    Crc32 crc;
    std::uint64_t total_output = 0;
    std::uint64_t inflate_ns = 0;
    std::uint64_t crc_ns = 0;
    std::uint64_t write_ns = 0;

    mz_stream stream {};
    stream.next_in = compressed_data;
    stream.avail_in = static_cast<mz_uint>(entry.compressed_size);

    if (mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK)
    {
        return MakeError(ZipStatus::IoError, "failed to initialize inflate decompressor");
    }

    while (true)
    {
        stream.next_out = output_buffer.data();
        stream.avail_out = static_cast<mz_uint>(output_buffer.size());

        const auto inflate_started_at = std::chrono::steady_clock::now();
        const auto inflate_status = mz_inflate(&stream, MZ_NO_FLUSH);
        inflate_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - inflate_started_at)
                .count());
        const auto produced = output_buffer.size() - static_cast<std::size_t>(stream.avail_out);

        if (produced > 0)
        {
            const auto crc_started_at = std::chrono::steady_clock::now();
            crc.Update(reinterpret_cast<const std::byte*>(output_buffer.data()), produced);
            crc_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - crc_started_at)
                    .count());
#if defined(_WIN32)
            DWORD chunk_written = 0;
            const auto write_started_at = std::chrono::steady_clock::now();
            if (!WriteFile(
                    handle,
                    output_buffer.data(),
                    static_cast<DWORD>(produced),
                    &chunk_written,
                    nullptr) ||
                chunk_written != produced)
            {
                mz_inflateEnd(&stream);
                CloseHandle(handle);
                return MakeError(ZipStatus::IoError, "failed to write extracted file: " + destination_path.string());
            }
            write_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - write_started_at)
                    .count());
#else
            const auto write_started_at = std::chrono::steady_clock::now();
            output.write(reinterpret_cast<const char*>(output_buffer.data()), static_cast<std::streamsize>(produced));
            if (!output)
            {
                mz_inflateEnd(&stream);
                return MakeError(ZipStatus::IoError, "failed to write extracted file: " + destination_path.string());
            }
            write_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - write_started_at)
                    .count());
#endif
            total_output += produced;
            if (progress)
            {
                progress(produced);
            }
        }

        if (inflate_status == MZ_STREAM_END)
        {
            break;
        }

        if (inflate_status == MZ_OK)
        {
            continue;
        }

        if (inflate_status == MZ_BUF_ERROR && produced > 0)
        {
            continue;
        }

        mz_inflateEnd(&stream);
#if defined(_WIN32)
        CloseHandle(handle);
#endif
        return MakeError(ZipStatus::InvalidJob, "deflate decompression failed while extracting: " + entry.name);
    }

    const auto end_status = mz_inflateEnd(&stream);
    if (end_status != MZ_OK)
    {
#if defined(_WIN32)
        CloseHandle(handle);
#endif
        return MakeError(ZipStatus::IoError, "failed to finalize inflate decompressor");
    }

    if (stream.total_in != entry.compressed_size || total_output != entry.uncompressed_size)
    {
#if defined(_WIN32)
        CloseHandle(handle);
#endif
        return MakeError(ZipStatus::InvalidJob, "deflate stream size mismatch while extracting: " + entry.name);
    }

    if (crc.Finalize() != entry.crc32)
    {
#if defined(_WIN32)
        CloseHandle(handle);
#endif
        return MakeError(ZipStatus::InvalidJob, "crc mismatch while extracting: " + entry.name);
    }

#if defined(_WIN32)
    CloseHandle(handle);
#endif
    std::ostringstream detail;
    detail << "chunk_kb=" << (inflate_output_chunk_bytes / 1024u)
           << " inflate_ms="
           << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::nanoseconds(inflate_ns)).count()
           << " crc_ms="
           << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::nanoseconds(crc_ns)).count()
           << " write_ms="
           << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::nanoseconds(write_ns)).count();
    TraceZipExtractEntry(entry, "stream", std::chrono::steady_clock::now() - started_at, detail.str());
    return {ZipStatus::Ok, {}};
}

ZipOperationResult ExtractDeflateEntryToMappedFile(std::span<const std::byte> bytes,
                                                   const ZipCentralDirectoryEntry& entry,
                                                   const fs::path& destination_path)
{
    const auto started_at = std::chrono::steady_clock::now();

    std::uint64_t data_offset = 0;
    auto offset_result = ReadLocalFileDataOffset(bytes, entry, data_offset);
    if (offset_result.status != ZipStatus::Ok)
    {
        return offset_result;
    }

    platform::MappedFileWriter writer;
    std::string error_message;
    if (!writer.Open(destination_path, entry.uncompressed_size, error_message))
    {
        return MakeError(ZipStatus::IoError, error_message);
    }

    platform::WritableMappedView mapped_view {};
    if (!writer.MapWindow(
            0,
            static_cast<std::size_t>(entry.uncompressed_size),
            mapped_view,
            error_message))
    {
        return MakeError(ZipStatus::IoError, error_message);
    }

    const auto compressed_size = static_cast<std::size_t>(entry.compressed_size);
    const auto* compressed_data = bytes.data() + static_cast<std::size_t>(data_offset);
    const auto success = codecs::DecompressDeflateToBufferLibdeflate(
        std::span<const std::byte> {compressed_data, compressed_size},
        mapped_view.bytes);
    if (!success)
    {
        return MakeError(ZipStatus::InvalidJob, "libdeflate decompression failed while extracting: " + entry.name);
    }

    Crc32 crc;
    if (!mapped_view.bytes.empty())
    {
        crc.Update(mapped_view.bytes.data(), mapped_view.bytes.size());
    }
    if (crc.Finalize() != entry.crc32)
    {
        return MakeError(ZipStatus::InvalidJob, "crc mismatch while extracting: " + entry.name);
    }

    TraceZipExtractEntry(entry, "mapped-libdeflate", std::chrono::steady_clock::now() - started_at);
    return {ZipStatus::Ok, {}};
}

ZipOperationResult WriteExtractedFile(std::span<const std::byte> bytes,
                                      const ZipCentralDirectoryEntry& entry,
                                      const fs::path& destination_path,
                                      storage::IStorageFactory& storage_factory,
                                      const core::ExecutionOptions& execution,
                                      const std::function<void(std::uint64_t)>& progress)
{
    if (entry.compression_method == static_cast<std::uint16_t>(codecs::ZipMethod::Store) &&
        !IsZipEntryEncrypted(entry))
    {
        std::uint64_t data_offset = 0;
        auto offset_result = ReadLocalFileDataOffset(bytes, entry, data_offset);
        if (offset_result.status != ZipStatus::Ok)
        {
            return offset_result;
        }

        const auto stored_size = static_cast<std::size_t>(entry.compressed_size);
        const auto* stored_data = bytes.data() + static_cast<std::size_t>(data_offset);
        auto write_result = WriteStoredEntryToFile(
            destination_path,
            std::span<const std::byte> {stored_data, stored_size},
            entry.crc32);
        if (write_result.status != ZipStatus::Ok)
        {
            return write_result;
        }
        if (progress)
        {
            progress(entry.uncompressed_size);
        }
        return {ZipStatus::Ok, {}};
    }

    if (entry.compression_method == static_cast<std::uint16_t>(codecs::ZipMethod::Deflate))
    {
        if (!IsZipEntryEncrypted(entry) &&
            execution.mapping_mode != core::MappingMode::ForceOff &&
            ShouldUseMappedLibdeflateExtract(entry, execution.memory_budget_mb))
        {
            auto result = ExtractDeflateEntryToMappedFile(bytes, entry, destination_path);
            if (result.status == ZipStatus::Ok && progress)
            {
                progress(entry.uncompressed_size);
            }
            return result;
        }

        if (!IsZipEntryEncrypted(entry) && ShouldUseStreamingDeflateExtract(entry))
        {
            return StreamDeflateEntryToFile(bytes, entry, destination_path, progress);
        }

        const auto started_at = std::chrono::steady_clock::now();
        std::vector<std::byte> uncompressed_bytes;
        auto decompress_result = DecompressEntryData(bytes, entry, execution, uncompressed_bytes);
        if (decompress_result.status != ZipStatus::Ok)
        {
            return decompress_result;
        }

        Crc32 crc;
        if (!uncompressed_bytes.empty())
        {
            crc.Update(uncompressed_bytes.data(), uncompressed_bytes.size());
        }
        if (crc.Finalize() != entry.crc32)
        {
            return MakeError(ZipStatus::InvalidJob, "crc mismatch while extracting: " + entry.name);
        }

        auto write_result = WriteBufferToFile(storage_factory, destination_path, uncompressed_bytes);
        if (write_result.status != ZipStatus::Ok)
        {
            return write_result;
        }

        if (progress)
        {
            progress(entry.uncompressed_size);
        }

        TraceZipExtractEntry(entry, "buffered", std::chrono::steady_clock::now() - started_at);
        return {ZipStatus::Ok, {}};
    }

    std::vector<std::byte> uncompressed_bytes;
    auto decompress_result = DecompressEntryData(bytes, entry, execution, uncompressed_bytes);
    if (decompress_result.status != ZipStatus::Ok)
    {
        return decompress_result;
    }

    Crc32 crc;
    if (!uncompressed_bytes.empty())
    {
        crc.Update(uncompressed_bytes.data(), uncompressed_bytes.size());
    }
    if (crc.Finalize() != entry.crc32)
    {
        return MakeError(ZipStatus::InvalidJob, "crc mismatch while extracting: " + entry.name);
    }

    auto write_result = WriteBufferToFile(
        storage_factory, destination_path, uncompressed_bytes);
    if (write_result.status == ZipStatus::Ok && progress)
    {
        progress(entry.uncompressed_size);
    }
    return write_result;
}

ZipOperationResult ValidateEntryData(std::span<const std::byte> bytes,
                                     const ZipCentralDirectoryEntry& entry,
                                     const core::ExecutionOptions& execution)
{
    if (entry.compression_method == static_cast<std::uint16_t>(codecs::ZipMethod::Store) &&
        !IsZipEntryEncrypted(entry))
    {
        std::uint64_t data_offset = 0;
        auto offset_result = ReadLocalFileDataOffset(bytes, entry, data_offset);
        if (offset_result.status != ZipStatus::Ok)
        {
            return offset_result;
        }

        if (entry.compressed_size != entry.uncompressed_size)
        {
            return MakeError(ZipStatus::InvalidJob, "store entry size mismatch: " + entry.name);
        }

        const auto stored_size = static_cast<std::size_t>(entry.compressed_size);
        const auto* stored_data = bytes.data() + static_cast<std::size_t>(data_offset);
        Crc32 crc;
        if (stored_size > 0)
        {
            crc.Update(stored_data, stored_size);
        }
        if (crc.Finalize() != entry.crc32)
        {
            return MakeError(ZipStatus::InvalidJob, "crc mismatch while validating: " + entry.name);
        }

        return {ZipStatus::Ok, {}};
    }

    std::vector<std::byte> uncompressed_bytes;
    auto decompress_result = DecompressEntryData(bytes, entry, execution, uncompressed_bytes);
    if (decompress_result.status != ZipStatus::Ok)
    {
        return decompress_result;
    }

    Crc32 crc;
    crc.Update(uncompressed_bytes.data(), uncompressed_bytes.size());
    if (crc.Finalize() != entry.crc32)
    {
        return MakeError(ZipStatus::InvalidJob, "crc mismatch while validating: " + entry.name);
    }

    return {ZipStatus::Ok, {}};
}

std::size_t ResolveZipParallelWorkerCount(const core::ArchiveJob& job,
                                          const core::ExecutionOptions& execution,
                                          const std::vector<ZipCentralDirectoryEntry>& entries,
                                          const std::vector<std::size_t>& file_indexes,
                                          std::size_t work_item_count,
                                          std::uint64_t total_uncompressed_bytes) noexcept
{
    if (work_item_count <= 1)
    {
        return work_item_count;
    }

    const char* override_value = std::getenv("COZIP_EXTRACT_THREADS");
    if (override_value != nullptr && *override_value != '\0')
    {
        const auto parsed = std::strtoull(override_value, nullptr, 10);
        if (parsed > 0)
        {
            return std::min<std::size_t>(static_cast<std::size_t>(parsed), work_item_count);
        }
    }

    const auto pipeline_plan = pipeline::BuildPipelinePlan(job);
    const auto configured_workers = std::max<std::size_t>(1, pipeline_plan.options.compressor_threads);
    const auto hardware_threads = std::thread::hardware_concurrency();
    const auto available_threads = hardware_threads == 0 ? 4u : hardware_threads;
    const auto hardware_budget = std::max<std::size_t>(1, available_threads > 2 ? available_threads - 2 : 1);
    const auto max_workers =
        std::min<std::size_t>(work_item_count, std::min<std::size_t>(configured_workers, hardware_budget));
    if (max_workers <= 1)
    {
        return max_workers;
    }

    constexpr std::uint64_t kBytesPerWorker = 192ull * 1024ull * 1024ull;
    constexpr std::uint64_t kMinPerWorkerMemoryBytes = 48ull * 1024ull * 1024ull;
    constexpr std::uint64_t kWorkerScratchMultiplier = 3ull;
    std::size_t parallelism_score = 0;
    std::size_t streaming_entries = 0;
    std::size_t mapped_entries = 0;
    std::uint64_t total_compressed_bytes = 0;
    for (const auto index : file_indexes)
    {
        const auto& entry = entries[index];
        total_compressed_bytes += entry.compressed_size;
        if (entry.compression_method == static_cast<std::uint16_t>(codecs::ZipMethod::Store))
        {
            if (entry.uncompressed_size >= (256ull * 1024ull * 1024ull))
            {
                parallelism_score += 8;
            }
            else if (entry.uncompressed_size >= (128ull * 1024ull * 1024ull))
            {
                parallelism_score += 6;
            }
            else if (entry.uncompressed_size >= (64ull * 1024ull * 1024ull))
            {
                parallelism_score += 4;
            }
            else
            {
                parallelism_score += 2;
            }
            continue;
        }

        if (ShouldUseMappedLibdeflateExtract(entry, execution.memory_budget_mb))
        {
            ++mapped_entries;
            parallelism_score += 9;
            continue;
        }

        if (ShouldUseStreamingDeflateExtract(entry))
        {
            ++streaming_entries;
            parallelism_score += 8;
            continue;
        }

        if (entry.compression_method == static_cast<std::uint16_t>(codecs::ZipMethod::Deflate))
        {
            parallelism_score += entry.uncompressed_size >= (8ull * 1024ull * 1024ull) ? 4 : 2;
            continue;
        }

        parallelism_score += 2;
    }

    const auto score_workers = std::max<std::size_t>(1, (parallelism_score + 7) / 8);
    const auto byte_workers = std::max<std::size_t>(
        1,
        static_cast<std::size_t>((total_uncompressed_bytes + (kBytesPerWorker - 1)) / kBytesPerWorker));
    const auto worker_scratch_bytes = std::max<std::uint64_t>(
        kMinPerWorkerMemoryBytes,
        static_cast<std::uint64_t>(pipeline_plan.options.chunk_size_bytes) * kWorkerScratchMultiplier);
    const auto memory_budget_bytes = static_cast<std::uint64_t>(execution.memory_budget_mb) * 1024ull * 1024ull;
    const auto memory_workers = memory_budget_bytes == 0
        ? max_workers
        : std::max<std::size_t>(1, static_cast<std::size_t>(memory_budget_bytes / worker_scratch_bytes));
    const auto average_expansion_x100 = total_compressed_bytes == 0
        ? 100ull
        : (total_uncompressed_bytes * 100ull) / total_compressed_bytes;
    auto io_workers = max_workers;
    if (average_expansion_x100 >= 240ull)
    {
        io_workers = std::max<std::size_t>(1, max_workers / 2);
    }
    else if (average_expansion_x100 >= 170ull)
    {
        io_workers = std::max<std::size_t>(1, (max_workers * 2) / 3);
    }

    if (mapped_entries == work_item_count && total_uncompressed_bytes <= (2ull * 1024ull * 1024ull * 1024ull))
    {
        io_workers = std::min<std::size_t>(io_workers, 4);
    }

    if (streaming_entries >= 2)
    {
        io_workers = std::min<std::size_t>(io_workers, std::max<std::size_t>(2, streaming_entries));
    }

    auto resolved_workers = std::min<std::size_t>(
        max_workers,
        std::min<std::size_t>(
            std::min<std::size_t>(score_workers, byte_workers),
            std::min<std::size_t>(memory_workers, io_workers)));
    resolved_workers = std::max<std::size_t>(1, resolved_workers);

    if (ZipExtractTraceEnabled())
    {
        std::cerr << "[cozip][extract] workers=" << resolved_workers
                  << " max_workers=" << max_workers
                  << " score_workers=" << score_workers
                  << " byte_workers=" << byte_workers
                  << " memory_workers=" << memory_workers
                  << " io_workers=" << io_workers
                  << " streaming_entries=" << streaming_entries
                  << " mapped_entries=" << mapped_entries
                  << " avg_expand_x100=" << average_expansion_x100
                  << " total_uncompressed=" << total_uncompressed_bytes
                  << " total_compressed=" << total_compressed_bytes
                  << " files=" << work_item_count
                  << '\n';
    }

    return resolved_workers;
}

std::size_t ResolveZipInitialWorkerCount(std::size_t target_worker_count,
                                         std::size_t work_item_count,
                                         std::uint64_t total_uncompressed_bytes) noexcept
{
    if (target_worker_count <= 2 || work_item_count <= 2)
    {
        return target_worker_count;
    }

    if (work_item_count <= target_worker_count + 1)
    {
        return target_worker_count;
    }

    if (work_item_count <= 5 && total_uncompressed_bytes <= (2ull * 1024ull * 1024ull * 1024ull))
    {
        return target_worker_count;
    }

    if (total_uncompressed_bytes >= (8ull * 1024ull * 1024ull * 1024ull) &&
        work_item_count <= target_worker_count + 2)
    {
        return target_worker_count;
    }

    return std::min<std::size_t>(3, target_worker_count);
}

}
