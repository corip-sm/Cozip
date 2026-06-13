#pragma once

#include <cstdint>

namespace cozip::codecs
{
enum class ZipMethod : std::uint16_t
{
    Store = 0,
    Deflate = 8,
};

const char* ToString(ZipMethod method) noexcept;
}
