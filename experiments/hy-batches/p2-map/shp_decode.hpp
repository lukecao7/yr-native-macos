#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct DecodedFrame {
    std::uint16_t width;
    std::uint16_t height;
    // Full SHP canvas, row-major. Index 0 is transparent, including outside
    // the frame's cropped rectangle. No palette or rendering dependency.
    std::vector<std::uint8_t> paletteIndices;
};

struct ShpInfo {
    std::uint16_t width;
    std::uint16_t height;
    std::uint16_t frameCount;
};

// TS/RA2 SHP (8-byte file header, 24-byte frame headers), little-endian.
// Throws std::runtime_error on invalid, truncated or unsupported input.
ShpInfo readShpInfo(const std::string& path);
DecodedFrame decodeFrame(const std::string& path, std::size_t frameIndex);
