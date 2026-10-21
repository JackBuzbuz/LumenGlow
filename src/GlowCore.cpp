// GlowCore.cpp - Lumen Glow rendering engine. See GlowCore.h for the method.

#include "GlowCore.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace lumen {
namespace {

// Approximate blur sigma of pyramid level k, in full-resolution pixels, is
// kSigmaPerLevel * 2^k. Radius is treated as ~3 sigma of the widest level.
constexpr float kSigmaPerLevel = 1.15f;
constexpr float kExtentSigmas = 3.0f;
// Upsampling tents widen the result slightly; the margin accounts for it.
constexpr float kReconstructionSpread = 1.25f;
// Stops of energy tilt per pyramid level at Falloff 0 or 1.
constexpr float kFalloffTilt = 1.5f;
// Calibration against Deep Glow 1.6.5 (Screen mode), fitted to a side-by-side
// AE render of a 470 px white square (tests/profile_fit.cpp): with these two
// constants and Falloff 39%, the same Radius and Exposure values produce the
// same glow as Deep Glow, within ~1.3/255 rms out to 200 px from the edge.
constexpr float kRadiusScale = 0.4788f;
constexpr float kBaseGainStops = 2.14f;
// Linear Light adds glow in linear light, which reads much hotter once
// converted for display; this offset keeps its overall strength comparable
// to the default when the option is switched on (fitted on test squares).
constexpr float kLinearLightGainStops = -1.3f;
// Lens dirt: at full strength a fully white dirt pixel brightens the glow
// passing through it this many times over.
constexpr float kDirtBoost = 4.0f;
// Iris kernels are wider than the 5-tap level blur; widen the margin to match.
constexpr float kIrisSpread = 2.5f;
// "QCA Look": Quick Chromatic Aberration darkens partly transparent pixels
// even with no shift. Measured on a white glow at three distances, it matches
// color x alpha^0.5 (displayed = alpha^1.5 + background x (1 - alpha)).
constexpr float kQcaAlphaExponent = 0.5f;
// Iris mode concentrates energy in the largest aperture (real bokeh is one
// dominant disc): extra stops of tilt per level on top of Falloff's.
constexpr float kIrisTilt = 1.0f;
// Below this many pixels a pass runs single-threaded (thread startup costs more).
constexpr int kParallelMinPixels = 1 << 16;

// Three-channel float plane (the glow carries no alpha of its own).
struct Plane {
    int w = 0;
    int h = 0;
    std::vector<float> d;

    Plane() = default;
    Plane(int W, int H) : w(W), h(H), d(size_t(W) * size_t(H) * 3, 0.0f) {}
    float* row(int y) { return d.data() + size_t(y) * size_t(w) * 3; }
    const float* row(int y) const { return d.data() + size_t(y) * size_t(w) * 3; }
};

template <class F>
void parallelRows(int rows, int cols, int threads, F&& fn) {
    if (threads <= 1 || size_t(rows) * size_t(cols) < size_t(kParallelMinPixels)) {
        fn(0, rows);
        return;
    }
    const int chunks = std::min(threads, rows);
    const int per = (rows + chunks - 1) / chunks;
    std::vector<std::thread> pool;
    pool.reserve(size_t(chunks));
    for (int i = 0; i < chunks; ++i) {
        const int a = i * per;
        const int b = std::min(rows, a + per);
        if (a >= b) break;
        pool.emplace_back([&fn, a, b] { fn(a, b); });
    }
    for (auto& t : pool) t.join();
}

inline float smoothstep(float e0, float e1, float x) {
    if (e1 <= e0) return x >= e1 ? 1.0f : 0.0f;
    const float t = std::min(1.0f, std::max(0.0f, (x - e0) / (e1 - e0)));
    return t * t * (3.0f - 2.0f * t);
}

inline float maxOf(float a, float b, float c) { return std::max(a, std::max(b, c)); }

float levelFloat(float radius) {
    return std::max(0.0f, std::log2(std::max(radius * kRadiusScale, 1.0f) / (kExtentSigmas * kSigmaPerLevel)));
}

const float kBinomial5[5] = {1.0f / 16, 4.0f / 16, 6.0f / 16, 4.0f / 16, 1.0f / 16};
const float kTent3[3] = {0.25f, 0.5f, 0.25f};

// Separable blur with kernel taps at multiples of `step` (fractional steps are
// linearly interpolated, which is how Aspect Ratio stretches the glow).
// Pixels outside the plane count as black so the glow fades into the padding.
void blurAxis(const Plane& src, Plane& dst, bool horizontal, float step, const float* kernel, int radius, int threads) {
    dst = Plane(src.w, src.h);
    const int len = horizontal ? src.w : src.h;
    parallelRows(src.h, src.w, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* out = dst.row(y);
            for (int x = 0; x < src.w; ++x) {
                float r = 0, g = 0, b = 0;
                const int pos = horizontal ? x : y;
                for (int t = -radius; t <= radius; ++t) {
                    const float p = float(pos) + float(t) * step;
                    const int i0 = int(std::floor(p));
                    const float f = p - float(i0);
                    const float k = kernel[t + radius];
                    for (int side = 0; side < 2; ++side) {
                        const int i = i0 + side;
                        const float wgt = k * (side ? f : 1.0f - f);
                        if (i < 0 || i >= len || wgt == 0.0f) continue;
                        const float* s = horizontal ? src.row(y) + size_t(i) * 3 : src.row(i) + size_t(x) * 3;
                        r += s[0] * wgt;
                        g += s[1] * wgt;
                        b += s[2] * wgt;
                    }
                }
                out[x * 3 + 0] = r;
                out[x * 3 + 1] = g;
                out[x * 3 + 2] = b;
            }
        }
    });
}

