#pragma once

#include "cozip/format_zip/zip_archive.h"
#include "cozip/codecs/deflate_codec.h"
#include "cozip/codecs/zip_method.h"
#include "cozip/pipeline/archive_pipeline.h"
#include "cozip/platform/filesystem_random_access.h"
#include "cozip/platform/filesystem_storage_factory.h"
#include "cozip/platform/mapped_file.h"
#include "miniz.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace cozip::format_zip
{
namespace fs = std::filesystem;

constexpr std::uint32_t kLocalFileHeaderSignature = 0x04034b50;
constexpr std::uint32_t kCentralDirectoryHeaderSignature = 0x02014b50;
constexpr std::uint32_t kEndOfCentralDirectorySignature = 0x06054b50;
constexpr std::uint32_t kZip64EndOfCentralDirectorySignature = 0x06064b50;
constexpr std::uint32_t kZip64EndOfCentralDirectoryLocatorSignature = 0x07064b50;
constexpr std::uint32_t kDataDescriptorSignature = 0x08074b50;
constexpr std::uint16_t kVersionNeeded = 20;
constexpr std::uint16_t kVersionMadeBy = 20;
constexpr std::uint16_t kVersionZip64Needed = 45;
constexpr std::uint16_t kDirectoryExternalAttributes = 0x10;
constexpr std::uint16_t kDosTime = 0;
constexpr std::uint16_t kDosDate = 33; // 1980-01-01
constexpr std::uint16_t kEncryptedFlag = 0x0001;
constexpr std::uint16_t kDataDescriptorFlag = 0x0008;
constexpr std::uint16_t kZip64ExtraFieldHeaderId = 0x0001;
constexpr std::uint32_t kZip64Sentinel32 = 0xFFFFFFFFu;
constexpr std::uint16_t kZip64Sentinel16 = 0xFFFFu;
constexpr std::size_t kZipTraditionalEncryptionHeaderSize = 12;

struct ZipEntrySource
{
    fs::path source_path;
    std::string archive_path;
    std::string source_label;
    std::uint32_t crc32 = 0;
    std::uint32_t size = 0;
    std::uint32_t compressed_size = 0;
    std::uint64_t local_header_offset = 0;
    codecs::ZipMethod method = codecs::ZipMethod::Store;
    std::uint16_t general_purpose_flag = 0;
    codecs::DeflateBackend prepared_backend = codecs::DeflateBackend::None;
    core::CompressionProfile compression_profile = core::CompressionProfile::Balanced;
    std::vector<std::byte> prepared_data;
    bool is_directory = false;
    bool adaptive_store_evaluated = false;
    core::MappingMode mapping_mode = core::MappingMode::Auto;
    storage::IStorageFactory* storage_factory = nullptr;
    storage::IRandomAccessReader* source_reader = nullptr;
};

struct ZipCentralDirectoryEntry
{
    std::string name;
    std::uint16_t general_purpose_flag = 0;
    std::uint16_t compression_method = 0;
    std::uint32_t crc32 = 0;
    std::uint64_t compressed_size = 0;
    std::uint64_t uncompressed_size = 0;
    std::uint64_t local_header_offset = 0;
    bool is_directory = false;
};

struct WholeFileInput
{
    std::unique_ptr<storage::IRandomAccessReader> reader;
    storage::MappedReadWindow mapped_window {};
    std::vector<std::byte> owned_bytes;
    std::span<const std::byte> bytes {};
};

struct ArchiveInput
{
    std::unique_ptr<storage::IRandomAccessReader> reader;
    storage::MappedReadWindow mapped_window {};
    std::vector<std::byte> owned_bytes;
    std::span<const std::byte> bytes {};
};

inline bool ZipTimingTraceEnabled() noexcept
{
    static const bool enabled = []() noexcept {
        const char* value = std::getenv("COZIP_TRACE_ZIP_TIMING");
        if (value == nullptr)
        {
            return false;
        }

        return value[0] != '\0' && value[0] != '0';
    }();

    return enabled;
}

inline void EmitZipTimingTrace(const std::string& line)
{
    static std::mutex trace_mutex;
    if (!ZipTimingTraceEnabled())
    {
        return;
    }

    std::lock_guard lock(trace_mutex);
    std::cerr << "[zip-trace] " << line << '\n';
}

}
