#include "isomap.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using isomap::Bytes;
using isomap::Map;

namespace {

const std::array<const char*, 12> required_names{{
    "entry_22_3393645847.map", "entry_28_3642608726.map", "entry_29_923683175.map",
    "entry_30_1418388757.map", "entry_31_3816317506.map", "entry_32_781720687.map",
    "entry_33_161755940.map", "entry_34_1485476374.map", "entry_35_498347118.map",
    "entry_36_1232232547.map", "entry_37_1453297755.map", "entry_38_4233700293.map"
}};

// Keep contract enumeration separate from the actual discovered input count.
bool contract_named(const std::string& name) {
    return std::find(required_names.begin(), required_names.end(), name) != required_names.end();
}

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

void word(Bytes& bytes, std::size_t value) {
    require(value <= 65535, "test chunk exceeds u16");
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void dword(Bytes& bytes, std::uint32_t value) {
    word(bytes, value & 65535);
    word(bytes, value >> 16);
}

Bytes literal_lcw(const Bytes& raw) {
    Bytes compressed;
    for (std::size_t i = 0; i < raw.size();) {
        const auto count = std::min<std::size_t>(63, raw.size() - i);
        compressed.push_back(static_cast<std::uint8_t>(0x80 | count));
        compressed.insert(compressed.end(), raw.begin() + static_cast<std::ptrdiff_t>(i),
                          raw.begin() + static_cast<std::ptrdiff_t>(i + count));
        i += count;
    }
    compressed.push_back(0x80);
    return compressed;
}

Bytes pack_lcw(const Bytes& raw, bool reversed = false, bool terminator = false) {
    Bytes result;
    for (std::size_t offset = 0; offset < raw.size();) {
        const std::size_t count = std::min<std::size_t>(8192, raw.size() - offset);
        Bytes block(raw.begin() + static_cast<std::ptrdiff_t>(offset),
                    raw.begin() + static_cast<std::ptrdiff_t>(offset + count));
        const auto compressed = literal_lcw(block);
        word(result, reversed ? count : compressed.size());
        word(result, reversed ? compressed.size() : count);
        result.insert(result.end(), compressed.begin(), compressed.end());
        offset += count;
    }
    if (terminator) result.insert(result.end(), 4, 0);
    return result;
}

Bytes packed_ini(const Bytes& packed, int width, int height) {
    const auto base64 = isomap::encode_base64(packed);
    std::ostringstream text;
    text << "[Preview]\nSize=0,0," << 2 * width << ',' << height
         << "\n[Map]\nSize=0,0," << width << ',' << height
         << "\nTheater=TEMPERATE\n[IsoMapPack5]\n";
    int key = 1;
    for (std::size_t i = 0; i < base64.size(); i += 70) text << key++ << '=' << base64.substr(i, 70) << '\n';
    const auto value = text.str();
    return Bytes(value.begin(), value.end());
}

Bytes synthetic_cells(int width = 2, int height = 2) {
    Bytes raw;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < 2 * width - 1; ++x) {
            const int difference = x - width + 1;
            int sum = 2 * y + width + 1;
            if ((sum - difference) % 2) ++sum;
            word(raw, static_cast<std::size_t>((sum + difference) / 2));
            word(raw, static_cast<std::size_t>((sum - difference) / 2));
            const int index = y * (2 * width - 1) + x;
            dword(raw, static_cast<std::uint32_t>(70000 + index)); // Exercise the high 16 tile bits.
            raw.push_back(static_cast<std::uint8_t>(index % 4));
            raw.push_back(static_cast<std::uint8_t>(index % 3));
            raw.push_back(static_cast<std::uint8_t>(index % 2));
        }
    }
    return raw;
}

struct Suite {
    std::size_t assertions = 0;
    std::size_t fuzz_streams = 0;

    void check(bool value, const std::string& name) {
        require(value, "self-test failed: " + name);
        ++assertions;
    }

    void rejects(const std::function<void()>& action, const std::string& name) {
        bool rejected = false;
        try { action(); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected, name);
    }
};

