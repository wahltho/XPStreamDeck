#include "key_label_renderer.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace xpstreamdeck {

namespace {

struct Glyph {
    char ch = '?';
    std::array<std::uint8_t, 7> rows{};
};

#define GLYPH(ch, a, b, c, d, e, f, g) Glyph{ch, std::array<std::uint8_t, 7>{a, b, c, d, e, f, g}}

constexpr std::array<Glyph, 42> kGlyphs = {{
    GLYPH(' ', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
    GLYPH('?', 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04),
    GLYPH('+', 0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00),
    GLYPH('-', 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00),
    GLYPH('.', 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06),
    GLYPH('/', 0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00),
    GLYPH('0', 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e),
    GLYPH('1', 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e),
    GLYPH('2', 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f),
    GLYPH('3', 0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e),
    GLYPH('4', 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02),
    GLYPH('5', 0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e),
    GLYPH('6', 0x0e, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x0e),
    GLYPH('7', 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08),
    GLYPH('8', 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e),
    GLYPH('9', 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x0e),
    GLYPH('A', 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11),
    GLYPH('B', 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e),
    GLYPH('C', 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e),
    GLYPH('D', 0x1c, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1c),
    GLYPH('E', 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f),
    GLYPH('F', 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10),
    GLYPH('G', 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0e),
    GLYPH('H', 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11),
    GLYPH('I', 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e),
    GLYPH('J', 0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0e),
    GLYPH('K', 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11),
    GLYPH('L', 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f),
    GLYPH('M', 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11),
    GLYPH('N', 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11),
    GLYPH('O', 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e),
    GLYPH('P', 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10),
    GLYPH('Q', 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d),
    GLYPH('R', 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11),
    GLYPH('S', 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e),
    GLYPH('T', 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04),
    GLYPH('U', 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e),
    GLYPH('V', 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04),
    GLYPH('W', 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a),
    GLYPH('X', 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11),
    GLYPH('Y', 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04),
    GLYPH('Z', 0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f),
}};

#undef GLYPH

const Glyph& glyphFor(char ch) {
    for (const auto& glyph : kGlyphs) {
        if (glyph.ch == ch) {
            return glyph;
        }
    }
    return kGlyphs[1];
}

std::string toUpperAscii(std::string text) {
    std::string out;
    out.reserve(text.size());
    bool lastWasSpace = false;
    for (unsigned char raw : text) {
        char ch = static_cast<char>(raw);
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            if (!out.empty() && out.back() == ' ') {
                out.pop_back();
            }
            out.push_back('\n');
            lastWasSpace = false;
            continue;
        }
        if (std::isspace(raw)) {
            if (!lastWasSpace && !out.empty() && out.back() != '\n') {
                out.push_back(' ');
                lastWasSpace = true;
            }
            continue;
        }
        lastWasSpace = false;
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(std::toupper(raw));
        }
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == ' ' || ch == '+' || ch == '-' || ch == '.' || ch == '/') {
            out.push_back(ch);
        } else {
            out.push_back('?');
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out.empty() ? std::string(" ") : out;
}

int glyphAdvance(char ch, int scale) {
    if (ch == ' ') {
        return scale * 4;
    }
    return scale * 6;
}

int measureText(const std::string& line, int scale) {
    int width = 0;
    for (char ch : line) {
        width += glyphAdvance(ch, scale);
    }
    if (!line.empty() && line.back() != ' ') {
        width -= scale;
    }
    return std::max(width, 0);
}

std::vector<std::string> splitWords(const std::string& paragraph) {
    std::vector<std::string> words;
    std::string current;
    for (char ch : paragraph) {
        if (ch == ' ') {
            if (!current.empty()) {
                words.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        words.push_back(current);
    }
    if (words.empty()) {
        words.push_back(" ");
    }
    return words;
}

std::vector<std::string> breakWord(const std::string& word, int scale, int maxWidth) {
    std::vector<std::string> pieces;
    std::string current;
    for (char ch : word) {
        std::string candidate = current;
        candidate.push_back(ch);
        if (!current.empty() && measureText(candidate, scale) > maxWidth) {
            pieces.push_back(current);
            current.assign(1, ch);
        } else {
            current = candidate;
        }
    }
    if (!current.empty()) {
        pieces.push_back(current);
    }
    return pieces;
}

std::vector<std::string> wrapText(const std::string& label, int scale, int maxWidth, int maxLines) {
    std::vector<std::string> lines;
    std::string paragraph;

    auto flushParagraph = [&](bool forcedBreak) {
        const auto words = splitWords(paragraph);
        std::string current;
        for (const auto& word : words) {
            if (measureText(word, scale) > maxWidth) {
                const auto pieces = breakWord(word, scale, maxWidth);
                for (const auto& piece : pieces) {
                    if (!current.empty()) {
                        lines.push_back(current);
                        current.clear();
                    }
                    lines.push_back(piece);
                    if (static_cast<int>(lines.size()) > maxLines) {
                        return false;
                    }
                }
                continue;
            }

            const std::string candidate = current.empty() ? word : (current + " " + word);
            if (measureText(candidate, scale) <= maxWidth) {
                current = candidate;
                continue;
            }

            lines.push_back(current);
            current = word;
            if (static_cast<int>(lines.size()) > maxLines) {
                return false;
            }
        }
        if (!current.empty()) {
            lines.push_back(current);
        } else if (forcedBreak) {
            lines.push_back(" ");
        }
        return static_cast<int>(lines.size()) <= maxLines;
    };

    for (char ch : label) {
        if (ch == '\n') {
            if (!flushParagraph(true)) {
                return {};
            }
            paragraph.clear();
            continue;
        }
        paragraph.push_back(ch);
    }

    if (!flushParagraph(false)) {
        return {};
    }

    while (!lines.empty() && lines.back() == " ") {
        lines.pop_back();
    }
    return lines;
}

struct Layout {
    int scale = 2;
    std::vector<std::string> lines;
};

Layout chooseLayout(const std::string& label, int width, int height) {
    const int horizontalPadding = 8;
    const int verticalPadding = 8;
    const int maxWidth = width - (horizontalPadding * 2);
    const int maxHeight = height - (verticalPadding * 2);

    for (int scale = 6; scale >= 2; --scale) {
        auto lines = wrapText(label, scale, maxWidth, 3);
        if (lines.empty()) {
            continue;
        }
        const int lineHeight = scale * 7;
        const int lineGap = scale;
        const int totalHeight = (static_cast<int>(lines.size()) * lineHeight) + (std::max<int>(static_cast<int>(lines.size()) - 1, 0) * lineGap);
        if (totalHeight <= maxHeight) {
            return Layout{scale, std::move(lines)};
        }
    }

    auto lines = wrapText(label, 2, maxWidth, 3);
    if (lines.empty()) {
        lines = {"?"};
    }
    return Layout{2, std::move(lines)};
}

void fill(std::vector<std::uint8_t>& pixels, int width, int height, RgbColor color) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t offset = static_cast<std::size_t>((y * width + x) * 3);
            pixels[offset + 0] = color.r;
            pixels[offset + 1] = color.g;
            pixels[offset + 2] = color.b;
        }
    }
}

