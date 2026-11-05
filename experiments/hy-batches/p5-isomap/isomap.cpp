#include "isomap.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <zlib.h>

namespace isomap {
namespace {

constexpr std::size_t max_file_bytes = 64 * 1024 * 1024;
constexpr std::size_t max_raw_bytes = 128 * 1024 * 1024;
constexpr std::size_t max_cells = 4 * 1024 * 1024;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::uint16_t u16(const Bytes& bytes, std::size_t pos) {
    if (pos > bytes.size() || bytes.size() - pos < 2) fail("truncated u16");
    return static_cast<std::uint16_t>(bytes[pos] | (bytes[pos + 1] << 8));
}

std::uint32_t u32(const Bytes& bytes, std::size_t pos) {
    if (pos > bytes.size() || bytes.size() - pos < 4) fail("truncated u32");
    return static_cast<std::uint32_t>(bytes[pos]) |
           (static_cast<std::uint32_t>(bytes[pos + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[pos + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[pos + 3]) << 24);
}

class BlockReader {
public:
    const Bytes& input;
    Bytes output;
    std::size_t pos = 0;
    std::size_t limit;

    BlockReader(const Bytes& bytes, std::size_t expected) : input(bytes), limit(expected) {
        if (expected > 65535) fail("block output exceeds u16 limit");
        output.reserve(expected);
    }

    unsigned byte() {
        if (pos == input.size()) fail("truncated compressed instruction");
        return input[pos++];
    }

    unsigned word() {
        const unsigned lo = byte();
        return lo | (byte() << 8);
    }

    void room(std::size_t count) const {
        if (count > limit - output.size()) fail("compressed block output overflow");
    }

    void literal(std::size_t count) {
        if (count > input.size() - pos) fail("truncated literal run");
        room(count);
        output.insert(output.end(), input.begin() + static_cast<std::ptrdiff_t>(pos),
                      input.begin() + static_cast<std::ptrdiff_t>(pos + count));
        pos += count;
    }

    void reference(std::size_t distance, std::size_t count) {
        if (!distance || distance > output.size()) fail("invalid compressed back-reference");
        room(count);
        // Bytewise copying intentionally permits overlapping matches.
        for (std::size_t i = 0; i < count; ++i) output.push_back(output[output.size() - distance]);
    }

    std::size_t extension(std::size_t base) {
        std::size_t count = base;
        unsigned b = byte();
        while (b == 0) {
            if (count > limit || limit - count < 255) fail("compressed run length overflow");
            count += 255;
            b = byte();
        }
        if (count > limit || b > limit - count) fail("compressed run length overflow");
        return count + b;
    }

    Bytes finish() {
        if (pos != input.size()) fail("trailing bytes after compressed end marker");
        if (output.size() != limit) fail("compressed block output length mismatch: got " +
            std::to_string(output.size()) + ", expected " + std::to_string(limit));
        return std::move(output);
    }
};

std::string trim(std::string value) {
    const auto whitespace = [](unsigned char c) { return std::isspace(c) != 0; };
    auto first = std::find_if_not(value.begin(), value.end(), whitespace);
    auto last = std::find_if_not(value.rbegin(), value.rend(), whitespace).base();
    return first < last ? std::string(first, last) : std::string();
}

std::string lower(std::string value) {
    for (char& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return value;
}

int integer(const std::string& text) {
    const std::string value = trim(text);
    int result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc() || parsed.ptr != value.data() + value.size()) {
        fail("invalid integer: " + value.substr(0, 40));
    }
    return result;
}

void append_utf8(std::string& text, std::uint32_t code) {
    if (code <= 0x7f) text.push_back(static_cast<char>(code));
    else if (code <= 0x7ff) {
        text.push_back(static_cast<char>(0xc0 | (code >> 6)));
        text.push_back(static_cast<char>(0x80 | (code & 63)));
    } else if (code <= 0xffff) {
        text.push_back(static_cast<char>(0xe0 | (code >> 12)));
        text.push_back(static_cast<char>(0x80 | ((code >> 6) & 63)));
        text.push_back(static_cast<char>(0x80 | (code & 63)));
    } else {
        text.push_back(static_cast<char>(0xf0 | (code >> 18)));
        text.push_back(static_cast<char>(0x80 | ((code >> 12) & 63)));
        text.push_back(static_cast<char>(0x80 | ((code >> 6) & 63)));
        text.push_back(static_cast<char>(0x80 | (code & 63)));
    }
}

std::string ini_text(const Bytes& input, Map& map) {
    if (input.empty()) fail("empty input file");
    if (input.size() > max_file_bytes) fail("input file exceeds 64 MiB limit");
    std::size_t offset = 0;
    if (input.size() >= 3 && input[0] == 0xef && input[1] == 0xbb && input[2] == 0xbf) {
        offset = 3;
        map.encoding = "UTF-8 BOM";
    } else if (input.size() >= 2 && ((input[0] == 0xff && input[1] == 0xfe) ||
                                   (input[0] == 0xfe && input[1] == 0xff))) {
        const bool little = input[0] == 0xff;
        // Some extracted maps have FF FE followed by ordinary 8-bit INI, not UTF-16.
        // Only recover this case when a substantial prefix has NO NUL bytes and
        // is overwhelmingly ASCII; never reinterpret broken real UTF-16 as ASCII.
        const std::size_t sample_end = std::min<std::size_t>(input.size(), 514);
        std::size_t ascii = 0;
        bool nul = false;
        for (std::size_t i = 2; i < sample_end; ++i) {
            const unsigned b = input[i];
            nul = nul || b == 0;
            if ((b >= 32 && b <= 126) || b == 9 || b == 10 || b == 13) ++ascii;
        }
        if (sample_end >= 34 && !nul && ascii * 100 >= (sample_end - 2) * 90) {
            map.encoding = "8-bit INI with stray UTF-16 BOM";
            map.warnings.push_back("BOM is followed by 8-bit text; stripped misleading BOM");
            offset = 2;
        } else {
            map.encoding = little ? "UTF-16LE" : "UTF-16BE";
            if ((input.size() - 2) % 2) fail("truncated UTF-16 code unit (odd byte count)");
            const auto unit = [&](std::size_t pos) -> std::uint16_t {
                return little ? u16(input, pos) :
                    static_cast<std::uint16_t>((input[pos] << 8) | input[pos + 1]);
            };
            std::string text;
            for (std::size_t i = 2; i < input.size(); i += 2) {
                std::uint32_t code = unit(i);
                if (code >= 0xd800 && code <= 0xdbff) {
                    if (i + 3 >= input.size()) fail("truncated UTF-16 surrogate pair");
                    const std::uint32_t low = unit(i + 2);
                    if (low < 0xdc00 || low > 0xdfff) fail("invalid UTF-16 surrogate pair");
                    code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                    i += 2;
                } else if (code >= 0xdc00 && code <= 0xdfff) fail("unpaired UTF-16 low surrogate");
                if (code == 0) fail("NUL in UTF-16 INI");
                append_utf8(text, code);
            }
            return text;
        }
    } else map.encoding = "8-bit INI (ASCII/UTF-8/ANSI metadata)";
    std::string text(input.begin() + static_cast<std::ptrdiff_t>(offset), input.end());
    if (text.find('\0') != std::string::npos) fail("NUL in 8-bit INI (UTF-16 requires BOM)");
    return text;
}

std::string parse_ini(const std::string& text, Map& map) {
    std::string normalized = text;
    std::replace(normalized.begin(), normalized.end(), '\r', '\n');
    std::istringstream stream(normalized);
    std::string line, section, header_size;
    std::map<int, std::string> parts;
    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            const auto close = line.find(']');
            section = close == std::string::npos ? "" : lower(trim(line.substr(1, close - 1)));
            if (section == "isomappack5") map.has_isomap = true;
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = lower(trim(line.substr(0, equals)));
        std::string value = trim(line.substr(equals + 1));
        const auto comment = value.find(';');
        if (comment != std::string::npos) value = trim(value.substr(0, comment));
        if (section == "isomappack5") {
            const int number = integer(key);
            if (number <= 0) fail("IsoMapPack5 key must be positive");
            if (!parts.emplace(number, value).second) fail("duplicate IsoMapPack5 line " + key);
        } else if (section == "map") {
            if (key == "size") map.map_size = value;
            else if (key == "theater") map.theater = value;
        } else if (section == "header" && key == "size") header_size = value;
        else if (section == "preview" && key == "size") map.preview_size = value;
    }
    if (!map.has_isomap) fail("no [IsoMapPack5] section");
    if (parts.empty()) fail("empty [IsoMapPack5] section");
    if (map.map_size.empty() && !header_size.empty()) {
        map.map_size = header_size;
        map.warnings.push_back("[Map] Size missing; used [Header] Size");
    }
    if (map.map_size.empty()) fail("missing [Map] Size");
    std::array<int, 4> size{};
    std::istringstream size_stream(map.map_size);
    for (int i = 0; i < 4; ++i) {
        std::string field;
        if (!std::getline(size_stream, field, ',')) fail("Size must have four integer fields");
        size[static_cast<std::size_t>(i)] = integer(field);
    }
    std::string extra;
    if (std::getline(size_stream, extra, ',')) fail("extra Size fields");
    if (size[0] != 0 || size[1] != 0) fail("nonzero Map Size origin is unsupported");
    if (size[2] <= 0 || size[3] <= 0 || size[2] > 4096 || size[3] > 4096) {
        fail("invalid Map Size dimensions (allowed 1..4096)");
    }
    map.map_width = size[2];
    map.map_height = size[3];
    map.width = 2 * size[2] - 1;
    map.height = size[3];
    map.expected_cells = static_cast<std::size_t>(map.width) * map.height;
    if (map.expected_cells > max_cells) fail("map exceeds cell safety limit");
    if (map.theater.empty()) fail("missing [Map] Theater");
    for (char& c : map.theater) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A'));
        if (!(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') && c != '_') {
            fail("invalid Theater name");
        }
    }
    std::string base64;
    int expected_key = 1;
    for (const auto& part : parts) {
        if (part.first != expected_key++) fail("missing IsoMapPack5 line before " + std::to_string(part.first));
        base64 += part.second;
    }
    return base64;
}

Bytes unpack_stream(const Bytes& input, bool input_first, bool lzo, std::size_t& blocks) {
    Bytes output;
    std::size_t pos = 0;
    blocks = 0;
    while (pos < input.size()) {
        if (input.size() - pos < 4) fail("truncated chunk header at byte " + std::to_string(pos));
        const auto a = u16(input, pos);
        const auto b = u16(input, pos + 2);
        pos += 4;
        const std::size_t compressed = input_first ? a : b;
        const std::size_t expanded = input_first ? b : a;
        if (!compressed) {
            if (expanded != 0 || pos != input.size()) fail("invalid chunk terminator/trailing bytes");
            break;
        }
        if (!expanded) fail("zero chunk output length");
        if (compressed > input.size() - pos) fail("truncated chunk payload at block " + std::to_string(blocks));
        if (expanded > max_raw_bytes - output.size()) fail("unpacked stream exceeds 128 MiB limit");
        const Bytes chunk(input.begin() + static_cast<std::ptrdiff_t>(pos),
                          input.begin() + static_cast<std::ptrdiff_t>(pos + compressed));
        Bytes decoded;
        try {
            decoded = lzo ? decode_lzo1x(chunk, expanded) : decode_lcw(chunk, expanded);
        } catch (const std::exception& e) {
            fail("block " + std::to_string(blocks) + ": " + e.what());
        }
        output.insert(output.end(), decoded.begin(), decoded.end());
        pos += compressed;
        ++blocks;
    }
    if (!blocks) fail("empty chunk stream");
    return output;
}

void unpack(Map& map) {
    // Real RA2 IsoMapPack5 uses Format5/LZO1X, input-length then output-length.
    // Format80 and the reversed header in TASK.md are supported independently.
    std::string failures;
    for (int mode = 0; mode < 4; ++mode) {
        const bool input_first = mode < 2;
        const bool lzo = mode % 2 == 0;
        const std::string name = std::string(lzo ? "LZO1X" : "LCW/Format80") +
            (input_first ? " input,output" : " output,input");
        try {
            std::size_t blocks = 0;
            auto raw = unpack_stream(map.packed, input_first, lzo, blocks);
            map.raw = std::move(raw);
            map.blocks = blocks;
            map.codec = lzo ? "LZO1X" : "LCW/Format80";
            map.chunk_header = input_first ? "u16 input_length, u16 output_length" :
                                            "u16 output_length, u16 input_length";
            return;
        } catch (const std::exception& e) {
            if (!failures.empty()) failures += "; ";
            failures += name + ": " + e.what();
        }
    }
    fail("no valid compressed stream (" + failures + ")");
}

void cells(Map& map) {
    map.decompressed_bytes = map.raw.size();
    map.cell_count = map.raw.size() / 11;
    if (map.raw.size() != map.expected_cells * 11) {
        fail("cell byte length mismatch: got " + std::to_string(map.raw.size()) +
             ", expected " + std::to_string(map.expected_cells * 11) +
             " = (2*Map.W-1)*Map.H*11; trailing bytes=" + std::to_string(map.raw.size() % 11));
    }
    std::vector<Tile> dense(map.expected_cells);
    std::vector<bool> seen(map.expected_cells, false);
    std::set<std::uint32_t> unique;
    bool all_zero = true;
    map.min_level = 255;
    map.max_level = 0;
    for (std::size_t i = 0; i < map.cell_count; ++i) {
        const std::size_t p = i * 11;
        const int rx = u16(map.raw, p);
        const int ry = u16(map.raw, p + 2);
        // Physical record: u16 rx, u16 ry, u32 tile, u8 sub, u8 level, u8 ice.
        // Original projected columns have Map.H entries each. Pair the two
        // staggered projected scanlines into one dense row, with no padding.
        const int x = rx - ry + map.map_width - 1;
        const int row_numerator = rx + ry - map.map_width - 1;
        if (x < 0 || x >= map.width || row_numerator < 0 ||
            row_numerator >= 2 * map.height) {
            fail("cell " + std::to_string(i) + " coordinate outside isometric diamond: " +
                 std::to_string(rx) + "," + std::to_string(ry));
        }
        const int y = row_numerator / 2;
        const std::size_t index = static_cast<std::size_t>(y) * map.width + x;
        if (seen[index]) fail("duplicate isometric coordinate at record " + std::to_string(i));
        seen[index] = true;
        Tile tile{x, y, u32(map.raw, p + 4), map.raw[p + 8], map.raw[p + 9], map.raw[p + 10]};
        dense[index] = tile;
        unique.insert(tile.tile);
        all_zero = all_zero && tile.tile == 0;
        if (tile.tile != std::numeric_limits<std::uint32_t>::max()) ++map.nonempty;
        map.min_level = std::min(map.min_level, static_cast<int>(tile.level));
        map.max_level = std::max(map.max_level, static_cast<int>(tile.level));
    }
    if (std::find(seen.begin(), seen.end(), false) != seen.end()) fail("missing diamond cells");
    if (all_zero) fail("all tile indices are zero; rejected as unverified terrain");
    map.unique_tiles = unique.size();
    map.tiles = std::move(dense);
}

void be32(Bytes& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 24));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes.push_back(static_cast<std::uint8_t>(value));
}

void png_chunk(Bytes& png, const char* type, const Bytes& payload) {
    be32(png, static_cast<std::uint32_t>(payload.size()));
    const std::size_t start = png.size();
    for (int i = 0; i < 4; ++i) png.push_back(static_cast<std::uint8_t>(type[i]));
    png.insert(png.end(), payload.begin(), payload.end());
    const auto crc = crc32(0, png.data() + start, static_cast<uInt>(payload.size() + 4));
    be32(png, static_cast<std::uint32_t>(crc));
}

} // namespace