Suite self_tests() {
    Suite suite;
    for (int length = 0; length < 260; ++length) {
        Bytes bytes;
        for (int i = 0; i < length; ++i) bytes.push_back(static_cast<std::uint8_t>(i));
        if (length == 0) continue; // Empty packs are deliberately rejected.
        suite.check(isomap::decode_base64(isomap::encode_base64(bytes)) == bytes, "Base64 binary round trip");
    }
    suite.rejects([] { isomap::decode_base64("A==A"); }, "Base64 interior padding");
    suite.rejects([] { isomap::decode_base64("AAAAA"); }, "Base64 truncated quartet");
    suite.rejects([] { isomap::decode_base64("AB=="); }, "Base64 padding bits");
    suite.check(isomap::decode_lcw({0x83, 'a', 'b', 'c', 0x80}, 3) == Bytes({'a','b','c'}), "LCW literal");
    suite.check(isomap::decode_lcw({0x83, 'a', 'b', 'c', 0, 3, 0x80}, 6) ==
                Bytes({'a','b','c','a','b','c'}), "LCW short back-reference");
    suite.check(isomap::decode_lcw({0x83, 'a', 'b', 'c', 0xc0, 0, 0, 0x80}, 6) ==
                Bytes({'a','b','c','a','b','c'}), "LCW absolute long reference");
    suite.check(isomap::decode_lcw({0, 0x83, 'a', 'b', 'c', 0xc0, 3, 0, 0x80}, 6) ==
                Bytes({'a','b','c','a','b','c'}), "LCW relative long reference");
    suite.check(isomap::decode_lcw({0x81, 'x', 0xff, 8, 0, 0, 0, 0x80}, 9) == Bytes(9, 'x'), "LCW overlap");
    suite.check(isomap::decode_lcw({0xfe, 6, 0, 'x', 0x80}, 6) == Bytes(6, 'x'), "LCW fill");
    suite.rejects([] { isomap::decode_lcw({0x81, 'x', 0x80}, 2); }, "LCW underfill");
    suite.rejects([] { isomap::decode_lcw({0x82, 'x', 'y', 0x80}, 1); }, "LCW output overflow");
    suite.rejects([] { isomap::decode_lcw({0xc0, 0, 0, 0x80}, 3); }, "LCW invalid reference");
    suite.check(isomap::decode_lzo1x({20, 'a', 'b', 'c', 17, 0, 0}, 3) == Bytes({'a','b','c'}), "LZO literals");
    suite.check(isomap::decode_lzo1x({21, 'a', 'b', 'c', 'd', 0x6c, 0, 17, 0, 0}, 8) ==
                Bytes({'a','b','c','d','a','b','c','d'}), "LZO M2 match");
    suite.check(isomap::decode_lzo1x({21, 'a', 'b', 'c', 'd', 0x24, 12, 0, 17, 0, 0}, 10) ==
                Bytes({'a','b','c','d','a','b','c','d','a','b'}), "LZO M3 match");
    suite.check(isomap::decode_lzo1x({18, 'a', 0, 0, 17, 0, 0}, 3) == Bytes(3, 'a'), "LZO short match");
    Bytes extended{0, 0, 27};
    extended.insert(extended.end(), 300, 'x');
    extended.insert(extended.end(), {17, 0, 0});
    suite.check(isomap::decode_lzo1x(extended, 300) == Bytes(300, 'x'), "LZO extended literals");
    Bytes distant{0};
    distant.insert(distant.end(), 64, 0);
    distant.push_back(47);
    distant.insert(distant.end(), 16385, 'q');
    distant.insert(distant.end(), {17, 4, 0, 17, 0, 0});
    suite.check(isomap::decode_lzo1x(distant, 16388) == Bytes(16388, 'q'), "LZO M4 distant match");
    suite.rejects([] { isomap::decode_lzo1x({20, 'a', 'b', 'c', 17, 0}, 3); }, "LZO truncated end");
    suite.rejects([] { isomap::decode_lzo1x({20, 'a', 'b', 'c', 17, 0, 0, 0}, 3); }, "LZO trailing bytes");
    suite.rejects([] { isomap::decode_lzo1x({18, 'a', 0, 9, 17, 0, 0}, 3); }, "LZO invalid lookbehind");
    const auto raw = synthetic_cells();
    for (const bool reversed : {false, true}) {
        const auto map = isomap::decode_bytes(packed_ini(pack_lcw(raw, reversed, true), 2, 2), "synthetic.map");
        suite.check(map.success && map.raw == raw && map.tiles.size() == 6, "LCW chunk header/end marker");
        suite.check(map.tiles[0].tile == 70000 && map.tiles[5].ice == 1 && map.tiles[5].level == 2,
                    "11-byte record fields and dense ordering");
    }
    Bytes duplicate = raw;
    std::copy_n(raw.begin(), 4, duplicate.begin() + 11);
    const auto duplicated = isomap::decode_bytes(packed_ini(pack_lcw(duplicate), 2, 2), "duplicate.map");
    suite.check(!duplicated.success && duplicated.reason.find("duplicate") != std::string::npos, "duplicate cells");
    Bytes missing(raw.begin(), raw.end() - 11);
    const auto partial = isomap::decode_bytes(packed_ini(pack_lcw(missing), 2, 2), "missing.map");
    suite.check(!partial.success && partial.reason.find("length mismatch") != std::string::npos, "missing record");
    Bytes zero = raw;
    for (std::size_t p = 4; p < zero.size(); p += 11) std::fill_n(zero.begin() + static_cast<std::ptrdiff_t>(p), 4, 0);
    const auto blank = isomap::decode_bytes(packed_ini(pack_lcw(zero), 2, 2), "zero.map");
    suite.check(!blank.success && blank.reason.find("all tile") != std::string::npos, "all-zero terrain rejection");
    // Fixed seed: malformed streams always terminate; run with the sanitizer build.
    std::uint32_t state = 0x7e31ad5bu;
    const auto random = [&]() -> std::uint32_t {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return state;
    };
    for (int trial = 0; trial < 5000; ++trial) {
        Bytes input(random() % 80);
        for (auto& byte : input) byte = static_cast<std::uint8_t>(random());
        const std::size_t expected = random() % 300;
        for (const bool lzo : {false, true}) {
            try {
                const auto output = lzo ? isomap::decode_lzo1x(input, expected) : isomap::decode_lcw(input, expected);
                if (output.size() != expected) throw std::logic_error("fuzz output size invariant");
            } catch (const std::runtime_error&) { }
            ++suite.fuzz_streams;
        }
    }
    return suite;
}

