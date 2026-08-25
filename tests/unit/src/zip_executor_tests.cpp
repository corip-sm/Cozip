#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "cozip/core/archive_request.h"
#include "cozip/format_zip/zip_archive.h"
#include "cozip/storage/storage_factory.h"
#include "zip_chunked_cpu.h"

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

struct AtomicCancelToken final : cozip::core::ICancelToken
{
    [[nodiscard]] bool IsCancellationRequested() const noexcept override
    {
        return cancelled.load(std::memory_order_relaxed);
    }

    std::atomic<bool> cancelled {false};
};

struct RecordingProgressSink final : cozip::core::IProgressSink
{
    void OnProgress(const cozip::core::ProgressEvent& event) override
    {
        if (event.phase == cozip::core::ProgressPhase::WritingOutput)
        {
            completed_paths.push_back(event.current_path);
        }
    }

    std::vector<std::string> completed_paths;
};

struct OutputState
{
    std::mutex mutex;
    std::vector<std::byte> bytes;
    std::thread::id owner;
    bool wrong_thread = false;
};

class OwnerWriter final : public cozip::storage::IRandomAccessWriter
{
public:
    explicit OwnerWriter(std::shared_ptr<OutputState> state)
        : state_(std::move(state))
    {
    }

    [[nodiscard]] std::uint64_t Size() const noexcept override
    {
        std::lock_guard lock(state_->mutex);
        return state_->bytes.size();
    }

    [[nodiscard]] cozip::storage::StorageCapabilities Capabilities() const noexcept override
    {
        cozip::storage::StorageCapabilities capabilities {};
        capabilities.supports_random_write = true;
        return capabilities;
    }

    bool Write(std::uint64_t offset,
               std::span<const std::byte> data,
               std::string&) override
    {
        CheckThread();
        std::lock_guard lock(state_->mutex);
        const auto end = static_cast<std::size_t>(offset) + data.size();
        if (state_->bytes.size() < end)
        {
            state_->bytes.resize(end);
        }
        std::copy(data.begin(), data.end(), state_->bytes.begin() + static_cast<std::size_t>(offset));
        return true;
    }

    bool Resize(std::uint64_t size, std::string&) override
    {
        CheckThread();
        std::lock_guard lock(state_->mutex);
        state_->bytes.resize(static_cast<std::size_t>(size));
        return true;
    }

    bool Flush(std::string&) override
    {
        CheckThread();
        return true;
    }

private:
    void CheckThread()
    {
        if (std::this_thread::get_id() != state_->owner)
        {
            std::lock_guard lock(state_->mutex);
            state_->wrong_thread = true;
        }
    }

    std::shared_ptr<OutputState> state_;
};

class MemoryFactory final : public cozip::storage::IStorageFactory
{
public:
    explicit MemoryFactory(std::shared_ptr<OutputState> output)
        : output_(std::move(output))
    {
    }

    [[nodiscard]] std::unique_ptr<cozip::storage::IRandomAccessReader> OpenReader(
        const std::filesystem::path&,
        cozip::core::MappingMode,
        std::string& error) override
    {
        error = "unexpected OpenReader call";
        return {};
    }

    [[nodiscard]] std::unique_ptr<cozip::storage::IRandomAccessWriter> OpenWriter(
        const std::filesystem::path&,
        std::string&) override
    {
        return std::make_unique<OwnerWriter>(output_);
    }

private:
    std::shared_ptr<OutputState> output_;
};