void blur(Plane& p, float stepX, float stepY, const float* kernel, int radius, int threads) {
    Plane tmp;
    blurAxis(p, tmp, true, stepX, kernel, radius, threads);
    blurAxis(tmp, p, false, stepY, kernel, radius, threads);
}

// Blur along an arbitrary direction (dx, dy) per tap, with bilinear sampling.
// Used for Aspect Ratio at a non-zero Angle.
void blurDirection(const Plane& src, Plane& dst, float dx, float dy, const float* kernel, int radius, int threads) {
    dst = Plane(src.w, src.h);
    parallelRows(src.h, src.w, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* out = dst.row(y);
            for (int x = 0; x < src.w; ++x) {
                float acc[3] = {0, 0, 0};
                for (int t = -radius; t <= radius; ++t) {
                    const float px = float(x) + float(t) * dx;
                    const float py = float(y) + float(t) * dy;
                    const int x0 = int(std::floor(px)), y0i = int(std::floor(py));
                    const float fx = px - float(x0), fy = py - float(y0i);
                    const float k = kernel[t + radius];
                    for (int j = 0; j < 2; ++j) {
                        const int sy = y0i + j;
                        if (sy < 0 || sy >= src.h) continue;
                        const float wy = j ? fy : 1.0f - fy;
                        for (int i = 0; i < 2; ++i) {
                            const int sx = x0 + i;
                            if (sx < 0 || sx >= src.w) continue;
                            const float w = k * wy * (i ? fx : 1.0f - fx);
                            if (w == 0.0f) continue;
                            const float* sp = src.row(sy) + size_t(sx) * 3;
                            acc[0] += sp[0] * w;
                            acc[1] += sp[1] * w;
                            acc[2] += sp[2] * w;
                        }
                    }
                }
                out[x * 3 + 0] = acc[0];
                out[x * 3 + 1] = acc[1];
                out[x * 3 + 2] = acc[2];
            }
        }
    });
}

// Per-level blur, stretched by Aspect Ratio along Angle.
struct LevelBlur {
    float along = 1.0f, across = 1.0f; // tap spacing along / across the stretch direction
    float cosA = 1.0f, sinA = 0.0f;
    bool rotated = false;

    LevelBlur(float aspect, float angleDegrees) {
        const float a = std::min(1.0f, std::max(-1.0f, aspect));
        along = std::pow(2.0f, a);
        across = std::pow(2.0f, -a);
        const float rad = angleDegrees * 3.14159265f / 180.0f;
        cosA = std::cos(rad);
        sinA = std::sin(rad);
        rotated = std::fabs(a) > 1e-4f && std::fabs(sinA) > 1e-4f && std::fabs(cosA) > 1e-4f;
        if (!rotated && std::fabs(a) > 1e-4f && std::fabs(sinA) > std::fabs(cosA)) std::swap(along, across);
    }

    void apply(Plane& p, int threads) const {
        if (!rotated) {
            blur(p, along, across, kBinomial5, 2, threads); // axis-aligned fast path
            return;
        }
        Plane tmp;
        blurDirection(p, tmp, cosA * along, sinA * along, kBinomial5, 2, threads);
        blurDirection(tmp, p, -sinA * across, cosA * across, kBinomial5, 2, threads);
    }
};

// 2x2 box downsample; odd edges average with black (the padding).
Plane downsample(const Plane& src, int threads) {
    Plane dst((src.w + 1) / 2, (src.h + 1) / 2);
    parallelRows(dst.h, dst.w, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* out = dst.row(y);
            for (int x = 0; x < dst.w; ++x) {
                float acc[3] = {0, 0, 0};
                for (int dy = 0; dy < 2; ++dy) {
                    const int sy = 2 * y + dy;
                    if (sy >= src.h) continue;
                    for (int dx = 0; dx < 2; ++dx) {
                        const int sx = 2 * x + dx;
                        if (sx >= src.w) continue;
                        const float* s = src.row(sy) + size_t(sx) * 3;
                        acc[0] += s[0];
                        acc[1] += s[1];
                        acc[2] += s[2];
                    }
                }
                out[x * 3 + 0] = acc[0] * 0.25f;
                out[x * 3 + 1] = acc[1] * 0.25f;
                out[x * 3 + 2] = acc[2] * 0.25f;
            }
        }
    });
    return dst;
}

// Bilinear 2x upsample to an exact target size (inverse of downsample's grid).
Plane upsample(const Plane& src, int w, int h, int threads) {
    Plane dst(w, h);
    parallelRows(h, w, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float v = (float(y) + 0.5f) * 0.5f - 0.5f;
            const int vy0 = std::min(src.h - 1, std::max(0, int(std::floor(v))));
            const int vy1 = std::min(src.h - 1, vy0 + 1);
            const float fy = std::min(1.0f, std::max(0.0f, v - float(vy0)));
            float* out = dst.row(y);
            for (int x = 0; x < w; ++x) {
                const float u = (float(x) + 0.5f) * 0.5f - 0.5f;
                const int ux0 = std::min(src.w - 1, std::max(0, int(std::floor(u))));
                const int ux1 = std::min(src.w - 1, ux0 + 1);
                const float fx = std::min(1.0f, std::max(0.0f, u - float(ux0)));
                const float* a = src.row(vy0) + size_t(ux0) * 3;
                const float* b = src.row(vy0) + size_t(ux1) * 3;
                const float* c = src.row(vy1) + size_t(ux0) * 3;
                const float* d = src.row(vy1) + size_t(ux1) * 3;
                for (int ch = 0; ch < 3; ++ch) {
                    const float top = a[ch] + (b[ch] - a[ch]) * fx;
                    const float bot = c[ch] + (d[ch] - c[ch]) * fx;
                    out[x * 3 + ch] = top + (bot - top) * fy;
                }
            }
        }
    });
    return dst;
}

