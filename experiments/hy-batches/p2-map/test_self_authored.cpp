#include "shp_decode.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <random>
#include <stdexcept>

namespace {
using Bytes = std::vector<std::uint8_t>;
const std::string kCase = ".build/cases/input.shp";
std::size_t checks = 0;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
    ++checks;
}

void put16(Bytes& bytes, std::size_t offset, std::uint16_t value) {
    bytes.at(offset) = static_cast<std::uint8_t>(value);
    bytes.at(offset + 1) = static_cast<std::uint8_t>(value >> 8);
}

void put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i)
        bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (8 * i));
}

void save(const Bytes& bytes) {
    std::ofstream file(kCase, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    file.close();
    check(static_cast<bool>(file), "cannot write test input");
}

void reject(const Bytes& bytes, const std::string& label, std::size_t index = 0) {
    save(bytes);
    bool rejected = false;
    try { (void)decodeFrame(kCase, index); }
    catch (const std::runtime_error&) { rejected = true; }
    check(rejected, "invalid input accepted: " + label);
}

Bytes tiny(std::uint32_t format, const Bytes& data) {
    Bytes bytes(32, 0);
    put16(bytes, 2, 8); put16(bytes, 4, 4); put16(bytes, 6, 1);
    put16(bytes, 8, 2); put16(bytes, 10, 1);
    put16(bytes, 12, 3); put16(bytes, 14, 2);
    put32(bytes, 16, format); put32(bytes, 28, 32);
    bytes.insert(bytes.end(), data.begin(), data.end());
    return bytes;
}

void checkTiny(const Bytes& bytes) {
    save(bytes);
    const auto frame = decodeFrame(kCase, 0);
    Bytes expected(32, 0);
    expected[10] = 7; expected[12] = 8;
    check(frame.width == 8 && frame.height == 4 && frame.paletteIndices == expected,
          "tiny frame placement/transparency differs");
}
} // namespace

int main() {
  try {
    std::filesystem::create_directories(".build/cases");
    checkTiny(tiny(0, {7, 0, 8, 0, 0, 0}));
    checkTiny(tiny(1, {7, 0, 8, 0, 0, 0}));
    checkTiny(tiny(2, {5, 0, 7, 0, 8, 5, 0, 0, 0, 0}));
    checkTiny(tiny(3, {6, 0, 7, 0, 1, 8, 4, 0, 0, 4}));
    checkTiny(tiny(3, {6, 0, 7, 0, 1, 8, 2, 0}));
    reject(tiny(3, {1, 0}), "short row header");
    reject(tiny(3, {3, 0, 0}), "dangling run");
    reject(tiny(3, {4, 0, 0, 0}), "zero run");
    reject(tiny(3, {4, 0, 0, 5}), "run crosses row");
    reject(tiny(3, {6, 0, 1, 2, 3, 4}), "literal crosses row");
    reject(tiny(3, {255, 255, 1}), "row crosses payload");
    reject(tiny(1, {1}), "truncated raw pixels");
    reject(tiny(2, {3, 0, 1}), "short raw row");
    reject(tiny(4, {1, 2}), "unsupported encoding");
    auto base = tiny(0, {7, 0, 8, 0, 0, 0});
    for (std::size_t cut=0; cut<32; ++cut)
      reject(Bytes(base.begin(), base.begin()+cut), "truncated header");
    reject(base, "invalid frame index", 1);
    auto bad=base; put32(bad, 28, 0xffffffffU); reject(bad, "invalid offset");
    bad=base; put16(bad, 2, 0); reject(bad, "zero canvas");
    std::cout << "SELF_AUTHORED SHP: " << checks << " checks passed\n";
    return 0;
  } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