class PartialOwnerReader final : public cozip::storage::IRandomAccessReader
{
public:
    PartialOwnerReader(std::vector<std::byte> bytes,
                       std::size_t max_read,
                       AtomicCancelToken* cancel = nullptr,
                       std::size_t cancel_after_reads = 0)
        : bytes_(std::move(bytes)),
          max_read_(max_read),
          owner_(std::this_thread::get_id()),
          cancel_(cancel),
          cancel_after_reads_(cancel_after_reads)
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
        return capabilities;
    }

    bool Read(std::uint64_t offset,
              std::span<std::byte> output,
              std::size_t& bytes_read,
              std::string& error) override
    {
        if (std::this_thread::get_id() != owner_)
        {
            wrong_thread_.store(true, std::memory_order_relaxed);
            error = "reader called from worker thread";
            return false;
        }
        const auto call = read_calls_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (offset > bytes_.size())
        {
            error = "offset out of range";
            return false;
        }
        const auto available = bytes_.size() - static_cast<std::size_t>(offset);
        bytes_read = std::min({available, output.size(), max_read_});
        std::copy_n(bytes_.data() + static_cast<std::size_t>(offset), bytes_read, output.data());
        if (cancel_ != nullptr && cancel_after_reads_ != 0 && call >= cancel_after_reads_)
        {
            cancel_->cancelled.store(true, std::memory_order_relaxed);
        }
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

    [[nodiscard]] bool WrongThread() const noexcept
    {
        return wrong_thread_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t ReadCalls() const noexcept
    {
        return read_calls_.load(std::memory_order_relaxed);
    }

private:
    std::vector<std::byte> bytes_;
    std::size_t max_read_;
    std::thread::id owner_;
    AtomicCancelToken* cancel_;
    std::size_t cancel_after_reads_;
    std::atomic<bool> wrong_thread_ {false};
    std::atomic<std::size_t> read_calls_ {0};
};

class TestExecutor final : public cozip::core::ITaskExecutor
{
public:
    explicit TestExecutor(std::size_t workers,
                          bool reorder = false,
                          std::size_t reject_submission = 0)
        : worker_count_(workers),
          reorder_(reorder),
          reject_submission_(reject_submission),
          start_after_submissions_(reorder ? 3u : 0u)
    {
        for (std::size_t index = 0; index < worker_count_; ++index)
        {
            workers_.emplace_back([this] { Run(); });
        }
    }

    ~TestExecutor() override
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_all();
        for (auto& worker : workers_)
        {
            worker.join();
        }
    }

    [[nodiscard]] std::size_t concurrency() const noexcept override
    {
        return worker_count_;
    }

    bool submit(cozip::core::MoveOnlyTask task) override
    {
        const auto sequence = submitted_.fetch_add(1, std::memory_order_relaxed);
        if (sequence + 1 == reject_submission_)
        {
            return false;
        }
        {
            std::lock_guard lock(mutex_);
            queue_.push_back({sequence, std::move(task)});
            ++accepted_;
            const auto pending = queue_.size() + running_;
            max_pending_ = std::max(max_pending_, pending);
        }
        ready_.notify_all();
        return true;
    }

    [[nodiscard]] std::size_t MaxPending() const
    {
        std::lock_guard lock(mutex_);
        return max_pending_;
    }

    [[nodiscard]] bool CompletedOutOfOrder() const
    {
        std::lock_guard lock(mutex_);
        return !std::is_sorted(completed_.begin(), completed_.end());
    }

    [[nodiscard]] std::size_t Accepted() const
    {
        std::lock_guard lock(mutex_);
        return accepted_;
    }

    [[nodiscard]] std::size_t Executions() const
    {
        std::lock_guard lock(mutex_);
        return executions_;
    }

    void WaitForIdle()
    {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [this] { return queue_.empty() && running_ == 0; });
    }

private:
    struct QueuedTask
    {
        std::size_t sequence;
        cozip::core::MoveOnlyTask task;
    };

    void Run()
    {
        while (true)
        {
            QueuedTask queued {};
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] {
                    return stopping_ ||
                        (!queue_.empty() &&
                         (start_after_submissions_ == 0 ||
                          submitted_.load(std::memory_order_relaxed) >=
                              start_after_submissions_));
                });
                if (stopping_ && queue_.empty())
                {
                    return;
                }
                if (reorder_)
                {
                    queued = std::move(queue_.back());
                    queue_.pop_back();
                }
                else
                {
                    queued = std::move(queue_.front());
                    queue_.pop_front();
                }
                ++running_;
            }
            queued.task();
            {
                std::lock_guard lock(mutex_);
                --running_;
                ++executions_;
                completed_.push_back(queued.sequence);
            }
            idle_.notify_all();
        }
    }

    std::size_t worker_count_;
    bool reorder_;
    std::size_t reject_submission_;
    std::size_t start_after_submissions_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable idle_;
    std::deque<QueuedTask> queue_;
    std::vector<std::thread> workers_;
    std::vector<std::size_t> completed_;
    std::atomic<std::size_t> submitted_ {0};
    std::size_t running_ = 0;
    std::size_t max_pending_ = 0;
    std::size_t accepted_ = 0;
    std::size_t executions_ = 0;
    bool stopping_ = false;
};

std::vector<std::byte> MakePayload(std::size_t size)
{
    std::vector<std::byte> bytes(size);
    for (std::size_t index = 0; index < size; ++index)
    {
        bytes[index] = static_cast<std::byte>((index * 17u + index / 97u) % 251u);
    }
    return bytes;
}