// Soft threshold and tint, at full resolution.
Plane brightPass(const ImageRGBA& src, const GlowSettings& s, const std::vector<float>* mask, int threads) {
    Plane p(src.width, src.height);
    const float knee = std::max(0.0f, s.thresholdSmooth) * 0.5f;
    const float t0 = s.threshold - knee;
    const float t1 = s.threshold + knee;
    const float tint = std::min(1.0f, std::max(0.0f, s.tintAmount));
    const float bias = std::min(1.0f, std::max(-1.0f, s.saturationBias));
    parallelRows(src.height, src.width, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* out = p.row(y);
            for (int x = 0; x < src.width; ++x) {
                const RGBA& c = src.at(x, y);
                const float m = maxOf(c.r, c.g, c.b);
                if (m <= 0.0f) continue;
                const float maskValue = mask ? (*mask)[size_t(y) * size_t(src.width) + size_t(x)] : 1.0f;
                if (maskValue <= 0.0f) continue;
                const float w = maskValue * (s.threshold <= 0.0f && knee <= 0.0f ? 1.0f : smoothstep(t0, t1, m));
                float r = c.r * w, g = c.g * w, b = c.b * w;
                if (bias != 0.0f) {
                    const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    r = std::max(0.0f, lum + (r - lum) * (1.0f + bias));
                    g = std::max(0.0f, lum + (g - lum) * (1.0f + bias));
                    b = std::max(0.0f, lum + (b - lum) * (1.0f + bias));
                }
                if (tint > 0.0f) {
                    // Neutralize toward the pixel's brightness; the tint color
                    // itself is applied per pyramid level (so inner/outer works).
                    const float mw = maxOf(r, g, b);
                    r += (mw - r) * tint;
                    g += (mw - g) * tint;
                    b += (mw - b) * tint;
                }
                out[x * 3 + 0] = r;
                out[x * 3 + 1] = g;
                out[x * 3 + 2] = b;
            }
        }
    });
    return p;
}

// Light scattering softens the object itself a little, not just its
// surroundings: a 1 px binomial blur of the source (alpha included), which
// rounds hard corners by ~2 px the way Deep Glow does.
ImageRGBA scatterSource(const ImageRGBA& src, int threads) {
    ImageRGBA tmp(src.width, src.height), out(src.width, src.height);
    auto pass = [&](const ImageRGBA& in, ImageRGBA& o, bool horizontal) {
        parallelRows(in.height, in.width, threads, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                for (int x = 0; x < in.width; ++x) {
                    RGBA acc{0, 0, 0, 0};
                    for (int t = -2; t <= 2; ++t) {
                        const int sx = horizontal ? x + t : x;
                        const int sy = horizontal ? y : y + t;
                        if (sx < 0 || sy < 0 || sx >= in.width || sy >= in.height) continue;
                        const RGBA& p = in.at(sx, sy);
                        const float k = kBinomial5[t + 2];
                        acc.r += p.r * k;
                        acc.g += p.g * k;
                        acc.b += p.b * k;
                        acc.a += p.a * k;
                    }
                    o.at(x, y) = acc;
                }
            }
        });
    };
    pass(src, tmp, true);
    pass(tmp, out, false);
    return out;
}

// Screen-mode soft clip: the glow approaches white asymptotically instead of
// clipping, so no hard contour forms where it saturates (that contour bulges
// at the middle of straight edges and looks like the shape is warping).
// Feathers (and optionally inverts) the glow mask: three box-blur passes per
// axis approximate a Gaussian whose soft edge is about `feather` pixels wide.
std::vector<float> prepareMask(const std::vector<float>& in, int w, int h, float feather, bool invert, int threads) {
    std::vector<float> m = in, tmp(in.size());
    const int r = int(std::lround(std::max(0.0f, feather) / 3.0f));
    auto pass = [&](const std::vector<float>& src, std::vector<float>& dst, bool horizontal) {
        const int len = horizontal ? w : h, lines = horizontal ? h : w;
        const float inv = 1.0f / float(2 * r + 1);
        parallelRows(lines, len, threads, [&](int l0, int l1) {
            for (int l = l0; l < l1; ++l) {
                auto at = [&](int k) { return horizontal ? src[size_t(l) * size_t(w) + size_t(k)] : src[size_t(k) * size_t(w) + size_t(l)]; };
                float sum = 0.0f;
                for (int k = 0; k <= std::min(r, len - 1); ++k) sum += at(k);
                for (int k = 0; k < len; ++k) {
                    (horizontal ? dst[size_t(l) * size_t(w) + size_t(k)] : dst[size_t(k) * size_t(w) + size_t(l)]) = sum * inv;
                    if (k + r + 1 < len) sum += at(k + r + 1);
                    if (k - r >= 0) sum -= at(k - r);
                }
            }
        });
    };
    if (r > 0)
        for (int i = 0; i < 3; ++i) {
            pass(m, tmp, true);
            pass(tmp, m, false);
        }
    if (invert)
        for (float& v : m) v = 1.0f - v;
    for (float& v : m) v = std::min(1.0f, std::max(0.0f, v));
    return m;
}

// Brightness compensation for Gamma Correction, scaled so it's -1.3 stops at
// gamma 2.2 (where it was fitted) and fades to 0 at gamma 1.
inline float linearGainOffset(float gamma) {
    return kLinearLightGainStops * std::min(1.5f, std::max(0.0f, (gamma - 1.0f) / 1.2f));
}

inline float softClip(float v) { return v > 0.0f ? 1.0f - std::exp(-v) : 0.0f; }

