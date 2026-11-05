#ifndef ISOMAP_H
#define ISOMAP_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace isomap {

using Bytes = std::vector<std::uint8_t>;

struct Tile {
    int x = 0;
    int y = 0;
    std::uint32_t tile = 0;
    std::uint8_t sub = 0;
    std::uint8_t level = 0;
    std::uint8_t ice = 0;
};

struct Map {
    std::string source;
    std::string encoding;
    bool has_isomap = false;
    bool success = false;
    std::string reason;
    // [Map] Size describes W,H isometric cells, NOT the preview image size.
    int map_width = 0;
    int map_height = 0;
    // Dense staggered grid: (2*map_width-1) columns by map_height rows.
    int width = 0;
    int height = 0;
    std::string map_size;
    std::string preview_size;
    std::string theater;
    std::string codec;
    std::string chunk_header;
    std::size_t blocks = 0;
    std::size_t packed_bytes = 0;
    std::size_t decompressed_bytes = 0;
    std::size_t cell_count = 0;
    std::size_t expected_cells = 0;
    std::size_t unique_tiles = 0;
    std::size_t nonempty = 0;
    int min_level = 0;
    int max_level = 0;
    std::vector<std::string> warnings;
    Bytes packed;
    Bytes raw;
    std::vector<Tile> tiles;
};

Bytes read_bytes(const std::filesystem::path& path);
void write_bytes(const std::filesystem::path& path, const Bytes& bytes);
// Malformed streams throw std::runtime_error. All copies have input/output bounds.
Bytes decode_lzo1x(const Bytes& input, std::size_t output_size);
Bytes decode_lcw(const Bytes& input, std::size_t output_size);
Bytes decode_base64(const std::string& text);
std::string encode_base64(const Bytes& bytes);
Map decode_bytes(const Bytes& input, const std::string& source);
Map decode_file(const std::filesystem::path& path);
void write_tiles_json(const std::filesystem::path& path, const Map& map);
void write_preview_png(const std::filesystem::path& path, const Map& map, int scale = 6);
std::string json_string(const std::string& text);

} // namespace isomap

#endif