cozip::format_zip::ZipOperationResult CreateArchive(
    PartialOwnerReader& reader,
    const std::shared_ptr<OutputState>& output,
    cozip::core::CompressionProfile profile,
    std::size_t chunk_size,
    std::size_t max_in_flight,
    cozip::core::ITaskExecutor* executor,
    cozip::core::ICancelToken* cancel = nullptr)
{
    MemoryFactory factory(output);
    cozip::core::ExecutionEnvironment environment {};
    environment.storage_factory = &factory;
    environment.task_executor = executor;

    cozip::core::ArchiveExecutionRequest request {};
    request.archive.operation = cozip::core::Operation::Create;
    request.archive.format = cozip::core::ArchiveFormat::Zip;
    request.archive.profile = profile;
    request.archive.output_path = "memory/out.zip";
    request.archive.execution.chunk_size_bytes = chunk_size;
    request.archive.execution.max_in_flight_chunks = max_in_flight;
    request.archive.inputs.push_back({
        .kind = cozip::core::ArchiveSourceKind::ReaderFile,
        .path = {},
        .recursive = false,
        .archive_path = "payload.bin",
        .reader = &reader,
    });
    request.context.cancel = cancel;
    request.context.environment = &environment;
    return cozip::format_zip::Execute(request);
}

cozip::format_zip::ZipOperationResult CreateMultiArchive(
    std::vector<std::unique_ptr<PartialOwnerReader>>& readers,
    const std::shared_ptr<OutputState>& output,
    std::size_t chunk_size,
    std::size_t max_in_flight,
    std::size_t memory_budget_mb,
    cozip::core::ITaskExecutor* executor,
    RecordingProgressSink* progress = nullptr,
    cozip::core::ICancelToken* cancel = nullptr)
{
    MemoryFactory factory(output);
    cozip::core::ExecutionEnvironment environment {};
    environment.storage_factory = &factory;
    environment.task_executor = executor;

    cozip::core::ArchiveExecutionRequest request {};
    request.archive.operation = cozip::core::Operation::Create;
    request.archive.format = cozip::core::ArchiveFormat::Zip;
    request.archive.profile = cozip::core::CompressionProfile::Fast;
    request.archive.output_path = "memory/multi.zip";
    request.archive.execution.chunk_size_bytes = chunk_size;
    request.archive.execution.max_in_flight_chunks = max_in_flight;
    request.archive.execution.memory_budget_mb = memory_budget_mb;
    for (std::size_t index = 0; index < readers.size(); ++index)
    {
        request.archive.inputs.push_back({
            .kind = cozip::core::ArchiveSourceKind::ReaderFile,
            .path = {},
            .recursive = false,
            .archive_path = "entry" + std::to_string(index) + ".bin",
            .reader = readers[index].get(),
        });
    }
    request.context.progress = progress;
    request.context.cancel = cancel;
    request.context.environment = &environment;
    return cozip::format_zip::Execute(request);
}

bool ValidateArchive(const std::vector<std::byte>& archive)
{
    static std::atomic<std::size_t> sequence {0};
    const auto path = std::filesystem::temp_directory_path() /
        ("cozip_executor_validation_" +
         std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".zip");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(
            reinterpret_cast<const char*>(archive.data()),
            static_cast<std::streamsize>(archive.size()));
    }
    cozip::core::ArchiveJob job {};
    job.type = cozip::core::JobType::TestArchive;
    job.format = cozip::core::ArchiveFormat::Zip;
    job.inputs.push_back({path.string(), false});
    const auto result = cozip::format_zip::Execute(job);
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
    return result.status == cozip::format_zip::ZipStatus::Ok;
}