// 2D convolution with an aperture kernel (zero taps skipped).
void convolveIris(Plane& p, const IrisKernel& k, int threads) {
    struct Tap { int dx, dy; float w; };
    std::vector<Tap> taps;
    const int r = k.size / 2;
    for (int j = 0; j < k.size; ++j)
        for (int i = 0; i < k.size; ++i) {
            const float w = k.weights[size_t(j) * size_t(k.size) + size_t(i)];
            if (w > 0.0f) taps.push_back({i - r, j - r, w});
        }
    Plane dst(p.w, p.h);
    parallelRows(p.h, p.w, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* out = dst.row(y);
            for (int x = 0; x < p.w; ++x) {
                float acc[3] = {0, 0, 0};
                for (const Tap& t : taps) {
                    const int sx = x + t.dx, sy = y + t.dy;
                    if (sx < 0 || sy < 0 || sx >= p.w || sy >= p.h) continue;
                    const float* sp = p.row(sy) + size_t(sx) * 3;
                    acc[0] += sp[0] * t.w;
                    acc[1] += sp[1] * t.w;
                    acc[2] += sp[2] * t.w;
                }
                out[x * 3 + 0] = acc[0];
                out[x * 3 + 1] = acc[1];
                out[x * 3 + 2] = acc[2];
            }
        }
    });
    p = std::move(dst);
}

void normalizeKernel(IrisKernel& k) {
    float sum = 0.0f;
    for (float w : k.weights) sum += w;
    if (sum <= 0.0f) {
        k = makeIrisKernel(0, 0.0f, k.size > 0 ? k.size : 9);
        return;
    }
    for (float& w : k.weights) w /= sum;
}

// Hable / Uncharted 2 filmic curve.
inline float hable(float x) {
    const float A = 0.15f, B = 0.50f, C = 0.10f, D = 0.20f, E = 0.02f, F = 0.30f;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}
// Narkowicz ACES filmic fit.
inline float aces(float x) { return (x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f); }

// Tone curve normalized so `white` maps to 1.
float toneCurve(ToneMap tm, float x, float white) {
    x = std::max(0.0f, x);
    switch (tm) {
    case ToneMap::Reinhard: return x * (1.0f + x / (white * white)) / (1.0f + x);
    case ToneMap::HableFilmic: return hable(2.0f * x) / hable(2.0f * white);
    case ToneMap::AcesFilmic: return aces(x) / aces(white);
    case ToneMap::None:
    default: return x;
    }
}

void toneMapPixel(RGBA& o, const GlowSettings& s) {
    if (s.toneMap == ToneMap::None) return;
    const float white = std::max(1.0f, s.whitePoint);
    const float mix = std::min(1.0f, std::max(0.0f, s.toneMapMix));
    float r, g, b;
    if (s.toneMapMethod == ToneMapMethod::PreserveHue) {
        const float m = maxOf(o.r, o.g, o.b);
        const float k = m > 0.0f ? toneCurve(s.toneMap, m, white) / m : 0.0f;
        r = o.r * k;
        g = o.g * k;
        b = o.b * k;
    } else {
        r = toneCurve(s.toneMap, o.r, white);
        g = toneCurve(s.toneMap, o.g, white);
        b = toneCurve(s.toneMap, o.b, white);
    }
    r = std::min(r, 1.0f);
    g = std::min(g, 1.0f);
    b = std::min(b, 1.0f);
    o.r += (r - o.r) * mix;
    o.g += (g - o.g) * mix;
    o.b += (b - o.b) * mix;
}

} // namespace

IrisKernel makeIrisKernel(int sides, float rotationDegrees, int size) {
    IrisKernel k;
    k.size = std::max(3, size | 1);
    k.weights.assign(size_t(k.size) * size_t(k.size), 0.0f);
    const float R = float(k.size) * 0.5f;
    const float rot = rotationDegrees * 3.14159265f / 180.0f;
    const int ss = 4;
    for (int j = 0; j < k.size; ++j)
        for (int i = 0; i < k.size; ++i) {
            int hits = 0;
            for (int sy = 0; sy < ss; ++sy)
                for (int sx = 0; sx < ss; ++sx) {
                    const float x = float(i) + (float(sx) + 0.5f) / ss - R;
                    const float y = float(j) + (float(sy) + 0.5f) / ss - R;
                    const float rr = std::sqrt(x * x + y * y);
                    bool inside;
                    if (sides < 3) {
                        inside = rr <= R;
                    } else {
                        // Regular polygon: distance to the nearest edge's apothem.
                        const float seg = 2.0f * 3.14159265f / float(sides);
                        float a = std::atan2(y, x) - rot;
                        a = a - seg * std::floor(a / seg) - seg * 0.5f;
                        inside = rr * std::cos(a) <= R * std::cos(seg * 0.5f);
                    }
                    hits += inside ? 1 : 0;
                }
            k.weights[size_t(j) * size_t(k.size) + size_t(i)] = float(hits) / float(ss * ss);
        }
    normalizeKernel(k);
    return k;
}

