#include <algorithm>
#include <cstdlib>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "cozip/core/archive_job.h"
#include "cozip/core/archive_request.h"
#include "cozip/format_zip/zip_archive.h"
#include "cozip/storage/storage_factory.h"

namespace
{
int Expect(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "Test failed: " << message << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

struct RecordingProgressSink final : cozip::core::IProgressSink
{
    void OnProgress(const cozip::core::ProgressEvent& event) override
    {
        events.push_back(event);
    }

    std::vector<cozip::core::ProgressEvent> events;
};

struct StaticCancelToken final : cozip::core::ICancelToken
{
    [[nodiscard]] bool IsCancellationRequested() const noexcept override
    {
        return cancelled;
    }

    bool cancelled = false;
};

class MemoryRandomAccessReader final : public cozip::storage::IRandomAccessReader
{
public:
    explicit MemoryRandomAccessReader(std::vector<std::byte> bytes)
        : bytes_(std::move(bytes))
    {
    }

    [[nodiscard]] std::uint64_t Size() const noexcept override
    {
        return bytes_.size();
    }

    [[nodiscard]] cozip::storage::StorageCapabilities Capabilities() const noexcept override
    {
        cozip::storage::StorageCapabilities capabilities {};
        capabilities.supports_random_read = true;
        capabilities.supports_parallel_reads = true;
        capabilities.max_efficient_read_bytes = bytes_.size();
        return capabilities;
    }

    bool Read(std::uint64_t offset,
              std::span<std::byte> output,
              std::size_t& bytes_read,
              std::string& error_message) override
    {
        bytes_read = 0;
        if (offset > bytes_.size())
        {
            error_message = "memory reader offset is outside buffer";
            return false;
        }

        const auto remaining = bytes_.size() - static_cast<std::size_t>(offset);
        bytes_read = std::min<std::size_t>(remaining, output.size());
        if (bytes_read == 0)
        {
            return true;
        }

        std::copy_n(bytes_.data() + static_cast<std::size_t>(offset), bytes_read, output.data());
        return true;
    }

    bool TryMapWindow(std::uint64_t,
                      std::size_t,
                      cozip::storage::MappedReadWindow& window,
                      std::string&) override
    {
        window = {};
        return false;
    }

private:
    std::vector<std::byte> bytes_;
};

class MemoryStorageFactory final : public cozip::storage::IStorageFactory
{
public:
    void PutFile(std::string path, std::vector<std::byte> bytes)
    {
        files_[std::move(path)] = std::move(bytes);
    }

    [[nodiscard]] std::unique_ptr<cozip::storage::IRandomAccessReader> OpenReader(
        const std::filesystem::path& path,
        cozip::core::MappingMode,
        std::string& error_message) override
    {
        const auto key = path.generic_string();
        const auto found = files_.find(key);
        if (found == files_.end())
        {
            error_message = "memory storage file not found: " + key;
            return {};
        }

        return std::make_unique<MemoryRandomAccessReader>(found->second);
    }

    [[nodiscard]] std::unique_ptr<cozip::storage::IRandomAccessWriter> OpenWriter(
        const std::filesystem::path&,
        std::string& error_message) override
    {
        error_message = "memory writer is not implemented in this test factory";
        return {};
    }

private:
    std::map<std::string, std::vector<std::byte>, std::less<>> files_;
};

void SetChunkedCpuDisabled(bool disabled)
{
#if defined(_WIN32)
    _putenv_s("COZIP_DISABLE_CHUNKED_CPU", disabled ? "1" : "0");
#else
    if (disabled)
    {
        setenv("COZIP_DISABLE_CHUNKED_CPU", "1", 1);
    }
    else
    {
        unsetenv("COZIP_DISABLE_CHUNKED_CPU");
    }
#endif
}
} // namespace

int main()
{
    namespace fs = std::filesystem;

    cozip::core::ArchiveJob valid_create {};
    valid_create.type = cozip::core::JobType::CreateArchive;
    valid_create.format = cozip::core::ArchiveFormat::Zip;
    valid_create.output_path = "archive.zip";
    valid_create.inputs.push_back({"input-folder", true});

    if (Expect(cozip::core::IsValid(valid_create), "create job should be valid") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveJob invalid_create {};
    invalid_create.type = cozip::core::JobType::CreateArchive;
    invalid_create.format = cozip::core::ArchiveFormat::Zip;

    if (Expect(!cozip::core::IsValid(invalid_create), "empty create job should be invalid") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (Expect(std::string_view {cozip::core::ToString(cozip::core::ArchiveFormat::Zip)} == "zip",
               "zip format string should match") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveJob execution_job {};
    execution_job.execution.mapping_mode = cozip::core::MappingMode::RequireOn;
    execution_job.execution.worker_count = 3;
    execution_job.execution.chunk_size_bytes = 512ull * 1024ull;
    const auto execution = cozip::core::ResolveExecutionOptions(execution_job);
    if (Expect(execution.mapping_mode == cozip::core::MappingMode::RequireOn,
               "execution mapping mode should be preserved") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (Expect(execution.worker_count == 3,
               "execution worker count should be preserved") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (Expect(execution.chunk_size_bytes == 512ull * 1024ull,
               "execution chunk size should be preserved") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    const auto archive_request = cozip::core::MakeArchiveRequest(valid_create);
    if (Expect(archive_request.operation == cozip::core::Operation::Create,
               "archive request should map create operation") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (Expect(archive_request.inputs.size() == 1 && archive_request.inputs.front().path == "input-folder",
               "archive request should preserve input path") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    const auto roundtrip_job = cozip::core::MakeArchiveJob(archive_request);
    if (Expect(roundtrip_job.type == cozip::core::JobType::CreateArchive,
               "archive request roundtrip should preserve job type") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    const auto archive_execution_request = cozip::core::MakeArchiveExecutionRequest(archive_request);
    if (Expect(cozip::core::IsValid(archive_execution_request),
               "archive execution request should validate through archive request") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    const auto temp_root = fs::temp_directory_path() / "cozip_unit_tests";
    fs::create_directories(temp_root);

    const auto input_file = temp_root / "sample.txt";
    const auto archive_file = temp_root / "sample.zip";
    const auto deflate_archive_file = temp_root / "sample_deflate.zip";
    const auto encrypted_archive_file = temp_root / "sample_encrypted.zip";
    const auto corrupted_archive_file = temp_root / "sample_corrupt.zip";
    const auto extract_root = temp_root / "extracted";
    const auto deflate_extract_root = temp_root / "extracted_deflate";
    const auto encrypted_extract_root = temp_root / "extracted_encrypted";

    {
        std::ofstream output(input_file, std::ios::binary | std::ios::trunc);
        output << "cozip test payload";
    }

    cozip::core::ArchiveJob create_job {};
    create_job.type = cozip::core::JobType::CreateArchive;
    create_job.format = cozip::core::ArchiveFormat::Zip;
    create_job.profile = cozip::core::CompressionProfile::Store;
    create_job.output_path = archive_file.string();
    create_job.inputs.push_back({input_file.string(), false});

    const auto create_result = cozip::format_zip::Execute(create_job);
    if (Expect(create_result.status == cozip::format_zip::ZipStatus::Ok,
               "store zip create should succeed") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveRequest list_request {};
    list_request.operation = cozip::core::Operation::List;
    list_request.format = cozip::core::ArchiveFormat::Zip;
    list_request.profile = cozip::core::CompressionProfile::Balanced;
    list_request.inputs.push_back({
        .kind = cozip::core::ArchiveSourceKind::Path,
        .path = archive_file.string(),
        .recursive = false,
        .archive_path = {},
        .reader = nullptr,
    });
    const auto list_request_result = cozip::format_zip::Execute(list_request);
    if (Expect(list_request_result.status == cozip::format_zip::ZipStatus::Ok,
               "zip list through archive request should succeed") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    RecordingProgressSink progress_sink;
    cozip::core::ArchiveExecutionRequest list_execution_request {};
    list_execution_request.archive = list_request;
    list_execution_request.context.progress = &progress_sink;
    const auto list_execution_result = cozip::format_zip::Execute(list_execution_request);
    if (Expect(list_execution_result.status == cozip::format_zip::ZipStatus::Ok,
               "zip list through archive execution request should succeed") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (Expect(!progress_sink.events.empty(),
               "zip execution request should emit progress events") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (Expect(progress_sink.events.front().phase == cozip::core::ProgressPhase::Started,
               "zip execution request should emit started progress first") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (Expect(progress_sink.events.back().phase == cozip::core::ProgressPhase::Completed,
               "zip execution request should emit completed progress last") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    StaticCancelToken cancelled_token;
    cancelled_token.cancelled = true;
    cozip::core::ArchiveExecutionRequest cancelled_list_request {};
    cancelled_list_request.archive = list_request;
    cancelled_list_request.context.cancel = &cancelled_token;
    const auto cancelled_list_result = cozip::format_zip::Execute(cancelled_list_request);
    if (Expect(cancelled_list_result.status == cozip::format_zip::ZipStatus::Cancelled,
               "zip execution request should honor cancellation before start") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveJob list_job {};
    list_job.type = cozip::core::JobType::ListArchive;
    list_job.format = cozip::core::ArchiveFormat::Zip;
    list_job.inputs.push_back({archive_file.string(), false});

    const auto list_result = cozip::format_zip::Execute(list_job);
    if (Expect(list_result.status == cozip::format_zip::ZipStatus::Ok,
               "zip list should succeed for archive created by cozip") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (Expect(list_result.message.find("sample.txt") != std::string::npos,
               "zip list output should contain the file name") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    std::vector<std::byte> archive_bytes;
    {
        std::ifstream source(archive_file, std::ios::binary);
        const std::vector<char> raw_archive_bytes {
            std::istreambuf_iterator<char>(source),
            std::istreambuf_iterator<char>()};
        archive_bytes.resize(raw_archive_bytes.size());
        std::transform(
            raw_archive_bytes.begin(),
            raw_archive_bytes.end(),
            archive_bytes.begin(),
            [](char value) { return static_cast<std::byte>(static_cast<unsigned char>(value)); });
    }

    MemoryStorageFactory memory_storage;
    memory_storage.PutFile("memory/sample.zip", archive_bytes);
    cozip::core::ExecutionEnvironment memory_environment {
        .storage_factory = &memory_storage,
    };

    cozip::core::ArchiveExecutionRequest memory_list_request {};
    memory_list_request.archive.operation = cozip::core::Operation::List;
    memory_list_request.archive.format = cozip::core::ArchiveFormat::Zip;
    memory_list_request.archive.profile = cozip::core::CompressionProfile::Balanced;
    memory_list_request.archive.execution.mapping_mode = cozip::core::MappingMode::ForceOff;
    memory_list_request.archive.inputs.push_back({
        .kind = cozip::core::ArchiveSourceKind::Path,
        .path = "memory/sample.zip",
        .recursive = false,
        .archive_path = {},
        .reader = nullptr,
    });
    memory_list_request.context.environment = &memory_environment;
    const auto memory_list_result = cozip::format_zip::Execute(memory_list_request);
    if (Expect(memory_list_result.status == cozip::format_zip::ZipStatus::Ok,
               "zip list should succeed through in-memory storage factory") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (Expect(memory_list_result.message.find("sample.txt") != std::string::npos,
               "in-memory zip list output should contain the file name") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveJob test_job {};
    test_job.type = cozip::core::JobType::TestArchive;
    test_job.format = cozip::core::ArchiveFormat::Zip;
    test_job.inputs.push_back({archive_file.string(), false});

    const auto test_result = cozip::format_zip::Execute(test_job);
    if (Expect(test_result.status == cozip::format_zip::ZipStatus::Ok,
               "store zip test should succeed") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (Expect(test_result.message.find("status=ok") != std::string::npos,
               "zip test output should report ok status") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveExecutionRequest memory_test_request {};
    memory_test_request.archive.operation = cozip::core::Operation::Test;
    memory_test_request.archive.format = cozip::core::ArchiveFormat::Zip;
    memory_test_request.archive.profile = cozip::core::CompressionProfile::Balanced;
    memory_test_request.archive.execution.mapping_mode = cozip::core::MappingMode::ForceOff;
    memory_test_request.archive.inputs.push_back({
        .kind = cozip::core::ArchiveSourceKind::Path,
        .path = "memory/sample.zip",
        .recursive = false,
        .archive_path = {},
        .reader = nullptr,
    });
    memory_test_request.context.environment = &memory_environment;
    const auto memory_test_result = cozip::format_zip::Execute(memory_test_request);
    if (Expect(memory_test_result.status == cozip::format_zip::ZipStatus::Ok,
               "zip test should succeed through in-memory storage factory") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    std::vector<std::byte> memory_source_bytes;
    {
        const std::string payload = "reader-backed create payload";
        memory_source_bytes.resize(payload.size());
        std::transform(
            payload.begin(),
            payload.end(),
            memory_source_bytes.begin(),
            [](char value) { return static_cast<std::byte>(static_cast<unsigned char>(value)); });
    }
    MemoryRandomAccessReader memory_source_reader(memory_source_bytes);
    const auto reader_backed_archive = temp_root / "reader_backed.zip";
    cozip::core::ArchiveExecutionRequest reader_backed_create_request {};
    reader_backed_create_request.archive.operation = cozip::core::Operation::Create;
    reader_backed_create_request.archive.format = cozip::core::ArchiveFormat::Zip;
    reader_backed_create_request.archive.profile = cozip::core::CompressionProfile::Store;
    reader_backed_create_request.archive.execution.mapping_mode = cozip::core::MappingMode::ForceOff;
    reader_backed_create_request.archive.output_path = reader_backed_archive.string();
    reader_backed_create_request.archive.inputs.push_back({
        .kind = cozip::core::ArchiveSourceKind::ReaderFile,
        .path = {},
        .recursive = false,
        .archive_path = "memory-source.txt",
        .reader = &memory_source_reader,
    });
    const auto reader_backed_create_result = cozip::format_zip::Execute(reader_backed_create_request);
    if (Expect(reader_backed_create_result.status == cozip::format_zip::ZipStatus::Ok,
               "zip create should succeed through reader-backed archive source") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveJob reader_backed_list_job {};
    reader_backed_list_job.type = cozip::core::JobType::ListArchive;
    reader_backed_list_job.format = cozip::core::ArchiveFormat::Zip;
    reader_backed_list_job.inputs.push_back({reader_backed_archive.string(), false});

    const auto reader_backed_list_result = cozip::format_zip::Execute(reader_backed_list_job);
    if (Expect(reader_backed_list_result.status == cozip::format_zip::ZipStatus::Ok,
               "zip list should succeed for reader-backed created archive") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (Expect(reader_backed_list_result.message.find("memory-source.txt") != std::string::npos,
               "reader-backed create archive should contain the requested archive path") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    {
        std::ifstream source(archive_file, std::ios::binary);
        std::vector<char> archive_bytes(
            (std::istreambuf_iterator<char>(source)),
            std::istreambuf_iterator<char>());

        const std::string payload = "cozip test payload";
        const auto payload_begin = std::search(
            archive_bytes.begin(),
            archive_bytes.end(),
            payload.begin(),
            payload.end());

        if (Expect(payload_begin != archive_bytes.end(),
                   "archive should contain the stored payload bytes") != EXIT_SUCCESS)
        {
            return EXIT_FAILURE;
        }

        *payload_begin = 'x';

        std::ofstream corrupted_output(corrupted_archive_file, std::ios::binary | std::ios::trunc);
        corrupted_output.write(archive_bytes.data(), static_cast<std::streamsize>(archive_bytes.size()));
    }

    cozip::core::ArchiveJob corrupt_test_job {};
    corrupt_test_job.type = cozip::core::JobType::TestArchive;
    corrupt_test_job.format = cozip::core::ArchiveFormat::Zip;
    corrupt_test_job.inputs.push_back({corrupted_archive_file.string(), false});

    const auto corrupt_test_result = cozip::format_zip::Execute(corrupt_test_job);
    if (Expect(corrupt_test_result.status != cozip::format_zip::ZipStatus::Ok,
               "corrupted zip test should fail") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveJob extract_job {};
    extract_job.type = cozip::core::JobType::ExtractArchive;
    extract_job.format = cozip::core::ArchiveFormat::Zip;
    extract_job.output_path = extract_root.string();
    extract_job.inputs.push_back({archive_file.string(), false});

    const auto extract_result = cozip::format_zip::Execute(extract_job);
    if (Expect(extract_result.status == cozip::format_zip::ZipStatus::Ok,
               "store zip extract should succeed") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    const auto extracted_file = extract_root / "sample.txt";
    if (Expect(fs::exists(extracted_file), "extracted file should exist") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    std::ifstream extracted_input(extracted_file, std::ios::binary);
    std::string extracted_text;
    std::getline(extracted_input, extracted_text);
    if (Expect(extracted_text == "cozip test payload",
               "extracted file content should match original") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveJob deflate_create_job {};
    deflate_create_job.type = cozip::core::JobType::CreateArchive;
    deflate_create_job.format = cozip::core::ArchiveFormat::Zip;
    deflate_create_job.profile = cozip::core::CompressionProfile::Balanced;
    deflate_create_job.output_path = deflate_archive_file.string();
    deflate_create_job.inputs.push_back({input_file.string(), false});

    const auto deflate_create_request = cozip::format_zip::MakeExecutionRequest(deflate_create_job);
    const auto deflate_create_result = cozip::format_zip::Execute(deflate_create_request);
    if (Expect(deflate_create_result.status == cozip::format_zip::ZipStatus::Ok,
               "deflate zip create should succeed") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveJob force_off_create_job {};
    force_off_create_job.type = cozip::core::JobType::CreateArchive;
    force_off_create_job.format = cozip::core::ArchiveFormat::Zip;
    force_off_create_job.profile = cozip::core::CompressionProfile::Balanced;
    force_off_create_job.output_path = (temp_root / "sample_force_off.zip").string();
    force_off_create_job.execution.mapping_mode = cozip::core::MappingMode::ForceOff;
    force_off_create_job.inputs.push_back({input_file.string(), false});

    const auto force_off_create_result =
        cozip::format_zip::Execute(cozip::format_zip::MakeExecutionRequest(force_off_create_job));
    if (Expect(force_off_create_result.status == cozip::format_zip::ZipStatus::Ok,
               "deflate zip create should succeed with mapping forced off") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveJob deflate_list_job {};
    deflate_list_job.type = cozip::core::JobType::ListArchive;
    deflate_list_job.format = cozip::core::ArchiveFormat::Zip;
    deflate_list_job.inputs.push_back({deflate_archive_file.string(), false});

    const auto deflate_list_result = cozip::format_zip::Execute(deflate_list_job);
    if (Expect(deflate_list_result.status == cozip::format_zip::ZipStatus::Ok,
               "deflate zip list should succeed") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (Expect(deflate_list_result.message.find("sample.txt") != std::string::npos,
               "deflate zip list output should report the archived file name") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveJob deflate_test_job {};
    deflate_test_job.type = cozip::core::JobType::TestArchive;
    deflate_test_job.format = cozip::core::ArchiveFormat::Zip;
    deflate_test_job.inputs.push_back({deflate_archive_file.string(), false});

    const auto deflate_test_result = cozip::format_zip::Execute(deflate_test_job);
    if (Expect(deflate_test_result.status == cozip::format_zip::ZipStatus::Ok,
               "deflate zip test should succeed") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveJob deflate_extract_job {};
    deflate_extract_job.type = cozip::core::JobType::ExtractArchive;
    deflate_extract_job.format = cozip::core::ArchiveFormat::Zip;
    deflate_extract_job.output_path = deflate_extract_root.string();
    deflate_extract_job.inputs.push_back({deflate_archive_file.string(), false});

    const auto deflate_extract_result = cozip::format_zip::Execute(deflate_extract_job);
    if (Expect(deflate_extract_result.status == cozip::format_zip::ZipStatus::Ok,
               "deflate zip extract should succeed") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    const auto deflate_extracted_file = deflate_extract_root / "sample.txt";
    if (Expect(fs::exists(deflate_extracted_file), "deflate extracted file should exist") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    std::ifstream deflate_extracted_input(deflate_extracted_file, std::ios::binary);
    std::string deflate_extracted_text;
    std::getline(deflate_extracted_input, deflate_extracted_text);
    if (Expect(deflate_extracted_text == "cozip test payload",
               "deflate extracted file content should match original") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveJob encrypted_create_job {};
    encrypted_create_job.type = cozip::core::JobType::CreateArchive;
    encrypted_create_job.format = cozip::core::ArchiveFormat::Zip;
    encrypted_create_job.profile = cozip::core::CompressionProfile::Balanced;
    encrypted_create_job.output_path = encrypted_archive_file.string();
    encrypted_create_job.execution.encryption.mode = cozip::core::EncryptionMode::ZipTraditional;
    encrypted_create_job.execution.encryption.password = "secret123";
    encrypted_create_job.inputs.push_back({input_file.string(), false});

    const auto encrypted_create_result = cozip::format_zip::Execute(encrypted_create_job);
    if (Expect(encrypted_create_result.status == cozip::format_zip::ZipStatus::Ok,
               "encrypted zip create should succeed") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    {
        std::ifstream source(encrypted_archive_file, std::ios::binary);
        std::vector<char> archive_bytes(
            (std::istreambuf_iterator<char>(source)),
            std::istreambuf_iterator<char>());
        const std::string payload = "cozip test payload";
        const auto payload_begin = std::search(
            archive_bytes.begin(),
            archive_bytes.end(),
            payload.begin(),
            payload.end());
        if (Expect(payload_begin == archive_bytes.end(),
                   "encrypted archive should not contain the plaintext payload bytes") != EXIT_SUCCESS)
        {
            return EXIT_FAILURE;
        }
    }

    cozip::core::ArchiveJob encrypted_list_job {};
    encrypted_list_job.type = cozip::core::JobType::ListArchive;
    encrypted_list_job.format = cozip::core::ArchiveFormat::Zip;
    encrypted_list_job.inputs.push_back({encrypted_archive_file.string(), false});

    const auto encrypted_list_result = cozip::format_zip::Execute(encrypted_list_job);
    if (Expect(encrypted_list_result.status == cozip::format_zip::ZipStatus::Ok,
               "encrypted zip list should succeed without a password") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (Expect(encrypted_list_result.message.find("encrypted=yes") != std::string::npos,
               "encrypted zip list should report encrypted entries") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveJob encrypted_test_job {};
    encrypted_test_job.type = cozip::core::JobType::TestArchive;
    encrypted_test_job.format = cozip::core::ArchiveFormat::Zip;
    encrypted_test_job.execution.encryption.mode = cozip::core::EncryptionMode::ZipTraditional;
    encrypted_test_job.execution.encryption.password = "secret123";
    encrypted_test_job.inputs.push_back({encrypted_archive_file.string(), false});

    const auto encrypted_test_result = cozip::format_zip::Execute(encrypted_test_job);
    if (Expect(encrypted_test_result.status == cozip::format_zip::ZipStatus::Ok,
               "encrypted zip test should succeed with the correct password") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveJob encrypted_test_wrong_password_job = encrypted_test_job;
    encrypted_test_wrong_password_job.execution.encryption.password = "wrong";
    const auto encrypted_test_wrong_password_result = cozip::format_zip::Execute(encrypted_test_wrong_password_job);
    if (Expect(encrypted_test_wrong_password_result.status != cozip::format_zip::ZipStatus::Ok,
               "encrypted zip test should fail with the wrong password") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveJob encrypted_extract_job {};
    encrypted_extract_job.type = cozip::core::JobType::ExtractArchive;
    encrypted_extract_job.format = cozip::core::ArchiveFormat::Zip;
    encrypted_extract_job.output_path = encrypted_extract_root.string();
    encrypted_extract_job.execution.encryption.mode = cozip::core::EncryptionMode::ZipTraditional;
    encrypted_extract_job.execution.encryption.password = "secret123";
    encrypted_extract_job.inputs.push_back({encrypted_archive_file.string(), false});

    const auto encrypted_extract_result = cozip::format_zip::Execute(encrypted_extract_job);
    if (Expect(encrypted_extract_result.status == cozip::format_zip::ZipStatus::Ok,
               "encrypted zip extract should succeed with the correct password") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    const auto encrypted_extracted_file = encrypted_extract_root / "sample.txt";
    if (Expect(fs::exists(encrypted_extracted_file),
               "encrypted extracted file should exist") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    std::ifstream encrypted_extracted_input(encrypted_extracted_file, std::ios::binary);
    std::string encrypted_extracted_text;
    std::getline(encrypted_extracted_input, encrypted_extracted_text);
    if (Expect(encrypted_extracted_text == "cozip test payload",
               "encrypted extracted file content should match original") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveJob encrypted_extract_wrong_password_job = encrypted_extract_job;
    encrypted_extract_wrong_password_job.output_path = (temp_root / "extracted_encrypted_wrong").string();
    encrypted_extract_wrong_password_job.execution.encryption.password = "wrong";
    const auto encrypted_extract_wrong_password_result =
        cozip::format_zip::Execute(encrypted_extract_wrong_password_job);
    if (Expect(encrypted_extract_wrong_password_result.status != cozip::format_zip::ZipStatus::Ok,
               "encrypted zip extract should fail with the wrong password") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveJob force_off_extract_job {};
    force_off_extract_job.type = cozip::core::JobType::ExtractArchive;
    force_off_extract_job.format = cozip::core::ArchiveFormat::Zip;
    force_off_extract_job.output_path = (temp_root / "extracted_force_off").string();
    force_off_extract_job.execution.mapping_mode = cozip::core::MappingMode::ForceOff;
    force_off_extract_job.inputs.push_back({deflate_archive_file.string(), false});

    const auto request = cozip::format_zip::MakeExecutionRequest(force_off_extract_job);
    if (Expect(request.job == &force_off_extract_job,
               "zip execution request should keep archive job reference") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (Expect(request.execution.mapping_mode == cozip::core::MappingMode::ForceOff,
               "zip execution request should preserve mapping mode") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    const auto force_off_extract_result = cozip::format_zip::Execute(request);
    if (Expect(force_off_extract_result.status == cozip::format_zip::ZipStatus::Ok,
               "deflate extract should succeed with mapping forced off") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    const auto force_off_extracted_file = temp_root / "extracted_force_off" / "sample.txt";
    if (Expect(fs::exists(force_off_extracted_file),
               "force-off extracted file should exist") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    const auto large_input_file = temp_root / "large_fast_input.bin";
    const auto large_chunked_archive = temp_root / "large_chunked_fast.zip";
    const auto large_fallback_archive = temp_root / "large_fallback_fast.zip";
    const auto large_extract_root = temp_root / "large_extract";
    {
        std::ofstream output(large_input_file, std::ios::binary | std::ios::trunc);
        std::string pattern(1024 * 1024, '\0');
        for (std::size_t index = 0; index < pattern.size(); ++index)
        {
            pattern[index] = static_cast<char>('A' + (index % 23));
        }
        for (std::size_t block = 0; block < 256; ++block)
        {
            output.write(pattern.data(), static_cast<std::streamsize>(pattern.size()));
        }
    }

    cozip::core::ArchiveJob large_fast_job {};
    large_fast_job.type = cozip::core::JobType::CreateArchive;
    large_fast_job.format = cozip::core::ArchiveFormat::Zip;
    large_fast_job.profile = cozip::core::CompressionProfile::Fast;
    large_fast_job.output_path = large_chunked_archive.string();
    large_fast_job.inputs.push_back({large_input_file.string(), false});

    SetChunkedCpuDisabled(false);
    const auto large_fast_result = cozip::format_zip::Execute(large_fast_job);
    if (Expect(large_fast_result.status == cozip::format_zip::ZipStatus::Ok,
               "large fast zip create should succeed with chunked cpu path enabled") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveJob large_fast_test_job {};
    large_fast_test_job.type = cozip::core::JobType::TestArchive;
    large_fast_test_job.format = cozip::core::ArchiveFormat::Zip;
    large_fast_test_job.inputs.push_back({large_chunked_archive.string(), false});
    const auto large_fast_test_result = cozip::format_zip::Execute(large_fast_test_job);
    if (Expect(large_fast_test_result.status == cozip::format_zip::ZipStatus::Ok,
               "large fast zip test should succeed with chunked cpu path enabled") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveJob large_fast_extract_job {};
    large_fast_extract_job.type = cozip::core::JobType::ExtractArchive;
    large_fast_extract_job.format = cozip::core::ArchiveFormat::Zip;
    large_fast_extract_job.output_path = large_extract_root.string();
    large_fast_extract_job.inputs.push_back({large_chunked_archive.string(), false});
    const auto large_fast_extract_result = cozip::format_zip::Execute(large_fast_extract_job);
    if (Expect(large_fast_extract_result.status == cozip::format_zip::ZipStatus::Ok,
               "large fast zip extract should succeed with chunked cpu path enabled") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    const auto large_extracted_file = large_extract_root / "large_fast_input.bin";
    if (Expect(fs::exists(large_extracted_file),
               "large fast extracted file should exist") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }
    if (Expect(fs::file_size(large_extracted_file) == fs::file_size(large_input_file),
               "large fast extracted file size should match original") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveJob large_fallback_job = large_fast_job;
    large_fallback_job.output_path = large_fallback_archive.string();
    SetChunkedCpuDisabled(true);
    const auto large_fallback_result = cozip::format_zip::Execute(large_fallback_job);
    if (Expect(large_fallback_result.status == cozip::format_zip::ZipStatus::Ok,
               "large fast zip create should succeed with chunked cpu path disabled") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }
    SetChunkedCpuDisabled(false);

    const auto chunked_size = fs::file_size(large_chunked_archive);
    const auto fallback_size = fs::file_size(large_fallback_archive);
    if (Expect(chunked_size > 0 && fallback_size > 0,
               "large fast archives should be created in both chunked and fallback modes") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    std::cout << "cozip_unit_tests passed\n";
    return EXIT_SUCCESS;
}
