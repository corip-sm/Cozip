#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "cozip/core/archive_job.h"
#include "cozip/format_zip/zip_archive.h"

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

std::string Quote(const std::filesystem::path& path)
{
    return "\"" + path.string() + "\"";
}

int CreateDotnetZip(const std::filesystem::path& archive_path,
                    const std::filesystem::path& source_root,
                    const char* mode)
{
    const auto script_path =
        std::filesystem::path(COZIP_TESTS_SOURCE_DIR) / "integration" / "scripts" / "create_dotnet_zip.ps1";

    const std::string command =
        "powershell -ExecutionPolicy Bypass -File " + Quote(script_path) +
        " -ArchivePath " + Quote(archive_path) +
        " -SourceRoot " + Quote(source_root) +
        " -Mode " + mode;

    return std::system(command.c_str());
}

int VerifyArchiveRoundTrip(const std::filesystem::path& archive_path,
                           const std::filesystem::path& extract_root,
                           const char* expected_method_snippet)
{
    cozip::core::ArchiveJob list_job {};
    list_job.type = cozip::core::JobType::ListArchive;
    list_job.format = cozip::core::ArchiveFormat::Zip;
    list_job.inputs.push_back({archive_path.string(), false});

    const auto list_result = cozip::format_zip::Execute(list_job);
    if (Expect(list_result.status == cozip::format_zip::ZipStatus::Ok,
               "compat list should succeed") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (Expect(list_result.message.find("nested/child.txt") != std::string::npos,
               "compat list should include nested file") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (expected_method_snippet != nullptr &&
        Expect(list_result.message.find(expected_method_snippet) != std::string::npos,
               "compat list should report expected compression method") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveJob test_job {};
    test_job.type = cozip::core::JobType::TestArchive;
    test_job.format = cozip::core::ArchiveFormat::Zip;
    test_job.inputs.push_back({archive_path.string(), false});

    const auto test_result = cozip::format_zip::Execute(test_job);
    if (Expect(test_result.status == cozip::format_zip::ZipStatus::Ok,
               "compat test should succeed") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveJob extract_job {};
    extract_job.type = cozip::core::JobType::ExtractArchive;
    extract_job.format = cozip::core::ArchiveFormat::Zip;
    extract_job.output_path = extract_root.string();
    extract_job.inputs.push_back({archive_path.string(), false});

    const auto extract_result = cozip::format_zip::Execute(extract_job);
    if (Expect(extract_result.status == cozip::format_zip::ZipStatus::Ok,
               "compat extract should succeed") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    const auto nested_file = extract_root / "nested" / "child.txt";
    if (Expect(std::filesystem::exists(nested_file),
               "compat extract should create nested file") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    std::ifstream input(nested_file, std::ios::binary);
    std::string text;
    std::getline(input, text);
    if (Expect(text.rfind("dotnet nested payload", 0) == 0,
               "compat extracted file content should match") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
} // namespace

int main()
{
    namespace fs = std::filesystem;

    const std::string root_payload = "dotnet root payload " + std::string(4096, 'A');
    const std::string nested_payload = "dotnet nested payload " + std::string(4096, 'B');

    const auto temp_root = fs::temp_directory_path() / "cozip_zip_compat_tests";
    const auto source_root = temp_root / "source";
    const auto nested_root = source_root / "nested";
    const auto store_archive = temp_root / "dotnet_store.zip";
    const auto deflate_archive = temp_root / "dotnet_deflate.zip";
    const auto store_extract_root = temp_root / "extract_store";
    const auto deflate_extract_root = temp_root / "extract_deflate";

    fs::create_directories(nested_root);

    {
        std::ofstream root_file(source_root / "root.txt", std::ios::binary | std::ios::trunc);
        root_file << root_payload;
    }

    {
        std::ofstream nested_file(nested_root / "child.txt", std::ios::binary | std::ios::trunc);
        nested_file << nested_payload;
    }

    if (Expect(CreateDotnetZip(store_archive, source_root, "Store") == 0,
               "powershell should create store compat zip") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (VerifyArchiveRoundTrip(store_archive, store_extract_root, nullptr) != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (Expect(CreateDotnetZip(deflate_archive, source_root, "Deflate") == 0,
               "powershell should create deflate compat zip") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (VerifyArchiveRoundTrip(deflate_archive, deflate_extract_root, "method=deflate(8)") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    std::cout << "cozip_zip_compat_tests passed\n";
    return EXIT_SUCCESS;
}
