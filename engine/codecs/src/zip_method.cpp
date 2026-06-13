#include "cozip/codecs/zip_method.h"

namespace cozip::codecs
{
const char* ToString(ZipMethod method) noexcept
{
    switch (method)
    {
    case ZipMethod::Store:
        return "store";
    case ZipMethod::Deflate:
        return "deflate";
    }

    return "unknown";
}
}
