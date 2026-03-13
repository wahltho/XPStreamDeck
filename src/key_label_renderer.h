#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xpstreamdeck {

struct RgbColor {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

std::vector<unsigned char> renderLabelKeyJpeg(
    const std::string& label,
    int width,
    int height,
    RgbColor background,
    RgbColor foreground,
    RgbColor accent,
    int maxTextScale = 0);

} // namespace xpstreamdeck
