#include "zip_archive_internal.h"

namespace cozip::format_zip
{
ZipOperationResult TestArchive(const core::ArchiveJob& job, const core::ExecutionContext& context)
{
    const auto execution = core::ResolveExecutionOptions(job);
    const fs::path archive_path = Utf8Path(job.inputs.front().path);
    ArchiveInput archive;
    auto load_result = LoadArchiveInput(
        archive_path,
        ResolveStorageFactory(context),
        execution.mapping_mode,
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

    std::vector<std::size_t> file_indexes;
    file_indexes.reserve(entries.size());
    std::uint64_t total_file_bytes = 0;

    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        const auto& entry = entries[index];
        if (HasPathTraversal(entry.name))
        {
            return MakeError(ZipStatus::InvalidJob, "unsafe archive path: " + entry.name);
        }

        if (entry.is_directory)
        {
            continue;
        }
        file_indexes.push_back(index);
        total_file_bytes += entry.uncompressed_size;
    }

    ReportProgress(
        context,
        {
            .phase = core::ProgressPhase::ProcessingItems,
            .completed_items = 0,
            .total_items = file_indexes.size(),
            .total_bytes = total_file_bytes,
            .current_path = archive_path.generic_string(),
            .message = "zip test queued files",
        });

    if (!file_indexes.empty())
    {
        std::atomic<std::size_t> next_index = 0;
        std::atomic<std::size_t> completed_count = 0;
        std::mutex error_mutex;
        std::atomic<bool> failed = false;
        ZipOperationResult failure {ZipStatus::Ok, {}};
        std::vector<std::thread> workers;
        const auto worker_count =
            ResolveZipParallelWorkerCount(job, execution, entries, file_indexes, file_indexes.size(), total_file_bytes);
        workers.reserve(worker_count);

        for (std::size_t worker = 0; worker < worker_count; ++worker)
        {
            workers.emplace_back([&] {
                while (true)
                {
                    if (failed.load(std::memory_order_relaxed))
                    {
                        return;
                    }

                    if (IsCancellationRequested(context))
                    {
                        std::lock_guard lock(error_mutex);
                        if (!failed.exchange(true))
                        {
                            failure = MakeCancelled("zip test cancelled");
                        }
                        return;
                    }

                    const auto position = next_index.fetch_add(1, std::memory_order_relaxed);
                    if (position >= file_indexes.size())
                    {
                        return;
                    }

                    const auto& entry = entries[file_indexes[position]];
                    const auto validate_result = ValidateEntryData(archive.bytes, entry, execution);
                    if (validate_result.status != ZipStatus::Ok)
                    {
                        std::lock_guard lock(error_mutex);
                        if (!failed.exchange(true))
                        {
                            failure = validate_result;
                        }
                        return;
                    }

                    completed_count.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        while (!failed.load(std::memory_order_relaxed) &&
               completed_count.load(std::memory_order_relaxed) < file_indexes.size())
        {
            ReportProgress(
                context,
                {
                    .phase = core::ProgressPhase::ProcessingItems,
                    .completed_items = completed_count.load(std::memory_order_relaxed),
                    .total_items = file_indexes.size(),
                    .total_bytes = total_file_bytes,
                    .current_path = archive_path.generic_string(),
                    .message = "zip test running",
                });
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        for (auto& worker : workers)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }

        if (failed.load(std::memory_order_relaxed))
        {
            return failure;
        }
    }

    return {
        ZipStatus::Ok,
        "tested zip archive files=" + std::to_string(file_indexes.size()) + " status=ok"};
}
}