IrisKernel irisKernelFromImage(const ImageRGBA& img, float rotationDegrees, int size) {
    IrisKernel k;
    k.size = std::max(3, size | 1);
    k.weights.assign(size_t(k.size) * size_t(k.size), 0.0f);
    if (img.width <= 0 || img.height <= 0) return makeIrisKernel(0, 0.0f, k.size);
    // Fit the image's larger side into the kernel, centered, and sample its
    // luminance (rotated) with 4x4 supersampling.
    const float side = float(std::max(img.width, img.height));
    const float cx = float(img.width) * 0.5f, cy = float(img.height) * 0.5f;
    const float rot = -rotationDegrees * 3.14159265f / 180.0f;
    const float cr = std::cos(rot), sr = std::sin(rot);
    const int ss = 4;
    for (int j = 0; j < k.size; ++j)
        for (int i = 0; i < k.size; ++i) {
            float acc = 0.0f;
            for (int sy = 0; sy < ss; ++sy)
                for (int sx = 0; sx < ss; ++sx) {
                    const float u = ((float(i) + (float(sx) + 0.5f) / ss) / float(k.size) - 0.5f) * side;
                    const float v = ((float(j) + (float(sy) + 0.5f) / ss) / float(k.size) - 0.5f) * side;
                    const int px = int(std::floor(cx + u * cr - v * sr));
                    const int py = int(std::floor(cy + u * sr + v * cr));
                    if (px < 0 || py < 0 || px >= img.width || py >= img.height) continue;
                    const RGBA& c = img.at(px, py);
                    acc += 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
                }
            k.weights[size_t(j) * size_t(k.size) + size_t(i)] = acc / float(ss * ss);
        }
    normalizeKernel(k);
    return k;
}

int glowMargin(const GlowSettings& s) {
    const float maxMul = std::max(1e-3f, maxOf(s.radiusMul[0], s.radiusMul[1], s.radiusMul[2]));
    const float levels =
        std::ceil(std::max(0.0f, levelFloat(s.radius) + std::log2(maxMul) + std::max(0.0f, s.chromaticAberration)));
    // Aspect scales every level's blur taps by up to 2x on one axis; iris
    // kernels are wider than the level blur.
    const float stretch = std::pow(2.0f, std::min(1.0f, std::fabs(s.aspect))) *
                          (s.glowMode == GlowMode::Iris ? kIrisSpread : 1.0f);
    // How many sigmas out the widest level stays visible (>= 1/512) at this
    // exposure: a hotter glow reaches further before it fades out.
    const float gain = std::pow(2.0f, s.exposure + kBaseGainStops + linearGainOffset(s.gamma));
    // Visibility floor is 1/512 on screen; in Linear Light that's far dimmer in linear terms.
    const float floorInv = std::pow(512.0f, std::max(1.0f, s.gamma));
    const float sigmas = std::max(kExtentSigmas, std::sqrt(2.0f * std::log(std::max(2.0f, floorInv * gain))));
    const float extent = sigmas * kSigmaPerLevel * std::pow(2.0f, levels) * kReconstructionSpread * stretch;
    const float aberration = s.aberration.target == AberrationTarget::Off ? 0.0f : std::max(0.0f, s.aberration.reach);
    return int(std::ceil(extent + aberration)) + 4;
}

// Converts premultiplied color between display gamma and linear light.
void convertGamma(const ImageRGBA& in, ImageRGBA& out, float exponent, int threads) {
    out = ImageRGBA(in.width, in.height);
    parallelRows(in.height, in.width, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < in.width; ++x) {
                const RGBA& p = in.at(x, y);
                RGBA& o = out.at(x, y);
                o.a = p.a;
                if (p.a <= 0.0f) { o.r = o.g = o.b = 0.0f; continue; }
                const float inv = 1.0f / p.a;
                o.r = std::pow(std::max(0.0f, p.r * inv), exponent) * p.a;
                o.g = std::pow(std::max(0.0f, p.g * inv), exponent) * p.a;
                o.b = std::pow(std::max(0.0f, p.b * inv), exponent) * p.a;
            }
        }
    });
}

// ------------------------------------------------------- chromatic aberration

namespace {

// Hue (0..1, wraps) to a fully saturated RGB primary blend.
void hueToRgb(float h, float out[3]) {
    h -= std::floor(h);
    out[0] = std::min(1.0f, std::max(0.0f, std::fabs(h * 6.0f - 3.0f) - 1.0f));
    out[1] = std::min(1.0f, std::max(0.0f, 2.0f - std::fabs(h * 6.0f - 2.0f)));
    out[2] = std::min(1.0f, std::max(0.0f, 2.0f - std::fabs(h * 6.0f - 4.0f)));
}

// Bilinear premultiplied sample with either repeated edges or transparency.
inline RGBA sampleRGBA(const ImageRGBA& img, float x, float y, bool repeatEdges) {
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const float fx = x - float(x0), fy = y - float(y0);
    RGBA acc{0, 0, 0, 0};
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            int sx = x0 + i, sy = y0 + j;
            if (repeatEdges) {
                sx = std::min(img.width - 1, std::max(0, sx));
                sy = std::min(img.height - 1, std::max(0, sy));
            } else if (sx < 0 || sy < 0 || sx >= img.width || sy >= img.height) {
                continue;
            }
            const float w = (i ? fx : 1.0f - fx) * (j ? fy : 1.0f - fy);
            const RGBA& p = img.at(sx, sy);
            acc.r += p.r * w;
            acc.g += p.g * w;
            acc.b += p.b * w;
            acc.a += p.a * w;
        }
    return acc;
}