Bytes read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) fail("cannot read " + path.string());
    const auto length = in.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) > max_file_bytes) fail("invalid/oversized input file");
    Bytes result(static_cast<std::size_t>(length));
    in.seekg(0);
    if (!result.empty()) in.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()));
    if (!in) fail("short read from " + path.string());
    return result;
}

void write_bytes(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) fail("cannot write " + path.string());
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) fail("write failed for " + path.string());
}

Bytes decode_lzo1x(const Bytes& input, std::size_t output_size) {
    BlockReader r(input, output_size);
    enum class State { literal_run, first_match, match, match_done, match_next };
    State state = State::literal_run;
    std::size_t t = r.byte();
    if (t > 17) {
        t -= 17;
        if (t < 4) state = State::match_next;
        else { r.literal(t); state = State::first_match; }
    } else r.pos = 0;
    for (;;) {
        if (state == State::literal_run) {
            t = r.byte();
            if (t >= 16) { state = State::match; continue; }
            if (t == 0) t = r.extension(15);
            r.literal(t + 3);
            state = State::first_match;
        }
        if (state == State::first_match) {
            t = r.byte();
            if (t >= 16) { state = State::match; continue; }
            const std::size_t distance = 1 + 0x800 + (t >> 2) + (r.byte() << 2);
            r.reference(distance, 3);
            state = State::match_done;
        }
        if (state == State::match) {
            std::size_t distance = 0, count = 0;
            if (t >= 64) {
                distance = 1 + ((t >> 2) & 7) + (r.byte() << 3);
                count = (t >> 5) + 1;
            } else if (t >= 32) {
                count = t & 31;
                if (!count) count = r.extension(31);
                const unsigned a = r.byte(), b = r.byte();
                distance = 1 + (a >> 2) + (b << 6);
                count += 2;
            } else if (t >= 16) {
                distance = (t & 8) << 11;
                count = t & 7;
                if (!count) count = r.extension(7);
                const unsigned a = r.byte(), b = r.byte();
                distance += (a >> 2) + (b << 6);
                if (distance == 0) {
                    if (count != 1) fail("invalid LZO1X end marker");
                    return r.finish();
                }
                distance += 0x4000;
                count += 2;
            } else {
                distance = 1 + (t >> 2) + (r.byte() << 2);
                count = 2;
            }
            r.reference(distance, count);
            state = State::match_done;
        }
        if (state == State::match_done) {
            if (r.pos < 2) fail("invalid LZO1X match state");
            t = input[r.pos - 2] & 3;
            state = t ? State::match_next : State::literal_run;
        }
        if (state == State::match_next) {
            r.literal(t);
            t = r.byte();
            state = State::match;
        }
    }
}