bool ValidateArchiveContents(
    const std::vector<std::byte>& archive,
    const std::vector<std::vector<std::byte>>& expected)
{
    static std::atomic<std::size_t> sequence {0};
    const auto suffix = std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    const auto archive_path = std::filesystem::temp_directory_path() /
        ("cozip_multi_validation_" + suffix + ".zip");
    const auto output_path = std::filesystem::temp_directory_path() /
        ("cozip_multi_validation_" + suffix);
    {
        std::ofstream stream(archive_path, std::ios::binary | std::ios::trunc);
        stream.write(
            reinterpret_cast<const char*>(archive.data()),
            static_cast<std::streamsize>(archive.size()));
    }

    cozip::core::ArchiveJob job {};
    job.type = cozip::core::JobType::ExtractArchive;
    job.format = cozip::core::ArchiveFormat::Zip;
    job.inputs.push_back({archive_path.string(), false});
    job.output_path = output_path.string();
    const auto result = cozip::format_zip::Execute(job);
    bool matches = result.status == cozip::format_zip::ZipStatus::Ok;
    for (std::size_t index = 0; matches && index < expected.size(); ++index)
    {
        std::ifstream stream(
            output_path / ("entry" + std::to_string(index) + ".bin"),
            std::ios::binary);
        const std::vector<char> chars {
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
        if (chars.size() != expected[index].size())
        {
            matches = false;
            break;
        }
        for (std::size_t byte = 0; byte < chars.size(); ++byte)
        {
            if (static_cast<std::byte>(static_cast<unsigned char>(chars[byte])) !=
                expected[index][byte])
            {
                matches = false;
                break;
            }
        }
    }
    std::error_code cleanup_error;
    std::filesystem::remove(archive_path, cleanup_error);
    cleanup_error.clear();
    std::filesystem::remove_all(output_path, cleanup_error);
    return matches;
}

int RunSuccessCase(std::size_t size,
                   cozip::core::CompressionProfile profile,
                   std::size_t workers,
                   bool reorder)
{
    constexpr std::size_t chunk_size = 64u * 1024u;
    auto output = std::make_shared<OutputState>();
    output->owner = std::this_thread::get_id();
    PartialOwnerReader reader(MakePayload(size), 997);
    std::unique_ptr<TestExecutor> executor;
    if (workers != 0)
    {
        executor = std::make_unique<TestExecutor>(workers, reorder);
    }
    const auto result = CreateArchive(
        reader, output, profile, chunk_size, 3, executor.get());
    if (Expect(result.status == cozip::format_zip::ZipStatus::Ok,
               "executor archive create should succeed") != EXIT_SUCCESS ||
        Expect(!reader.WrongThread(), "reader must stay on owner thread") != EXIT_SUCCESS ||
        Expect(!output->wrong_thread, "writer must stay on owner thread") != EXIT_SUCCESS ||
        Expect(size == 0 || reader.ReadCalls() > 1, "partial reader should be called repeatedly") != EXIT_SUCCESS ||
        Expect(ValidateArchive(output->bytes), "created archive should pass extraction and CRC test") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }
    if (executor != nullptr &&
        Expect(executor->MaxPending() <= 3, "in-flight task count must stay bounded") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }
    if (reorder && size > chunk_size * 4 &&
        Expect(executor->CompletedOutOfOrder(), "test executor should complete chunks out of order") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int RunMultiEntryCase(std::size_t workers,
                      bool reorder,
                      std::size_t max_in_flight = 6)
{
    constexpr std::size_t chunk_size = 64u * 1024u;
    constexpr std::size_t memory_budget_mb = 1;
    const std::vector<std::size_t> sizes {
        chunk_size * 3 + 11,
        chunk_size * 3 + 23,
        chunk_size * 3 + 37,
    };

    std::vector<std::vector<std::byte>> payloads;
    std::vector<std::unique_ptr<PartialOwnerReader>> readers;
    for (const auto size : sizes)
    {
        payloads.push_back(MakePayload(size));
        readers.push_back(std::make_unique<PartialOwnerReader>(payloads.back(), 997));
    }

    auto output = std::make_shared<OutputState>();
    output->owner = std::this_thread::get_id();
    RecordingProgressSink progress;
    TestExecutor executor(workers, reorder);
    const auto result = CreateMultiArchive(
        readers,
        output,
        chunk_size,
        max_in_flight,
        memory_budget_mb,
        &executor,
        &progress);
    executor.WaitForIdle();
    const auto metrics =
        cozip::format_zip::LastChunkedCpuSchedulerMetricsForTesting();
    const std::vector<std::string> expected_progress {
        "entry0.bin",
        "entry1.bin",
        "entry2.bin",
    };

    if (Expect(result.status == cozip::format_zip::ZipStatus::Ok,
               "multi-entry archive create should succeed") != EXIT_SUCCESS ||
        Expect(metrics.entries_started_before_first_completed >= 1,
               "later entry tasks must start before the first entry completes") != EXIT_SUCCESS ||
        Expect(metrics.max_in_flight_chunks <= max_in_flight,
               "global chunk admission must respect max_in_flight_chunks") != EXIT_SUCCESS ||
        Expect(metrics.max_reserved_bytes <= memory_budget_mb * 1024u * 1024u,
               "global byte admission must respect memory budget") != EXIT_SUCCESS ||
        Expect(metrics.max_in_flight_payload_bytes <= metrics.max_reserved_bytes,
               "actual raw plus compressed payload must fit reserved admission") != EXIT_SUCCESS ||
        Expect(progress.completed_paths == expected_progress,
               "progress must report each completed entry once in archive order") != EXIT_SUCCESS ||
        Expect(ValidateArchiveContents(output->bytes, payloads),
               "out-of-order multi-entry archive must extract byte-for-byte") != EXIT_SUCCESS ||
        Expect(executor.Accepted() == executor.Executions(),
               "all accepted multi-entry tasks must finish") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }
    if (workers == 1 &&
        Expect(metrics.max_active_tasks <= max_in_flight,
               "single-worker executor must preserve bounded scheduling") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }
    if (reorder &&
        Expect(executor.CompletedOutOfOrder(),
               "multi-entry executor must complete tasks out of order") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
} // namespace

int main()
{
    constexpr std::size_t chunk = 64u * 1024u;
    if (RunSuccessCase(0, cozip::core::CompressionProfile::Fast, 0, false) != EXIT_SUCCESS ||
        RunSuccessCase(chunk - 1, cozip::core::CompressionProfile::Fast, 1, false) != EXIT_SUCCESS ||
        RunSuccessCase(chunk, cozip::core::CompressionProfile::Balanced, 4, false) != EXIT_SUCCESS ||
        RunSuccessCase(chunk + 1, cozip::core::CompressionProfile::Small, 4, false) != EXIT_SUCCESS ||
        RunSuccessCase(chunk * 7 + 13, cozip::core::CompressionProfile::Maximum, 4, true) != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (RunMultiEntryCase(1, false, 2) != EXIT_SUCCESS ||
        RunMultiEntryCase(4, true) != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    {
        auto output = std::make_shared<OutputState>();
        output->owner = std::this_thread::get_id();
        PartialOwnerReader reader(MakePayload(chunk * 4), 4096);
        TestExecutor executor(1, false, 2);
        const auto result = CreateArchive(
            reader, output, cozip::core::CompressionProfile::Fast, chunk, 2, &executor);
        executor.WaitForIdle();
        if (Expect(result.status == cozip::format_zip::ZipStatus::IoError,
                   "submit rejection must fail archive creation") != EXIT_SUCCESS ||
            Expect(executor.Accepted() == executor.Executions(),
                   "submit rejection must drain accepted tasks") != EXIT_SUCCESS)
        {
            return EXIT_FAILURE;
        }
    }

    {
        AtomicCancelToken cancel;
        auto output = std::make_shared<OutputState>();
        output->owner = std::this_thread::get_id();
        PartialOwnerReader reader(MakePayload(chunk * 4), 1024, &cancel, 3);
        TestExecutor executor(4);
        const auto result = CreateArchive(
            reader, output, cozip::core::CompressionProfile::Balanced, chunk, 3, &executor, &cancel);
        executor.WaitForIdle();
        if (Expect(result.status == cozip::format_zip::ZipStatus::Cancelled,
                   "mid-read cancellation must cancel archive creation") != EXIT_SUCCESS ||
            Expect(executor.Accepted() == executor.Executions(),
                   "cancellation must drain accepted tasks") != EXIT_SUCCESS)
        {
            return EXIT_FAILURE;
        }
    }

    {
        auto output = std::make_shared<OutputState>();
        output->owner = std::this_thread::get_id();
        PartialOwnerReader reader(MakePayload(chunk * 5), 4096);
        TestExecutor executor(4, true);
        cozip::format_zip::SetChunkCompressionFailureForTesting(2);
        const auto result = CreateArchive(
            reader, output, cozip::core::CompressionProfile::Fast, chunk, 4, &executor);
        executor.WaitForIdle();
        cozip::format_zip::ClearChunkCompressionFailureForTesting();
        if (Expect(result.status == cozip::format_zip::ZipStatus::IoError,
                   "middle chunk compression failure must fail archive creation") != EXIT_SUCCESS ||
            Expect(executor.Accepted() == executor.Executions(),
                   "compression failure must drain accepted tasks") != EXIT_SUCCESS)
        {
            return EXIT_FAILURE;
        }
    }

    std::cout << "cozip_zip_executor_tests passed\n";
    return EXIT_SUCCESS;
}