bool same_grid(const Map& a, const Map& b) {
    if (!a.success || !b.success || a.raw != b.raw || a.width != b.width || a.height != b.height ||
        a.tiles.size() != b.tiles.size()) return false;
    for (std::size_t i = 0; i < a.tiles.size(); ++i) {
        const auto& x = a.tiles[i]; const auto& y = b.tiles[i];
        if (x.x != y.x || x.y != y.y || x.tile != y.tile || x.sub != y.sub || x.level != y.level || x.ice != y.ice) return false;
    }
    return true;
}

struct InputCheck {
    std::string file;
    bool expected_success = false;
    bool passed = false;
    Map result;
};

bool skipped(const Map& map) {
    return !map.success && map.reason == "no [IsoMapPack5] section";
}

std::vector<InputCheck> input_checks(const fs::path& directory, const Map& primary, const Bytes& source) {
    fs::create_directories(directory);
    std::vector<InputCheck> checks;
    const auto run = [&](const std::string& name, const Bytes& data, bool expected) {
        const auto path = directory / name;
        isomap::write_bytes(path, data);
        InputCheck test;
        test.file = "test_inputs/" + name;
        test.expected_success = expected;
        test.result = isomap::decode_file(path);
        test.passed = expected ? same_grid(primary, test.result) : !test.result.success;
        checks.push_back(std::move(test));
    };
    Bytes little{0xff, 0xfe}, big{0xfe, 0xff};
    for (const auto c : source) { little.push_back(c); little.push_back(0); big.push_back(0); big.push_back(c); }
    run("utf16le.map", little, true);
    run("utf16be.map", big, true);
    little.pop_back();
    run("utf16_odd_truncated.map", little, false);
    Bytes truncated(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(source.size() / 2));
    run("file_truncated.map", truncated, false);
    Bytes packed = primary.packed;
    packed.pop_back();
    run("stream_truncated.map", packed_ini(packed, primary.map_width, primary.map_height), false);
    // Also test a well-formed compressed stream carrying an incomplete record set.
    Bytes missing(primary.raw.begin(), primary.raw.end() - 11);
    run("record_truncated.map", packed_ini(pack_lcw(missing), primary.map_width, primary.map_height), false);
    return checks;
}