Bytes decode_lcw(const Bytes& input, std::size_t output_size) {
    BlockReader r(input, output_size);
    // Format80's optional leading zero selects relative long references.
    bool relative = false;
    if (!input.empty() && input[0] == 0) { r.pos = 1; relative = true; }
    for (;;) {
        const unsigned code = r.byte();
        if (!(code & 0x80)) {
            const std::size_t count = (code >> 4) + 3;
            const std::size_t distance = ((code & 15) << 8) | r.byte();
            r.reference(distance, count);
        } else if (!(code & 0x40)) {
            const std::size_t count = code & 63;
            if (!count) return r.finish();
            r.literal(count);
        } else if (code == 0xfe) {
            const std::size_t count = r.word();
            const auto value = static_cast<std::uint8_t>(r.byte());
            r.room(count);
            r.output.insert(r.output.end(), count, value);
        } else {
            const std::size_t count = code == 0xff ? r.word() : (code & 63) + 3;
            const std::size_t address = r.word();
            if (!relative && address >= r.output.size()) fail("invalid LCW absolute reference");
            r.reference(relative ? address : r.output.size() - address, count);
        }
    }
}

Bytes decode_base64(const std::string& text) {
    std::string compact;
    compact.reserve(text.size());
    for (unsigned char c : text) if (!std::isspace(c)) compact.push_back(static_cast<char>(c));
    if (compact.empty() || compact.size() % 4) fail("truncated Base64 (length is not a multiple of four)");
    const auto digit = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    Bytes result;
    result.reserve(compact.size() / 4 * 3);
    for (std::size_t i = 0; i < compact.size(); i += 4) {
        const int a = digit(compact[i]), b = digit(compact[i + 1]);
        const bool pad_c = compact[i + 2] == '=', pad_d = compact[i + 3] == '=';
        const int c = pad_c ? 0 : digit(compact[i + 2]);
        const int d = pad_d ? 0 : digit(compact[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0 || (pad_c && !pad_d) ||
            ((pad_c || pad_d) && i + 4 != compact.size())) fail("invalid Base64 character/padding");
        if ((pad_c && (b & 15)) || (pad_d && !pad_c && (c & 3))) fail("noncanonical Base64 padding bits");
        const std::uint32_t value = static_cast<std::uint32_t>((a << 18) | (b << 12) | (c << 6) | d);
        result.push_back(static_cast<std::uint8_t>(value >> 16));
        if (!pad_c) result.push_back(static_cast<std::uint8_t>(value >> 8));
        if (!pad_d) result.push_back(static_cast<std::uint8_t>(value));
    }
    return result;
}

std::string encode_base64(const Bytes& bytes) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t value = (static_cast<std::uint32_t>(bytes[i]) << 16) |
            (i + 1 < bytes.size() ? static_cast<std::uint32_t>(bytes[i + 1]) << 8 : 0) |
            (i + 2 < bytes.size() ? bytes[i + 2] : 0);
        result.push_back(alphabet[(value >> 18) & 63]);
        result.push_back(alphabet[(value >> 12) & 63]);
        result.push_back(i + 1 < bytes.size() ? alphabet[(value >> 6) & 63] : '=');
        result.push_back(i + 2 < bytes.size() ? alphabet[value & 63] : '=');
    }
    return result;
}

