#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cozip/core/archive_job.h"
#include "cozip/core/archive_request.h"
#include "cozip/format_zip/zip_archive.h"
#include "cozip/platform/filesystem_random_access.h"
#include "cozip/storage/storage_factory.h"

namespace
{
namespace fs = std::filesystem;

int Expect(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "Test failed: " << message << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

struct CancelAfterFirstFileToken final : cozip::core::ICancelToken
{
    [[nodiscard]] bool IsCancellationRequested() const noexcept override
    {
        return calls.fetch_add(1, std::memory_order_relaxed) >= 2;
    }

    mutable std::atomic<std::size_t> calls {0};
};

struct ProgressSink final : cozip::core::IProgressSink
{
    void OnProgress(const cozip::core::ProgressEvent& event) override
    {
        if (event.phase != cozip::core::ProgressPhase::WritingOutput)
        {
            return;
        }
        events.push_back(event);
    }

    std::vector<cozip::core::ProgressEvent> events;
};

class FailingWriter final : public cozip::storage::IRandomAccessWriter
{
public:
    [[nodiscard]] std::uint64_t Size() const noexcept override { return 0; }
    [[nodiscard]] cozip::storage::StorageCapabilities Capabilities() const noexcept override
    {
        cozip::storage::StorageCapabilities capabilities {};
        capabilities.supports_random_write = true;
        return capabilities;
    }
    bool Write(std::uint64_t,
               std::span<const std::byte>,
               std::string& error) override
    {
        error = "injected extraction write failure";
        return false;
    }
    bool Resize(std::uint64_t, std::string&) override { return true; }
    bool Flush(std::string&) override { return true; }
};

class FailingOutputFactory final : public cozip::storage::IStorageFactory
{
public:
    [[nodiscard]] std::unique_ptr<cozip::storage::IRandomAccessReader> OpenReader(
        const fs::path& path,
        cozip::core::MappingMode mode,
        std::string& error) override
    {
        auto reader = std::make_unique<cozip::platform::FilesystemRandomAccessReader>(mode);
        if (!reader->Open(path, error))
        {
            return {};
        }
        return reader;
    }

    [[nodiscard]] std::unique_ptr<cozip::storage::IRandomAccessWriter> OpenWriter(
        const fs::path&,
        std::string&) override
    {
        return std::make_unique<FailingWriter>();
    }
};

class TrackingReader final : public cozip::storage::IRandomAccessReader
{
public:
    TrackingReader(const fs::path& path,
                   cozip::core::MappingMode mode,
                   std::atomic<std::size_t>& maximum_read,
                   std::string& error)
        : source_(mode), maximum_read_(maximum_read)
    {
        ready_ = source_.Open(path, error);
    }
    [[nodiscard]] bool Ready() const noexcept { return ready_; }
    [[nodiscard]] std::uint64_t Size() const noexcept override { return source_.Size(); }
    [[nodiscard]] cozip::storage::StorageCapabilities Capabilities() const noexcept override
    {
        return source_.Capabilities();
    }
    bool Read(std::uint64_t offset,
              std::span<std::byte> output,
              std::size_t& bytes_read,
              std::string& error) override
    {
        auto observed = maximum_read_.load(std::memory_order_relaxed);
        while (observed < output.size() &&
               !maximum_read_.compare_exchange_weak(
                   observed, output.size(), std::memory_order_relaxed)) {}
        return source_.Read(offset, output, bytes_read, error);
    }
    bool TryMapWindow(std::uint64_t,
                      std::size_t,
                      cozip::storage::MappedReadWindow&,
                      std::string&) override
    {
        return false;
    }
private:
    cozip::platform::FilesystemRandomAccessReader source_;
    std::atomic<std::size_t>& maximum_read_;
    bool ready_{};
};

class TrackingStorageFactory final : public cozip::storage::IStorageFactory
{
public:
    [[nodiscard]] std::unique_ptr<cozip::storage::IRandomAccessReader> OpenReader(
        const fs::path& path,
        cozip::core::MappingMode mode,
        std::string& error) override
    {
        auto reader = std::make_unique<TrackingReader>(path, mode, maximum_read, error);
        return reader->Ready() ? std::move(reader) : nullptr;
    }
    [[nodiscard]] std::unique_ptr<cozip::storage::IRandomAccessWriter> OpenWriter(
        const fs::path& path,
        std::string& error) override
    {
        auto writer = std::make_unique<cozip::platform::FilesystemRandomAccessWriter>();
        return writer->Open(path, error) ? std::move(writer) : nullptr;
    }
    std::atomic<std::size_t> maximum_read{};
};

void WritePatternFile(const fs::path& path, std::size_t size)
{
    std::vector<char> block(1024u * 1024u);
    for (std::size_t index = 0; index < block.size(); ++index)
    {
        block[index] = static_cast<char>((index * 29u + index / 101u) % 251u);
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    std::size_t written = 0;
    while (written < size)
    {
        const auto count = std::min(block.size(), size - written);
        output.write(block.data(), static_cast<std::streamsize>(count));
        written += count;
    }
}

cozip::format_zip::ZipOperationResult CreateArchive(
    const std::vector<fs::path>& inputs,
    const fs::path& archive,
    cozip::core::CompressionProfile profile)
{
    cozip::core::ArchiveJob job {};
    job.type = cozip::core::JobType::CreateArchive;
    job.format = cozip::core::ArchiveFormat::Zip;
    job.profile = profile;
    job.output_path = archive.string();
    job.execution.worker_count = 4;
    job.execution.chunk_size_bytes = 8u * 1024u * 1024u;
    job.execution.max_in_flight_chunks = 8;
    job.execution.memory_budget_mb = 256;
    for (const auto& input : inputs)
    {
        job.inputs.push_back({input.string(), false});
    }
    return cozip::format_zip::Execute(job);
}

cozip::format_zip::ZipOperationResult ExtractArchive(
    const fs::path& archive,
    const fs::path& output,
    cozip::core::MappingMode mapping_mode,
    ProgressSink* progress,
    cozip::core::ICancelToken* cancel = nullptr,
    cozip::storage::IStorageFactory* storage_factory = nullptr,
    bool incremental = false)
{
    cozip::core::ArchiveExecutionRequest request {};
    request.archive.operation = cozip::core::Operation::Extract;
    request.archive.format = cozip::core::ArchiveFormat::Zip;
    request.archive.output_path = output.string();
    request.archive.execution.worker_count = 1;
    request.archive.execution.memory_budget_mb = 256;
    request.archive.execution.mapping_mode = mapping_mode;
    request.archive.execution.incremental_extract = incremental;
    request.archive.execution.chunk_size_bytes = 1024u * 1024u;
    request.archive.inputs.push_back({
        .kind = cozip::core::ArchiveSourceKind::Path,
        .path = archive.string(),
        .recursive = false,
    });
    cozip::core::ExecutionEnvironment environment {};
    environment.storage_factory = storage_factory;
    request.context.progress = progress;
    request.context.cancel = cancel;
    request.context.environment = storage_factory != nullptr ? &environment : nullptr;
    return cozip::format_zip::Execute(request);
}

int ValidateProgress(const ProgressSink& progress,
                     std::uint64_t expected_bytes,
                     std::size_t expected_items)
{
    if (Expect(!progress.events.empty(), "extract should publish writing progress") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }
    std::uint64_t previous_bytes = 0;
    std::size_t previous_items = 0;
    for (const auto& event : progress.events)
    {
        if (Expect(event.completed_bytes >= previous_bytes,
                   "extract byte progress must be monotonic") != EXIT_SUCCESS ||
            Expect(event.completed_bytes <= expected_bytes,
                   "extract byte progress must not double-count writes") != EXIT_SUCCESS ||
            Expect(event.completed_items >= previous_items,
                   "extract item progress must be monotonic") != EXIT_SUCCESS)
        {
            return EXIT_FAILURE;
        }
        previous_bytes = event.completed_bytes;
        previous_items = event.completed_items;
    }
    return Expect(previous_bytes == expected_bytes,
                  "extract byte progress must equal bytes written") != EXIT_SUCCESS ||
            Expect(previous_items == expected_items,
                   "extract item progress must equal files written") != EXIT_SUCCESS
        ? EXIT_FAILURE
        : EXIT_SUCCESS;
}

int RunProgressCase(const fs::path& root,
                    std::string_view name,
                    std::size_t size,
                    cozip::core::CompressionProfile profile,
                    cozip::core::MappingMode mapping_mode,
                    bool expect_intermediate,
                    bool incremental = false)
{
    const auto input = root / (std::string(name) + ".bin");
    const auto archive = root / (std::string(name) + ".zip");
    const auto output = root / (std::string(name) + "-out");
    WritePatternFile(input, size);
    const auto create_result = CreateArchive({input}, archive, profile);
    if (Expect(create_result.status == cozip::format_zip::ZipStatus::Ok,
               "progress test archive create should succeed") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }
    ProgressSink progress;
    const auto extract_result = ExtractArchive(
        archive, output, mapping_mode, &progress, nullptr, nullptr, incremental);
    const auto has_intermediate = std::any_of(
        progress.events.begin(),
        progress.events.end(),
        [size](const auto& event) {
            return event.completed_bytes > 0 && event.completed_bytes < size;
        });
    if (Expect(extract_result.status == cozip::format_zip::ZipStatus::Ok,
               "progress test archive extract should succeed") != EXIT_SUCCESS ||
        ValidateProgress(progress, size, 1) != EXIT_SUCCESS ||
        Expect(!expect_intermediate || has_intermediate,
               "small Deflate extraction should publish real intermediate bytes") != EXIT_SUCCESS ||
        Expect(fs::file_size(output / input.filename()) == size,
               "extracted output size should match source") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
} // namespace

int main()
{
    static std::atomic<std::size_t> sequence {0};
    const auto root = fs::temp_directory_path() /
        ("cozip_extract_progress_" +
         std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
    fs::create_directories(root);

    const auto fail = [&](int result) {
        std::error_code error;
        fs::remove_all(root, error);
        return result;
    };

    if (RunProgressCase(
            root, "store", 1u * 1024u * 1024u,
            cozip::core::CompressionProfile::Store,
            cozip::core::MappingMode::ForceOff,
            false,
            true) != EXIT_SUCCESS ||
        RunProgressCase(
            root, "small-deflate", 2u * 1024u * 1024u,
            cozip::core::CompressionProfile::Fast,
            cozip::core::MappingMode::ForceOff,
            true,
            true) != EXIT_SUCCESS ||
        RunProgressCase(
            root, "mapped", 32u * 1024u * 1024u,
            cozip::core::CompressionProfile::Fast,
            cozip::core::MappingMode::PreferOn,
            true) != EXIT_SUCCESS ||
        RunProgressCase(
            root, "streaming", 64u * 1024u * 1024u,
            cozip::core::CompressionProfile::Fast,
            cozip::core::MappingMode::ForceOff,
            true) != EXIT_SUCCESS)
    {
        return fail(EXIT_FAILURE);
    }

    TrackingStorageFactory tracking_factory;
    const auto bounded_result = ExtractArchive(
        root / "small-deflate.zip",
        root / "bounded-out",
        cozip::core::MappingMode::ForceOff,
        nullptr,
        nullptr,
        &tracking_factory,
        true);
    if (Expect(bounded_result.status == cozip::format_zip::ZipStatus::Ok,
               "incremental extraction through a tracked reader should succeed") != EXIT_SUCCESS ||
        Expect(tracking_factory.maximum_read.load(std::memory_order_relaxed) <= 1024u * 1024u,
               "incremental extraction must keep every archive read bounded") != EXIT_SUCCESS)
    {
        return fail(EXIT_FAILURE);
    }

    const auto cancel_a = root / "cancel-a.bin";
    const auto cancel_b = root / "cancel-b.bin";
    const auto cancel_archive = root / "cancel.zip";
    WritePatternFile(cancel_a, 2u * 1024u * 1024u);
    WritePatternFile(cancel_b, 2u * 1024u * 1024u);
    if (Expect(CreateArchive(
            {cancel_a, cancel_b}, cancel_archive,
            cozip::core::CompressionProfile::Fast).status ==
            cozip::format_zip::ZipStatus::Ok,
            "cancellation archive create should succeed") != EXIT_SUCCESS)
    {
        return fail(EXIT_FAILURE);
    }
    CancelAfterFirstFileToken cancel;
    ProgressSink cancel_progress;
    const auto cancel_result = ExtractArchive(
        cancel_archive,
        root / "cancel-out",
        cozip::core::MappingMode::ForceOff,
        &cancel_progress,
        &cancel);
    if (Expect(cancel_result.status == cozip::format_zip::ZipStatus::Cancelled,
               "progress-triggered cancellation should wake extraction wait") != EXIT_SUCCESS)
    {
        return fail(EXIT_FAILURE);
    }

    FailingOutputFactory failing_factory;
    const auto failure_result = ExtractArchive(
        root / "small-deflate.zip",
        root / "failure-out",
        cozip::core::MappingMode::ForceOff,
        nullptr,
        nullptr,
        &failing_factory);
    if (Expect(failure_result.status == cozip::format_zip::ZipStatus::IoError,
               "writer failure should wake extraction wait") != EXIT_SUCCESS)
    {
        return fail(EXIT_FAILURE);
    }

    std::cout << "cozip_zip_extract_progress_tests passed\n";
    return fail(EXIT_SUCCESS);
}
