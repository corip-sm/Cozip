#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "cozip/core/archive_request.h"
#include "cozip/format_zip/zip_archive.h"
#include "cozip/storage/storage_factory.h"
#include "zip_chunked_cpu.h"

namespace
{
constexpr std::size_t kMiB = 1024u * 1024u;

class Reader final : public cozip::storage::IRandomAccessReader
{
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    [[nodiscard]] std::uint64_t Size() const noexcept override { return bytes_.size(); }
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
        if (offset > bytes_.size())
        {
            error = "benchmark reader offset out of range";
            return false;
        }
        bytes_read = std::min<std::size_t>(
            output.size(), bytes_.size() - static_cast<std::size_t>(offset));
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
    std::span<const std::byte> bytes_;
};

class Writer final : public cozip::storage::IRandomAccessWriter
{
public:
    explicit Writer(std::vector<std::byte>& bytes) : bytes_(bytes) {}
    [[nodiscard]] std::uint64_t Size() const noexcept override { return bytes_.size(); }
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
        const auto end = static_cast<std::size_t>(offset) + data.size();
        if (bytes_.size() < end) bytes_.resize(end);
        std::copy(data.begin(), data.end(), bytes_.begin() + static_cast<std::size_t>(offset));
        return true;
    }
    bool Resize(std::uint64_t size, std::string&) override
    {
        bytes_.resize(static_cast<std::size_t>(size));
        return true;
    }
    bool Flush(std::string&) override { return true; }

private:
    std::vector<std::byte>& bytes_;
};

class Factory final : public cozip::storage::IStorageFactory
{
public:
    explicit Factory(std::vector<std::byte>& output) : output_(output) {}
    [[nodiscard]] std::unique_ptr<cozip::storage::IRandomAccessReader> OpenReader(
        const std::filesystem::path&,
        cozip::core::MappingMode,
        std::string& error) override
    {
        error = "unexpected benchmark OpenReader";
        return {};
    }
    [[nodiscard]] std::unique_ptr<cozip::storage::IRandomAccessWriter> OpenWriter(
        const std::filesystem::path&,
        std::string&) override
    {
        output_.clear();
        return std::make_unique<Writer>(output_);
    }

private:
    std::vector<std::byte>& output_;
};

class Executor final : public cozip::core::ITaskExecutor
{
public:
    explicit Executor(std::size_t concurrency) : concurrency_(concurrency)
    {
        for (std::size_t index = 0; index < concurrency_; ++index)
        {
            workers_.emplace_back([this] { Run(); });
        }
    }
    ~Executor() override
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_all();
        for (auto& worker : workers_) worker.join();
    }
    [[nodiscard]] std::size_t concurrency() const noexcept override { return concurrency_; }
    bool submit(cozip::core::MoveOnlyTask task) override
    {
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(task));
        }
        ready_.notify_one();
        return true;
    }
    [[nodiscard]] std::size_t MaxRunning() const
    {
        std::lock_guard lock(mutex_);
        return max_running_;
    }

private:
    void Run()
    {
        while (true)
        {
            cozip::core::MoveOnlyTask task;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (stopping_ && queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.pop_front();
                ++running_;
                max_running_ = std::max(max_running_, running_);
            }
            task();
            {
                std::lock_guard lock(mutex_);
                --running_;
            }
        }
    }

    std::size_t concurrency_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<cozip::core::MoveOnlyTask> queue_;
    std::vector<std::thread> workers_;
    std::size_t running_ = 0;
    std::size_t max_running_ = 0;
    bool stopping_ = false;
};

std::vector<std::byte> MakePayload(std::size_t size)
{
    std::vector<std::byte> bytes(size);
    for (std::size_t index = 0; index < size; ++index)
    {
        const auto pattern_index = index % (64u * 1024u);
        bytes[index] = static_cast<std::byte>(
            (pattern_index * 37u + pattern_index / 113u) % 251u);
    }
    return bytes;
}

struct RunResult
{
    double wall_seconds = 0.0;
    cozip::format_zip::ChunkedCpuSchedulerMetrics metrics;
    std::size_t max_running = 0;
};

cozip::format_zip::ZipOperationResult ExecuteCreate(
    std::span<Reader> readers,
    std::vector<std::byte>& output,
    Executor& executor)
{
    Factory factory(output);
    cozip::core::ExecutionEnvironment environment {};
    environment.storage_factory = &factory;
    environment.task_executor = &executor;
    cozip::core::ArchiveExecutionRequest request {};
    request.archive.operation = cozip::core::Operation::Create;
    request.archive.format = cozip::core::ArchiveFormat::Zip;
    request.archive.profile = cozip::core::CompressionProfile::Fast;
    request.archive.output_path = "memory/benchmark.zip";
    request.archive.execution.worker_count = executor.concurrency();
    request.archive.execution.chunk_size_bytes = 8u * kMiB;
    request.archive.execution.max_in_flight_chunks =
        std::min<std::size_t>(12, std::max<std::size_t>(1, executor.concurrency() * 2));
    request.archive.execution.memory_budget_mb = 256;
    request.archive.execution.mapping_mode = cozip::core::MappingMode::ForceOff;
    for (std::size_t index = 0; index < readers.size(); ++index)
    {
        request.archive.inputs.push_back({
            .kind = cozip::core::ArchiveSourceKind::ReaderFile,
            .path = {},
            .recursive = false,
            .archive_path = "entry" + std::to_string(index) + ".bin",
            .reader = &readers[index],
        });
    }
    request.context.environment = &environment;
    return cozip::format_zip::Execute(request);
}