Map decode_bytes(const Bytes& input, const std::string& source) {
    Map result;
    result.source = source;
    try {
        const auto text = ini_text(input, result);
        const auto base64 = parse_ini(text, result);
        result.packed = decode_base64(base64);
        result.packed_bytes = result.packed.size();
        unpack(result);
        cells(result);
        result.success = true;
        result.reason = "complete: every expected diamond cell decoded exactly once";
    } catch (const std::exception& e) {
        result.reason = e.what();
        result.tiles.clear();
    }
    return result;
}

Map decode_file(const std::filesystem::path& path) {
    try { return decode_bytes(read_bytes(path), path.filename().string()); }
    catch (const std::exception& e) {
        Map result;
        result.source = path.filename().string();
        result.reason = e.what();
        return result;
    }
}

std::string json_string(const std::string& text) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') { result.push_back('\\'); result.push_back(static_cast<char>(c)); }
        else if (c < 32 || c >= 127) {
            // Report fields are ASCII; escaping also guarantees valid JSON for
            // arbitrary filenames or malformed input in diagnostic messages.
            result += "\\u00";
            result.push_back(hex[c >> 4]);
            result.push_back(hex[c & 15]);
        } else result.push_back(static_cast<char>(c));
    }
    result.push_back('"');
    return result;
}

