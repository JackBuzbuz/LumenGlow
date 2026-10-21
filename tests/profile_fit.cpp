// profile_fit.cpp - renders a white square over a grey background with the
// Lumen Glow engine and prints the brightness at fixed distances outside its
// edge, in screenshot pixels. Used to calibrate against Deep Glow renders.
//
//   profile_fit <zoom> <square_px_on_screen> <radius> <exposure> <falloff 0-1> [blend 1-4]
//
// zoom is the AE viewer zoom of the screenshot (1 = 100%).

#include "GlowCore.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr, "usage: profile_fit zoom square radius exposure falloff\n");
        return 1;
    }
    const double zoom = std::atof(argv[1]);
    const int square = int(std::lround(std::atof(argv[2]) / zoom)); // comp pixels
    lumen::GlowSettings s;
    s.radius = float(std::atof(argv[3]));
    s.exposure = float(std::atof(argv[4]));
    s.falloff = float(std::atof(argv[5]));
    s.whiteness = 0.3f;
    if (argc > 6) s.blend = lumen::BlendMode(std::atoi(argv[6]));
    if (argc > 7) s.gamma = float(std::atof(argv[7]));

    const int pad = lumen::glowMargin(s) + 8;
    const int size = square + 2 * pad;
    lumen::ImageRGBA src(size, size), dst;
    for (int y = pad; y < pad + square; ++y)
        for (int x = pad; x < pad + square; ++x) src.at(x, y) = lumen::RGBA{1, 1, 1, 1};
    lumen::renderGlow(src, dst, s);

    const float bg = 80.0f / 255.0f;
    const int cy = size / 2;
    const int edge = pad + square - 1; // last pixel inside the square
    const int dists[] = {2, 5, 10, 20, 30, 40, 60, 80, 100, 130, 160, 200, 250};
    for (int d : dists) {
        // Screenshot distance d -> comp distance d/zoom, sampled with linear interpolation.
        const double x = edge + d / zoom;
        const int x0 = int(std::floor(x));
        const double f = x - x0;
        double v = 0;
        for (int k = 0; k < 2; ++k) {
            const int xi = std::min(size - 1, x0 + k);
            const lumen::RGBA& p = dst.at(xi, cy);
            // Comp-over-background, matching how AE composites the layer.
            const double c = std::min(1.0, p.r + bg * (1.0 - p.a));
            v += (k ? f : 1 - f) * c;
        }
        std::printf("%d %.1f\n", d, v * 255.0);
    }
    return 0;
}