RgbColor lighten(RgbColor color, int delta) {
    auto apply = [delta](std::uint8_t value) {
        return static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(value) + delta, 0, 255));
    };
    return RgbColor{apply(color.r), apply(color.g), apply(color.b)};
}

void drawRect(std::vector<std::uint8_t>& pixels, int width, int height, int x0, int y0, int x1, int y1, RgbColor color) {
    x0 = std::clamp(x0, 0, width);
    y0 = std::clamp(y0, 0, height);
    x1 = std::clamp(x1, 0, width);
    y1 = std::clamp(y1, 0, height);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t offset = static_cast<std::size_t>((y * width + x) * 3);
            pixels[offset + 0] = color.r;
            pixels[offset + 1] = color.g;
            pixels[offset + 2] = color.b;
        }
    }
}

void drawGlyph(std::vector<std::uint8_t>& pixels, int width, int height, int originX, int originY, char ch, int scale, RgbColor color) {
    const auto& glyph = glyphFor(ch);
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if ((glyph.rows[static_cast<std::size_t>(row)] & (1u << (4 - col))) == 0) {
                continue;
            }
            drawRect(
                pixels,
                width,
                height,
                originX + (col * scale),
                originY + (row * scale),
                originX + ((col + 1) * scale),
                originY + ((row + 1) * scale),
                color);
        }
    }
}

void drawLine(std::vector<std::uint8_t>& pixels, int width, int height, int x, int y, const std::string& line, int scale, RgbColor color) {
    int cursorX = x;
    for (char ch : line) {
        if (ch != ' ') {
            drawGlyph(pixels, width, height, cursorX, y, ch, scale, color);
        }
        cursorX += glyphAdvance(ch, scale);
    }
}

std::vector<std::uint8_t> rotate180(const std::vector<std::uint8_t>& pixels, int width, int height) {
    std::vector<std::uint8_t> rotated(pixels.size(), 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t src = static_cast<std::size_t>((y * width + x) * 3);
            const int rx = (width - 1) - x;
            const int ry = (height - 1) - y;
            const std::size_t dst = static_cast<std::size_t>((ry * width + rx) * 3);
            rotated[dst + 0] = pixels[src + 0];
            rotated[dst + 1] = pixels[src + 1];
            rotated[dst + 2] = pixels[src + 2];
        }
    }
    return rotated;
}

void appendToVector(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<unsigned char>*>(context);
    auto* bytes = static_cast<unsigned char*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

} // namespace

std::vector<unsigned char> renderLabelKeyJpeg(
    const std::string& label,
    int width,
    int height,
    RgbColor background,
    RgbColor foreground) {
    const std::string prepared = toUpperAscii(label);
    const auto layout = chooseLayout(prepared, width, height);

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width * height * 3), 0);
    fill(pixels, width, height, background);

    drawRect(pixels, width, height, 3, 3, width - 3, 5, lighten(background, 18));
    drawRect(pixels, width, height, 3, height - 5, width - 3, height - 3, lighten(background, 30));
    drawRect(pixels, width, height, 3, 3, 5, height - 3, lighten(background, 18));
    drawRect(pixels, width, height, width - 5, 3, width - 3, height - 3, lighten(background, 18));

    const int lineHeight = layout.scale * 7;
    const int lineGap = layout.scale;
    const int totalHeight = (static_cast<int>(layout.lines.size()) * lineHeight) + (std::max<int>(static_cast<int>(layout.lines.size()) - 1, 0) * lineGap);
    int y = (height - totalHeight) / 2;
    for (const auto& line : layout.lines) {
        const int lineWidth = measureText(line, layout.scale);
        const int x = std::max((width - lineWidth) / 2, 0);
        drawLine(pixels, width, height, x, y, line, layout.scale, foreground);
        y += lineHeight + lineGap;
    }

    const auto rotated = rotate180(pixels, width, height);

    std::vector<unsigned char> jpeg;
    stbi_write_jpg_to_func(appendToVector, &jpeg, width, height, 3, rotated.data(), 90);
    return jpeg;
}

} // namespace xpstreamdeck