RunResult RunMulti(const std::vector<std::vector<std::byte>>& payloads,
                   std::size_t concurrency)
{
    std::vector<Reader> readers;
    for (const auto& payload : payloads) readers.emplace_back(payload);
    std::vector<std::byte> output;
    Executor executor(concurrency);
    const auto started_at = std::chrono::steady_clock::now();
    const auto result = ExecuteCreate(readers, output, executor);
    const auto wall = std::chrono::steady_clock::now() - started_at;
    if (result.status != cozip::format_zip::ZipStatus::Ok)
    {
        std::cerr << "multi benchmark failed: " << result.message << '\n';
        std::exit(1);
    }
    return {
        .wall_seconds = std::chrono::duration<double>(wall).count(),
        .metrics = cozip::format_zip::LastChunkedCpuSchedulerMetricsForTesting(),
        .max_running = executor.MaxRunning(),
    };
}

RunResult RunSequentialEntries(const std::vector<std::vector<std::byte>>& payloads,
                               std::size_t concurrency)
{
    Executor executor(concurrency);
    RunResult aggregate {};
    const auto started_at = std::chrono::steady_clock::now();
    for (const auto& payload : payloads)
    {
        std::vector<Reader> reader;
        reader.emplace_back(payload);
        std::vector<std::byte> output;
        const auto result = ExecuteCreate(reader, output, executor);
        if (result.status != cozip::format_zip::ZipStatus::Ok)
        {
            std::cerr << "sequential benchmark failed: " << result.message << '\n';
            std::exit(1);
        }
        const auto metrics = cozip::format_zip::LastChunkedCpuSchedulerMetricsForTesting();
        aggregate.metrics.max_active_tasks = std::max(
            aggregate.metrics.max_active_tasks, metrics.max_active_tasks);
        aggregate.metrics.max_in_flight_raw_bytes = std::max(
            aggregate.metrics.max_in_flight_raw_bytes, metrics.max_in_flight_raw_bytes);
        aggregate.metrics.max_in_flight_compressed_bytes = std::max(
            aggregate.metrics.max_in_flight_compressed_bytes,
            metrics.max_in_flight_compressed_bytes);
        aggregate.metrics.entries.insert(
            aggregate.metrics.entries.end(), metrics.entries.begin(), metrics.entries.end());
    }
    aggregate.wall_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started_at).count();
    aggregate.max_running = executor.MaxRunning();
    return aggregate;
}

void PrintResult(std::string_view mode,
                 std::size_t concurrency,
                 std::size_t total_bytes,
                 const RunResult& result)
{
    const auto throughput = static_cast<double>(total_bytes) /
        (1024.0 * 1024.0 * result.wall_seconds);
    std::cout << std::fixed << std::setprecision(2)
              << "mode=" << mode
              << " concurrency=" << concurrency
              << " wall_ms=" << result.wall_seconds * 1000.0
              << " throughput_mib_s=" << throughput
              << " max_tasks=" << result.max_running
              << " max_raw_bytes=" << result.metrics.max_in_flight_raw_bytes
              << " max_compressed_bytes="
              << result.metrics.max_in_flight_compressed_bytes
              << " entries_before_first="
              << result.metrics.entries_started_before_first_completed << '\n';
    for (std::size_t index = 0; index < result.metrics.entries.size(); ++index)
    {
        const auto& entry = result.metrics.entries[index];
        std::cout << "  entry=" << index
                  << " read_ms=" << entry.read_ns / 1000000.0
                  << " deflate_ms=" << entry.deflate_ns / 1000000.0
                  << " write_ms=" << entry.write_ns / 1000000.0 << '\n';
    }
}
} // namespace

int main()
{
    const std::vector<std::size_t> sizes_mib {15, 15, 53, 61, 62, 62};
    std::vector<std::vector<std::byte>> payloads;
    std::size_t total_bytes = 0;
    for (const auto size_mib : sizes_mib)
    {
        total_bytes += size_mib * kMiB;
        payloads.push_back(MakePayload(size_mib * kMiB));
    }

    for (const auto concurrency : {std::size_t {4}, std::size_t {8}})
    {
        const auto sequential = RunSequentialEntries(payloads, concurrency);
        const auto multi = RunMulti(payloads, concurrency);
        PrintResult("sequential-entry", concurrency, total_bytes, sequential);
        PrintResult("multi-entry", concurrency, total_bytes, multi);
        std::cout << "  speedup=" << sequential.wall_seconds / multi.wall_seconds << "x\n";
    }
    return 0;
}