void write_tiles_json(const std::filesystem::path& path, const Map& map) {
    if (!map.success || map.tiles.size() != map.expected_cells) fail("cannot export incomplete map");
    std::ofstream out(path);
    if (!out) fail("cannot write " + path.string());
    out << "{\n  \"source\": " << json_string(map.source)
        << ",\n  \"width\": " << map.width << ", \"height\": " << map.height
        << ", \"theater\": " << json_string(map.theater)
        << ",\n  \"cell_count\": " << map.tiles.size() << ",\n  \"tiles\": [\n";
    for (std::size_t i = 0; i < map.tiles.size(); ++i) {
        const auto& tile = map.tiles[i];
        out << "    {\"x\":" << tile.x << ",\"y\":" << tile.y << ",\"tile\":" << tile.tile
            << ",\"sub\":" << static_cast<unsigned>(tile.sub)
            << ",\"level\":" << static_cast<unsigned>(tile.level)
            << ",\"ice\":" << static_cast<unsigned>(tile.ice) << "}"
            << (i + 1 == map.tiles.size() ? "\n" : ",\n");
    }
    out << "  ],\n  \"stats\": {\"unique_tiles\":" << map.unique_tiles
        << ",\"min_level\":" << map.min_level << ",\"max_level\":" << map.max_level
        << ",\"nonempty\":" << map.nonempty << "}\n}\n";
    out.close();
    if (!out) fail("tile JSON write failed");
}

