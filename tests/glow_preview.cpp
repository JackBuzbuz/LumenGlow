// glow_preview.cpp - renders test images with the Lumen Glow engine, without
// After Effects, and times a 1080p frame.
//
//   clang++ -std=c++17 -O2 -I../src ../src/GlowCore.cpp glow_preview.cpp -o glow_preview
//   ./glow_preview out_dir

#include "GlowCore.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using lumen::BlendMode;
using lumen::GlowSettings;
using lumen::ImageRGBA;
using lumen::RGBA;

namespace {

// 5x7 bitmap glyphs, enough for the test strings.
const char* glyph(char c) {
    switch (c) {
    case 'L': return "10000100001000010000100001000011111";
    case 'U': return "10001100011000110001100011000101110";
    case 'M': return "10001110111010110101100011000110001";
    case 'E': return "11111100001000011110100001000011111";
    case 'N': return "10001110011010110011100011000110001";
    case 'G': return "01110100011000010111100011000101111";
    case 'O': return "01110100011000110001100011000101110";
    case 'W': return "10001100011000110101101011101110001";
    default: return "00000000000000000000000000000000000";
    }
}

// Draws text with 4x4 supersampled antialiasing (premultiplied color).
void drawText(ImageRGBA& img, const char* text, int x0, int y0, int cell, const float color[3]) {
    const int ss = 4;
    const int n = int(std::strlen(text));
    for (int i = 0; i < n; ++i) {
        const char* g = glyph(text[i]);
        const int gx = x0 + i * 6 * cell;
        for (int py = y0; py < y0 + 7 * cell; ++py) {
            for (int px = gx; px < gx + 5 * cell; ++px) {
                if (px < 0 || py < 0 || px >= img.width || py >= img.height) continue;
                int hits = 0;
                for (int sy = 0; sy < ss; ++sy) {
                    for (int sx = 0; sx < ss; ++sx) {
                        // Rounded glyph cells: a disc per bit reads more like type than squares.
                        const float fx = (float(px - gx) + (sx + 0.5f) / ss) / cell;
                        const float fy = (float(py - y0) + (sy + 0.5f) / ss) / cell;
                        const int cx = int(fx), cy = int(fy);
                        if (cx < 0 || cx > 4 || cy < 0 || cy > 6 || g[cy * 5 + cx] != '1') continue;
                        hits++;
                    }
                }
                const float a = float(hits) / float(ss * ss);
                RGBA& p = img.at(px, py);
                p.r = p.r * (1 - a) + color[0] * a;
                p.g = p.g * (1 - a) + color[1] * a;
                p.b = p.b * (1 - a) + color[2] * a;
                p.a = p.a * (1 - a) + a;
            }
        }
    }
}

// Composites over a flat background and appends rows to an 8-bit RGB buffer.
void appendOver(const ImageRGBA& img, float bg, std::vector<unsigned char>& rgb) {
    for (const RGBA& p : img.pixels) {
        const float c[3] = {p.r + bg * (1 - p.a), p.g + bg * (1 - p.a), p.b + bg * (1 - p.a)};
        for (float v : c) rgb.push_back((unsigned char)(std::min(1.0f, std::max(0.0f, v)) * 255.0f + 0.5f));
    }
}

ImageRGBA textPanel(const float color[3], int w, int h) {
    ImageRGBA img(w, h);
    drawText(img, "LUMEN GLOW", 60, (h - 7 * 9) / 2, 9, color);
    return img;
}

} // namespace

