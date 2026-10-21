// GlowCore.h - Lumen Glow rendering engine.
//
// Host-independent: no After Effects headers. The plugin layer converts AE
// worlds to/from ImageRGBA and calls renderGlow(); the test harness in
// tests/ drives the same code directly.
//
// Method (the "bloom pyramid" used by Deep Glow and real-time engines):
//   1. Bright pass: soft threshold + tint, at full resolution.
//   2. Pyramid: each level is the previous one downsampled 2x and blurred
//      with a small kernel, so level k is a blur of ~2^k pixels for the cost
//      of a few taps on a tiny image.
//   3. Reconstruction: levels are upsampled progressively and summed with
//      per-level energy weights. Equal energy per level gives a 1/r^2
//      (inverse-square) falloff; Falloff tilts it toward halo or core.
//   4. Whiteness, then composite with the source.

#pragma once

#include <vector>

namespace lumen {

// Premultiplied RGBA, linear float. Values may exceed 1 (32 bpc).
struct RGBA {
    float r, g, b, a;
};

struct ImageRGBA {
    int width = 0;
    int height = 0;
    std::vector<RGBA> pixels;

    ImageRGBA() = default;
    ImageRGBA(int w, int h) : width(w), height(h), pixels(size_t(w) * size_t(h), RGBA{0, 0, 0, 0}) {}
    RGBA& at(int x, int y) { return pixels[size_t(y) * size_t(width) + size_t(x)]; }
    const RGBA& at(int x, int y) const { return pixels[size_t(y) * size_t(width) + size_t(x)]; }
};

enum class BlendMode : int {
    Add = 1,      // glow added over the source: hottest, can clip to white
    Screen = 2,   // default (like Deep Glow): rolls off smoothly toward white
    Behind = 3,   // glow behind the source: keeps colored objects crisp
    GlowOnly = 4  // glow without the source (also selectable via View)
};

enum class GlowMode : int {
    Exponential = 1, // smooth inverse-square falloff (Deep Glow's classic look)
    Iris = 2         // the glow takes the shape of a lens aperture (bokeh)
};

enum class TintMode : int {
    Single = 1,      // one color for the whole glow
    InnerOuter = 2   // core takes Tint Color, halo takes Outer Color
};

enum class ToneMap : int { None = 1, Reinhard = 2, HableFilmic = 3, AcesFilmic = 4 };

enum class ToneMapMethod : int {
    PerChannel = 1,  // classic: bright colors desaturate toward white
    PreserveHue = 2  // maps brightness only: saturated highlights stay colorful
};

// Aperture shape used by GlowMode::Iris: a normalized size x size kernel.
struct IrisKernel {
    int size = 0;
    std::vector<float> weights;
};

// sides = 0 for a circle, otherwise a regular polygon (6 = hexagon, 8 = octagon).
IrisKernel makeIrisKernel(int sides, float rotationDegrees, int size = 9);
// Any image as the aperture (its luminance, fitted into the kernel and rotated).
IrisKernel irisKernelFromImage(const ImageRGBA& img, float rotationDegrees, int size = 9);

// Lens-style chromatic aberration (the Quick Chromatic Aberration feature set).
// The image is split into `iterations` spectral samples spread around the hue
// wheel; sample i is transformed by t in [-1, 1] times each control, so one
// end of the spectrum moves one way and the other end the other way.
enum class AberrationTarget : int { Off = 1, Glow = 2, GlowAndLayer = 3 };

struct Aberration {
    AberrationTarget target = AberrationTarget::Off;
    float amount = 1.0f;           // master multiplier for every transform below
    float centerX = 0.0f, centerY = 0.0f; // lens center, in image pixels
    float shiftX = 0.0f, shiftY = 0.0f;   // pixels at the spectrum ends (lateral RGB split)
    float scale = 0.0f;            // percent: radial fringing growing toward the frame edges
    float rotationDegrees = 0.0f;  // spectrum ends rotate around the center
    float distortion = 0.0f;       // percent: barrel (+) / pincushion (-) per spectrum end
    float skew = 0.0f;             // percent: horizontal skew per spectrum end
    float blur = 0.0f;             // pixels of blur at the spectrum ends (center stays sharp)
    int iterations = 3;            // 3 = classic RGB split; more = smooth spectrum
    float hueOffsetDegrees = 0.0f; // rotates which hues go which way (full 360 control); Rainbow only
    bool customSpectrum = false;   // use spectrumColors instead of the hue wheel
    float spectrumColors[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}; // start, middle, end of the spectrum
    bool repeatEdges = true;       // sample past the image edge by repeating edge pixels
    bool preserveAlpha = false;    // keep the original alpha instead of aberrating it
    bool qcaLook = false;          // darken partly transparent pixels like Quick Chromatic Aberration does
    float reach = 0.0f;            // how far pixels can move (set by the host, for buffer sizing)
};

// Applies the aberration to a premultiplied image in place.
void applyAberration(ImageRGBA& img, const Aberration& a, int threads = 0);

enum class View : int {
    Final = 1,      // the composited result
    GlowOnly = 2,   // just the glow, for comping it yourself
    Highlights = 3  // what passes the threshold: tune Threshold against this
};

struct GlowSettings {
    float radius = 550.0f;           // glow size in pixels; same scale as Deep Glow's Radius
    float exposure = 1.0f;           // stops; same scale as Deep Glow's Exposure
    float falloff = 0.422f;          // 0..1; 0.422 matches Deep Glow, lower = more halo, higher = tighter core
    float threshold = 0.0f;          // 0..1+, brightness where pixels start to glow
    float thresholdSmooth = 0.25f;   // 0..1, softness of the threshold knee
    float tint[3] = {1.0f, 1.0f, 1.0f};
    float tintAmount = 0.0f;         // 0..1
    float whiteness = 0.3f;          // 0..1, desaturates the hottest parts of the glow toward white
    float chromaticAberration = 0.0f;// 0..1, red spreads wider and blue tighter (up to one octave)
    float aspect = 0.0f;             // -1..1, >0 stretches along Angle, <0 across it
    float angleDegrees = 0.0f;       // direction of the Aspect Ratio stretch (0 = horizontal)
    BlendMode blend = BlendMode::Screen;
    View view = View::Final;
    float opacity = 1.0f;            // 0..1, glow mix
    float sourceOpacity = 1.0f;      // 0..1, fades the original layer while keeping its glow

