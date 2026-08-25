#include "zip_chunked_cpu.h"

#include <condition_variable>
#include <map>

namespace cozip::format_zip
{
namespace
{
#if defined(COZIP_ENABLE_TEST_HOOKS)
std::atomic<std::size_t> g_failing_chunk_index {std::numeric_limits<std::size_t>::max()};
#endif

struct DeflateStreamLayout
{
    std::size_t final_header_bit = 0;
    std::size_t end_bit = 0;
};

struct ChunkedCompressedChunk
{
    std::size_t index = 0;
    std::size_t raw_size = 0;
    std::vector<std::byte> compressed;
    DeflateStreamLayout layout {};
};

class DeflateBitCursor
{
public:
    explicit DeflateBitCursor(std::span<const std::byte> data)
        : data_(data)
    {
    }

    [[nodiscard]] std::size_t BitPosition() const noexcept
    {
        return bit_pos_;
    }

    bool ReadBit(std::uint8_t& bit)
    {
        if (bit_pos_ >= data_.size() * 8)
        {
            return false;
        }

        const auto byte_index = bit_pos_ / 8;
        const auto shift = bit_pos_ % 8;
        bit = (static_cast<std::uint8_t>(data_[byte_index]) >> shift) & 1u;
        ++bit_pos_;
        return true;
    }

    bool ReadBits(std::size_t bit_count, std::uint32_t& value)
    {
        value = 0;
        for (std::size_t index = 0; index < bit_count; ++index)
        {
            std::uint8_t bit = 0;
            if (!ReadBit(bit))
            {
                return false;
            }
            value |= static_cast<std::uint32_t>(bit) << index;
        }
        return true;
    }

    void AlignToByte() noexcept
    {
        bit_pos_ = (bit_pos_ + 7u) & ~std::size_t(7u);
    }