// Separable box blur (O(1) per pixel at any radius), applied twice for a
// near-Gaussian result.
void boxBlurRGBA(ImageRGBA& img, int radius, int threads) {
    if (radius <= 0) return;
    ImageRGBA tmp(img.width, img.height);
    auto pass = [&](const ImageRGBA& in, ImageRGBA& out, bool horizontal) {
        const int len = horizontal ? in.width : in.height;
        const int lines = horizontal ? in.height : in.width;
        const float inv = 1.0f / float(2 * radius + 1);
        parallelRows(lines, len, threads, [&](int l0, int l1) {
            for (int l = l0; l < l1; ++l) {
                auto at = [&](int k) -> const RGBA& { return horizontal ? in.at(k, l) : in.at(l, k); };
                RGBA sum{0, 0, 0, 0};
                for (int k = -radius; k <= radius; ++k) {
                    if (k < 0 || k >= len) continue;
                    const RGBA& p = at(k);
                    sum.r += p.r; sum.g += p.g; sum.b += p.b; sum.a += p.a;
                }
                for (int k = 0; k < len; ++k) {
                    RGBA& o = horizontal ? out.at(k, l) : out.at(l, k);
                    o = RGBA{sum.r * inv, sum.g * inv, sum.b * inv, sum.a * inv};
                    const int add = k + radius + 1, sub = k - radius;
                    if (add < len) { const RGBA& p = at(add); sum.r += p.r; sum.g += p.g; sum.b += p.b; sum.a += p.a; }
                    if (sub >= 0) { const RGBA& p = at(sub); sum.r -= p.r; sum.g -= p.g; sum.b -= p.b; sum.a -= p.a; }
                }
            }
        });
    };
    for (int iter = 0; iter < 2; ++iter) {
        pass(img, tmp, true);
        pass(tmp, img, false);
    }
}

} // namespace

void applyAberration(ImageRGBA& img, const Aberration& a, int threads) {
    if (a.target == AberrationTarget::Off || img.width <= 0 || img.height <= 0) return;
    if (threads <= 0) threads = int(std::max(1u, std::min(16u, std::thread::hardware_concurrency())));
    const int n = std::min(64, std::max(3, a.iterations));
    const float amt = a.amount;
    const float halfDiag = 0.5f * std::sqrt(float(img.width) * float(img.width) + float(img.height) * float(img.height));
    const float invR2 = 1.0f / std::max(1.0f, halfDiag * halfDiag);

    // Per-sample color weights around the hue wheel, normalized so the
    // samples add back up to the original image (white stays white).
    std::vector<float> col(size_t(n) * 3);
    float sum[3] = {0, 0, 0};
    for (int i = 0; i < n; ++i) {
        float* ci = &col[size_t(i) * 3];
        if (a.customSpectrum) {
            // Custom Colors: blend start -> middle -> end across the samples.
            const float u = float(i) / float(n - 1);
            const float* c0 = a.spectrumColors[u < 0.5f ? 0 : 1];
            const float* c1 = a.spectrumColors[u < 0.5f ? 1 : 2];
            const float f = u < 0.5f ? u * 2.0f : (u - 0.5f) * 2.0f;
            for (int c = 0; c < 3; ++c) ci[c] = std::max(0.0f, c0[c] + (c1[c] - c0[c]) * f);
        } else {
            hueToRgb(a.hueOffsetDegrees / 360.0f + float(i) / float(n), ci);
        }
        for (int c = 0; c < 3; ++c) sum[c] += ci[c];
    }
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c) {
            // A channel no chosen color contains is kept, just not split, so
            // the image never loses a color.
            float& w = col[size_t(i) * 3 + c];
            w = sum[c] > 1e-4f ? w / sum[c] : 1.0f / float(n);
        }

    const ImageRGBA src = img;
    ImageRGBA acc(img.width, img.height), sample;
    for (int i = 0; i < n; ++i) {
        const float t = (2.0f * float(i) / float(n - 1) - 1.0f) * amt; // -amount .. +amount
        const float scale = 1.0f + t * a.scale * 0.01f;
        const float rot = -t * a.rotationDegrees * 3.14159265f / 180.0f;
        const float cr = std::cos(rot), sr = std::sin(rot);
        const float dist = t * a.distortion * 0.01f;
        const float skew = t * a.skew * 0.01f;
        const float sx = t * a.shiftX, sy = t * a.shiftY;
        const int blur = int(std::lround(std::fabs(t) * a.blur * 0.5f));
        const float* w = &col[size_t(i) * 3];

        // Inverse map: where in the source each output pixel of this sample comes from.
        sample = ImageRGBA(img.width, img.height);
        parallelRows(img.height, img.width, threads, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < img.width; ++x) {
                    float dx = float(x) - a.centerX - sx, dy = float(y) - a.centerY - sy;
                    const float k = 1.0f / (std::max(0.05f, scale) * std::max(0.05f, 1.0f + dist * (dx * dx + dy * dy) * invR2));
                    dx *= k;
                    dy *= k;
                    float rx = dx * cr - dy * sr, ry = dx * sr + dy * cr;
                    rx -= skew * ry;
                    sample.at(x, y) = sampleRGBA(src, a.centerX + rx, a.centerY + ry, a.repeatEdges);
                }
        });
        boxBlurRGBA(sample, blur, threads);
        parallelRows(img.height, img.width, threads, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < img.width; ++x) {
                    const RGBA& p = sample.at(x, y);
                    RGBA& o = acc.at(x, y);
                    o.r += p.r * w[0];
                    o.g += p.g * w[1];
                    o.b += p.b * w[2];
                    o.a += p.a / float(n);
                }
        });
    }

    parallelRows(img.height, img.width, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < img.width; ++x) {
                RGBA& o = acc.at(x, y);
                // Colored fringes over transparency need enough alpha to show.
                o.a = a.preserveAlpha ? src.at(x, y).a : std::min(1.0f, std::max(o.a, maxOf(o.r, o.g, o.b)));
                img.at(x, y) = o;
            }
    });
}

void renderGlowLinear(const ImageRGBA& src, ImageRGBA& dst, const GlowSettings& s, int threads);

