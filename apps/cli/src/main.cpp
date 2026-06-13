#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "cozip/core/archive_job.h"
#include "cozip/core/version.h"
#include "cozip/format_zip/zip_archive.h"

namespace
{
void PrintBanner()
{
    std::cout << "Cozip CLI " << cozip::core::VersionString() << '\n';
    std::cout << "High-performance archive engine skeleton\n";
}

void PrintUsage()
{
    std::cout << "Usage:\n";
    std::cout << "  cozip_cli version\n";
    std::cout << "  cozip_cli help\n";
    std::cout << "  cozip_cli create [--store|--deflate|--fast|--balanced|--small|--max] [--threads N] [--memory-mb N] [--chunk-kb N] [--password VALUE] <output.zip> <input...>\n";
    std::cout << "  cozip_cli extract [--password VALUE] <archive.zip> [output-dir]\n";
    std::cout << "  cozip_cli list <archive.zip>\n";
    std::cout << "  cozip_cli test [--password VALUE] <archive.zip>\n";
}

cozip::core::ArchiveJob MakeZipJob(
    cozip::core::JobType type,
    std::span<const std::string> args)
{
    cozip::core::ArchiveJob job {};
    job.type = type;
    job.format = cozip::core::ArchiveFormat::Zip;

    switch (type)
    {
    case cozip::core::JobType::CreateArchive:
    {
        job.profile = cozip::core::CompressionProfile::Fast;
        std::size_t start_index = 0;
        while (start_index < args.size())
        {
            const std::string_view current_arg {args[start_index]};
            if (current_arg == "--store")
            {
                job.profile = cozip::core::CompressionProfile::Store;
                ++start_index;
            }
            else if (current_arg == "--deflate")
            {
                job.profile = cozip::core::CompressionProfile::Fast;
                ++start_index;
            }
            else if (current_arg == "--fast")
            {
                job.profile = cozip::core::CompressionProfile::Fast;
                ++start_index;
            }
            else if (current_arg == "--balanced")
            {
                job.profile = cozip::core::CompressionProfile::Balanced;
                ++start_index;
            }
            else if (current_arg == "--small")
            {
                job.profile = cozip::core::CompressionProfile::Small;
                ++start_index;
            }
            else if (current_arg == "--max")
            {
                job.profile = cozip::core::CompressionProfile::Maximum;
                ++start_index;
            }
            else if (current_arg == "--threads" && start_index + 1 < args.size())
            {
                job.execution.worker_count = static_cast<std::size_t>(std::stoull(args[start_index + 1]));
                start_index += 2;
            }
            else if (current_arg == "--memory-mb" && start_index + 1 < args.size())
            {
                job.execution.memory_budget_mb = static_cast<std::size_t>(std::stoull(args[start_index + 1]));
                start_index += 2;
            }
            else if (current_arg == "--chunk-kb" && start_index + 1 < args.size())
            {
                job.execution.chunk_size_bytes = static_cast<std::size_t>(std::stoull(args[start_index + 1])) * 1024ull;
                start_index += 2;
            }
            else if (current_arg == "--password" && start_index + 1 < args.size())
            {
                job.execution.encryption.mode = cozip::core::EncryptionMode::ZipTraditional;
                job.execution.encryption.password = args[start_index + 1];
                start_index += 2;
            }
            else
            {
                break;
            }
        }

        if (args.size() >= start_index + 2)
        {
            job.output_path = args[start_index];
            for (std::size_t index = start_index + 1; index < args.size(); ++index)
            {
                job.inputs.push_back({args[index], true});
            }
        }
        break;
    }
    case cozip::core::JobType::ExtractArchive:
        if (args.size() >= 2 && std::string_view {args[0]} == "--password")
        {
            job.execution.encryption.mode = cozip::core::EncryptionMode::ZipTraditional;
            job.execution.encryption.password = args[1];
            if (args.size() >= 3)
            {
                job.inputs.push_back({args[2], false});
            }
            if (args.size() >= 4)
            {
                job.output_path = args[3];
            }
            break;
        }

        if (!args.empty())
        {
            job.inputs.push_back({args[0], false});
        }

        if (args.size() >= 2)
        {
            job.output_path = args[1];
        }
        break;
    case cozip::core::JobType::ListArchive:
    case cozip::core::JobType::TestArchive:
        if (type == cozip::core::JobType::TestArchive &&
            args.size() >= 3 &&
            std::string_view {args[0]} == "--password")
        {
            job.execution.encryption.mode = cozip::core::EncryptionMode::ZipTraditional;
            job.execution.encryption.password = args[1];
            job.inputs.push_back({args[2], false});
            break;
        }

        if (!args.empty())
        {
            job.inputs.push_back({args[0], false});
        }
        break;
    }

    return job;
}

int RunZipJob(const cozip::core::ArchiveJob& job)
{
    const auto result = cozip::format_zip::Execute(job);

    if (result.status != cozip::format_zip::ZipStatus::Ok)
    {
        std::cerr << "zip operation failed: " << cozip::format_zip::ToString(result.status)
                  << " - " << result.message << '\n';
        return 1;
    }

    std::cout << result.message << '\n';
    return 0;
}

#ifdef _WIN32
std::string WideToUtf8(const wchar_t* value)
{
    if (value == nullptr || *value == L'\0')
    {
        return {};
    }

    const int required_size =
        WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (required_size <= 1)
    {
        return {};
    }

    std::string result(static_cast<std::size_t>(required_size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), required_size, nullptr, nullptr);
    return result;
}

std::vector<std::string> ConvertArgsToUtf8(int argc, wchar_t** argv)
{
    std::vector<std::string> utf8_args {};
    utf8_args.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index)
    {
        utf8_args.push_back(WideToUtf8(argv[index]));
    }
    return utf8_args;
}
#endif
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
    std::vector<std::string> utf8_args {};

#ifdef _WIN32
    utf8_args = ConvertArgsToUtf8(argc, argv);
#else
    utf8_args.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index)
    {
        utf8_args.emplace_back(argv[index]);
    }
#endif

    if (utf8_args.size() <= 1)
    {
        PrintBanner();
        PrintUsage();
        return 0;
    }

    const std::string_view command {utf8_args[1]};
    const std::span<const std::string> args {utf8_args.data() + 2, utf8_args.size() - 2};

    if (command == "version")
    {
        std::cout << cozip::core::VersionString() << '\n';
        return 0;
    }

    if (command == "help")
    {
        PrintBanner();
        PrintUsage();
        return 0;
    }

    if (command == "create")
    {
        return RunZipJob(MakeZipJob(cozip::core::JobType::CreateArchive, args));
    }

    if (command == "extract")
    {
        return RunZipJob(MakeZipJob(cozip::core::JobType::ExtractArchive, args));
    }

    if (command == "list")
    {
        return RunZipJob(MakeZipJob(cozip::core::JobType::ListArchive, args));
    }

    if (command == "test")
    {
        return RunZipJob(MakeZipJob(cozip::core::JobType::TestArchive, args));
    }

    std::cerr << "Unknown command: " << command << '\n';
    PrintUsage();
    return 1;
}
