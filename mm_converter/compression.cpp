#include "compression.h"

#include <algorithm>
#include <array>
#include <limits>

namespace
{
    class BitReader
    {
    public:
        BitReader(const uint8_t* data, size_t size) : data(data), size(size) {}

        bool read_bits(unsigned count, uint32_t& value)
        {
            if (count > 24) return false;
            while (bits < count) {
                if (pos >= size) return false;
                buffer |= uint64_t(data[pos++]) << bits;
                bits += 8;
            }
            value = uint32_t(buffer & ((uint64_t(1) << count) - 1));
            buffer >>= count;
            bits -= count;
            return true;
        }

        // Looks at the next count bits without consuming them. Missing bits
        // past the end of the input are returned as 0, same as a real deflate
        // stream would have after the final block (the Huffman table lookup
        // just needs something stable to index with).
        uint32_t peek_bits(unsigned count)
        {
            while (bits < count && pos < size) {
                buffer |= uint64_t(data[pos++]) << bits;
                bits += 8;
            }
            return uint32_t(buffer & ((uint64_t(1) << count) - 1));
        }

        void consume_bits(unsigned count)
        {
            buffer >>= count;
            bits -= count;
        }

        void align()
        {
            buffer >>= bits & 7u;
            bits &= ~7u;
        }

        size_t consumed_bytes() const { return pos - (bits / 8); }

    private:
        const uint8_t* data = nullptr;
        size_t size = 0;
        size_t pos = 0;
        uint64_t buffer = 0;
        unsigned bits = 0;
    };

    uint32_t reverse_bits(uint32_t value, unsigned count)
    {
        uint32_t result = 0;
        for (unsigned i = 0; i < count; ++i) {
            result = (result << 1) | (value & 1u);
            value >>= 1;
        }
        return result;
    }

    struct HuffmanCode
    {
        uint32_t code = 0;
        uint16_t symbol = 0;
        uint8_t length = 0;
    };

    // Max code length allowed by deflate is 15 bits, so a direct 2^15 entry
    // table gives O(1) decode: index by the next 15 raw bits and the table
    // already knows how many of them actually belong to the code.
    constexpr unsigned kMaxCodeLength = 15;
    constexpr size_t kTableSize = size_t(1) << kMaxCodeLength;

    class HuffmanTree
    {
    public:
        bool build(const std::vector<uint8_t>& lengths)
        {
            std::array<unsigned, 16> count{};
            unsigned max_length = 0;

            for (size_t symbol = 0; symbol < lengths.size(); ++symbol) {
                const unsigned length = lengths[symbol];
                if (length > 15) return false;
                if (length != 0) {
                    ++count[length];
                    max_length = std::max(max_length, length);
                }
            }

            if (max_length == 0) return false;

            std::array<unsigned, 16> next{};
            unsigned code = 0;
            for (unsigned length = 1; length <= 15; ++length) {
                code = (code + count[length - 1]) << 1;
                next[length] = code;
            }

            table.assign(kTableSize, TableEntry{});

            for (size_t symbol = 0; symbol < lengths.size(); ++symbol) {
                const unsigned length = lengths[symbol];
                if (!length) continue;
                const uint32_t reversed = reverse_bits(next[length]++, length);

                // The reversed code only pins down the low `length` bits of the
                // table index. Every table slot whose low bits match gets this
                // entry, since the remaining high bits are unread lookahead.
                const uint32_t step = uint32_t(1) << length;
                for (uint32_t index = reversed; index < kTableSize; index += step)
                    table[index] = TableEntry{ uint16_t(symbol), uint8_t(length) };
            }

            return true;
        }

        bool decode(BitReader& reader, uint16_t& symbol) const
        {
            const uint32_t index = reader.peek_bits(kMaxCodeLength);
            const TableEntry& entry = table[index];
            if (entry.length == 0) return false;
            reader.consume_bits(entry.length);
            symbol = entry.symbol;
            return true;
        }

    private:
        struct TableEntry
        {
            uint16_t symbol = 0;
            uint8_t length = 0; // 0 means "no code has this prefix"
        };

        std::vector<TableEntry> table;
    };

    static const int length_base[] = {
        3, 4, 5, 6, 7, 8, 9, 10,
        11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115,
        131, 163, 195, 227, 258
    };

    static const int length_extra[] = {
        0, 0, 0, 0, 0, 0, 0, 0,
        1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4,
        5, 5, 5, 5, 0
    };

