#pragma once

#include "zip_archive_streams.h"

#include <functional>

namespace cozip::format_zip
{
std::string NormalizeArchivePath(const fs::path& path);
fs::path Utf8Path(const std::string& value);
std::string LowercaseAscii(std::string value);
bool IsSamePath(const fs::path& left, const fs::path& right);
bool FitsInUint16(std::size_t value) noexcept;
bool FitsInUint32(std::uintmax_t value) noexcept;
bool ShouldPreferStoreForZipEntry(const ZipEntrySource& entry) noexcept;
bool ShouldStorePreparedDeflateResult(std::size_t input_size, std::size_t compressed_size) noexcept;
void WriteU16(std::ostream& stream, std::uint16_t value);
void WriteU32(std::ostream& stream, std::uint32_t value);
void WriteU64(std::ostream& stream, std::uint64_t value);
std::uint16_t ReadU16(std::span<const std::byte> data, std::size_t offset);
std::uint32_t ReadU32(std::span<const std::byte> data, std::size_t offset);
std::uint64_t ReadU64(std::span<const std::byte> data, std::size_t offset);
bool EntryNeedsZip64CentralDirectoryOffset(const ZipEntrySource& entry) noexcept;
std::vector<std::byte> BuildCentralDirectoryZip64ExtraField(const ZipEntrySource& entry);
ZipOperationResult MakeError(ZipStatus status, std::string message);
ZipOperationResult MakeCancelled(std::string message);
bool IsCancellationRequested(const core::ExecutionContext& context) noexcept;
void ReportProgress(const core::ExecutionContext& context, core::ProgressEvent event);
void ReportDiagnostic(const core::ExecutionContext& context, core::DiagnosticSeverity severity, std::string message, std::string path = {});
std::string DescribeJobOperation(core::JobType type);
storage::IStorageFactory& DefaultStorageFactory();
storage::IStorageFactory& ResolveStorageFactory(const core::ExecutionContext& context);
bool OpenRandomAccessReader(storage::IStorageFactory& factory, const fs::path& path, core::MappingMode mapping_mode, std::unique_ptr<storage::IRandomAccessReader>& reader, std::string& error_message);
bool OpenRandomAccessWriter(storage::IStorageFactory& factory, const fs::path& path, std::unique_ptr<storage::IRandomAccessWriter>& writer, std::string& error_message);
bool ShouldTryMapReader(const storage::IRandomAccessReader& reader, core::MappingMode mapping_mode, std::uint64_t file_size, std::size_t requested_size, std::uintmax_t threshold) noexcept;
codecs::ZipMethod SelectZipMethod(const core::ArchiveJob& job) noexcept;
int ToMinizLevel(codecs::ZipMethod method, core::CompressionProfile profile) noexcept;
bool IsSupportedWriterMethod(codecs::ZipMethod method) noexcept;
bool IsSupportedReaderMethod(std::uint16_t method) noexcept;
const char* ZipMethodName(std::uint16_t method) noexcept;
std::string EntrySourceLabel(const ZipEntrySource& entry);
bool AddDirectoryEntry(const fs::path& directory_path, const fs::path& archive_relative_path, std::vector<ZipEntrySource>& entries);
ZipOperationResult CollectRegularFile(const fs::path& file_path, const fs::path& archive_relative_path, std::vector<ZipEntrySource>& entries);
ZipOperationResult CollectReaderFile(const core::ArchiveSource& input, std::vector<ZipEntrySource>& entries);
ZipOperationResult CollectInputEntries(const core::ArchiveInput& input, const fs::path& output_path, std::vector<ZipEntrySource>& entries);
ZipOperationResult CollectEntries(const core::ArchiveJob& job, const core::ExecutionContext& context, const core::ArchiveRequest* archive_request, std::vector<ZipEntrySource>& entries);
std::size_t ResolveLibdeflateWholeBufferThreshold(const pipeline::PipelineOptions& pipeline_options, std::size_t memory_budget_mb) noexcept;
std::size_t ResolveZipStreamChunkSize(const ZipEntrySource& entry, const pipeline::PipelineOptions& pipeline_options) noexcept;
ZipOperationResult WriteLocalFileHeader(std::ostream& output, const ZipEntrySource& entry);
ZipOperationResult WriteDataDescriptor(std::ostream& output, const ZipEntrySource& entry);
ZipOperationResult StreamStoreEntry(std::istream& input, std::ostream& output, std::vector<std::byte>& buffer, ZipEntrySource& entry);
ZipOperationResult ReadAllBytes(storage::IRandomAccessReader& reader, std::uint64_t offset, std::span<std::byte> destination);
ZipOperationResult LoadFileBytes(storage::IStorageFactory& storage_factory, const fs::path& path, std::vector<std::byte>& bytes);
ZipOperationResult ReadAllBytes(storage::IRandomAccessReader& reader, std::uint64_t offset, std::vector<std::byte>& bytes);
ZipOperationResult LoadWholeFileInput(storage::IStorageFactory& storage_factory, const fs::path& path, core::MappingMode mapping_mode, storage::IRandomAccessReader* existing_reader, WholeFileInput& input);
ZipOperationResult LoadFileSample(storage::IStorageFactory& storage_factory, const fs::path& path, std::size_t sample_size, core::MappingMode mapping_mode, storage::IRandomAccessReader* existing_reader, std::vector<std::byte>& bytes);
ZipOperationResult CreateZipArchive(const core::ArchiveJob& job, const core::ArchiveRequest* archive_request, const core::ExecutionContext& context);
ZipOperationResult LoadArchiveInput(const fs::path& archive_path, storage::IStorageFactory& storage_factory, core::MappingMode mapping_mode, ArchiveInput& input);
ZipOperationResult ParseCentralDirectory(std::span<const std::byte> bytes, std::vector<ZipCentralDirectoryEntry>& entries);
ZipOperationResult ListArchive(const core::ArchiveJob& job, const core::ExecutionContext& context);
core::EncryptionMode ResolveZipEncryptionMode(const core::ExecutionOptions& execution) noexcept;
bool IsZipEntryEncrypted(const ZipCentralDirectoryEntry& entry) noexcept;
ZipOperationResult ValidateZipEncryptionOptions(const core::ExecutionOptions& execution);
ZipOperationResult EncryptZipEntryPayload(std::span<const std::byte> compressed_bytes, const ZipEntrySource& entry, const core::ExecutionOptions& execution, std::vector<std::byte>& encrypted_bytes);
ZipOperationResult DecryptZipEntryPayload(std::span<const std::byte> encrypted_bytes, const ZipCentralDirectoryEntry& entry, std::uint16_t password_verifier, const core::ExecutionOptions& execution, std::vector<std::byte>& decrypted_bytes);
bool HasPathTraversal(std::string_view path);
ZipOperationResult ResolveExtractionRoot(const core::ArchiveJob& job, fs::path& output_root);
bool ZipExtractTraceEnabled() noexcept;
ZipOperationResult WriteExtractedFile(std::span<const std::byte> bytes, const ZipCentralDirectoryEntry& entry, const fs::path& destination_path, storage::IStorageFactory& storage_factory, const core::ExecutionOptions& execution, const std::function<void(std::uint64_t)>& progress = {});
ZipOperationResult ValidateEntryData(std::span<const std::byte> bytes, const ZipCentralDirectoryEntry& entry, const core::ExecutionOptions& execution);
std::size_t ResolveZipParallelWorkerCount(const core::ArchiveJob& job, const core::ExecutionOptions& execution, const std::vector<ZipCentralDirectoryEntry>& entries, const std::vector<std::size_t>& file_indexes, std::size_t work_item_count, std::uint64_t total_uncompressed_bytes) noexcept;
std::size_t ResolveZipInitialWorkerCount(std::size_t target_worker_count, std::size_t work_item_count, std::uint64_t total_uncompressed_bytes) noexcept;
ZipOperationResult ExtractArchive(const core::ArchiveJob& job, const core::ExecutionContext& context);
ZipOperationResult TestArchive(const core::ArchiveJob& job, const core::ExecutionContext& context);
}
