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

struct GlyphMetrics {
    int left = 0;
    int right = 4;
    bool empty = false;
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

GlyphMetrics glyphMetrics(char ch) {
    if (ch == ' ') {
        return GlyphMetrics{0, 0, true};
    }

    const auto& glyph = glyphFor(ch);
    int left = 5;
    int right = -1;
    for (std::uint8_t rowBits : glyph.rows) {
        for (int col = 0; col < 5; ++col) {
            if ((rowBits & (1u << (4 - col))) == 0) {
                continue;
            }
            left = std::min(left, col);
            right = std::max(right, col);
        }
    }

    if (right < left) {
        return GlyphMetrics{0, 0, true};
    }
    return GlyphMetrics{left, right, false};
}

int glyphSpacing(int scale) {
    return std::max(1, (scale + 1) / 3);
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
        return (scale * 2) + glyphSpacing(scale);
    }
    const auto metrics = glyphMetrics(ch);
    const int inkWidth = metrics.empty ? 3 : ((metrics.right - metrics.left) + 1);
    return (inkWidth * scale) + glyphSpacing(scale);
}

int measureText(const std::string& line, int scale) {
    int width = 0;
    for (char ch : line) {
        width += glyphAdvance(ch, scale);
    }
    if (!line.empty()) {
        width -= glyphSpacing(scale);
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

std::string joinWords(const std::vector<std::string>& words, std::size_t begin, std::size_t endExclusive) {
    std::string joined;
    for (std::size_t i = begin; i < endExclusive; ++i) {
        if (!joined.empty()) {
            joined.push_back(' ');
        }
        joined += words[i];
    }
    return joined;
}

struct PartitionCandidate {
    std::vector<std::string> lines;
    int score = 0;
};

struct WrappedText {
    std::vector<std::string> lines;
    int brokenWordCount = 0;
    bool usedWordGrouping = false;
};

struct Layout {
    int scale = 2;
    std::vector<std::string> lines;
    int brokenWordCount = 0;
    bool usedWordGrouping = false;
    int score = -1000000;
};

int visibleCharacterCount(const std::string& line) {
    int count = 0;
    for (char ch : line) {
        if (ch != ' ') {
            ++count;
        }
    }
    return count;
}

int preferredLineCount(const std::string& label) {
    if (label.find('\n') != std::string::npos) {
        return static_cast<int>(std::count(label.begin(), label.end(), '\n')) + 1;
    }
    return std::clamp<int>(static_cast<int>(splitWords(label).size()), 1, 3);
}

int scorePartition(const std::vector<std::string>& lines, int scale, int maxWidth, int preferredLines) {
    if (lines.empty()) {
        return -1000000;
    }

    int widestLine = 0;
    int narrowestLine = maxWidth;
    int widthSum = 0;
    int shortLinePenalty = 0;
    for (const auto& line : lines) {
        const int width = measureText(line, scale);
        widestLine = std::max(widestLine, width);
        narrowestLine = std::min(narrowestLine, width);
        widthSum += width;

        const int visibleChars = visibleCharacterCount(line);
        if (visibleChars <= 1) {
            shortLinePenalty += 900;
        } else if (visibleChars == 2) {
            shortLinePenalty += 350;
        }
    }

    const int averageWidth = widthSum / static_cast<int>(lines.size());
    const int raggedness = widestLine - narrowestLine;
    const int preferredLinePenalty = std::abs(static_cast<int>(lines.size()) - preferredLines) * 1000;
    const int widthPenalty = widestLine * 4;
    const int raggedPenalty = raggedness * 3;
    const int densityBonus = averageWidth;

    return densityBonus - preferredLinePenalty - widthPenalty - raggedPenalty - shortLinePenalty;
}

void searchWordPartitions(
    const std::vector<std::string>& words,
    std::size_t index,
    int remainingLines,
    int scale,
    int maxWidth,
    int preferredLines,
    std::vector<std::string>& currentLines,
    PartitionCandidate& best) {
    if (index >= words.size()) {
        if (remainingLines != 0) {
            return;
        }
        const int score = scorePartition(currentLines, scale, maxWidth, preferredLines);
        if (best.lines.empty() || score > best.score) {
            best.lines = currentLines;
            best.score = score;
        }
        return;
    }
    if (remainingLines <= 0) {
        return;
    }

    const std::size_t wordsLeft = words.size() - index;
    if (wordsLeft < static_cast<std::size_t>(remainingLines)) {
        return;
    }

    for (std::size_t end = index + 1; end <= words.size(); ++end) {
        const std::size_t groupedWords = end - index;
        const std::size_t remainingWords = words.size() - end;
        if (remainingWords < static_cast<std::size_t>(remainingLines - 1)) {
            break;
        }

        const std::string line = joinWords(words, index, end);
        if (measureText(line, scale) > maxWidth) {
            break;
        }

        currentLines.push_back(line);
        searchWordPartitions(words, end, remainingLines - 1, scale, maxWidth, preferredLines, currentLines, best);
        currentLines.pop_back();

        if (groupedWords == wordsLeft) {
            break;
        }
    }
}

WrappedText wrapWordGroups(const std::vector<std::string>& words, int scale, int maxWidth, int maxLines) {
    for (const auto& word : words) {
        if (measureText(word, scale) > maxWidth) {
            return {};
        }
    }

    const int preferredLines = std::clamp<int>(static_cast<int>(words.size()), 1, 3);
    PartitionCandidate best;
    std::vector<std::string> currentLines;

    const int maxCandidateLines = std::min<int>(maxLines, static_cast<int>(words.size()));
    for (int lineCount = 1; lineCount <= maxCandidateLines; ++lineCount) {
        searchWordPartitions(words, 0, lineCount, scale, maxWidth, preferredLines, currentLines, best);
    }

    return WrappedText{best.lines, 0, !best.lines.empty()};
}

WrappedText wrapText(const std::string& label, int scale, int maxWidth, int maxLines) {
    if (label.find('\n') == std::string::npos) {
        const auto words = splitWords(label);
        if (words.size() > 1) {
            const auto grouped = wrapWordGroups(words, scale, maxWidth, maxLines);
            if (!grouped.lines.empty()) {
                return grouped;
            }
        }
    }

    WrappedText wrapped;
    std::string paragraph;

    auto flushParagraph = [&](bool forcedBreak) {
        const auto words = splitWords(paragraph);
        std::string current;
        for (const auto& word : words) {
            if (measureText(word, scale) > maxWidth) {
                const auto pieces = breakWord(word, scale, maxWidth);
                for (const auto& piece : pieces) {
                    if (!current.empty()) {
                        wrapped.lines.push_back(current);
                        current.clear();
                    }
                    wrapped.lines.push_back(piece);
                    if (static_cast<int>(wrapped.lines.size()) > maxLines) {
                        return false;
                    }
                }
                wrapped.brokenWordCount += std::max<int>(static_cast<int>(pieces.size()) - 1, 0);
                continue;
            }

            const std::string candidate = current.empty() ? word : (current + " " + word);
            if (measureText(candidate, scale) <= maxWidth) {
                current = candidate;
                continue;
            }

            wrapped.lines.push_back(current);
            current = word;
            if (static_cast<int>(wrapped.lines.size()) > maxLines) {
                return false;
            }
        }
        if (!current.empty()) {
            wrapped.lines.push_back(current);
        } else if (forcedBreak) {
            wrapped.lines.push_back(" ");
        }
        return static_cast<int>(wrapped.lines.size()) <= maxLines;
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

    while (!wrapped.lines.empty() && wrapped.lines.back() == " ") {
        wrapped.lines.pop_back();
    }
    return wrapped;
}

int lineGapForScale(int scale) {
    return std::max(1, (scale + 1) / 3);
}

int scoreLayout(const Layout& layout, int maxWidth, int preferredLines) {
    if (layout.lines.empty()) {
        return -1000000;
    }

    int widestLine = 0;
    int narrowestLine = maxWidth;
    int widthSum = 0;
    int orphanPenalty = 0;
    for (const auto& line : layout.lines) {
        const int width = measureText(line, layout.scale);
        widestLine = std::max(widestLine, width);
        narrowestLine = std::min(narrowestLine, width);
        widthSum += width;

        const int visibleChars = visibleCharacterCount(line);
        if (visibleChars <= 1) {
            orphanPenalty += 1800;
        } else if (visibleChars == 2) {
            orphanPenalty += 900;
        } else if (visibleChars == 3 && layout.lines.size() > 2) {
            orphanPenalty += 250;
        }
    }

    const int lineHeight = layout.scale * 7;
    const int lineGap = lineGapForScale(layout.scale);
    const int totalHeight =
        (static_cast<int>(layout.lines.size()) * lineHeight) +
        (std::max<int>(static_cast<int>(layout.lines.size()) - 1, 0) * lineGap);
    const int averageWidth = widthSum / std::max<int>(static_cast<int>(layout.lines.size()), 1);
    const int raggedness = widestLine - narrowestLine;
    const int lineCountPenalty = std::max<int>(static_cast<int>(layout.lines.size()) - 1, 0) * 180;
    const int preferredLinePenalty = std::abs(static_cast<int>(layout.lines.size()) - preferredLines) * 450;
    const int extraLinePenalty = std::max<int>(static_cast<int>(layout.lines.size()) - 3, 0) * 400;
    const int brokenWordPenalty = layout.brokenWordCount * 5000;
    const int wordGroupingBonus = layout.usedWordGrouping ? 500 : 0;
    const int scaleScore = layout.scale * 1600;
    const int fillBonus = (averageWidth * 5) + (totalHeight * 10);
    const int widthWastePenalty = std::max(maxWidth - widestLine, 0) * 2;
    const int raggedPenalty = raggedness * 5;

    return scaleScore + fillBonus + wordGroupingBonus -
           widthWastePenalty - raggedPenalty - lineCountPenalty -
           preferredLinePenalty - extraLinePenalty - orphanPenalty -
           brokenWordPenalty;
}

Layout chooseLayout(const std::string& label, int width, int height, int maxTextScale) {
    const int horizontalPadding = 4;
    const int verticalPadding = 4;
    const int maxWidth = width - (horizontalPadding * 2);
    const int maxHeight = height - (verticalPadding * 2);
    const int preferredLines = preferredLineCount(label);
    const int scaleLimit = std::clamp(maxTextScale <= 0 ? 6 : maxTextScale, 1, 6);

    constexpr std::array<int, 5> kMaxLineOptions = {{1, 2, 3, 4, 5}};
    Layout bestLayout;
    for (int scale = scaleLimit; scale >= 1; --scale) {
        for (int maxLines : kMaxLineOptions) {
            auto wrapped = wrapText(label, scale, maxWidth, maxLines);
            if (wrapped.lines.empty()) {
                continue;
            }
            const int lineHeight = scale * 7;
            const int lineGap = lineGapForScale(scale);
            const int totalHeight =
                (static_cast<int>(wrapped.lines.size()) * lineHeight) +
                (std::max<int>(static_cast<int>(wrapped.lines.size()) - 1, 0) * lineGap);
            if (totalHeight <= maxHeight) {
                Layout candidate;
                candidate.scale = scale;
                candidate.lines = std::move(wrapped.lines);
                candidate.brokenWordCount = wrapped.brokenWordCount;
                candidate.usedWordGrouping = wrapped.usedWordGrouping;
                candidate.score = scoreLayout(candidate, maxWidth, preferredLines);
                if (bestLayout.lines.empty() || candidate.score > bestLayout.score) {
                    bestLayout = std::move(candidate);
                }
            }
        }
    }

    if (!bestLayout.lines.empty()) {
        return bestLayout;
    }

    auto wrapped = wrapText(label, 1, maxWidth, 5);
    if (wrapped.lines.empty()) {
        wrapped.lines = {"?"};
    }
    return Layout{1, std::move(wrapped.lines), wrapped.brokenWordCount, wrapped.usedWordGrouping, -1000000};
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
    const auto metrics = glyphMetrics(ch);
    if (metrics.empty) {
        return;
    }
    for (int row = 0; row < 7; ++row) {
        for (int col = metrics.left; col <= metrics.right; ++col) {
            if ((glyph.rows[static_cast<std::size_t>(row)] & (1u << (4 - col))) == 0) {
                continue;
            }
            drawRect(
                pixels,
                width,
                height,
                originX + ((col - metrics.left) * scale),
                originY + (row * scale),
                originX + (((col - metrics.left) + 1) * scale),
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
    RgbColor foreground,
    RgbColor accent,
    int maxTextScale) {
    const std::string prepared = toUpperAscii(label);
    const auto layout = chooseLayout(prepared, width, height, maxTextScale);

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width * height * 3), 0);
    fill(pixels, width, height, background);

    drawRect(pixels, width, height, 2, 2, width - 2, 3, lighten(accent, 6));
    drawRect(pixels, width, height, 2, height - 3, width - 2, height - 2, lighten(accent, 14));
    drawRect(pixels, width, height, 2, 2, 3, height - 2, accent);
    drawRect(pixels, width, height, width - 3, 2, width - 2, height - 2, accent);

    const int lineHeight = layout.scale * 7;
    const int lineGap = lineGapForScale(layout.scale);
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
