#include "zip_chunked_cpu.h"

#include "libdeflate.h"

#include <condition_variable>
#include <map>

namespace cozip::format_zip
{
namespace
{
#if defined(COZIP_ENABLE_TEST_HOOKS)
std::atomic<std::size_t> g_failing_chunk_index {std::numeric_limits<std::size_t>::max()};
std::mutex g_last_scheduler_metrics_mutex;
ChunkedCpuSchedulerMetrics g_last_scheduler_metrics;
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
    std::size_t retained_reservation = 0;
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
        if (!PeekBits(bit_count, value))
        {
            return false;
        }
        bit_pos_ += bit_count;
        return true;
    }

    bool PeekBits(std::size_t bit_count, std::uint32_t& value) const
    {
        if (bit_count > 24 || bit_count > data_.size() * 8 - bit_pos_)
        {
            return false;
        }
        const auto byte_index = bit_pos_ / 8;
        const auto shift = bit_pos_ % 8;
        std::uint32_t packed = 0;
        const auto byte_count = (shift + bit_count + 7) / 8;
        for (std::size_t index = 0; index < byte_count; ++index)
        {
            packed |= static_cast<std::uint32_t>(
                          static_cast<std::uint8_t>(data_[byte_index + index]))
                << (index * 8);
        }
        value = bit_count == 0
            ? 0
            : (packed >> shift) & ((std::uint32_t {1} << bit_count) - 1u);
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
    static constexpr std::size_t kLookupBits = 10;
    std::array<std::uint16_t, 16> counts {};
    std::vector<std::uint16_t> symbols;
    std::array<std::uint16_t, std::size_t {1} << kLookupBits> lookup_symbols {};
    std::array<std::uint8_t, std::size_t {1} << kLookupBits> lookup_lengths {};
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

    for (std::size_t pattern = 0; pattern < decoder.lookup_lengths.size(); ++pattern)
    {
        std::uint32_t code = 0;
        std::uint32_t first = 0;
        std::size_t symbol_index = 0;
        for (std::size_t len = 1;
             len <= std::min(decoder.max_bits, HuffmanDecoder::kLookupBits);
             ++len)
        {
            code |= static_cast<std::uint32_t>((pattern >> (len - 1)) & 1u);
            const auto count = static_cast<std::uint32_t>(decoder.counts[len]);
            if (code >= first && code - first < count)
            {
                const auto offset = symbol_index + static_cast<std::size_t>(code - first);
                if (offset < decoder.symbols.size())
                {
                    decoder.lookup_symbols[pattern] = decoder.symbols[offset];
                    decoder.lookup_lengths[pattern] = static_cast<std::uint8_t>(len);
                }
                break;
            }
            symbol_index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
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

    std::uint32_t prefix = 0;
    if (cursor.PeekBits(HuffmanDecoder::kLookupBits, prefix))
    {
        const auto length = decoder.lookup_lengths[prefix];
        if (length != 0)
        {
            if (!cursor.SkipBits(length))
            {
                return false;
            }
            symbol = decoder.lookup_symbols[prefix];
            return true;
        }
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

class ChunkedCpuCreateScheduler::Impl
{
public:
    Impl(const pipeline::PipelineOptions& pipeline_options,
         std::size_t memory_budget_mb,
         const core::ExecutionContext& context)
        : max_in_flight_chunks_(std::max<std::size_t>(1, pipeline_options.max_in_flight_chunks)),
          memory_budget_bytes_(ResolveMemoryBudget(memory_budget_mb)),
          context_(context),
          executor_(context.environment != nullptr
                  ? context.environment->task_executor
                  : nullptr),
          started_at_(std::chrono::steady_clock::now()),
          shared_(std::make_shared<SharedState>())
    {
        if (executor_ != nullptr && executor_->concurrency() == 0)
        {
            executor_ = nullptr;
        }
        foreground_target_ = executor_ != nullptr
            ? std::max<std::size_t>(1, executor_->concurrency())
            : 1;
    }

    ~Impl()
    {
        std::unique_lock lock(shared_->mutex);
        if (shared_->terminal == TerminalState::Running)
        {
            PublishTerminalLocked(
                TerminalState::Failed,
                MakeError(ZipStatus::IoError, "chunk scheduler abandoned before completion"));
        }
        shared_->completion.wait(lock, [this] { return shared_->active_tasks == 0; });
        lock.unlock();
        PublishTestMetrics();
    }

    ZipOperationResult Initialize(std::span<ZipEntrySource> entries,
                                  std::size_t chunk_size)
    {
        if (chunk_size == 0)
        {
            return MakeError(ZipStatus::InvalidJob, "chunk size must be greater than zero");
        }

        try
        {
            for (auto& entry : entries)
            {
                if (entry.is_directory || !entry.prepared_data.empty() ||
                    entry.method != codecs::ZipMethod::Deflate)
                {
                    continue;
                }

                auto state = std::make_unique<EntryState>();
                state->entry = &entry;
                state->chunk_size = chunk_size;
                state->file_size = entry.source_reader != nullptr
                    ? entry.source_reader->Size()
                    : static_cast<std::uint64_t>(entry.size);
                if (!FitsInUint32(state->file_size))
                {
                    return MakeError(
                        ZipStatus::Unsupported,
                        "zip64 is not implemented for file: " + entry.source_label);
                }
                state->chunk_count = state->file_size == 0
                    ? std::size_t {1}
                    : static_cast<std::size_t>(state->file_size / chunk_size +
                          (state->file_size % chunk_size != 0 ? 1u : 0u));

                if (entry.source_reader != nullptr)
                {
                    state->reader = entry.source_reader;
                }
                else
                {
                    std::string open_error;
                    if (!OpenRandomAccessReader(
                            *entry.storage_factory,
                            entry.source_path,
                            entry.mapping_mode,
                            state->opened_reader,
                            open_error))
                    {
                        return MakeError(ZipStatus::IoError, open_error);
                    }
                    state->reader = state->opened_reader.get();
                }

                ChunkedCpuEntryMetrics metrics {};
                metrics.archive_path = entry.archive_path;
                shared_->metrics.entries.push_back(std::move(metrics));
                entries_.push_back(std::move(state));
            }
            shared_->completed_chunks.resize(entries_.size());
        }
        catch (const std::bad_alloc&)
        {
            return MakeError(ZipStatus::IoError, "failed to allocate chunk scheduler state");
        }

        if (entries_.empty())
        {
            return {ZipStatus::Ok, {}};
        }

        foreground_target_ = std::min(foreground_target_, max_in_flight_chunks_);
        if (entries_.size() > 1 && max_in_flight_chunks_ > 1)
        {
            foreground_target_ = std::min(
                foreground_target_, max_in_flight_chunks_ - 1);
        }

        return AdmitAvailable(0);
    }

    [[nodiscard]] bool Empty() const noexcept
    {
        return entries_.empty();
    }

    [[nodiscard]] bool Handles(const ZipEntrySource& entry) const noexcept
    {
        return FindEntry(entry) != kNoEntry;
    }

    ZipOperationResult WriteEntry(std::ostream& output, ZipEntrySource& entry)
    {
        const auto entry_index = FindEntry(entry);
        if (entry_index == kNoEntry)
        {
            return MakeError(ZipStatus::InvalidJob, "entry is not registered with chunk scheduler");
        }
        if (entry_index != next_output_entry_)
        {
            return MakeError(ZipStatus::InvalidJob, "chunk scheduler entry order mismatch");
        }

        auto admitted = AdmitAvailable(entry_index);
        if (admitted.status != ZipStatus::Ok)
        {
            return FinishFailure(std::move(admitted));
        }

        auto& state = *entries_[entry_index];
        DeflateBitWriter bit_writer(output);
        while (state.next_write_index < state.chunk_count)
        {
            std::unique_lock lock(shared_->mutex);
            shared_->completion.wait(lock, [&] {
                return shared_->terminal != TerminalState::Running ||
                    shared_->completed_chunks[entry_index].contains(state.next_write_index);
            });
            if (IsCancellationRequested(context_) &&
                shared_->terminal == TerminalState::Running)
            {
                PublishTerminalLocked(
                    TerminalState::Cancelled,
                    MakeCancelled("zip create cancelled"));
            }
            if (shared_->terminal != TerminalState::Running)
            {
                shared_->completion.wait(lock, [this] { return shared_->active_tasks == 0; });
                const auto result = shared_->terminal_result;
                lock.unlock();
                PublishTestMetrics();
                return result;
            }

            auto completed_node =
                shared_->completed_chunks[entry_index].extract(state.next_write_index);
            auto completed = std::move(completed_node.mapped());
            ReleaseCompletedChunkLocked(
                completed.compressed.capacity(),
                completed.retained_reservation);
            lock.unlock();

            const auto write_started_at = std::chrono::steady_clock::now();
            const auto is_final = state.next_write_index + 1 == state.chunk_count;
            const auto write_ok = is_final
                ? WriteChunkBitsWithFinalOverride(
                      bit_writer, completed.compressed, completed.layout, 1u)
                : bit_writer.WriteBitsFromSlice(
                      completed.compressed, 0, completed.layout.end_bit);
            const auto write_ns = DurationNs(write_started_at);
            {
                std::lock_guard metrics_lock(shared_->mutex);
                shared_->metrics.entries[entry_index].write_ns += write_ns;
            }
            if (!write_ok)
            {
                return FinishFailure(MakeError(
                    ZipStatus::IoError,
                    "failed to assemble chunked deflate stream"));
            }

            ++state.next_write_index;
            if (state.next_write_index < state.chunk_count)
            {
                admitted = AdmitAvailable(entry_index);
                if (admitted.status != ZipStatus::Ok)
                {
                    return FinishFailure(std::move(admitted));
                }
            }
        }

        if (!bit_writer.Finish())
        {
            return FinishFailure(MakeError(
                ZipStatus::IoError,
                "failed to flush assembled deflate stream"));
        }
        if (!FitsInUint32(bit_writer.TotalBytesWritten()))
        {
            return FinishFailure(MakeError(
                ZipStatus::Unsupported,
                "compressed entry requires zip64: " + entry.source_label));
        }

        entry.crc32 = state.crc.Finalize();
        entry.size = static_cast<std::uint32_t>(state.file_size);
        entry.compressed_size = static_cast<std::uint32_t>(bit_writer.TotalBytesWritten());
        entry.general_purpose_flag = kDataDescriptorFlag;
        state.output_complete = true;

        {
            std::lock_guard lock(shared_->mutex);
            if (!shared_->first_entry_completed)
            {
                shared_->first_entry_completed = true;
                shared_->metrics.entries_started_before_first_completed =
                    static_cast<std::size_t>(std::count_if(
                        entries_.begin() + 1,
                        entries_.end(),
                        [](const auto& candidate) { return candidate->started; }));
            }
        }

        TraceEntry(entry_index);
        ++next_output_entry_;
        if (next_output_entry_ < entries_.size())
        {
            admitted = AdmitAvailable(next_output_entry_);
            if (admitted.status != ZipStatus::Ok)
            {
                return FinishFailure(std::move(admitted));
            }
        }
        else
        {
            std::lock_guard lock(shared_->mutex);
            PublishTerminalLocked(TerminalState::Succeeded, {ZipStatus::Ok, {}});
            TraceSchedulerLocked();
        }
        PublishTestMetrics();
        return {ZipStatus::Ok, {}};
    }

    [[nodiscard]] ChunkedCpuSchedulerMetrics Metrics() const
    {
        std::lock_guard lock(shared_->mutex);
        auto metrics = shared_->metrics;
        metrics.wall_ns = DurationNs(started_at_);
        return metrics;
    }

private:
    enum class TerminalState
    {
        Running,
        Succeeded,
        Failed,
        Cancelled,
    };

    struct EntryState
    {
        ZipEntrySource* entry = nullptr;
        std::unique_ptr<storage::IRandomAccessReader> opened_reader;
        storage::IRandomAccessReader* reader = nullptr;
        std::uint64_t file_size = 0;
        std::size_t chunk_size = 0;
        std::size_t chunk_count = 0;
        std::uint64_t next_read_offset = 0;
        std::size_t next_submit_index = 0;
        std::size_t next_write_index = 0;
        Crc32 crc;
        bool started = false;
        bool output_complete = false;
    };

    struct SharedState
    {
        std::mutex mutex;
        std::condition_variable completion;
        std::vector<std::map<std::size_t, ChunkedCompressedChunk>> completed_chunks;
        TerminalState terminal = TerminalState::Running;
        ZipOperationResult terminal_result {ZipStatus::Ok, {}};
        std::size_t active_tasks = 0;
        std::size_t in_flight_chunks = 0;
        std::size_t reserved_bytes = 0;
        std::size_t raw_bytes = 0;
        std::size_t compressed_bytes = 0;
        bool first_entry_completed = false;
        ChunkedCpuSchedulerMetrics metrics;
    };

    static constexpr std::size_t kNoEntry = std::numeric_limits<std::size_t>::max();
    static constexpr std::size_t kChunkMetadataReserve = 256;

    static std::size_t ResolveMemoryBudget(std::size_t memory_budget_mb) noexcept
    {
        constexpr std::size_t bytes_per_mb = 1024u * 1024u;
        if (memory_budget_mb == 0)
        {
            return std::numeric_limits<std::size_t>::max();
        }
        if (memory_budget_mb > std::numeric_limits<std::size_t>::max() / bytes_per_mb)
        {
            return std::numeric_limits<std::size_t>::max();
        }
        return memory_budget_mb * bytes_per_mb;
    }

    static std::uint64_t DurationNs(std::chrono::steady_clock::time_point started_at) noexcept
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started_at).count());
    }

    [[nodiscard]] std::size_t FindEntry(const ZipEntrySource& entry) const noexcept
    {
        for (std::size_t index = 0; index < entries_.size(); ++index)
        {
            if (entries_[index]->entry == &entry)
            {
                return index;
            }
        }
        return kNoEntry;
    }

    [[nodiscard]] std::size_t RawSize(const EntryState& state) const noexcept
    {
        if (state.file_size == 0)
        {
            return 0;
        }
        return static_cast<std::size_t>(std::min<std::uint64_t>(
            state.chunk_size,
            state.file_size - state.next_read_offset));
    }

    [[nodiscard]] bool HasCapacityLocked(std::size_t reservation) const noexcept
    {
        return reservation != std::numeric_limits<std::size_t>::max() &&
            shared_->in_flight_chunks < max_in_flight_chunks_ &&
            reservation <= memory_budget_bytes_ -
                std::min(memory_budget_bytes_, shared_->reserved_bytes);
    }

    [[nodiscard]] std::size_t ReservationFor(std::size_t raw_size) const noexcept
    {
        const auto compressed_bound = libdeflate_deflate_compress_bound(nullptr, raw_size);
        if (compressed_bound > std::numeric_limits<std::size_t>::max() - raw_size ||
            compressed_bound + raw_size >
                std::numeric_limits<std::size_t>::max() - kChunkMetadataReserve - 8u)
        {
            return std::numeric_limits<std::size_t>::max();
        }
        return raw_size + compressed_bound + kChunkMetadataReserve + 8u;
    }

    ZipOperationResult AdmitAvailable(std::size_t required_entry)
    {
        while (true)
        {
            if (IsCancellationRequested(context_))
            {
                return Cancel();
            }

            auto& required = *entries_[required_entry];
            const auto required_outstanding =
                required.next_submit_index - required.next_write_index;
            const auto required_target = std::min(
                foreground_target_,
                required.chunk_count - required.next_write_index);
            const auto needs_foreground =
                required.next_submit_index < required.chunk_count &&
                required_outstanding < required_target;
            if (needs_foreground)
            {
                const auto reservation = ReservationFor(RawSize(required));
                std::lock_guard lock(shared_->mutex);
                if (!HasCapacityLocked(reservation))
                {
                    if (shared_->in_flight_chunks == 0)
                    {
                        return MakeError(
                            ZipStatus::IoError,
                            "memory budget is too small for one compression chunk");
                    }
                    return {ZipStatus::Ok, {}};
                }
            }

            std::size_t candidate = kNoEntry;
            {
                std::lock_guard lock(shared_->mutex);
                if (shared_->terminal != TerminalState::Running)
                {
                    return shared_->terminal_result;
                }
                if (shared_->in_flight_chunks >= max_in_flight_chunks_)
                {
                    return {ZipStatus::Ok, {}};
                }

                if (needs_foreground)
                {
                    const auto reservation = ReservationFor(RawSize(required));
                    if (HasCapacityLocked(reservation))
                    {
                        candidate = required_entry;
                        round_robin_cursor_ = (required_entry + 1) % entries_.size();
                    }
                }

                if (candidate == kNoEntry)
                {
                    for (std::size_t attempt = 0; attempt < entries_.size(); ++attempt)
                    {
                        const auto index = (round_robin_cursor_ + attempt) % entries_.size();
                        auto& state = *entries_[index];
                        if (state.next_submit_index >= state.chunk_count)
                        {
                            continue;
                        }
                        const auto reservation = ReservationFor(RawSize(state));
                        if (HasCapacityLocked(reservation))
                        {
                            candidate = index;
                            round_robin_cursor_ = (index + 1) % entries_.size();
                            break;
                        }
                    }
                }
            }

            if (candidate == kNoEntry)
            {
                return {ZipStatus::Ok, {}};
            }
            auto submitted = AdmitOne(candidate);
            if (submitted.status != ZipStatus::Ok)
            {
                return submitted;
            }
        }
    }

    ZipOperationResult AdmitOne(std::size_t entry_index)
    {
        auto& state = *entries_[entry_index];
        const auto raw_size = RawSize(state);
        const auto reservation = ReservationFor(raw_size);
        const auto compressed_reserve =
            reservation - raw_size - kChunkMetadataReserve;
        {
            std::lock_guard lock(shared_->mutex);
            if (!HasCapacityLocked(reservation))
            {
                return {ZipStatus::Ok, {}};
            }
            ++shared_->in_flight_chunks;
            shared_->reserved_bytes += reservation;
            shared_->raw_bytes += raw_size;
            shared_->metrics.max_in_flight_chunks = std::max(
                shared_->metrics.max_in_flight_chunks,
                shared_->in_flight_chunks);
            shared_->metrics.max_reserved_bytes = std::max(
                shared_->metrics.max_reserved_bytes,
                shared_->reserved_bytes);
            shared_->metrics.max_in_flight_raw_bytes = std::max(
                shared_->metrics.max_in_flight_raw_bytes,
                shared_->raw_bytes);
            UpdateMaxPayloadLocked();
        }

        std::vector<std::byte> raw;
        try
        {
            raw.resize(raw_size);
        }
        catch (const std::bad_alloc&)
        {
            return FailUnsubmitted(
                reservation,
                raw_size,
                "failed to allocate raw compression chunk");
        }

        std::size_t filled = 0;
        const auto read_started_at = std::chrono::steady_clock::now();
        while (filled < raw.size())
        {
            if (IsCancellationRequested(context_))
            {
                ReleaseUnsubmitted(reservation, raw_size);
                return Cancel();
            }
            std::size_t bytes_read = 0;
            std::string error_message;
            if (!state.reader->Read(
                    state.next_read_offset + filled,
                    std::span<std::byte>(raw).subspan(filled),
                    bytes_read,
                    error_message))
            {
                ReleaseUnsubmitted(reservation, raw_size);
                return Fail(MakeError(
                    ZipStatus::IoError,
                    error_message.empty()
                        ? "failed to read input file: " + state.entry->source_label
                        : error_message));
            }
            if (bytes_read == 0 || bytes_read > raw.size() - filled)
            {
                ReleaseUnsubmitted(reservation, raw_size);
                return Fail(MakeError(
                    ZipStatus::IoError,
                    bytes_read == 0
                        ? "unexpected end of input: " + state.entry->source_label
                        : "reader returned more bytes than requested"));
            }
            filled += bytes_read;
        }
        const auto read_ns = DurationNs(read_started_at);

        if (!raw.empty())
        {
            state.crc.Update(raw.data(), raw.size());
        }
        const auto chunk_index = state.next_submit_index;
        const auto is_final = chunk_index + 1 == state.chunk_count;
        state.next_read_offset += raw.size();
        ++state.next_submit_index;
        state.started = true;
        {
            std::lock_guard lock(shared_->mutex);
            shared_->metrics.entries[entry_index].raw_bytes += raw_size;
            shared_->metrics.entries[entry_index].read_ns += read_ns;
            ++shared_->active_tasks;
            shared_->metrics.max_active_tasks = std::max(
                shared_->metrics.max_active_tasks,
                shared_->active_tasks);
        }

        auto task = core::MoveOnlyTask(
            [shared = shared_,
             entry_index,
             chunk_index,
             is_final,
             profile = state.entry->compression_profile,
             reservation,
             compressed_reserve,
             raw_size,
             raw = std::move(raw)]() mutable {
                ExecuteTask(
                    std::move(shared),
                    entry_index,
                    chunk_index,
                    is_final,
                    profile,
                    reservation,
                    compressed_reserve,
                    raw_size,
                    std::move(raw));
            });

        if (executor_ == nullptr)
        {
            task();
            return {ZipStatus::Ok, {}};
        }

        bool accepted = false;
        try
        {
            accepted = executor_->submit(std::move(task));
        }
        catch (const std::exception& error)
        {
            RejectTask(reservation, raw_size);
            return Fail(MakeError(
                ZipStatus::IoError,
                std::string("task executor submit failed: ") + error.what()));
        }
        catch (...)
        {
            RejectTask(reservation, raw_size);
            return Fail(MakeError(ZipStatus::IoError, "task executor submit failed"));
        }
        if (!accepted)
        {
            RejectTask(reservation, raw_size);
            return Fail(MakeError(
                ZipStatus::IoError,
                "task executor rejected compression chunk"));
        }
        return {ZipStatus::Ok, {}};
    }

    static void ExecuteTask(std::shared_ptr<SharedState> shared,
                            std::size_t entry_index,
                            std::size_t chunk_index,
                            bool is_final,
                            core::CompressionProfile profile,
                            std::size_t reservation,
                            std::size_t compressed_reserve,
                            std::size_t raw_size,
                            std::vector<std::byte> raw)
    {
        bool terminal = false;
        {
            std::lock_guard lock(shared->mutex);
            terminal = shared->terminal != TerminalState::Running;
        }
        if (terminal)
        {
            std::vector<std::byte>().swap(raw);
            AbandonTask(*shared, reservation, raw_size);
            return;
        }

        {
            std::lock_guard lock(shared->mutex);
            shared->compressed_bytes += compressed_reserve;
            shared->metrics.max_in_flight_compressed_bytes = std::max(
                shared->metrics.max_in_flight_compressed_bytes,
                shared->compressed_bytes);
            shared->metrics.max_in_flight_payload_bytes = std::max(
                shared->metrics.max_in_flight_payload_bytes,
                shared->raw_bytes + shared->compressed_bytes);
        }

        try
        {
#if defined(COZIP_ENABLE_TEST_HOOKS)
            if (g_failing_chunk_index.load(std::memory_order_relaxed) == chunk_index)
            {
                std::vector<std::byte>().swap(raw);
                FailTask(
                    *shared,
                    reservation,
                    compressed_reserve,
                    raw_size,
                    "injected chunk compression failure");
                return;
            }
#endif
            const auto deflate_started_at = std::chrono::steady_clock::now();
            auto compressed = codecs::CompressDeflateBuffer(raw, profile);
            const auto deflate_ns = DurationNs(deflate_started_at);
            if (!compressed.success)
            {
                std::vector<std::byte>().swap(raw);
                FailTask(
                    *shared,
                    reservation,
                    compressed_reserve,
                    raw_size,
                    compressed.error_message);
                return;
            }

            ChunkedCompressedChunk result {};
            result.index = chunk_index;
            result.raw_size = raw_size;
            result.compressed = std::move(compressed.bytes);
            if (!ParseDeflateStreamLayout(result.compressed, result.layout) ||
                (!is_final && !PrepareChunkForNonFinalStream(result)))
            {
                std::vector<std::byte>().swap(raw);
                FailTask(
                    *shared,
                    reservation,
                    compressed_reserve,
                    raw_size,
                    "failed to prepare deflate chunk stream");
                return;
            }

            std::vector<std::byte>().swap(raw);
            if (result.compressed.capacity() > result.compressed.size() * 2u &&
                result.compressed.capacity() - result.compressed.size() > 1024u * 1024u)
            {
                std::vector<std::byte> compact(
                    result.compressed.begin(), result.compressed.end());
                result.compressed.swap(compact);
            }
            const auto compressed_capacity = result.compressed.capacity();
            result.retained_reservation =
                compressed_capacity + kChunkMetadataReserve;

            std::lock_guard lock(shared->mutex);
            if (shared->raw_bytes >= raw_size)
            {
                shared->raw_bytes -= raw_size;
            }
            if (shared->terminal != TerminalState::Running)
            {
                ReleaseTaskLocked(*shared, reservation, compressed_reserve);
                shared->completion.notify_all();
                return;
            }
            shared->compressed_bytes -= compressed_reserve;
            shared->compressed_bytes += compressed_capacity;
            shared->reserved_bytes -= reservation;
            shared->reserved_bytes += result.retained_reservation;
            shared->metrics.entries[entry_index].deflate_ns += deflate_ns;
            shared->metrics.max_in_flight_compressed_bytes = std::max(
                shared->metrics.max_in_flight_compressed_bytes,
                shared->compressed_bytes);
            shared->metrics.max_reserved_bytes = std::max(
                shared->metrics.max_reserved_bytes,
                shared->reserved_bytes);
            shared->metrics.max_in_flight_payload_bytes = std::max(
                shared->metrics.max_in_flight_payload_bytes,
                shared->raw_bytes + shared->compressed_bytes);
            shared->completed_chunks[entry_index].emplace(chunk_index, std::move(result));
            if (shared->active_tasks > 0)
            {
                --shared->active_tasks;
            }
            shared->completion.notify_all();
        }
        catch (const std::exception& error)
        {
            std::vector<std::byte>().swap(raw);
            FailTask(
                *shared,
                reservation,
                compressed_reserve,
                raw_size,
                std::string("chunk compression failed: ") + error.what());
        }
        catch (...)
        {
            std::vector<std::byte>().swap(raw);
            FailTask(
                *shared,
                reservation,
                compressed_reserve,
                raw_size,
                "chunk compression failed");
        }
    }

    static void AbandonTask(SharedState& shared,
                            std::size_t reservation,
                            std::size_t raw_size)
    {
        std::lock_guard lock(shared.mutex);
        if (shared.raw_bytes >= raw_size)
        {
            shared.raw_bytes -= raw_size;
        }
        ReleaseTaskLocked(shared, reservation, 0);
        shared.completion.notify_all();
    }

    static void FailTask(SharedState& shared,
                         std::size_t reservation,
                         std::size_t compressed_reserve,
                         std::size_t raw_size,
                         std::string message)
    {
        std::lock_guard lock(shared.mutex);
        if (shared.raw_bytes >= raw_size)
        {
            shared.raw_bytes -= raw_size;
        }
        ReleaseTaskLocked(shared, reservation, compressed_reserve);
        if (shared.terminal == TerminalState::Running)
        {
            shared.terminal = TerminalState::Failed;
            shared.terminal_result = MakeError(ZipStatus::IoError, std::move(message));
        }
        shared.completion.notify_all();
    }

    static void ReleaseTaskLocked(SharedState& shared,
                                  std::size_t reservation,
                                  std::size_t compressed_reserve)
    {
        if (shared.reserved_bytes >= reservation)
        {
            shared.reserved_bytes -= reservation;
        }
        if (shared.compressed_bytes >= compressed_reserve)
        {
            shared.compressed_bytes -= compressed_reserve;
        }
        if (shared.in_flight_chunks > 0)
        {
            --shared.in_flight_chunks;
        }
        if (shared.active_tasks > 0)
        {
            --shared.active_tasks;
        }
    }

    void ReleaseCompletedChunkLocked(std::size_t compressed_capacity,
                                     std::size_t retained_reservation)
    {
        if (shared_->compressed_bytes >= compressed_capacity)
        {
            shared_->compressed_bytes -= compressed_capacity;
        }
        if (shared_->reserved_bytes >= retained_reservation)
        {
            shared_->reserved_bytes -= retained_reservation;
        }
        if (shared_->in_flight_chunks > 0)
        {
            --shared_->in_flight_chunks;
        }
    }

    void ReleaseUnsubmitted(std::size_t reservation, std::size_t raw_size)
    {
        std::lock_guard lock(shared_->mutex);
        if (shared_->reserved_bytes >= reservation)
        {
            shared_->reserved_bytes -= reservation;
        }
        if (shared_->raw_bytes >= raw_size)
        {
            shared_->raw_bytes -= raw_size;
        }
        if (shared_->in_flight_chunks > 0)
        {
            --shared_->in_flight_chunks;
        }
    }

    void RejectTask(std::size_t reservation, std::size_t raw_size)
    {
        std::lock_guard lock(shared_->mutex);
        if (shared_->reserved_bytes >= reservation)
        {
            shared_->reserved_bytes -= reservation;
        }
        if (shared_->raw_bytes >= raw_size)
        {
            shared_->raw_bytes -= raw_size;
        }
        if (shared_->in_flight_chunks > 0)
        {
            --shared_->in_flight_chunks;
        }
        if (shared_->active_tasks > 0)
        {
            --shared_->active_tasks;
        }
        shared_->completion.notify_all();
    }

    ZipOperationResult FailUnsubmitted(std::size_t reservation,
                                       std::size_t raw_size,
                                       std::string message)
    {
        ReleaseUnsubmitted(reservation, raw_size);
        return Fail(MakeError(ZipStatus::IoError, std::move(message)));
    }

    ZipOperationResult Fail(ZipOperationResult result)
    {
        std::lock_guard lock(shared_->mutex);
        PublishTerminalLocked(TerminalState::Failed, result);
        return shared_->terminal_result;
    }

    ZipOperationResult Cancel()
    {
        std::lock_guard lock(shared_->mutex);
        PublishTerminalLocked(
            TerminalState::Cancelled,
            MakeCancelled("zip create cancelled"));
        return shared_->terminal_result;
    }

    ZipOperationResult FinishFailure(ZipOperationResult result)
    {
        {
            std::lock_guard lock(shared_->mutex);
            const auto terminal = result.status == ZipStatus::Cancelled
                ? TerminalState::Cancelled
                : TerminalState::Failed;
            PublishTerminalLocked(terminal, std::move(result));
        }
        std::unique_lock lock(shared_->mutex);
        shared_->completion.wait(lock, [this] { return shared_->active_tasks == 0; });
        const auto terminal_result = shared_->terminal_result;
        lock.unlock();
        PublishTestMetrics();
        return terminal_result;
    }

    void PublishTerminalLocked(TerminalState terminal, ZipOperationResult result)
    {
        if (shared_->terminal == TerminalState::Running)
        {
            shared_->terminal = terminal;
            shared_->terminal_result = std::move(result);
        }
        shared_->completion.notify_all();
    }

    void UpdateMaxPayloadLocked()
    {
        shared_->metrics.max_in_flight_payload_bytes = std::max(
            shared_->metrics.max_in_flight_payload_bytes,
            shared_->raw_bytes + shared_->compressed_bytes);
    }

    void TraceEntry(std::size_t entry_index) const
    {
        if (!ZipTimingTraceEnabled())
        {
            return;
        }
        const auto metrics = Metrics();
        const auto& entry = metrics.entries[entry_index];
        std::ostringstream stream;
        stream << "entry=\"" << entry.archive_path << "\""
               << " size=" << entry.raw_bytes
               << " read=" << entry.read_ns / 1000000u << "ms"
               << " deflate=" << entry.deflate_ns / 1000000u << "ms"
               << " write=" << entry.write_ns / 1000000u << "ms";
        EmitZipTimingTrace(stream.str());
    }

    void TraceSchedulerLocked()
    {
        if (!ZipTimingTraceEnabled())
        {
            return;
        }
        shared_->metrics.wall_ns = DurationNs(started_at_);
        std::ostringstream stream;
        stream << "chunk-scheduler wall=" << shared_->metrics.wall_ns / 1000000u << "ms"
               << " max_tasks=" << shared_->metrics.max_active_tasks
               << " max_chunks=" << shared_->metrics.max_in_flight_chunks
               << " max_raw=" << shared_->metrics.max_in_flight_raw_bytes
               << " max_compressed=" << shared_->metrics.max_in_flight_compressed_bytes
               << " max_payload=" << shared_->metrics.max_in_flight_payload_bytes
               << " max_reserved=" << shared_->metrics.max_reserved_bytes
               << " entries_before_first="
               << shared_->metrics.entries_started_before_first_completed;
        EmitZipTimingTrace(stream.str());
    }

    void PublishTestMetrics() const
    {
#if defined(COZIP_ENABLE_TEST_HOOKS)
        const auto metrics = Metrics();
        std::lock_guard lock(g_last_scheduler_metrics_mutex);
        g_last_scheduler_metrics = metrics;
#endif
    }

    std::size_t max_in_flight_chunks_;
    std::size_t foreground_target_ = 1;
    std::size_t memory_budget_bytes_;
    core::ExecutionContext context_;
    core::ITaskExecutor* executor_ = nullptr;
    std::chrono::steady_clock::time_point started_at_;
    std::shared_ptr<SharedState> shared_;
    std::vector<std::unique_ptr<EntryState>> entries_;
    std::size_t round_robin_cursor_ = 0;
    std::size_t next_output_entry_ = 0;
};

ChunkedCpuCreateScheduler::ChunkedCpuCreateScheduler(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl))
{
}

ChunkedCpuCreateScheduler::~ChunkedCpuCreateScheduler() = default;
ChunkedCpuCreateScheduler::ChunkedCpuCreateScheduler(ChunkedCpuCreateScheduler&&) noexcept = default;
ChunkedCpuCreateScheduler& ChunkedCpuCreateScheduler::operator=(
    ChunkedCpuCreateScheduler&&) noexcept = default;

bool ChunkedCpuCreateScheduler::Handles(const ZipEntrySource& entry) const noexcept
{
    return impl_ != nullptr && impl_->Handles(entry);
}

ZipOperationResult ChunkedCpuCreateScheduler::WriteEntry(
    std::ostream& output,
    ZipEntrySource& entry)
{
    return impl_ != nullptr
        ? impl_->WriteEntry(output, entry)
        : MakeError(ZipStatus::InvalidJob, "chunk scheduler is not initialized");
}

ChunkedCpuSchedulerMetrics ChunkedCpuCreateScheduler::Metrics() const
{
    return impl_ != nullptr ? impl_->Metrics() : ChunkedCpuSchedulerMetrics {};
}

ZipOperationResult CreateChunkedCpuScheduler(
    std::span<ZipEntrySource> entries,
    const pipeline::PipelineOptions& pipeline_options,
    std::size_t memory_budget_mb,
    const core::ExecutionContext& context,
    std::unique_ptr<ChunkedCpuCreateScheduler>& scheduler)
{
    scheduler.reset();
    try
    {
        auto impl = std::make_unique<ChunkedCpuCreateScheduler::Impl>(
            pipeline_options,
            memory_budget_mb,
            context);
        auto result = impl->Initialize(entries, pipeline_options.chunk_size_bytes);
        if (result.status != ZipStatus::Ok)
        {
            return result;
        }
        if (!impl->Empty())
        {
            scheduler = std::unique_ptr<ChunkedCpuCreateScheduler>(
                new ChunkedCpuCreateScheduler(std::move(impl)));
        }
        return {ZipStatus::Ok, {}};
    }
    catch (const std::bad_alloc&)
    {
        return MakeError(ZipStatus::IoError, "failed to allocate chunk scheduler");
    }
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

ChunkedCpuSchedulerMetrics LastChunkedCpuSchedulerMetricsForTesting()
{
    std::lock_guard lock(g_last_scheduler_metrics_mutex);
    return g_last_scheduler_metrics;
}
#endif
}
