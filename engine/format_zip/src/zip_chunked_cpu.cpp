#include "zip_chunked_cpu.h"

namespace cozip::format_zip
{
namespace
{
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
                                          const pipeline::PipelineOptions& pipeline_options,
                                          std::size_t chunk_size)
{
    ScopedZipEntryTimer timer(entry);
    WholeFileInput input;
    const auto load_started_at = std::chrono::steady_clock::now();
    auto load_result = LoadWholeFileInput(
        *entry.storage_factory,
        entry.source_path,
        entry.mapping_mode,
        entry.source_reader,
        input);
    if (load_result.status != ZipStatus::Ok)
    {
        return load_result;
    }
    timer.AddPhase("load", std::chrono::steady_clock::now() - load_started_at);

    const auto chunk_count = input.bytes.empty() ? 0 :
        (input.bytes.size() + chunk_size - 1) / chunk_size;
    std::vector<ChunkedCompressedChunk> chunks(chunk_count);
    for (std::size_t index = 0; index < chunk_count; ++index)
    {
        chunks[index].index = index;
    }

    const auto deflate_started_at = std::chrono::steady_clock::now();
    const auto configured_workers = std::max<std::size_t>(1, pipeline_options.compressor_threads);
    const auto worker_count = std::max<std::size_t>(
        1,
        std::min<std::size_t>(configured_workers, std::max<std::size_t>(chunk_count, 1)));
    std::atomic<std::size_t> next_index = 0;
    std::mutex error_mutex;
    bool failed = false;
    std::string failure_message;
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    for (std::size_t worker = 0; worker < worker_count; ++worker)
    {
        workers.emplace_back([&] {
            while (true)
            {
                const auto index = next_index.fetch_add(1);
                if (index >= chunk_count || failed)
                {
                    return;
                }

                const auto offset = index * chunk_size;
                const auto size = std::min<std::size_t>(chunk_size, input.bytes.size() - offset);
                const auto chunk_bytes = input.bytes.subspan(offset, size);
                auto compressed =
                    codecs::CompressDeflateBuffer(chunk_bytes, entry.compression_profile);
                if (!compressed.success)
                {
                    std::lock_guard lock(error_mutex);
                    failed = true;
                    failure_message = compressed.error_message;
                    return;
                }

                DeflateStreamLayout layout {};
                if (!ParseDeflateStreamLayout(compressed.bytes, layout))
                {
                    std::lock_guard lock(error_mutex);
                    failed = true;
                    failure_message = "failed to parse deflate chunk layout";
                    return;
                }

                chunks[index].raw_size = size;
                chunks[index].compressed = std::move(compressed.bytes);
                chunks[index].layout = layout;
                if (index + 1 < chunk_count)
                {
                    if (!PrepareChunkForNonFinalStream(chunks[index]))
                    {
                        std::lock_guard lock(error_mutex);
                        failed = true;
                        failure_message = "failed to prepare non-final chunk stream";
                        return;
                    }
                }
            }
        });
    }

    for (auto& worker : workers)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }

    if (failed)
    {
        return MakeError(ZipStatus::IoError, failure_message);
    }
    timer.AddPhase("deflate", std::chrono::steady_clock::now() - deflate_started_at);

    const auto crc_started_at = std::chrono::steady_clock::now();
    Crc32 crc;
    if (!input.bytes.empty())
    {
        crc.Update(input.bytes.data(), input.bytes.size());
    }
    entry.crc32 = crc.Finalize();
    timer.AddPhase("crc", std::chrono::steady_clock::now() - crc_started_at);

    const auto write_started_at = std::chrono::steady_clock::now();
    DeflateBitWriter bit_writer(output);
    for (std::size_t index = 0; index < chunks.size(); ++index)
    {
        const auto is_final = index + 1 == chunks.size();
        const auto ok = is_final
            ? WriteChunkBitsWithFinalOverride(
                  bit_writer,
                  chunks[index].compressed,
                  chunks[index].layout,
                  1u)
            : bit_writer.WriteBitsFromSlice(
                  chunks[index].compressed,
                  0,
                  chunks[index].layout.end_bit);
        if (!ok)
        {
            return MakeError(ZipStatus::IoError, "failed to assemble chunked deflate stream");
        }
    }
    if (!bit_writer.Finish())
    {
        return MakeError(ZipStatus::IoError, "failed to flush assembled deflate stream");
    }
    timer.AddPhase("write", std::chrono::steady_clock::now() - write_started_at);

    entry.size = static_cast<std::uint32_t>(input.bytes.size());
    entry.compressed_size = static_cast<std::uint32_t>(bit_writer.TotalBytesWritten());
    entry.general_purpose_flag = kDataDescriptorFlag;
    timer.Finish();
    return {ZipStatus::Ok, {}};
}
}