void map_json(std::ostream& out, const Map& map, bool outputs) {
    const auto quote = isomap::json_string;
    out << "{\"filename\":" << quote(map.source)
        << ",\"contract_named\":" << (contract_named(map.source) ? "true" : "false")
        << ",\"width\":" << map.width << ",\"height\":" << map.height
        << ",\"map_width\":" << map.map_width << ",\"map_height\":" << map.map_height
        << ",\"map_size\":" << quote(map.map_size) << ",\"preview_size\":" << quote(map.preview_size)
        << ",\"theater\":" << quote(map.theater) << ",\"encoding\":" << quote(map.encoding)
        << ",\"packed_bytes\":" << map.packed_bytes << ",\"decompressed_bytes\":" << map.decompressed_bytes
        << ",\"cell_count\":" << map.cell_count << ",\"expected_cell_count\":" << map.expected_cells
        << ",\"unique_tiles\":" << map.unique_tiles << ",\"min_level\":" << map.min_level
        << ",\"max_level\":" << map.max_level << ",\"codec\":" << quote(map.codec)
        << ",\"chunk_header\":" << quote(map.chunk_header) << ",\"blocks\":" << map.blocks
        << ",\"has_isomap\":" << (map.has_isomap ? "true" : "false")
        << ",\"success\":" << (map.success ? "true" : "false")
        << ",\"status\":" << quote(map.success ? "success" : (skipped(map) ? "skipped" : "failed"))
        << ",\"reason\":" << quote(map.reason) << ",\"warnings\":[";
    for (std::size_t i = 0; i < map.warnings.size(); ++i) out << (i ? "," : "") << quote(map.warnings[i]);
    out << ']';
    if (outputs && map.success) {
        const auto stem = fs::path(map.source).stem().string();
        out << ",\"grid_output\":" << quote("decoded/" + stem + ".tiles.json")
            << ",\"raw_output\":" << quote("decoded/" + stem + ".cells.bin");
    }
    out << '}';
}

void closed(std::ofstream& out, const std::string& name) {
    out.close();
    require(static_cast<bool>(out), "write failed: " + name);
}

