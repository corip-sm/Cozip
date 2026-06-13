#pragma once

#include "zip_archive_base.h"

namespace cozip::format_zip
{
constexpr std::size_t kRandomAccessWriterStreamBufferBytes = 32u * 1024u * 1024u;
constexpr std::size_t kRandomAccessReaderStreamBufferBytes = 16u * 1024u * 1024u;

class RandomAccessWriterStreamBuf final : public std::streambuf
{
public:
    explicit RandomAccessWriterStreamBuf(storage::IRandomAccessWriter& writer)
        : writer_(writer)
    {
        setp(buffer_.data(), buffer_.data() + buffer_.size());
    }

    [[nodiscard]] std::uint64_t Position() const noexcept
    {
        return position_ + static_cast<std::uint64_t>(pptr() - pbase());
    }

protected:
    int sync() override
    {
        return FlushPending() ? 0 : -1;
    }

    int_type overflow(int_type character) override
    {
        if (character == traits_type::eof())
        {
            return FlushPending() ? traits_type::not_eof(character) : traits_type::eof();
        }

        if (!FlushPending())
        {
            return traits_type::eof();
        }

        *pptr() = static_cast<char>(character);
        pbump(1);
        return character;
    }

    std::streamsize xsputn(const char* data, std::streamsize count) override
    {
        if (count <= 0)
        {
            return 0;
        }

        std::streamsize written = 0;
        while (written < count)
        {
            const auto available = epptr() - pptr();
            if (available == 0)
            {
                if (!FlushPending())
                {
                    break;
                }
                continue;
            }

            const auto chunk = std::min<std::streamsize>(count - written, available);
            std::memcpy(pptr(), data + written, static_cast<std::size_t>(chunk));
            pbump(static_cast<int>(chunk));
            written += chunk;
        }

        return written;
    }

    pos_type seekoff(off_type offset,
                     std::ios_base::seekdir direction,
                     std::ios_base::openmode which) override
    {
        if ((which & std::ios_base::out) == 0)
        {
            return pos_type(off_type(-1));
        }

        if (!FlushPending())
        {
            return pos_type(off_type(-1));
        }

        std::uint64_t base = 0;
        switch (direction)
        {
        case std::ios_base::beg:
            base = 0;
            break;
        case std::ios_base::cur:
            base = position_;
            break;
        case std::ios_base::end:
            base = writer_.Size();
            break;
        default:
            return pos_type(off_type(-1));
        }

        if (offset < 0)
        {
            const auto distance = static_cast<std::uint64_t>(-offset);
            if (distance > base)
            {
                return pos_type(off_type(-1));
            }
            position_ = base - distance;
        }
        else
        {
            position_ = base + static_cast<std::uint64_t>(offset);
        }

        setp(buffer_.data(), buffer_.data() + buffer_.size());
        return pos_type(static_cast<off_type>(position_));
    }

    pos_type seekpos(pos_type position, std::ios_base::openmode which) override
    {
        return seekoff(static_cast<off_type>(position), std::ios_base::beg, which);
    }

private:
    bool FlushPending()
    {
        const auto pending = static_cast<std::size_t>(pptr() - pbase());
        if (pending == 0)
        {
            return true;
        }

        std::string error_message;
        const auto bytes = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(pbase()),
            pending);
        if (!writer_.Write(position_, bytes, error_message))
        {
            return false;
        }

        position_ += pending;
        setp(buffer_.data(), buffer_.data() + buffer_.size());
        return true;
    }

    storage::IRandomAccessWriter& writer_;
    std::vector<char> buffer_ = std::vector<char>(kRandomAccessWriterStreamBufferBytes);
    std::uint64_t position_ = 0;
};

class RandomAccessWriterOStream final : public std::ostream
{
public:
    explicit RandomAccessWriterOStream(storage::IRandomAccessWriter& writer)
        : std::ostream(nullptr),
          buffer_(writer)
    {
        rdbuf(&buffer_);
    }

    [[nodiscard]] std::uint64_t Position() const noexcept
    {
        return buffer_.Position();
    }

private:
    RandomAccessWriterStreamBuf buffer_;
};

class RandomAccessReaderStreamBuf final : public std::streambuf
{
public:
    explicit RandomAccessReaderStreamBuf(storage::IRandomAccessReader& reader)
        : reader_(reader)
    {
        setg(buffer_.data(), buffer_.data(), buffer_.data());
    }

    [[nodiscard]] bool Failed() const noexcept
    {
        return failed_;
    }

    [[nodiscard]] const std::string& ErrorMessage() const noexcept
    {
        return error_message_;
    }

protected:
    int_type underflow() override
    {
        if (gptr() < egptr())
        {
            return traits_type::to_int_type(*gptr());
        }

        if (failed_ || offset_ >= reader_.Size())
        {
            return traits_type::eof();
        }

        std::size_t bytes_read = 0;
        error_message_.clear();
        if (!reader_.Read(
                offset_,
                std::span<std::byte>(reinterpret_cast<std::byte*>(buffer_.data()), buffer_.size()),
                bytes_read,
                error_message_))
        {
            failed_ = true;
            return traits_type::eof();
        }

        if (bytes_read == 0)
        {
            return traits_type::eof();
        }

        offset_ += bytes_read;
        setg(buffer_.data(), buffer_.data(), buffer_.data() + static_cast<std::ptrdiff_t>(bytes_read));
        return traits_type::to_int_type(*gptr());
    }

private:
    storage::IRandomAccessReader& reader_;
    std::vector<char> buffer_ = std::vector<char>(kRandomAccessReaderStreamBufferBytes);
    std::uint64_t offset_ = 0;
    bool failed_ = false;
    std::string error_message_;
};

class RandomAccessReaderIStream final : public std::istream
{
public:
    explicit RandomAccessReaderIStream(storage::IRandomAccessReader& reader)
        : std::istream(nullptr),
          buffer_(reader)
    {
        rdbuf(&buffer_);
    }

    [[nodiscard]] bool Failed() const noexcept
    {
        return buffer_.Failed();
    }

    [[nodiscard]] const std::string& ErrorMessage() const noexcept
    {
        return buffer_.ErrorMessage();
    }

private:
    RandomAccessReaderStreamBuf buffer_;
};

class Crc32
{
public:
    Crc32()
        = default;

    void Update(const std::byte* data, std::size_t size)
    {
        if (size == 0)
        {
            return;
        }

        value_ = codecs::UpdateCrc32(
            value_,
            std::span<const std::byte>(data, size));
    }

    [[nodiscard]] std::uint32_t Finalize() const noexcept
    {
        return value_;
    }

private:
    std::uint32_t value_ = 0;
};
}
