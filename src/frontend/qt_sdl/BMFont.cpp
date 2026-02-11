#include "BMFont.h"
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QPainter>
#include <cstring>
#include <cmath>

bool BMFont::load(const QString& fntPath, const QString& pngPath)
{
    // Load atlas PNG
    if (!atlas.load(pngPath))
        return false;
    atlas = atlas.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    // Parse .fnt file
    QFile file(fntPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QTextStream in(&file);
    while (!in.atEnd())
    {
        QString line = in.readLine().trimmed();

        if (line.startsWith("common "))
        {
            // parse lineHeight and base
            for (const QString& token : line.split(' ', Qt::SkipEmptyParts))
            {
                if (token.startsWith("lineHeight="))
                    lineHeight = token.mid(11).toInt();
                else if (token.startsWith("base="))
                    base = token.mid(5).toInt();
            }
        }
        else if (line.startsWith("char ") && !line.startsWith("chars "))
        {
            BMFontGlyph g = {};
            int id = -1;

            for (const QString& token : line.split(' ', Qt::SkipEmptyParts))
            {
                if (token.startsWith("id="))        id = token.mid(3).toInt();
                else if (token.startsWith("x="))    g.x = token.mid(2).toInt();
                else if (token.startsWith("y="))    g.y = token.mid(2).toInt();
                else if (token.startsWith("width="))    g.width = token.mid(6).toInt();
                else if (token.startsWith("height="))   g.height = token.mid(7).toInt();
                else if (token.startsWith("xoffset="))  g.xoffset = token.mid(8).toInt();
                else if (token.startsWith("yoffset="))  g.yoffset = token.mid(8).toInt();
                else if (token.startsWith("xadvance=")) g.xadvance = token.mid(9).toInt();
            }

            if (id >= 0)
                glyphs[id] = g;
        }
    }

    return !glyphs.empty();
}

const BMFontGlyph* BMFont::getGlyph(uint32_t codepoint) const
{
    auto it = glyphs.find(codepoint);
    if (it != glyphs.end())
        return &it->second;
    return nullptr;
}

uint32_t BMFont::decodeUTF8(const char* text, int* bytesRead)
{
    uint8_t c = (uint8_t)text[0];

    if (c < 0x80)
    {
        *bytesRead = 1;
        return c;
    }
    else if ((c & 0xE0) == 0xC0)
    {
        *bytesRead = 2;
        return ((c & 0x1F) << 6) | (text[1] & 0x3F);
    }
    else if ((c & 0xF0) == 0xE0)
    {
        *bytesRead = 3;
        return ((c & 0x0F) << 12) | ((text[1] & 0x3F) << 6) | (text[2] & 0x3F);
    }
    else if ((c & 0xF8) == 0xF0)
    {
        *bytesRead = 4;
        return ((c & 0x07) << 18) | ((text[1] & 0x3F) << 12) | ((text[2] & 0x3F) << 6) | (text[3] & 0x3F);
    }

    *bytesRead = 1;
    return '?';
}

int BMFont::measureText(const char* utf8text, float scale)
{
    int w = 0;
    int i = 0;
    while (utf8text[i] != '\0')
    {
        int bytes;
        uint32_t cp = decodeUTF8(&utf8text[i], &bytes);
        i += bytes;

        const BMFontGlyph* g = getGlyph(cp);
        if (g)
            w += (int)(g->xadvance * scale);
        else
            w += (int)(lineHeight * scale / 2);
    }
    return w;
}

uint32_t BMFont::rainbowColor(int inc)
{
    if      (inc < 100) return 0xFFFF9B9B + (inc << 8);
    else if (inc < 200) return 0xFFFFFF9B - ((inc-100) << 16);
    else if (inc < 300) return 0xFF9BFF9B + (inc-200);
    else if (inc < 400) return 0xFF9BFFFF - ((inc-300) << 8);
    else if (inc < 500) return 0xFF9B9BFF + ((inc-400) << 16);
    else                return 0xFFFF9BFF - (inc-500);
}

QImage BMFont::renderText(const char* utf8text, uint32_t color, bool rainbow, int rainbowStart, int* rainbowEnd, float scale)
{
    int textW = measureText(utf8text, scale);
    int textH = (int)(lineHeight * scale);

    if (textW <= 0 || textH <= 0)
        return QImage();

    QImage result(textW + 2, textH + 2, QImage::Format_ARGB32_Premultiplied);
    result.fill(0);

    uint32_t* dst = (uint32_t*)result.bits();
    int dstStride = result.width();

    uint32_t rainbowinc = 0;
    if (rainbow)
    {
        if (rainbowStart == -1)
        {
            uint32_t ticks = (uint32_t)QDateTime::currentMSecsSinceEpoch();
            rainbowinc = ((utf8text[0] * 17) + (ticks * 13)) % 600;
        }
        else
            rainbowinc = (uint32_t)rainbowStart;
    }

    color |= 0xFF000000;
    const uint32_t shadow = 0xE0000000;

    int x = 1, y = 1;
    int i = 0;
    while (utf8text[i] != '\0')
    {
        int bytes;
        uint32_t cp = decodeUTF8(&utf8text[i], &bytes);
        i += bytes;

        const BMFontGlyph* g = getGlyph(cp);
        if (!g)
        {
            x += (int)(lineHeight * scale / 2);
            continue;
        }

        if (rainbow)
        {
            color = rainbowColor(rainbowinc);
            rainbowinc = (rainbowinc + 30) % 600;
        }

        int gw = (int)(g->width * scale);
        int gh = (int)(g->height * scale);
        int gx = x + (int)(g->xoffset * scale);
        int gy = y + (int)(g->yoffset * scale);

        // Blit glyph from atlas with color tinting
        for (int cy = 0; cy < gh; cy++)
        {
            int srcY = g->y + (int)(cy / scale);
            if (srcY >= atlas.height()) continue;
            int dstY = gy + cy;
            if (dstY < 0 || dstY >= result.height()) continue;

            const uint32_t* srcRow = (const uint32_t*)atlas.scanLine(srcY);

            for (int cx = 0; cx < gw; cx++)
            {
                int srcX = g->x + (int)(cx / scale);
                if (srcX >= atlas.width()) continue;
                int dstX = gx + cx;
                if (dstX < 0 || dstX >= result.width()) continue;

                uint32_t srcPixel = srcRow[srcX];
                uint32_t srcA = (srcPixel >> 24) & 0xFF;

                if (srcA > 0)
                {
                    // Apply color: use source alpha with our color
                    uint32_t r = ((color >> 16) & 0xFF) * srcA / 255;
                    uint32_t g2 = ((color >> 8) & 0xFF) * srcA / 255;
                    uint32_t b = (color & 0xFF) * srcA / 255;
                    dst[dstY * dstStride + dstX] = (srcA << 24) | (r << 16) | (g2 << 8) | b;
                }
            }
        }

        x += (int)(g->xadvance * scale);
    }

    if (rainbowEnd)
        *rainbowEnd = (int)rainbowinc;

    // Add shadow
    QImage shadowed(result.width(), result.height(), QImage::Format_ARGB32_Premultiplied);
    shadowed.fill(0);
    uint32_t* sdst = (uint32_t*)shadowed.bits();
    int sw = shadowed.width();
    int sh = shadowed.height();

    for (int sy = 0; sy < sh; sy++)
    {
        for (int sx = 0; sx < sw; sx++)
        {
            uint32_t val = dst[sy * sw + sx];
            if ((val >> 24) == 0xFF)
            {
                sdst[sy * sw + sx] = val;
                continue;
            }

            // Check neighbors for shadow
            uint32_t neighbor = 0;
            if (sx > 0)    neighbor |= dst[sy * sw + sx - 1];
            if (sx < sw-1) neighbor |= dst[sy * sw + sx + 1];
            if (sy > 0)    neighbor |= dst[(sy-1) * sw + sx];
            if (sy < sh-1) neighbor |= dst[(sy+1) * sw + sx];

            if (neighbor & 0xFF000000)
                sdst[sy * sw + sx] = shadow;
            else
                sdst[sy * sw + sx] = val;
        }
    }

    return shadowed;
}