void renderGlow(const ImageRGBA& src, ImageRGBA& dst, const GlowSettings& s, int threads) {
    if (threads <= 0) threads = int(std::max(1u, std::min(16u, std::thread::hardware_concurrency())));
    if (std::fabs(s.gamma - 1.0f) < 1e-3f) {
        renderGlowLinear(src, dst, s, threads);
        return;
    }
    ImageRGBA lin, out;
    GlowSettings ls = s;
    ls.exposure += linearGainOffset(s.gamma);
    convertGamma(src, lin, s.gamma, threads);
    renderGlowLinear(lin, out, ls, threads);
    // Encode for display. The glow is light, so its premultiplied values are
    // encoded directly (what it would look like over black), and alpha grows
    // to cover the encoded glow; opaque source pixels are unaffected.
    const float inv = 1.0f / s.gamma;
    dst = ImageRGBA(out.width, out.height);
    parallelRows(out.height, out.width, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < out.width; ++x) {
                const RGBA& p = out.at(x, y);
                RGBA& o = dst.at(x, y);
                o.r = std::pow(std::max(0.0f, p.r), inv);
                o.g = std::pow(std::max(0.0f, p.g), inv);
                o.b = std::pow(std::max(0.0f, p.b), inv);
                o.a = std::min(1.0f, std::max(p.a, maxOf(o.r, o.g, o.b)));
            }
        }
    });
}