int main(int argc, char** argv) {
    const std::string outDir = argc > 1 ? argv[1] : ".";
    const int W = 760, H = 170;
    const float white[3] = {1, 1, 1};
    const float red[3] = {0.9f, 0.08f, 0.06f};

    struct Case {
        const char* label;
        const float* color;
        GlowSettings s;
    };
    // Synthetic lens dirt: soft random specks.
    static std::vector<float> dirt(size_t(W) * size_t(H), 0.0f);
    {
        unsigned seed = 12345;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / float(1 << 24); };
        for (int i = 0; i < 90; ++i) {
            const float cx = rnd() * W, cy = rnd() * H, r = 3 + rnd() * 14, b = 0.3f + rnd() * 0.7f;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) / r;
                    if (d < 1) dirt[size_t(y) * W + x] = std::min(1.0f, dirt[size_t(y) * W + x] + b * (1 - d * d));
                }
        }
    }
    const float orange[3] = {1.0f, 0.45f, 0.1f};
    std::vector<Case> cases;
    auto add = [&](const char* label, const float* color, GlowSettings g) { cases.push_back({label, color, g}); };
    { GlowSettings s; add("default (Deep Glow Match)", white, s); }
    { GlowSettings s; s.glowMode = lumen::GlowMode::Iris; s.iris = lumen::makeIrisKernel(6, 0); s.radius = 500; s.exposure = 1.5f; s.threshold = 0.3f; add("iris: hexagon", white, s); }
    { GlowSettings s; s.glowMode = lumen::GlowMode::Iris; s.iris = lumen::makeIrisKernel(0, 0); s.radius = 500; s.exposure = 1.5f; s.threshold = 0.3f; add("iris: circle", white, s); }
    { GlowSettings s; s.tintMode = lumen::TintMode::InnerOuter; s.tintAmount = 1; s.tint[0]=1; s.tint[1]=0.85f; s.tint[2]=0.4f; s.tintOuter[0]=0.2f; s.tintOuter[1]=0.35f; s.tintOuter[2]=1; s.radius = 700; s.exposure = 1.5f; add("tint inner gold / outer blue", white, s); }
    { GlowSettings s; s.radiusMul[0] = 2.0f; s.radiusMul[2] = 0.5f; s.radius = 500; add("RGB radius: R x2, B x0.5", white, s); }
    { GlowSettings s; s.dirt = &dirt; s.dirtStrength = 1; s.radius = 900; s.exposure = 1.5f; add("lens dirt", white, s); }
    { GlowSettings s; s.blend = BlendMode::Add; s.exposure = 3; add("Add, hot, no tone map", orange, s); }
    { GlowSettings s; s.blend = BlendMode::Add; s.exposure = 3; s.toneMap = lumen::ToneMap::AcesFilmic; s.toneMapMethod = lumen::ToneMapMethod::PerChannel; add("ACES per channel", orange, s); }
    { GlowSettings s; s.blend = BlendMode::Add; s.exposure = 3; s.toneMap = lumen::ToneMap::AcesFilmic; s.toneMapMethod = lumen::ToneMapMethod::PreserveHue; add("ACES preserve hue", orange, s); }
    { GlowSettings s; s.saturationBias = 1; s.exposure = 1.5f; add("saturation bias +100", orange, s); }

    std::vector<unsigned char> rgb;
    for (const Case& c : cases) {
        ImageRGBA src = textPanel(c.color, W, H), dst;
        lumen::renderGlow(src, dst, c.s);
        appendOver(dst, 0.13f, rgb);
        std::printf("row %-34s margin %d px\n", c.label, lumen::glowMargin(c.s));
    }
    const std::string path = outDir + "/glow_preview.ppm";
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", W, H * int(cases.size()));
        std::fwrite(rgb.data(), 1, rgb.size(), f);
        std::fclose(f);
        std::printf("wrote %s\n", path.c_str());
    }

    // Timing: a 1080p frame with full-screen text-like content.
    ImageRGBA big(1920, 1080), out;
    for (int row = 0; row < 8; ++row) drawText(big, "LUMEN GLOW LUMEN GLOW", 40, 40 + row * 125, 12, white);
    for (float angle : {0.0f, 30.0f}) for (float radius : {150.0f, 550.0f, 1500.0f}) {
        GlowSettings s; s.radius = radius; s.angleDegrees = angle; s.aspect = angle != 0 ? 0.5f : 0.0f;
        const auto t0 = std::chrono::steady_clock::now();
        const int runs = 5;
        for (int i = 0; i < runs; ++i) lumen::renderGlow(big, out, s);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / runs;
        std::printf("1920x1080, radius %4.0f, angle %2.0f: %.1f ms/frame\n", radius, angle, ms);
    }
    return 0;
}