    static const int distance_base[] = {
        1, 2, 3, 4, 5, 7, 9, 13,
        17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073,
        4097, 6145, 8193, 12289, 16385, 24577
    };

    static const int distance_extra[] = {
        0, 0, 0, 0, 1, 1, 2, 2,
        3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10,
        11, 11, 12, 12, 13, 13
    };

    bool build_fixed_trees(HuffmanTree& litlen, HuffmanTree& distance)
    {
        std::vector<uint8_t> lengths(288, 0);
        for (int i = 0; i <= 143; ++i) lengths[i] = 8;
        for (int i = 144; i <= 255; ++i) lengths[i] = 9;
        for (int i = 256; i <= 279; ++i) lengths[i] = 7;
        for (int i = 280; i <= 287; ++i) lengths[i] = 8;
        if (!litlen.build(lengths)) return false;
        return distance.build(std::vector<uint8_t>(32, 5));
    }

    bool build_dynamic_trees(BitReader& reader, HuffmanTree& litlen, HuffmanTree& distance)
    {
        uint32_t hlit = 0, hdist = 0, hclen = 0;
        if (!reader.read_bits(5, hlit) || !reader.read_bits(5, hdist) || !reader.read_bits(4, hclen)) return false;
        hlit += 257;
        hdist += 1;
        hclen += 4;
        if (hlit > 286 || hdist > 32) return false;

        static const int order[] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
        std::vector<uint8_t> code_lengths(19, 0);
        for (uint32_t i = 0; i < hclen; ++i) {
            uint32_t value = 0;
            if (!reader.read_bits(3, value)) return false;
            code_lengths[order[i]] = uint8_t(value);
        }

        HuffmanTree code_tree;
        if (!code_tree.build(code_lengths)) return false;

        std::vector<uint8_t> lengths;
        lengths.reserve(hlit + hdist);
        while (lengths.size() < hlit + hdist) {
            uint16_t symbol = 0;
            if (!code_tree.decode(reader, symbol)) return false;
            if (symbol <= 15) {
                lengths.push_back(uint8_t(symbol));
            }
            else if (symbol == 16) {
                if (lengths.empty()) return false;
                uint32_t extra = 0;
                if (!reader.read_bits(2, extra)) return false;
                const size_t count = size_t(extra) + 3;
                if (lengths.size() + count > hlit + hdist) return false;
                const uint8_t previous = lengths.back();
                lengths.insert(lengths.end(), count, previous);
            }
            else if (symbol == 17) {
                uint32_t extra = 0;
                if (!reader.read_bits(3, extra)) return false;
                const size_t count = size_t(extra) + 3;
                if (lengths.size() + count > hlit + hdist) return false;
                lengths.insert(lengths.end(), count, 0);
            }
            else if (symbol == 18) {
                uint32_t extra = 0;
                if (!reader.read_bits(7, extra)) return false;
                const size_t count = size_t(extra) + 11;
                if (lengths.size() + count > hlit + hdist) return false;
                lengths.insert(lengths.end(), count, 0);
            }
            else {
                return false;
            }
        }

        std::vector<uint8_t> lit_lengths(lengths.begin(), lengths.begin() + hlit);
        std::vector<uint8_t> dist_lengths(lengths.begin() + hlit, lengths.end());
        if (!litlen.build(lit_lengths)) return false;

        bool any_distance = false;
        for (uint8_t length : dist_lengths) any_distance |= length != 0;
        if (!any_distance)
            return distance.build(std::vector<uint8_t>{1});
        return distance.build(dist_lengths);
    }

    uint32_t adler32(const uint8_t* data, size_t size)
    {
        uint32_t a = 1;
        uint32_t b = 0;
        for (size_t pos = 0; pos < size;) {
            const size_t count = std::min<size_t>(size - pos, 5552);
            for (size_t i = 0; i < count; ++i) {
                a += data[pos + i];
                if (a >= 65521) a -= 65521;
                b += a;
                if (b >= 65521) b -= 65521;
            }
            pos += count;
        }
        return (b << 16) | a;
    }

    uint32_t crc32(const uint8_t* data, size_t size)
    {
        static uint32_t table[256] = {};
        static bool initialized = false;
        if (!initialized) {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t value = i;
                for (int j = 0; j < 8; ++j)
                    value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
                table[i] = value;
            }
            initialized = true;
        }