    bool SkipBits(std::size_t bit_count)
    {
        if (bit_count > data_.size() * 8 - bit_pos_)
        {
            return false;
        }
        bit_pos_ += bit_count;
        return true;
    }

private:
    std::span<const std::byte> data_;
    std::size_t bit_pos_ = 0;
};

struct HuffmanDecoder
{
    std::array<std::uint16_t, 16> counts {};
    std::vector<std::uint16_t> symbols;
    std::size_t max_bits = 0;
};

constexpr std::array<std::uint8_t, 29> kLengthExtraBits {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};
constexpr std::array<std::uint8_t, 30> kDistanceExtraBits {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12,
    13, 13,
};
constexpr std::array<std::size_t, 19> kCodeLengthOrder {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
};

bool BuildHuffmanDecoder(std::span<const std::uint8_t> lengths,
                         HuffmanDecoder& decoder)
{
    decoder = {};
    std::size_t max_bits = 0;
    for (const auto length : lengths)
    {
        if (length > 15)
        {
            return false;
        }
        if (length > 0)
        {
            ++decoder.counts[length];
            max_bits = std::max(max_bits, static_cast<std::size_t>(length));
        }
    }
    decoder.max_bits = max_bits;
    if (max_bits == 0)
    {
        return true;
    }

    std::array<std::size_t, 16> offsets {};
    std::size_t total = 0;
    for (std::size_t bits = 1; bits <= max_bits; ++bits)
    {
        offsets[bits] = total;
        total += decoder.counts[bits];
    }
    decoder.symbols.assign(total, 0);
    auto next_offsets = offsets;
    for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol)
    {
        const auto length = lengths[symbol];
        if (length == 0)
        {
            continue;
        }
        decoder.symbols[next_offsets[length]++] = static_cast<std::uint16_t>(symbol);
    }
    return true;
}

bool DecodeHuffmanSymbol(DeflateBitCursor& cursor,
                         const HuffmanDecoder& decoder,
                         std::uint16_t& symbol)
{
    if (decoder.max_bits == 0)
    {
        return false;
    }

    std::uint32_t code = 0;
    std::uint32_t first = 0;
    std::size_t index = 0;
    for (std::size_t len = 1; len <= decoder.max_bits; ++len)
    {
        std::uint8_t bit = 0;
        if (!cursor.ReadBit(bit))
        {
            return false;
        }
        code |= static_cast<std::uint32_t>(bit);
        const auto count = static_cast<std::uint32_t>(decoder.counts[len]);
        if (code >= first && code - first < count)
        {
            const auto offset = index + static_cast<std::size_t>(code - first);
            if (offset >= decoder.symbols.size())
            {
                return false;
            }
            symbol = decoder.symbols[offset];
            return true;
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }

    return false;
}

bool FixedHuffmanTrees(HuffmanDecoder& litlen_decoder,
                       HuffmanDecoder& dist_decoder)
{
    std::vector<std::uint8_t> litlen_lengths(288, 0);
    std::fill(litlen_lengths.begin(), litlen_lengths.begin() + 144, 8);
    std::fill(litlen_lengths.begin() + 144, litlen_lengths.begin() + 256, 9);
    std::fill(litlen_lengths.begin() + 256, litlen_lengths.begin() + 280, 7);
    std::fill(litlen_lengths.begin() + 280, litlen_lengths.end(), 8);
    std::vector<std::uint8_t> dist_lengths(32, 5);
    return BuildHuffmanDecoder(litlen_lengths, litlen_decoder) &&
        BuildHuffmanDecoder(dist_lengths, dist_decoder);
}

bool ReadDynamicHuffmanTrees(DeflateBitCursor& cursor,
                             HuffmanDecoder& litlen_decoder,
                             HuffmanDecoder& dist_decoder)
{
    std::uint32_t hlit_bits = 0;
    std::uint32_t hdist_bits = 0;
    std::uint32_t hclen_bits = 0;
    if (!cursor.ReadBits(5, hlit_bits) ||
        !cursor.ReadBits(5, hdist_bits) ||
        !cursor.ReadBits(4, hclen_bits))
    {
        return false;
    }

    const auto hlit = static_cast<std::size_t>(hlit_bits) + 257;
    const auto hdist = static_cast<std::size_t>(hdist_bits) + 1;
    const auto hclen = static_cast<std::size_t>(hclen_bits) + 4;

    std::array<std::uint8_t, 19> codelen_lengths {};
    for (std::size_t index = 0; index < hclen; ++index)
    {
        std::uint32_t bits = 0;
        if (!cursor.ReadBits(3, bits))
        {
            return false;
        }
        codelen_lengths[kCodeLengthOrder[index]] = static_cast<std::uint8_t>(bits);
    }

    HuffmanDecoder codelen_decoder;
    if (!BuildHuffmanDecoder(codelen_lengths, codelen_decoder))
    {
        return false;
    }

    const auto total = hlit + hdist;
    std::vector<std::uint8_t> lengths;
    lengths.reserve(total);
    while (lengths.size() < total)
    {
        std::uint16_t sym = 0;
        if (!DecodeHuffmanSymbol(cursor, codelen_decoder, sym))
        {
            return false;
        }
        switch (sym)
        {
        case 0 ... 15:
            lengths.push_back(static_cast<std::uint8_t>(sym));
            break;
        case 16:
        {
            if (lengths.empty())
            {
                return false;
            }
            std::uint32_t repeat_bits = 0;
            if (!cursor.ReadBits(2, repeat_bits))
            {
                return false;
            }
            const auto repeat = static_cast<std::size_t>(repeat_bits) + 3;
            lengths.insert(lengths.end(), repeat, lengths.back());
            break;
        }
        case 17:
        {
            std::uint32_t repeat_bits = 0;
            if (!cursor.ReadBits(3, repeat_bits))
            {
                return false;
            }
            const auto repeat = static_cast<std::size_t>(repeat_bits) + 3;
            lengths.insert(lengths.end(), repeat, 0);
            break;
        }
        case 18:
        {
            std::uint32_t repeat_bits = 0;
            if (!cursor.ReadBits(7, repeat_bits))
            {
                return false;
            }
            const auto repeat = static_cast<std::size_t>(repeat_bits) + 11;
            lengths.insert(lengths.end(), repeat, 0);
            break;
        }
        default:
            return false;
        }
        if (lengths.size() > total)
        {
            return false;
        }
    }

    return BuildHuffmanDecoder(std::span<const std::uint8_t>(lengths.data(), hlit), litlen_decoder) &&
        BuildHuffmanDecoder(std::span<const std::uint8_t>(lengths.data() + hlit, hdist), dist_decoder);
}

bool ParseHuffmanBlockPayload(DeflateBitCursor& cursor,
                              const HuffmanDecoder& litlen_decoder,
                              const HuffmanDecoder& dist_decoder)
{
    while (true)
    {
        std::uint16_t sym = 0;
        if (!DecodeHuffmanSymbol(cursor, litlen_decoder, sym))
        {
            return false;
        }
        if (sym < 256)
        {
            continue;
        }
        if (sym == 256)
        {
            return true;
        }
        if (sym < 257 || sym > 285)
        {
            return false;
        }
        std::uint32_t extra_bits = 0;
        const auto len_index = static_cast<std::size_t>(sym - 257);
        if (!cursor.ReadBits(kLengthExtraBits[len_index], extra_bits))
        {
            return false;
        }

        std::uint16_t dist_sym = 0;
        if (!DecodeHuffmanSymbol(cursor, dist_decoder, dist_sym))
        {
            return false;
        }
        if (dist_sym >= 30)
        {
            return false;
        }
        if (!cursor.ReadBits(kDistanceExtraBits[dist_sym], extra_bits))
        {
            return false;
        }
    }
}

bool ParseDeflateStreamLayout(std::span<const std::byte> stream,
                              DeflateStreamLayout& layout)
{
    if (stream.empty())
    {
        return false;
    }

    DeflateBitCursor cursor(stream);
    while (true)
    {
        layout.final_header_bit = cursor.BitPosition();
        std::uint8_t bfinal = 0;
        std::uint32_t btype = 0;
        if (!cursor.ReadBit(bfinal) || !cursor.ReadBits(2, btype))
        {
            return false;
        }

        switch (btype)
        {
        case 0:
        {
            cursor.AlignToByte();
            std::uint32_t len = 0;
            std::uint32_t nlen = 0;
            if (!cursor.ReadBits(16, len) || !cursor.ReadBits(16, nlen))
            {
                return false;
            }
            if ((len ^ 0xFFFFu) != nlen)
            {
                return false;
            }
            if (!cursor.SkipBits(static_cast<std::size_t>(len) * 8))
            {
                return false;
            }
            break;
        }
        case 1:
        {
            HuffmanDecoder litlen_decoder;
            HuffmanDecoder dist_decoder;
            if (!FixedHuffmanTrees(litlen_decoder, dist_decoder) ||
                !ParseHuffmanBlockPayload(cursor, litlen_decoder, dist_decoder))
            {
                return false;
            }
            break;
        }
        case 2:
        {
            HuffmanDecoder litlen_decoder;
            HuffmanDecoder dist_decoder;
            if (!ReadDynamicHuffmanTrees(cursor, litlen_decoder, dist_decoder) ||
                !ParseHuffmanBlockPayload(cursor, litlen_decoder, dist_decoder))
            {
                return false;
            }
            break;
        }
        default:
            return false;
        }

        if (bfinal == 1u)
        {
            layout.end_bit = cursor.BitPosition();
            return true;
        }
    }
}

class DeflateBitWriter
{
public:
    explicit DeflateBitWriter(std::ostream& output)
        : output_(output)
    {
    }

    [[nodiscard]] std::size_t UsedBits() const noexcept
    {
        return used_bits_;
    }

    [[nodiscard]] std::size_t TotalBytesWritten() const noexcept
    {
        return total_bytes_written_ + (used_bits_ > 0 ? 1u : 0u);
    }

    bool WriteBit(std::uint8_t bit)
    {
        current_byte_ |= (bit & 1u) << used_bits_;
        ++used_bits_;
        if (used_bits_ == 8)
        {
            return FlushByte();
        }
        return true;
    }

    bool WriteBitsFromSlice(std::span<const std::byte> src,
                            std::size_t start_bit,
                            std::size_t bit_len)
    {
        if (bit_len == 0)
        {
            return true;
        }

        if (used_bits_ == 0 && start_bit % 8 == 0)
        {
            const auto byte_offset = start_bit / 8;
            const auto whole_bytes = bit_len / 8;
            const auto tail_bits = bit_len % 8;
            if (byte_offset + whole_bytes > src.size())
            {
                return false;
            }
            if (whole_bytes > 0 &&
                !WriteBytes(src.subspan(byte_offset, whole_bytes)))
            {
                return false;
            }
            if (tail_bits == 0)
            {
                return true;
            }
            if (byte_offset + whole_bytes >= src.size())
            {
                return false;
            }
            const auto tail =
                static_cast<std::uint8_t>(src[byte_offset + whole_bytes]);
            for (std::size_t shift = 0; shift < tail_bits; ++shift)
            {
                if (!WriteBit((tail >> shift) & 1u))
                {
                    return false;
                }
            }
            return true;
        }

        for (std::size_t index = 0; index < bit_len; ++index)
        {
            const auto absolute = start_bit + index;
            if (absolute / 8 >= src.size())
            {
                return false;
            }
            const auto bit =
                (static_cast<std::uint8_t>(src[absolute / 8]) >> (absolute % 8)) & 1u;
            if (!WriteBit(bit))
            {
                return false;
            }
        }
        return true;
    }

    bool WriteByteBits(std::uint8_t value)
    {
        for (std::size_t shift = 0; shift < 8; ++shift)
        {
            if (!WriteBit((value >> shift) & 1u))
            {
                return false;
            }
        }
        return true;
    }

    bool Finish()
    {
        if (used_bits_ == 0)
        {
            return true;
        }
        return FlushByte();
    }

private:
    bool WriteBytes(std::span<const std::byte> bytes)
    {
        output_.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        if (!output_)
        {
            return false;
        }
        total_bytes_written_ += bytes.size();
        return true;
    }

    bool FlushByte()
    {
        output_.put(static_cast<char>(current_byte_));
        if (!output_)
        {
            return false;
        }
        current_byte_ = 0;
        used_bits_ = 0;
        ++total_bytes_written_;
        return true;
    }

    std::ostream& output_;
    std::uint8_t current_byte_ = 0;
    std::size_t used_bits_ = 0;
    std::size_t total_bytes_written_ = 0;
};

template <typename TWriter>
bool WriteChunkBitsWithFinalOverride(TWriter& writer,
                                     std::span<const std::byte> chunk,
                                     const DeflateStreamLayout& layout,
                                     std::uint8_t desired_final)
{
    if (layout.end_bit == 0 || layout.final_header_bit >= layout.end_bit)
    {
        return false;
    }

    const auto current_final =
        (static_cast<std::uint8_t>(chunk[layout.final_header_bit / 8]) >>
         (layout.final_header_bit % 8)) &
        1u;

    const auto end_bytes = layout.end_bit / 8;
    const auto tail_bits = layout.end_bit % 8;
    if (writer.UsedBits() == 0 && layout.final_header_bit < layout.end_bit)
    {
        const auto total_bytes = end_bytes + (tail_bits != 0 ? 1u : 0u);
        if (total_bytes > chunk.size())
        {
            return false;
        }
        const auto header_byte_index = layout.final_header_bit / 8;
        if (header_byte_index > end_bytes || header_byte_index >= total_bytes)
        {
            return false;
        }

        if (header_byte_index > 0 &&
            !writer.WriteBitsFromSlice(chunk, 0, header_byte_index * 8))
        {
            return false;
        }

        auto header_byte = static_cast<std::uint8_t>(chunk[header_byte_index]);
        if (current_final != desired_final)
        {
            const auto mask = static_cast<std::uint8_t>(1u << (layout.final_header_bit % 8));
            header_byte = desired_final == 0 ? static_cast<std::uint8_t>(header_byte & ~mask)
                                             : static_cast<std::uint8_t>(header_byte | mask);
        }

        const auto header_bits_to_write = tail_bits == 0 && header_byte_index + 1 == total_bytes
            ? 8
            : std::min<std::size_t>(8, layout.end_bit - header_byte_index * 8);
        for (std::size_t shift = 0; shift < header_bits_to_write; ++shift)
        {
            if (!writer.WriteBit((header_byte >> shift) & 1u))
            {
                return false;
            }
        }

        const auto remaining_start_bit = (header_byte_index + 1) * 8;
        if (remaining_start_bit < layout.end_bit)
        {
            return writer.WriteBitsFromSlice(
                chunk,
                remaining_start_bit,
                layout.end_bit - remaining_start_bit);
        }
        return true;
    }

    if (current_final == desired_final)
    {
        return writer.WriteBitsFromSlice(chunk, 0, layout.end_bit);
    }

    if (layout.final_header_bit > 0 &&
        !writer.WriteBitsFromSlice(chunk, 0, layout.final_header_bit))
    {
        return false;
    }
    if (!writer.WriteBit(desired_final & 1u))
    {
        return false;
    }
    const auto tail_start = layout.final_header_bit + 1;
    if (tail_start < layout.end_bit)
    {
        return writer.WriteBitsFromSlice(chunk, tail_start, layout.end_bit - tail_start);
    }
    return true;
}

template <typename TWriter>
bool AppendEmptyStoredBlockNonFinal(TWriter& writer)
{
    if (!writer.WriteBit(0) || !writer.WriteBit(0) || !writer.WriteBit(0))
    {
        return false;
    }
    while (writer.UsedBits() != 0)
    {
        if (!writer.WriteBit(0))
        {
            return false;
        }
    }
    return writer.WriteByteBits(0x00) &&
        writer.WriteByteBits(0x00) &&
        writer.WriteByteBits(0xff) &&
        writer.WriteByteBits(0xff);
}

void AppendBitToVectorLsb(std::vector<std::byte>& bytes,
                          std::size_t& bit_len,
                          std::uint8_t bit)
{
    const auto byte_index = bit_len / 8;
    if (byte_index == bytes.size())
    {
        bytes.push_back(std::byte {0});
    }
    const auto bit_index = bit_len % 8;
    if ((bit & 1u) != 0)
    {
        auto value = static_cast<std::uint8_t>(bytes[byte_index]);
        value |= static_cast<std::uint8_t>(1u << bit_index);
        bytes[byte_index] = static_cast<std::byte>(value);
    }
    ++bit_len;
}

void AppendByteToVectorLsb(std::vector<std::byte>& bytes,
                           std::size_t& bit_len,
                           std::uint8_t value)
{
    for (std::size_t shift = 0; shift < 8; ++shift)
    {
        AppendBitToVectorLsb(bytes, bit_len, (value >> shift) & 1u);
    }
}

void AppendEmptyStoredBlockNonFinalBits(std::vector<std::byte>& bytes,
                                        std::size_t& bit_len)
{
    AppendBitToVectorLsb(bytes, bit_len, 0);
    AppendBitToVectorLsb(bytes, bit_len, 0);
    AppendBitToVectorLsb(bytes, bit_len, 0);
    while ((bit_len % 8) != 0)
    {
        AppendBitToVectorLsb(bytes, bit_len, 0);
    }
    AppendByteToVectorLsb(bytes, bit_len, 0x00);
    AppendByteToVectorLsb(bytes, bit_len, 0x00);
    AppendByteToVectorLsb(bytes, bit_len, 0xff);
    AppendByteToVectorLsb(bytes, bit_len, 0xff);
}

bool PrepareChunkForNonFinalStream(ChunkedCompressedChunk& chunk)
{
    auto& layout = chunk.layout;
    if (layout.end_bit == 0 || layout.final_header_bit >= layout.end_bit)
    {
        return false;
    }

    const auto required_bytes = (layout.end_bit + 7u) / 8u;
    if (chunk.compressed.size() < required_bytes)
    {
        return false;
    }

    chunk.compressed.resize(required_bytes);

    const auto header_byte_index = layout.final_header_bit / 8;
    auto header_value = static_cast<std::uint8_t>(chunk.compressed[header_byte_index]);
    header_value &= static_cast<std::uint8_t>(~(1u << (layout.final_header_bit % 8)));
    chunk.compressed[header_byte_index] = static_cast<std::byte>(header_value);

    auto end_bit = layout.end_bit;
    if ((end_bit % 8) != 0)
    {
        AppendEmptyStoredBlockNonFinalBits(chunk.compressed, end_bit);
    }
    layout.end_bit = end_bit;
    return true;
}
} // namespace

ZipOperationResult ExecuteChunkedCpuEntry(std::ostream& output,
                                          ZipEntrySource& entry,
                                          storage::IRandomAccessReader& reader,
                                          const pipeline::PipelineOptions& pipeline_options,
                                          std::size_t chunk_size,
                                          const core::ExecutionContext& context)
{
    ScopedZipEntryTimer timer(entry);
    if (chunk_size == 0)
    {
        return MakeError(ZipStatus::InvalidJob, "chunk size must be greater than zero");
    }
    if (!FitsInUint32(reader.Size()))
    {
        return MakeError(ZipStatus::Unsupported, "zip64 is not implemented for file: " + entry.source_label);
    }

    enum class TerminalState
    {
        Running,
        Succeeded,
        Failed,
        Cancelled,
    };

    struct SharedState
    {
        std::mutex mutex;
        std::condition_variable completion;
        std::map<std::size_t, ChunkedCompressedChunk> completed_chunks;
        TerminalState terminal = TerminalState::Running;
        ZipOperationResult terminal_result {ZipStatus::Ok, {}};
        std::size_t active_tasks = 0;
        std::atomic<std::uint64_t> deflate_ns {0};
    };

    const auto shared = std::make_shared<SharedState>();
    const auto publish_terminal = [shared](TerminalState terminal, ZipOperationResult result) {
        std::lock_guard lock(shared->mutex);
        if (shared->terminal == TerminalState::Running)
        {
            shared->terminal = terminal;
            shared->terminal_result = std::move(result);
        }
        shared->completion.notify_all();
    };

    const auto complete_chunk = [shared](ChunkedCompressedChunk chunk) {
        std::lock_guard lock(shared->mutex);
        if (shared->terminal == TerminalState::Running)
        {
            shared->completed_chunks.emplace(chunk.index, std::move(chunk));
        }
        if (shared->active_tasks > 0)
        {
            --shared->active_tasks;
        }
        shared->completion.notify_all();
    };

    const auto fail_task = [shared, publish_terminal](std::string message) {
        {
            std::lock_guard lock(shared->mutex);
            if (shared->active_tasks > 0)
            {
                --shared->active_tasks;
            }
        }
        publish_terminal(
            TerminalState::Failed,
            MakeError(ZipStatus::IoError, std::move(message)));
    };

    const auto profile = entry.compression_profile;
    const auto compress_chunk = [shared, complete_chunk, fail_task, profile](
                                    std::size_t index,
                                    bool is_final,
                                    std::vector<std::byte> raw) {
        {
            std::lock_guard lock(shared->mutex);
            if (shared->terminal != TerminalState::Running)
            {
                if (shared->active_tasks > 0)
                {
                    --shared->active_tasks;
                }
                shared->completion.notify_all();
                return;
            }
        }

        try
        {
#if defined(COZIP_ENABLE_TEST_HOOKS)
            if (g_failing_chunk_index.load(std::memory_order_relaxed) == index)
            {
                fail_task("injected chunk compression failure");
                return;
            }
#endif
            const auto started_at = std::chrono::steady_clock::now();
            auto compressed = codecs::CompressDeflateBuffer(raw, profile);
            shared->deflate_ns.fetch_add(
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - started_at).count()),
                std::memory_order_relaxed);
            if (!compressed.success)
            {
                fail_task(compressed.error_message);
                return;
            }

            ChunkedCompressedChunk result {};
            result.index = index;
            result.raw_size = raw.size();
            result.compressed = std::move(compressed.bytes);
            if (!ParseDeflateStreamLayout(result.compressed, result.layout))
            {
                fail_task("failed to parse deflate chunk layout");
                return;
            }
            if (!is_final && !PrepareChunkForNonFinalStream(result))
            {
                fail_task("failed to prepare non-final chunk stream");
                return;
            }
            complete_chunk(std::move(result));
        }
        catch (const std::exception& error)
        {
            fail_task(std::string("chunk compression failed: ") + error.what());
        }
        catch (...)
        {
            fail_task("chunk compression failed with an unknown exception");
        }
    };

    auto* executor = context.environment != nullptr
        ? context.environment->task_executor
        : nullptr;
    if (executor != nullptr && executor->concurrency() == 0)
    {
        executor = nullptr;
    }
    const auto submit_task = [executor, publish_terminal](core::MoveOnlyTask task) {
        try
        {
            return executor->submit(std::move(task));
        }
        catch (const std::exception& error)
        {
            publish_terminal(
                TerminalState::Failed,
                MakeError(
                    ZipStatus::IoError,
                    std::string("task executor submit failed: ") + error.what()));
        }
        catch (...)
        {
            publish_terminal(
                TerminalState::Failed,
                MakeError(ZipStatus::IoError, "task executor submit failed"));
        }
        return false;
    };

    const auto file_size = reader.Size();
    const auto chunk_count = file_size == 0
        ? std::size_t {1}
        : static_cast<std::size_t>(file_size / chunk_size +
              (file_size % chunk_size != 0 ? 1u : 0u));
    const auto max_in_flight = std::max<std::size_t>(
        1,
        std::min<std::size_t>(pipeline_options.max_in_flight_chunks, chunk_count));

    Crc32 crc;
    DeflateBitWriter bit_writer(output);
    std::uint64_t next_read_offset = 0;
    std::size_t next_submit_index = 0;
    std::size_t next_write_index = 0;
    std::size_t outstanding_chunks = 0;
    std::uint64_t read_ns = 0;
    std::uint64_t crc_ns = 0;
    std::uint64_t write_ns = 0;

    while (next_write_index < chunk_count)
    {
        while (next_submit_index < chunk_count && outstanding_chunks < max_in_flight)
        {
            if (IsCancellationRequested(context))
            {
                publish_terminal(TerminalState::Cancelled, MakeCancelled("zip create cancelled"));
                break;
            }

            const auto raw_size = file_size == 0
                ? std::size_t {0}
                : static_cast<std::size_t>(std::min<std::uint64_t>(
                      chunk_size, file_size - next_read_offset));
            std::vector<std::byte> raw(raw_size);
            std::size_t filled = 0;
            const auto read_started_at = std::chrono::steady_clock::now();
            while (filled < raw.size())
            {
                if (IsCancellationRequested(context))
                {
                    publish_terminal(TerminalState::Cancelled, MakeCancelled("zip create cancelled"));
                    break;
                }
                std::size_t bytes_read = 0;
                std::string error_message;
                if (!reader.Read(
                        next_read_offset + filled,
                        std::span<std::byte>(raw).subspan(filled),
                        bytes_read,
                        error_message))
                {
                    publish_terminal(
                        TerminalState::Failed,
                        MakeError(ZipStatus::IoError, error_message.empty()
                            ? "failed to read input file: " + entry.source_label
                            : error_message));
                    break;
                }
                if (bytes_read == 0)
                {
                    publish_terminal(
                        TerminalState::Failed,
                        MakeError(ZipStatus::IoError, "unexpected end of input: " + entry.source_label));
                    break;
                }
                if (bytes_read > raw.size() - filled)
                {
                    publish_terminal(
                        TerminalState::Failed,
                        MakeError(ZipStatus::IoError, "reader returned more bytes than requested"));
                    break;
                }
                filled += bytes_read;
            }
            read_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - read_started_at).count());

            {
                std::lock_guard lock(shared->mutex);
                if (shared->terminal != TerminalState::Running)
                {
                    break;
                }
            }

            const auto crc_started_at = std::chrono::steady_clock::now();
            if (!raw.empty())
            {
                crc.Update(raw.data(), raw.size());
            }
            crc_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - crc_started_at).count());

            const auto index = next_submit_index;
            const auto is_final = index + 1 == chunk_count;
            ++next_submit_index;
            ++outstanding_chunks;
            next_read_offset += raw.size();
            {
                std::lock_guard lock(shared->mutex);
                ++shared->active_tasks;
            }

            if (executor == nullptr)
            {
                compress_chunk(index, is_final, std::move(raw));
            }
            else if (!submit_task(core::MoveOnlyTask(
                         [compress_chunk, index, is_final, raw = std::move(raw)]() mutable {
                             compress_chunk(index, is_final, std::move(raw));
                         })))
            {
                {
                    std::lock_guard lock(shared->mutex);
                    if (shared->active_tasks > 0)
                    {
                        --shared->active_tasks;
                    }
                }
                publish_terminal(
                    TerminalState::Failed,
                    MakeError(ZipStatus::IoError, "task executor rejected compression chunk"));
                break;
            }
        }

        std::unique_lock lock(shared->mutex);
        while (shared->terminal == TerminalState::Running &&
               !shared->completed_chunks.contains(next_write_index))
        {
            shared->completion.wait(lock);
            if (IsCancellationRequested(context))
            {
                lock.unlock();
                publish_terminal(TerminalState::Cancelled, MakeCancelled("zip create cancelled"));
                lock.lock();
            }
        }

        if (shared->terminal != TerminalState::Running)
        {
            shared->completion.wait(lock, [&shared] { return shared->active_tasks == 0; });
            const auto result = shared->terminal_result;
            lock.unlock();
            return result;
        }

        auto completed_node = shared->completed_chunks.extract(next_write_index);
        lock.unlock();
        auto completed = std::move(completed_node.mapped());

        const auto write_started_at = std::chrono::steady_clock::now();
        const auto is_final = next_write_index + 1 == chunk_count;
        const auto write_ok = is_final
            ? WriteChunkBitsWithFinalOverride(
                  bit_writer, completed.compressed, completed.layout, 1u)
            : bit_writer.WriteBitsFromSlice(
                  completed.compressed, 0, completed.layout.end_bit);
        write_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - write_started_at).count());
        if (!write_ok)
        {
            publish_terminal(
                TerminalState::Failed,
                MakeError(ZipStatus::IoError, "failed to assemble chunked deflate stream"));
            std::unique_lock terminal_lock(shared->mutex);
            shared->completion.wait(
                terminal_lock,
                [&shared] { return shared->active_tasks == 0; });
            return shared->terminal_result;
        }
        ++next_write_index;
        --outstanding_chunks;
    }

    if (!bit_writer.Finish())
    {
        publish_terminal(
            TerminalState::Failed,
            MakeError(ZipStatus::IoError, "failed to flush assembled deflate stream"));
        return shared->terminal_result;
    }
    if (!FitsInUint32(bit_writer.TotalBytesWritten()))
    {
        publish_terminal(
            TerminalState::Failed,
            MakeError(ZipStatus::Unsupported, "compressed entry requires zip64: " + entry.source_label));
        return shared->terminal_result;
    }

    publish_terminal(TerminalState::Succeeded, {ZipStatus::Ok, {}});
    entry.crc32 = crc.Finalize();
    entry.size = static_cast<std::uint32_t>(file_size);
    entry.compressed_size = static_cast<std::uint32_t>(bit_writer.TotalBytesWritten());
    entry.general_purpose_flag = kDataDescriptorFlag;
    timer.AddPhase("read", std::chrono::nanoseconds(read_ns));
    timer.AddPhase("crc", std::chrono::nanoseconds(crc_ns));
    timer.AddPhase("deflate", std::chrono::nanoseconds(
        shared->deflate_ns.load(std::memory_order_relaxed)));
    timer.AddPhase("write", std::chrono::nanoseconds(write_ns));
    timer.Finish();
    return {ZipStatus::Ok, {}};
}

#if defined(COZIP_ENABLE_TEST_HOOKS)
void SetChunkCompressionFailureForTesting(std::size_t chunk_index) noexcept
{
    g_failing_chunk_index.store(chunk_index, std::memory_order_relaxed);
}

void ClearChunkCompressionFailureForTesting() noexcept
{
    g_failing_chunk_index.store(std::numeric_limits<std::size_t>::max(), std::memory_order_relaxed);
}
#endif
}