void renderGlowLinear(const ImageRGBA& src, ImageRGBA& dst, const GlowSettings& s, int threads) {
    dst = ImageRGBA(src.width, src.height);
    if (src.width <= 0 || src.height <= 0) return;

    const LevelBlur levelBlur(s.aspect, s.angleDegrees);

    std::vector<float> maskStorage;
    const std::vector<float>* mask = nullptr;
    if (s.mask && s.mask->size() == size_t(src.width) * size_t(src.height)) {
        maskStorage = prepareMask(*s.mask, src.width, src.height, s.maskFeather, s.maskInvert, threads);
        mask = &maskStorage;
    }

    // 1-2. Bright pass and pyramid.
    std::vector<Plane> levels;
    levels.push_back(brightPass(src, s, mask, threads));

    if (s.view == View::Highlights) {
        // Threshold preview: show exactly what will glow.
        const Plane& hp = levels[0];
        for (int y = 0; y < src.height; ++y)
            for (int x = 0; x < src.width; ++x) {
                const float* c = hp.row(y) + size_t(x) * 3;
                dst.at(x, y) = RGBA{c[0], c[1], c[2], std::min(1.0f, std::max(src.at(x, y).a, maxOf(c[0], c[1], c[2])))};
            }
        return;
    }
    levelBlur.apply(levels[0], threads);

    const float ca = std::min(1.0f, std::max(0.0f, s.chromaticAberration));
    const float kf = levelFloat(s.radius);
    const bool iris = s.glowMode == GlowMode::Iris && s.iris.size > 0;
    // In Iris mode the aperture kernel already spans size/2 pixels per level,
    // so fewer levels reach the same radius.
    const float kBase =
        iris ? std::max(0.0f, std::log2(std::max(1.0f, s.radius * kRadiusScale / (0.5f * float(s.iris.size))))) : kf;
    auto chLevel = [&](int c, float caOffset) {
        return std::max(0.0f, kBase + std::log2(std::max(1e-3f, s.radiusMul[c])) + caOffset);
    };
    const float channelLevels[3] = {chLevel(0, ca), chLevel(1, 0.0f), chLevel(2, -ca)};
    const int wanted = int(std::ceil(maxOf(channelLevels[0], channelLevels[1], channelLevels[2])));
    // Exponential: each level is the previous (already blurred) level, halved
    // and blurred again. Iris: each level must see the aperture exactly once
    // (chaining would convolve it with itself into a round blob), so a
    // separate un-convolved copy is downsampled and the aperture applied per
    // level, giving stacked aperture shapes like real out-of-focus lights.
    Plane clean;
    if (iris) clean = brightPass(src, s, mask, threads);
    while (int(levels.size()) <= wanted) {
        const Plane& prev = iris ? clean : levels.back();
        if (prev.w <= 2 || prev.h <= 2) break;
        Plane next = downsample(prev, threads);
        if (iris) {
            blur(next, 1.0f, 1.0f, kTent3, 1, threads); // light anti-aliasing only
            clean = next;
            convolveIris(next, s.iris, threads);
        } else {
            levelBlur.apply(next, threads);
        }
        levels.push_back(std::move(next));
    }
    const int top = int(levels.size()) - 1;

    // Per-channel energy weight of each level. Levels up to the radius count
    // fully, the last one fractionally (so Radius animates smoothly), then the
    // Falloff tilt is applied and each channel is normalized to unit energy.
    const float tilt = (1.0f - 2.0f * std::min(1.0f, std::max(0.0f, s.falloff))) * kFalloffTilt + (iris ? kIrisTilt : 0.0f);
    std::vector<float> weight(size_t(top + 1) * 3, 0.0f);
    for (int ch = 0; ch < 3; ++ch) {
        const float kc = std::min(float(top), channelLevels[ch]);
        const int full = int(std::floor(kc));
        const float frac = kc - float(full);
        float sum = 0.0f;
        for (int k = 0; k <= top; ++k) {
            float w = k <= full ? 1.0f : (k == full + 1 ? frac : 0.0f);
            w *= std::pow(2.0f, tilt * float(k));
            weight[size_t(k) * 3 + ch] = w;
            sum += w;
        }
        for (int k = 0; k <= top; ++k) weight[size_t(k) * 3 + ch] /= std::max(sum, 1e-8f);
    }
    // Tint: Single colors every level; Inner/Outer blends from Tint Color at
    // the core (small levels) to Outer Color in the halo (large levels).
    const float tintAmount = std::min(1.0f, std::max(0.0f, s.tintAmount));
    if (tintAmount > 0.0f) {
        for (int k = 0; k <= top; ++k) {
            const float t = (s.tintMode == TintMode::InnerOuter && top > 0) ? float(k) / float(top) : 0.0f;
            for (int ch = 0; ch < 3; ++ch) {
                const float col = s.tint[ch] + (s.tintOuter[ch] - s.tint[ch]) * t;
                weight[size_t(k) * 3 + ch] *= 1.0f + (col - 1.0f) * tintAmount;
            }
        }
    }

    // 3. Progressive reconstruction, coarsest level first.
    Plane acc(levels[size_t(top)].w, levels[size_t(top)].h);
    for (int k = top; k >= 0; --k) {
        const Plane& L = levels[size_t(k)];
        if (k != top) {
            acc = upsample(acc, L.w, L.h, threads);
            if (!iris) blur(acc, 1.0f, 1.0f, kTent3, 1, threads); // keep aperture edges crisp in Iris mode
        }
        const float* w = &weight[size_t(k) * 3];
        if (w[0] == 0.0f && w[1] == 0.0f && w[2] == 0.0f) continue;
        parallelRows(L.h, L.w, threads, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                float* a = acc.row(y);
                const float* l = L.row(y);
                for (int x = 0; x < L.w * 3; x += 3) {
                    a[x + 0] += l[x + 0] * w[0];
                    a[x + 1] += l[x + 1] * w[1];
                    a[x + 2] += l[x + 2] * w[2];
                }
            }
        });
    }

    // 4. Gain, whiteness and composite.
    const bool glowOnly = s.blend == BlendMode::GlowOnly || s.view == View::GlowOnly;
    const ImageRGBA scattered = glowOnly ? ImageRGBA() : scatterSource(src, threads);
    const float sourceOpacity = std::min(1.0f, std::max(0.0f, s.sourceOpacity));
    const float gain = std::pow(2.0f, s.exposure + kBaseGainStops) * std::min(1.0f, std::max(0.0f, s.opacity));
    const float whiteness = std::min(1.0f, std::max(0.0f, s.whiteness));
    const float dirtStrength = std::min(1.0f, std::max(0.0f, s.dirtStrength));
    const std::vector<float>* dirt =
        (s.dirt && dirtStrength > 0.0f && s.dirt->size() == size_t(src.width) * size_t(src.height)) ? s.dirt : nullptr;
    // Glow image (gain, lens dirt, whiteness), so it can be aberrated as a whole.
    ImageRGBA glow(src.width, src.height);
    parallelRows(src.height, src.width, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* g = acc.row(y);
            for (int x = 0; x < src.width; ++x) {
                float gr = g[x * 3 + 0] * gain, gg = g[x * 3 + 1] * gain, gb = g[x * 3 + 2] * gain;
                if (dirt) {
                    const float d = 1.0f + dirtStrength * kDirtBoost * (*dirt)[size_t(y) * size_t(src.width) + size_t(x)];
                    gr *= d;
                    gg *= d;
                    gb *= d;
                }
                const float m = maxOf(gr, gg, gb);
                if (whiteness > 0.0f && m > 0.0f) {
                    const float h = whiteness * smoothstep(0.15f, 1.0f, m);
                    gr += (m - gr) * h;
                    gg += (m - gg) * h;
                    gb += (m - gb) * h;
                }
                glow.at(x, y) = RGBA{gr, gg, gb, std::min(1.0f, std::max(0.0f, maxOf(gr, gg, gb)))};
            }
        }
    });
    if (s.aberration.target == AberrationTarget::Glow) applyAberration(glow, s.aberration, threads);

    parallelRows(src.height, src.width, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < src.width; ++x) {
                const RGBA& gp = glow.at(x, y);
                float gr = gp.r, gg = gp.g, gb = gp.b;
                RGBA& o = dst.at(x, y);
                if (glowOnly) {
                    o = RGBA{gr, gg, gb, std::min(1.0f, std::max(0.0f, maxOf(gr, gg, gb)))};
                    toneMapPixel(o, s);
                    continue;
                }
                const RGBA& so = scattered.at(x, y);
                const RGBA sp{so.r * sourceOpacity, so.g * sourceOpacity, so.b * sourceOpacity, so.a * sourceOpacity};
                if (s.blend == BlendMode::Screen) {
                    gr = softClip(gr);
                    gg = softClip(gg);
                    gb = softClip(gb);
                }
                const float ga = std::min(1.0f, std::max(0.0f, maxOf(gr, gg, gb)));
                switch (s.blend) {
                case BlendMode::Screen:
                    o.r = sp.r + gr - std::min(sp.r, 1.0f) * gr;
                    o.g = sp.g + gg - std::min(sp.g, 1.0f) * gg;
                    o.b = sp.b + gb - std::min(sp.b, 1.0f) * gb;
                    o.a = sp.a + ga * (1.0f - sp.a);
                    break;
                case BlendMode::Behind: {
                    const float k = 1.0f - sp.a;
                    o.r = sp.r + gr * k;
                    o.g = sp.g + gg * k;
                    o.b = sp.b + gb * k;
                    o.a = sp.a + ga * k;
                    break;
                }
                case BlendMode::Add:
                default:
                    o.r = sp.r + gr;
                    o.g = sp.g + gg;
                    o.b = sp.b + gb;
                    o.a = sp.a + ga * (1.0f - sp.a);
                    break;
                }
                toneMapPixel(o, s);
            }
        }
    });
    if (s.aberration.target == AberrationTarget::GlowAndLayer) applyAberration(dst, s.aberration, threads);
    if (s.aberration.target != AberrationTarget::Off && s.aberration.qcaLook) {
        parallelRows(dst.height, dst.width, threads, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < dst.width; ++x) {
                    RGBA& o = dst.at(x, y);
                    if (o.a <= 0.0f || o.a >= 1.0f) continue;
                    const float k = std::pow(o.a, kQcaAlphaExponent);
                    o.r *= k;
                    o.g *= k;
                    o.b *= k;
                }
        });
    }
}

} // namespace lumen