        uint32_t result = 0xFFFFFFFFu;
        for (size_t i = 0; i < size; ++i)
            result = table[(result ^ data[i]) & 0xFFu] ^ (result >> 8);
        return ~result;
    }

    bool inflate_raw(const uint8_t* input, size_t input_size, std::vector<uint8_t>& output, size_t& consumed)
    {
        BitReader reader(input, input_size);
        bool final_block = false;
        output.clear();

        while (!final_block) {
            uint32_t final = 0, type = 0;
            if (!reader.read_bits(1, final) || !reader.read_bits(2, type)) return false;
            final_block = final != 0;

            if (type == 0) {
                reader.align();
                uint32_t len = 0, nlen = 0;
                if (!reader.read_bits(16, len) || !reader.read_bits(16, nlen)) return false;
                if ((len ^ 0xFFFFu) != nlen) return false;
                if (len > input_size || output.size() > std::numeric_limits<size_t>::max() - len) return false;
                for (uint32_t i = 0; i < len; ++i) {
                    uint32_t value = 0;
                    if (!reader.read_bits(8, value)) return false;
                    output.push_back(uint8_t(value));
                }
                continue;
            }

            if (type == 3) return false;

            HuffmanTree litlen, distance;
            if (type == 1) {
                if (!build_fixed_trees(litlen, distance)) return false;
            }
            else if (!build_dynamic_trees(reader, litlen, distance)) {
                return false;
            }

            for (;;) {
                uint16_t symbol = 0;
                if (!litlen.decode(reader, symbol)) return false;
                if (symbol < 256) {
                    output.push_back(uint8_t(symbol));
                    continue;
                }
                if (symbol == 256) break;
                if (symbol < 257 || symbol > 285) return false;

                const int length_index = int(symbol) - 257;
                uint32_t extra = 0;
                if (!reader.read_bits(unsigned(length_extra[length_index]), extra)) return false;
                const size_t length = size_t(length_base[length_index]) + extra;

                uint16_t distance_symbol = 0;
                if (!distance.decode(reader, distance_symbol) || distance_symbol >= 30) return false;
                if (!reader.read_bits(unsigned(distance_extra[distance_symbol]), extra)) return false;
                const size_t distance_value = size_t(distance_base[distance_symbol]) + extra;
                if (distance_value == 0 || distance_value > output.size()) return false;
                if (output.size() > std::numeric_limits<size_t>::max() - length) return false;

                const size_t start = output.size() - distance_value;
                for (size_t i = 0; i < length; ++i)
                    output.push_back(output[start + i % distance_value]);
            }
        }

        consumed = reader.consumed_bytes();
        return true;
    }

    void write_bits(std::vector<uint8_t>& output, uint32_t& buffer, unsigned& bits,
        uint32_t value, unsigned count)
    {
        buffer |= value << bits;
        bits += count;
        while (bits >= 8) {
            output.push_back(uint8_t(buffer & 0xFFu));
            buffer >>= 8;
            bits -= 8;
        }
    }

    void align_writer(std::vector<uint8_t>& output, uint32_t& buffer, unsigned& bits)
    {
        if (bits) {
            output.push_back(uint8_t(buffer & 0xFFu));
            buffer = 0;
            bits = 0;
        }
    }
}

bool decompress_data(const uint8_t* input, size_t input_size,
    std::vector<uint8_t>& output, size_t expected_size)
{
    if (!input || input_size < 2) return false;

    size_t raw_offset = 0;
    bool gzip = false;

    if (input[0] == 0x1F && input[1] == 0x8B) {
        if (input_size < 18 || input[2] != 8) return false;
        gzip = true;
        size_t pos = 10;
        const uint8_t flags = input[3];
        if (flags & 0x04) {
            if (pos + 2 > input_size) return false;
            const size_t length = input[pos] | (size_t(input[pos + 1]) << 8);
            pos += 2;
            if (pos + length > input_size) return false;
            pos += length;
        }
        if (flags & 0x08) {
            while (pos < input_size && input[pos]) ++pos;
            if (pos >= input_size) return false;
            ++pos;
        }
        if (flags & 0x10) {
            while (pos < input_size && input[pos]) ++pos;
            if (pos >= input_size) return false;
            ++pos;
        }
        if (flags & 0x02) {
            if (pos + 2 > input_size) return false;
            pos += 2;
        }
        raw_offset = pos;
    }
    else {
        const uint16_t header = (uint16_t(input[0]) << 8) | uint16_t(input[1]);
        if ((input[0] & 0x0F) != 8 || (header % 31) != 0 || (input[1] & 0x20)) return false;
        raw_offset = 2;
    }

    if (raw_offset >= input_size) return false;

    size_t consumed = 0;
    if (!inflate_raw(input + raw_offset, input_size - raw_offset, output, consumed)) return false;
    if (expected_size && output.size() != expected_size) return false;

    const size_t trailer = raw_offset + consumed;
    if (gzip) {
        if (trailer + 8 > input_size) return false;
        const uint32_t expected_crc = uint32_t(input[trailer]) |
            (uint32_t(input[trailer + 1]) << 8) |
            (uint32_t(input[trailer + 2]) << 16) |
            (uint32_t(input[trailer + 3]) << 24);
        const uint32_t expected_size32 = uint32_t(input[trailer + 4]) |
            (uint32_t(input[trailer + 5]) << 8) |
            (uint32_t(input[trailer + 6]) << 16) |
            (uint32_t(input[trailer + 7]) << 24);
        if (crc32(output.data(), output.size()) != expected_crc || uint32_t(output.size()) != expected_size32)
            return false;
    }
    else {
        if (trailer + 4 > input_size) return false;
        const uint32_t expected_adler = (uint32_t(input[trailer]) << 24) |
            (uint32_t(input[trailer + 1]) << 16) |
            (uint32_t(input[trailer + 2]) << 8) |
            uint32_t(input[trailer + 3]);
        if (adler32(output.data(), output.size()) != expected_adler) return false;
    }

    return true;
}

