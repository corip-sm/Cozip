#include "zip_archive_internal.h"

namespace cozip::format_zip
{
ZipOperationResult ExtractArchive(const core::ArchiveJob& job, const core::ExecutionContext& context)
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

    fs::path output_root;
    auto resolve_result = ResolveExtractionRoot(job, output_root);
    if (resolve_result.status != ZipStatus::Ok)
    {
        return resolve_result;
    }

    std::error_code create_error;
    fs::create_directories(output_root, create_error);
    if (create_error)
    {
        return MakeError(ZipStatus::IoError, "failed to create output directory: " + output_root.string());
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

        if (!IsSupportedReaderMethod(entry.compression_method))
        {
            return MakeError(
                ZipStatus::Unsupported,
                std::string("zip extraction method is not implemented yet: ") +
                    ZipMethodName(entry.compression_method) + " entry=" + entry.name);
        }

        const auto destination_path = output_root / fs::path(entry.name);
        const auto destination_parent = destination_path.parent_path();
        if (!destination_parent.empty())
        {
            fs::create_directories(destination_parent, create_error);
            if (create_error)
            {
                return MakeError(ZipStatus::IoError, "failed to create directory: " + destination_parent.string());
            }
        }

        if (entry.is_directory)
        {
            fs::create_directories(destination_path, create_error);
            if (create_error)
            {
                return MakeError(ZipStatus::IoError, "failed to create directory: " + destination_path.string());
            }
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
            .message = "zip extract queued files",
        });

    if (!file_indexes.empty())
    {
        std::atomic<std::size_t> next_index = 0;
        std::atomic<std::size_t> completed_count = 0;
        std::mutex error_mutex;
        std::atomic<bool> failed = false;
        ZipOperationResult failure {ZipStatus::Ok, {}};
        std::vector<std::thread> workers;
        const auto target_worker_count =
            ResolveZipParallelWorkerCount(job, execution, entries, file_indexes, file_indexes.size(), total_file_bytes);
        const auto initial_worker_count =
            ResolveZipInitialWorkerCount(target_worker_count, file_indexes.size(), total_file_bytes);
        workers.reserve(target_worker_count);
        auto spawn_worker = [&] {
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
                            failure = MakeCancelled("zip extract cancelled");
                        }
                        return;
                    }

                    const auto position = next_index.fetch_add(1, std::memory_order_relaxed);
                    if (position >= file_indexes.size())
                    {
                        return;
                    }

                    const auto& entry = entries[file_indexes[position]];
                    const auto destination_path = output_root / fs::path(entry.name);
                    const auto write_result =
                        WriteExtractedFile(
                            archive.bytes,
                            entry,
                            destination_path,
                            ResolveStorageFactory(context),
                            execution);
                    if (write_result.status != ZipStatus::Ok)
                    {
                        std::lock_guard lock(error_mutex);
                        if (!failed.exchange(true))
                        {
                            failure = write_result;
                        }
                        return;
                    }

                    completed_count.fetch_add(1, std::memory_order_relaxed);
                }
            });
        };
        for (std::size_t worker = 0; worker < initial_worker_count; ++worker)
        {
            spawn_worker();
        }

        if (ZipExtractTraceEnabled())
        {
            std::cerr << "[cozip][extract] autotune_initial_workers=" << initial_worker_count
                      << " autotune_target_workers=" << target_worker_count
                      << '\n';
        }

        while (workers.size() < target_worker_count && !failed.load(std::memory_order_relaxed))
        {
            const auto assigned = next_index.load(std::memory_order_relaxed);
            const auto completed = completed_count.load(std::memory_order_relaxed);
            const auto remaining = file_indexes.size() > completed ? file_indexes.size() - completed : 0;
            const auto in_flight = assigned > completed ? assigned - completed : 0;
            ReportProgress(
                context,
                {
                    .phase = core::ProgressPhase::WritingOutput,
                    .completed_items = completed,
                    .total_items = file_indexes.size(),
                    .total_bytes = total_file_bytes,
                    .current_path = archive_path.generic_string(),
                    .message = "zip extract running",
                });
            if (remaining == 0)
            {
                break;
            }

            if (in_flight >= workers.size() && remaining > workers.size())
            {
                spawn_worker();
                continue;
            }

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

    const auto extracted_count = file_indexes.size();

    return {
        ZipStatus::Ok,
        "extracted zip archive files=" + std::to_string(extracted_count) + " output=" + output_root.string()};
}

}