    // Deep Glow 2-style extras
    float saturationBias = 0.0f;     // -1..1, desaturates (-) or saturates (+) what glows
    float radiusMul[3] = {1.0f, 1.0f, 1.0f}; // per-channel radius multipliers (R, G, B)
    TintMode tintMode = TintMode::Single;
    float tintOuter[3] = {1.0f, 1.0f, 1.0f}; // halo color for TintMode::InnerOuter
    GlowMode glowMode = GlowMode::Exponential;
    IrisKernel iris;                 // required when glowMode == Iris
    const std::vector<float>* dirt = nullptr; // lens dirt luminance, width*height of src, or null
    float dirtStrength = 0.0f;       // 0..1
    ToneMap toneMap = ToneMap::None;
    ToneMapMethod toneMapMethod = ToneMapMethod::PreserveHue;
    float whitePoint = 4.0f;         // input brightness that maps to pure white
    float toneMapMix = 1.0f;         // 0..1
    Aberration aberration;

    // Glow mask: limits which parts of the layer produce glow (the glow can
    // still spread past the mask's edge). 0..1 per pixel, width*height of src.
    const std::vector<float>* mask = nullptr;
    float maskFeather = 0.0f;        // pixels of edge softening applied to the mask
    bool maskInvert = false;         // glow everywhere except inside the mask
    float gamma = 1.0f;              // working-space gamma: 1 = process as-is, 2.2 = linearize display-referred input
};

// How far (in pixels) the glow can reach beyond the source content. The host
// must give renderGlow() at least this much transparent padding around the
// source so the glow isn't clipped.
int glowMargin(const GlowSettings& s);

// Renders the glow of src (premultiplied) and composites it into dst, which is
// resized to src's dimensions. threads <= 0 picks a sensible default.
void renderGlow(const ImageRGBA& src, ImageRGBA& dst, const GlowSettings& s, int threads = 0);

} // namespace lumen