void usage() {
    std::cout << "Usage: ./isomap_probe [--input ../p5-mapdata/maps] [--out .] [--main FILE]\n"
                 "       ./isomap_probe --check FILE\n"
                 "       ./isomap_probe --self-test\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        fs::path input = "../p5-mapdata/maps", out = ".";
        std::string primary_name = "entry_22_3393645847.map";
        bool test_only = false;
        fs::path check_file;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--self-test") test_only = true;
            else if (arg == "--help" || arg == "-h") { usage(); return 0; }
            else if ((arg == "--input" || arg == "--out" || arg == "--main" || arg == "--check") && i + 1 < argc) {
                const std::string value = argv[++i];
                if (arg == "--input") input = value;
                else if (arg == "--out") out = value;
                else if (arg == "--main") primary_name = value;
                else check_file = value;
            } else throw std::runtime_error("unknown/incomplete argument: " + arg);
        }
        if (!check_file.empty()) {
            const auto map = isomap::decode_file(check_file);
            map_json(std::cout, map, false);
            std::cout << '\n';
            return map.success ? 0 : 1;
        }
        const auto suite = self_tests();
        std::cout << "Self-tests: " << suite.assertions << " assertions passed; " << suite.fuzz_streams << " bounded codec fuzz streams\n";
        if (test_only) return 0;
        require(fs::is_directory(input), "input directory does not exist");
        std::vector<fs::path> paths;
        for (const auto& entry : fs::directory_iterator(input)) {
            if (entry.is_regular_file() && entry.path().extension() == ".map") paths.push_back(entry.path());
        }
        std::sort(paths.begin(), paths.end());
        require(!paths.empty(), "no .map inputs");
        fs::create_directories(out / "decoded");
        std::vector<Map> maps;
        std::size_t successes = 0, candidates = 0, failures = 0, skips = 0, named_successes = 0, named_found = 0;
        std::size_t primary_index = paths.size();
        for (const auto& path : paths) {
            auto map = isomap::decode_file(path);
            if (map.has_isomap) ++candidates;
            if (skipped(map)) ++skips;
            else if (!map.success) ++failures;
            if (contract_named(map.source)) { ++named_found; if (map.success) ++named_successes; }
            if (map.success) {
                ++successes;
                const auto stem = path.stem().string();
                isomap::write_tiles_json(out / "decoded" / (stem + ".tiles.json"), map);
                isomap::write_bytes(out / "decoded" / (stem + ".cells.bin"), map.raw);
            }
            std::cout << map.source << ": " << (map.success ? "OK" : (skipped(map) ? "SKIP" : "FAIL")) << " cells=" << map.cell_count
                      << '/' << map.expected_cells << " unique=" << map.unique_tiles;
            if (!map.success) std::cout << " reason=" << map.reason;
            std::cout << '\n';
            if (map.source == primary_name) primary_index = maps.size();
            maps.push_back(std::move(map));
        }
        require(primary_index < maps.size(), "primary map is missing");
        const auto& primary = maps[primary_index];
        require(primary.success, "primary map failed: " + primary.reason);
        isomap::write_tiles_json(out / "tiles.json", primary);
        isomap::write_preview_png(out / "tiles_preview.png", primary);
        const auto checks = input_checks(out / "test_inputs", primary, isomap::read_bytes(input / primary_name));
        bool checks_passed = true;
        for (const auto& check : checks) checks_passed = checks_passed && check.passed;

        std::ofstream summary(out / "all_maps.json");
        require(static_cast<bool>(summary), "cannot write all_maps.json");
        summary << "{\n  \"summary\": {\"files_scanned\":" << maps.size() << ",\"isomap_files_found\":" << candidates
                << ",\"successful_maps\":" << successes << ",\"failed_maps\":" << failures << ",\"skipped_maps\":" << skips
                << ",\"contract_named_maps\":" << named_found << ",\"contract_named_successes\":" << named_successes
                << ",\"self_test_assertions\":" << suite.assertions << ",\"fuzz_streams\":" << suite.fuzz_streams
                << ",\"input_checks_passed\":" << (checks_passed ? "true" : "false") << "},\n"
                << "  \"format\": {\"record\":\"u16 rx,u16 ry,u32 tile,u8 sub,u8 level,u8 ice (11 bytes, little endian)\","
                   "\"dense_width\":\"2*Map.W-1\",\"dense_height\":\"Map.H\","
                   "\"dense_x\":\"rx-ry+Map.W-1\",\"dense_y\":\"floor((rx+ry-Map.W-1)/2)\","
                   "\"expected_cells\":\"(2*Map.W-1)*Map.H\","
                   "\"size_note\":\"Preview Size is image metadata, not the terrain cell grid; no padded/fabricated cells\","
                   "\"input_note\":\"CONTRACT lists 12 entry numbers as 13; scanning all files finds 17 packed maps, including 5 with misleading BOM\"},\n"
                << "  \"maps\": [\n";
        for (std::size_t i = 0; i < maps.size(); ++i) {
            summary << "    "; map_json(summary, maps[i], true); summary << (i + 1 < maps.size() ? ",\n" : "\n");
        }
        summary << "  ],\n  \"input_checks\": [\n";
        for (std::size_t i = 0; i < checks.size(); ++i) {
            const auto& check = checks[i];
            summary << "    {\"file\":" << isomap::json_string(check.file)
                    << ",\"expected_success\":" << (check.expected_success ? "true" : "false")
                    << ",\"passed\":" << (check.passed ? "true" : "false") << ",\"result\":";
            map_json(summary, check.result, false);
            summary << '}' << (i + 1 < checks.size() ? ",\n" : "\n");
        }
        summary << "  ]\n}\n";
        closed(summary, "all_maps.json");

        std::ofstream run(out / "RUN.txt");
        require(static_cast<bool>(run), "cannot write RUN.txt");
        run << "Compile: make\nCompiler command: c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic main.cpp isomap.cpp -lz -o isomap_probe\n"
            << "Compiler: " << __VERSION__ << "\nRun:";
        for (int i = 0; i < argc; ++i) run << ' ' << argv[i];
        run << "\n\nScanned " << maps.size() << " files; found " << candidates << " IsoMapPack5 streams; decoded " << successes << ".\n"
            << "Contract enumerates 12 entry numbers (22,28..38), despite saying 13: " << named_successes << '/' << named_found << " complete.\n"
            << "Recovered additional 8-bit maps with stray UTF-16 BOM; strict failures remain failures.\n"
            << "Actual format: LZO1X; u16 compressed length then u16 expanded length; EOF at an exact chunk boundary is valid.\n"
            << "Cell: u16 rx, u16 ry, u32 tile, u8 sub, u8 level, u8 ice; little endian; 11 bytes.\n"
            << "Size comes from [Map], not [Header] Width/Height or [Preview] Size.\n"
            << "Dense grid x=rx-ry+Map.W-1; y=floor((rx+ry-Map.W-1)/2).\n"
            << "Columns=2*Map.W-1; rows=Map.H; every expected cell must appear exactly once.\n"
            << "Tile 0 is valid clear terrain; nonempty excludes only 0xffffffff.\n\n"
            << "Primary: " << primary.source << "\n[Map] Size=" << primary.map_size << "; [Preview] Size=" << primary.preview_size
            << "\nExported: " << primary.width << 'x' << primary.height << "; cells=" << primary.tiles.size()
            << "; unique tiles=" << primary.unique_tiles << "; raw bytes=" << primary.decompressed_bytes
            << "; levels=" << primary.min_level << ".." << primary.max_level << "\n"
            << "The 160x80 preview size is not 12,800 cells: the real diamond contains 12,720. No cells were invented.\n"
            << "Preview: tiles_preview.png, RGBA8, " << primary.width * 6 << 'x' << primary.height * 6
            << ", deterministic tile-index colors (not game terrain textures).\n"
            << "Per-map grids and uncompressed cell streams: decoded/*.tiles.json, decoded/*.cells.bin\n\n"
            << "Self-tests: " << suite.assertions << " assertions passed; " << suite.fuzz_streams << " bounded malformed codec streams.\n";
        for (const auto& map : maps) {
            run << map.source << ' ' << (map.success ? "OK" : (skipped(map) ? "SKIP" : "FAIL"))
                << " map=" << map.map_width << 'x' << map.map_height << " grid=" << map.width << 'x' << map.height
                << " raw=" << map.decompressed_bytes << " cells=" << map.cell_count << '/' << map.expected_cells
                << " unique=" << map.unique_tiles;
            if (!map.success) run << " reason=" << map.reason;
            run << '\n';
        }
        run << '\n';
        for (const auto& check : checks) {
            run << check.file << ' ' << (check.passed ? "PASS" : "FAIL") << " expected=" << check.expected_success
                << " actual=" << check.result.success << " reason=" << check.result.reason << '\n';
        }
        const bool accepted = named_found == 12 && named_successes >= 8 && successes >= 13 && checks_passed &&
            primary.tiles.size() == primary.cell_count && primary.unique_tiles > 10;
        run << "\nAcceptance: " << (accepted ? "PASS" : "FAIL") << "\n";
        closed(run, "RUN.txt");
        std::cout << "Decoded " << successes << '/' << candidates << " packed maps; contract-named "
                  << named_successes << '/' << named_found << "; input checks=" << (checks_passed ? "PASS" : "FAIL")
                  << "\nPrimary cells=" << primary.tiles.size() << " unique_tiles=" << primary.unique_tiles
                  << " grid=" << primary.width << 'x' << primary.height << '\n';
        return accepted ? 0 : 2;
    } catch (const std::exception& e) {
        std::cerr << "isomap_probe: " << e.what() << '\n';
        return 1;
    }
}
