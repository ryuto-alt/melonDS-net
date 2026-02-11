#ifndef BMFONT_H
#define BMFONT_H

#include <QImage>
#include <QString>
#include <unordered_map>
#include <cstdint>

struct BMFontGlyph
{
    int x, y, width, height;
    int xoffset, yoffset, xadvance;
};

class BMFont
{
public:
    bool load(const QString& fntPath, const QString& pngPath);
    const BMFontGlyph* getGlyph(uint32_t codepoint) const;
    int getLineHeight() const { return lineHeight; }
    int getBase() const { return base; }

    QImage renderText(const char* utf8text, uint32_t color, bool rainbow, int rainbowStart, int* rainbowEnd, float scale = 1.0f);

private:
    QImage atlas;
    std::unordered_map<uint32_t, BMFontGlyph> glyphs;
    int lineHeight = 32;
    int base = 25;

    static uint32_t decodeUTF8(const char* text, int* bytesRead);
    int measureText(const char* utf8text, float scale);
    uint32_t rainbowColor(int inc);
};

#endif // BMFONT_H
