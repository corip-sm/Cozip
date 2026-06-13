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

int CreateExternalZip(const char* tool,
                      const std::filesystem::path& archive_path,
                      const std::filesystem::path& source_root,
                      const char* mode)
{
    const auto script_path =
        std::filesystem::path(COZIP_TESTS_SOURCE_DIR) / "integration" / "scripts" / "create_external_zip.ps1";

    const std::string command =
        "powershell -ExecutionPolicy Bypass -File " + Quote(script_path) +
        " -ArchivePath " + Quote(archive_path) +
        " -SourceRoot " + Quote(source_root) +
        " -Mode " + mode +
        " -Tool " + tool;

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
               "external compat list should succeed") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (Expect(list_result.message.find("nested/child.txt") != std::string::npos,
               "external compat list should include nested file") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (Expect(list_result.message.find(expected_method_snippet) != std::string::npos,
               "external compat list should report expected compression method") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    cozip::core::ArchiveJob test_job {};
    test_job.type = cozip::core::JobType::TestArchive;
    test_job.format = cozip::core::ArchiveFormat::Zip;
    test_job.inputs.push_back({archive_path.string(), false});

    const auto test_result = cozip::format_zip::Execute(test_job);
    if (Expect(test_result.status == cozip::format_zip::ZipStatus::Ok,
               "external compat test should succeed") != EXIT_SUCCESS)
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
               "external compat extract should succeed") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    const auto nested_file = extract_root / "nested" / "child.txt";
    if (Expect(std::filesystem::exists(nested_file),
               "external compat extract should create nested file") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    std::ifstream input(nested_file, std::ios::binary);
    std::string text;
    std::getline(input, text);
    if (Expect(text.rfind("external nested payload", 0) == 0,
               "external compat extracted file content should match") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

int RunScenario(const char* tool, const char* mode, const char* expected_method_snippet)
{
    namespace fs = std::filesystem;

    const std::string root_payload = "external root payload " + std::string(4096, 'C');
    const std::string nested_payload = "external nested payload " + std::string(4096, 'D');

    const auto temp_root = fs::temp_directory_path() / ("cozip_zip_external_" + std::string(tool) + "_" + mode);
    const auto source_root = temp_root / "source";
    const auto nested_root = source_root / "nested";
    const auto archive_path = temp_root / (std::string(tool) + "_" + mode + ".zip");
    const auto extract_root = temp_root / "extract";

    fs::create_directories(nested_root);

    {
        std::ofstream root_file(source_root / "root.txt", std::ios::binary | std::ios::trunc);
        root_file << root_payload;
    }

    {
        std::ofstream nested_file(nested_root / "child.txt", std::ios::binary | std::ios::trunc);
        nested_file << nested_payload;
    }

    const auto create_result = CreateExternalZip(tool, archive_path, source_root, mode);
    if (create_result == 100)
    {
        std::cout << "cozip_zip_external_compat_tests skipped for " << tool << ' ' << mode << '\n';
        return EXIT_SUCCESS;
    }

    if (Expect(create_result == 0, "external tool should create compat zip") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    return VerifyArchiveRoundTrip(archive_path, extract_root, expected_method_snippet);
}
} // namespace

int main()
{
    if (RunScenario("7zip", "Store", "method=store(0)") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (RunScenario("7zip", "Deflate", "method=deflate(8)") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (RunScenario("Bandizip", "Store", "method=store(0)") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    if (RunScenario("Bandizip", "Deflate", "method=deflate(8)") != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    std::cout << "cozip_zip_external_compat_tests passed\n";
    return EXIT_SUCCESS;
}