namespace
{
    struct Match
    {
        int length = 0;
        int distance = 0;
    };

    uint32_t hash3(const uint8_t* p)
    {
        return ((uint32_t(p[0]) * 251u) ^ (uint32_t(p[1]) * 911u) ^ uint32_t(p[2])) & 65535u;
    }

    void fixed_code(unsigned symbol, uint32_t& code, unsigned& bits)
    {
        if (symbol <= 143) {
            code = reverse_bits(0x30u + symbol, 8);
            bits = 8;
        }
        else if (symbol <= 255) {
            code = reverse_bits(0x190u + (symbol - 144), 9);
            bits = 9;
        }
        else if (symbol <= 279) {
            code = reverse_bits(symbol - 256, 7);
            bits = 7;
        }
        else {
            code = reverse_bits(0xC0u + (symbol - 280), 8);
            bits = 8;
        }
    }

    void fixed_distance_code(unsigned symbol, uint32_t& code, unsigned& bits)
    {
        code = reverse_bits(symbol, 5);
        bits = 5;
    }

    void emit_fixed(std::vector<uint8_t>& output, uint32_t& buffer, unsigned& bits, unsigned symbol)
    {
        uint32_t code;
        unsigned count;
        fixed_code(symbol, code, count);
        write_bits(output, buffer, bits, code, count);
    }

    void emit_distance(std::vector<uint8_t>& output, uint32_t& buffer, unsigned& bits, unsigned symbol)
    {
        uint32_t code;
        unsigned count;
        fixed_distance_code(symbol, code, count);
        write_bits(output, buffer, bits, code, count);
    }

    Match find_match(const uint8_t* input, size_t size, size_t pos,
        std::vector<int>& head, std::vector<int>& previous)
    {
        Match best;
        if (pos + 2 >= size)
            return best;

        const uint32_t hash = hash3(input + pos);
        int candidate = head[hash];
        int attempts = 0;
        const size_t max_length = std::min<size_t>(258, size - pos);

        while (candidate >= 0 && attempts++ < 192) {
            const size_t candidate_pos = size_t(candidate);
            if (pos <= candidate_pos || pos - candidate_pos > 32768)
                break;

            // A candidate can only beat the current best if it still matches at
            // the position the current best already reached, so check that byte
            // first instead of rescanning from zero on every candidate.
            if (best.length == 0) {
                // no shortcut possible yet, always do the full scan below
            }
            else if (size_t(best.length) >= max_length) {
                // best already reached the longest length possible at this
                // position, nothing can beat it
                candidate = previous[candidate_pos];
                continue;
            }
            else if (input[candidate_pos + best.length] != input[pos + best.length]) {
                candidate = previous[candidate_pos];
                continue;
            }

            {
                size_t length = 0;
                while (length < max_length && input[candidate_pos + length] == input[pos + length])
                    ++length;

                if (length >= 3 && int(length) > best.length) {
                    best.length = int(length);
                    best.distance = int(pos - candidate_pos);
                    if (length == 258)
                        break;
                }
            }

            candidate = previous[candidate_pos];
        }

        return best;
    }