void write_preview_png(const std::filesystem::path& path, const Map& map, int scale) {
    if (!map.success || scale < 1 || scale > 16) fail("invalid preview input/scale");
    const std::size_t width = static_cast<std::size_t>(map.width) * scale;
    const std::size_t height = static_cast<std::size_t>(map.height) * scale;
    if (width * height > 16 * 1024 * 1024) fail("preview exceeds 16 megapixels");
    Bytes raw;
    raw.reserve((width * 4 + 1) * height);
    for (std::size_t y = 0; y < height; ++y) {
        raw.push_back(0); // PNG filter: None
        for (std::size_t x = 0; x < width; ++x) {
            const auto& tile = map.tiles[(y / scale) * map.width + x / scale];
            // A fixed hash keeps all occurrences of one tile the same color.
            std::uint32_t hash = tile.tile + 0x9e3779b9u;
            hash = (hash ^ (hash >> 16)) * 0x85ebca6bu;
            hash = (hash ^ (hash >> 13)) * 0xc2b2ae35u;
            hash ^= hash >> 16;
            raw.push_back(static_cast<std::uint8_t>(48 + (hash & 191)));
            raw.push_back(static_cast<std::uint8_t>(48 + ((hash >> 8) & 191)));
            raw.push_back(static_cast<std::uint8_t>(48 + ((hash >> 16) & 191)));
            raw.push_back(tile.tile == std::numeric_limits<std::uint32_t>::max() ? 0 : 255);
        }
    }
    uLongf length = compressBound(static_cast<uLong>(raw.size()));
    Bytes compressed(length);
    if (compress2(compressed.data(), &length, raw.data(), static_cast<uLong>(raw.size()), 9) != Z_OK) {
        fail("PNG zlib compression failed");
    }
    compressed.resize(length);
    Bytes png{137, 80, 78, 71, 13, 10, 26, 10};
    Bytes header;
    be32(header, static_cast<std::uint32_t>(width));
    be32(header, static_cast<std::uint32_t>(height));
    header.insert(header.end(), {8, 6, 0, 0, 0}); // RGBA8, no interlace
    png_chunk(png, "IHDR", header);
    png_chunk(png, "IDAT", compressed);
    png_chunk(png, "IEND", {});
    write_bytes(path, png);
}

} // namespace isomap
