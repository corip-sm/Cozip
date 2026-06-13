#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace cozip::core
{
enum class MappingMode : std::uint8_t
{
    Auto,
    ForceOff,
    PreferOn,
    RequireOn,
};

enum class EncryptionMode : std::uint8_t
{
    None,
    ZipTraditional,
};

struct EncryptionOptions
{
    EncryptionMode mode = EncryptionMode::None;
    std::string password;
};

struct ExecutionOptions
{
    std::size_t worker_count = 0;
    std::size_t chunk_size_bytes = 1024 * 1024;
    std::size_t memory_budget_mb = 2048;
    MappingMode mapping_mode = MappingMode::Auto;
    bool allow_parallel_read = true;
    bool allow_parallel_write = true;
    EncryptionOptions encryption {};
};
}