    void length_code(int length, unsigned& symbol, unsigned& extra, unsigned& extra_bits)
    {
        static const int base[] = {
            3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
        };
        static const int extra_count[] = {
            0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
        };

        for (unsigned i = 0; i < 29; ++i) {
            const int end = base[i] + ((1 << extra_count[i]) - 1);
            if (length <= end || i == 28) {
                symbol = 257 + i;
                extra_bits = unsigned(extra_count[i]);
                extra = unsigned(length - base[i]);
                return;
            }
        }
    }

    void distance_code(int distance, unsigned& symbol, unsigned& extra, unsigned& extra_bits)
    {
        static const int base[] = {
            1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
        };
        static const int extra_count[] = {
            0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
        };

        for (unsigned i = 0; i < 30; ++i) {
            const int end = base[i] + ((1 << extra_count[i]) - 1);
            if (distance <= end || i == 29) {
                symbol = i;
                extra_bits = unsigned(extra_count[i]);
                extra = unsigned(distance - base[i]);
                return;
            }
        }
    }
}

bool compress_data(const uint8_t* input, size_t input_size,
    std::vector<uint8_t>& output)
{
    if (!input && input_size != 0) return false;
    if (input_size > std::numeric_limits<uint32_t>::max()) return false;

    output.clear();
    output.reserve(input_size / 2 + 64);

    // CMF/FLG: DEFLATE, 32 KiB window, fastest legal preset dictionary setting.
    output.push_back(0x78);
    output.push_back(0x01);

    uint32_t bit_buffer = 0;
    unsigned bit_count = 0;

    // A single stored block is smaller than the matcher for tiny inputs.
    if (input_size < 32) {
        write_bits(output, bit_buffer, bit_count, 1u, 1);
        write_bits(output, bit_buffer, bit_count, 0u, 2);
        align_writer(output, bit_buffer, bit_count);
        const uint16_t length = uint16_t(input_size);
        const uint16_t inverse = uint16_t(~length);
        output.push_back(uint8_t(length));
        output.push_back(uint8_t(length >> 8));
        output.push_back(uint8_t(inverse));
        output.push_back(uint8_t(inverse >> 8));
        output.insert(output.end(), input, input + input_size);
    }
    else {
        // Huffman DEFLATE with a bounded hash-chain LZ77 matcher.
        std::vector<int> head(65536, -1);
        std::vector<int> previous(input_size, -1);

        write_bits(output, bit_buffer, bit_count, 1u, 1);
        write_bits(output, bit_buffer, bit_count, 1u, 2);

        size_t pos = 0;
        while (pos < input_size) {
            Match match = find_match(input, input_size, pos, head, previous);

            // A small amount of lazy matching avoids taking a short match
            // when the next byte begins a substantially better one. Once a
            // match is already long there is nothing left to gain, so skip
            // the extra chain walk in that case.
            if (match.length >= 3 && match.length < 32 && pos + 1 < input_size) {
                Match next = find_match(input, input_size, pos + 1, head, previous);
                if (next.length > match.length + 1)
                    match = Match{};
            }

            if (match.length >= 3) {
                unsigned symbol, extra, extra_bits;
                length_code(match.length, symbol, extra, extra_bits);
                emit_fixed(output, bit_buffer, bit_count, symbol);
                if (extra_bits)
                    write_bits(output, bit_buffer, bit_count, extra, extra_bits);

                unsigned distance_symbol, distance_extra, distance_bits;
                distance_code(match.distance, distance_symbol, distance_extra, distance_bits);
                emit_distance(output, bit_buffer, bit_count, distance_symbol);
                if (distance_bits)
                    write_bits(output, bit_buffer, bit_count, distance_extra, distance_bits);

                const size_t end = pos + size_t(match.length);
                while (pos < end) {
                    if (pos + 2 < input_size) {
                        const uint32_t hash = hash3(input + pos);
                        previous[pos] = head[hash];
                        head[hash] = int(pos);
                    }
                    ++pos;
                }
            }
            else {
                emit_fixed(output, bit_buffer, bit_count, input[pos]);
                if (pos + 2 < input_size) {
                    const uint32_t hash = hash3(input + pos);
                    previous[pos] = head[hash];
                    head[hash] = int(pos);
                }
                ++pos;
            }
        }

        emit_fixed(output, bit_buffer, bit_count, 256);
        align_writer(output, bit_buffer, bit_count);
    }

    const uint32_t checksum = adler32(input, input_size);
    output.push_back(uint8_t(checksum >> 24));
    output.push_back(uint8_t(checksum >> 16));
    output.push_back(uint8_t(checksum >> 8));
    output.push_back(uint8_t(checksum));
    return true;
}