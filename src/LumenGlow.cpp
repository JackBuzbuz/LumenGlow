// LumenGlow.cpp - After Effects effect plugin (SmartFX).
//
// This file is the AE glue: parameters, presets, buffer expansion and pixel
// format conversion. All glow math lives in GlowCore.cpp, which has no AE
// dependencies and is tested on its own (tests/).

#include "AEConfig.h"
#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectCBSuites.h"
#include "AE_EffectSuites.h"
#include "AE_Macros.h"
#include "Param_Utils.h"

#include "GlowCore.h"
#include "LumenGlow_Version.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <new>
#include <vector>

// Diagnostic logging (build with -DLG_DEBUG_LOG). Appends one line per
// pre-render / render to /tmp/LumenGlow.log so buffer sizes can be checked
// against what's on screen.
#ifdef LG_DEBUG_LOG
#include <cstdarg>
#include <mutex>
static void DebugLog(const char* fmt, ...) {
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    if (FILE* f = std::fopen("/tmp/LumenGlow.log", "a")) {
        va_list args;
        va_start(args, fmt);
        std::vfprintf(f, fmt, args);
        va_end(args);
        std::fputc('\n', f);
        std::fclose(f);
    }
}
#define LG_LOG(...) DebugLog(__VA_ARGS__)
#else
#define LG_LOG(...) ((void)0)
#endif

#ifndef AE_RESERVED_INFO
#define AE_RESERVED_INFO 8 // same value as the SDK's Util/entry.h
#endif

// Fail the build, not the AE load, if a hand-written PiPL value is wrong.
static_assert(LG_VERSION_NUM == PF_VERSION(LG_MAJOR, LG_MINOR, LG_BUGFIX, LG_STAGE, LG_BUILD),
              "LG_VERSION_NUM does not match PF_VERSION");
static_assert(LG_STAGE == PF_Stage_RELEASE, "LG_STAGE must be PF_Stage_RELEASE");
static_assert(LG_OUT_FLAGS == PF_OutFlag_DEEP_COLOR_AWARE, "LG_OUT_FLAGS does not match the SDK");
static_assert(LG_OUT_FLAGS2 == (PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG | PF_OutFlag2_REVEALS_ZERO_ALPHA |
                                PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_FLOAT_COLOR_AWARE |
                                PF_OutFlag2_SUPPORTS_THREADED_RENDERING),
              "LG_OUT_FLAGS2 does not match the SDK");

namespace {

// Parameter order in the Effect Controls panel. Topics count as parameters.
enum ParamIndex {
    LG_INPUT = 0,
    LG_PRESET,
    LG_VIEW,
    LG_GLOW_MODE,
    LG_GLOW_START,
    LG_RADIUS,
    LG_EXPOSURE,
    LG_FALLOFF,
    LG_THRESHOLD,
    LG_THRESHOLD_SMOOTH,
    LG_SATURATION_BIAS,
    LG_MASK_START,
    LG_MASK_LAYER,
    LG_MASK_MODE,
    LG_MASK_INVERT,
    LG_MASK_FEATHER,
    LG_MASK_END,
    LG_GLOW_END,
    LG_COLOR_START,
    LG_TINT_MODE,
    LG_TINT_COLOR,
    LG_TINT_OUTER,
    LG_TINT_AMOUNT,
    LG_WHITENESS,
    LG_RADIUS_R,
    LG_RADIUS_G,
    LG_RADIUS_B,
    LG_COLOR_END,
    LG_CA_START,
    LG_CA_TARGET,
    LG_CA_AMOUNT,
    LG_CA_CENTER,
    LG_CA_POSITION,
    LG_CA_DIRECTION,
    LG_CA_SCALE,
    LG_CA_ROTATION,
    LG_CA_DISTORTION,
    LG_CA_SKEW,
    LG_CA_BLUR,
    LG_CA_ITERATIONS,
    LG_CA_HUE,
    LG_CA_SPECTRUM,
    LG_CA_COLOR1,
    LG_CA_COLOR2,
    LG_CA_COLOR3,
    LG_CA_EDGES,
    LG_CA_ALPHA,
    LG_CA_END,
    LG_SHAPE_START,
    LG_ASPECT,
    LG_ANGLE,
    LG_SHAPE_END,
    LG_IRIS_START,
    LG_IRIS_SHAPE,
    LG_IRIS_LAYER,
    LG_IRIS_ROTATION,
    LG_IRIS_END,
    LG_DIRT_START,
    LG_DIRT_LAYER,
    LG_DIRT_STRENGTH,
    LG_DIRT_END,
    LG_TONE_START,
    LG_TONEMAP,
    LG_TONEMAP_METHOD,
    LG_WHITE_POINT,
    LG_TONEMAP_MIX,
    LG_TONE_END,
    LG_COMP_START,
    LG_BLEND,
    LG_OPACITY,
    LG_SOURCE_OPACITY,
    LG_COMP_END,
    LG_ADVANCED_START,
    LG_GAMMA,
    LG_DITHER,
    LG_ADVANCED_END,
    LG_NUM_PARAMS
};

// Disk IDs identify parameters in saved projects. Never renumber or reuse
// them; append new ones at the end.
enum DiskId {
    ID_GLOW_START = 1,
    ID_RADIUS,
    ID_EXPOSURE,
    ID_FALLOFF,
    ID_THRESHOLD,
    ID_THRESHOLD_SMOOTH,
    ID_GLOW_END,
    ID_COLOR_START,
    ID_TINT_COLOR,
    ID_TINT_AMOUNT,
    ID_WHITENESS,
    ID_CHROMA_RETIRED, // the 2.0 Chromatic Aberration slider; replaced by the CA section (2.2)
    ID_COLOR_END,
    ID_SHAPE_START,
    ID_ASPECT,
    ID_SHAPE_END,
    ID_COMP_START,
    ID_BLEND,
    ID_OPACITY,
    ID_COMP_END,
    // added in 2.0
    ID_PRESET,
    ID_VIEW,
    ID_ANGLE,
    ID_SOURCE_OPACITY,
    ID_ADVANCED_START,
    ID_LINEAR_RETIRED, // was the Linear Light checkbox (2.0); replaced by ID_GAMMA
    ID_DITHER,
    ID_ADVANCED_END,
    // added in 2.1
    ID_GLOW_MODE,
    ID_SATURATION_BIAS,
    ID_TINT_MODE,
    ID_TINT_OUTER,
    ID_RADIUS_R,
    ID_RADIUS_G,
    ID_RADIUS_B,
    ID_IRIS_START,
    ID_IRIS_SHAPE,
    ID_IRIS_LAYER,
    ID_IRIS_ROTATION,
    ID_IRIS_END,
    ID_DIRT_START,
    ID_DIRT_LAYER,
    ID_DIRT_STRENGTH,
    ID_DIRT_END,
    ID_TONE_START,
    ID_TONEMAP,
    ID_TONEMAP_METHOD,
    ID_WHITE_POINT,
    ID_TONEMAP_MIX,
    ID_TONE_END,
    ID_GAMMA,
    // added in 2.2: Chromatic Aberration section
    ID_CA_START,
    ID_CA_TARGET,
    ID_CA_AMOUNT,
    ID_CA_CENTER,
    ID_CA_SHIFT_X_RETIRED, // 2.2 Shift X/Y sliders, replaced by Position + Direction (2.3)
    ID_CA_SHIFT_Y_RETIRED,
    ID_CA_SCALE,
    ID_CA_ROTATION,
    ID_CA_DISTORTION,
    ID_CA_SKEW,
    ID_CA_BLUR,
    ID_CA_ITERATIONS,
    ID_CA_HUE,
    ID_CA_EDGES,
    ID_CA_ALPHA,
    ID_CA_END,
    // added in 2.3
    ID_CA_POSITION,
    ID_CA_DIRECTION,
    // added in 2.4: custom spectrum colors
    ID_CA_SPECTRUM,
    ID_CA_COLOR1,
    ID_CA_COLOR2,
    ID_CA_COLOR3,
    // added in 2.5: glow mask
    ID_MASK_START,
    ID_MASK_LAYER,
    ID_MASK_MODE,
    ID_MASK_INVERT,
    ID_MASK_FEATHER,
    ID_MASK_END
};

enum MaskMode { MASK_ALPHA = 1, MASK_LUMINANCE };

enum SpectrumMode { SPECTRUM_RAINBOW = 1, SPECTRUM_CUSTOM };

// Iris aperture menu and kernel resolution.
enum IrisShape { IRIS_CIRCLE = 1, IRIS_HEXAGON, IRIS_OCTAGON, IRIS_CUSTOM };
constexpr int kIrisKernelSize = 15;

// ------------------------------------------------------------------ presets

// Picking a preset fills in every control below; touching any of them
// switches the menu back to Custom, so it always describes what you see.
// Layer pickers (Iris Layer, Dirt Layer) are never changed by presets.
struct Preset {
    const char* name = "Deep Glow Match";
    lumen::GlowMode glowMode = lumen::GlowMode::Exponential;
    float radius = 550, exposure = 1.0f, falloff = 42.2f, threshold = 0, thresholdSmooth = 25, saturationBias = 0;
    lumen::TintMode tintMode = lumen::TintMode::Single;
    A_u_char tint[3] = {255, 255, 255};
    A_u_char tintOuter[3] = {255, 255, 255};
    float tintAmount = 0, whiteness = 30;
    float radiusR = 100, radiusG = 100, radiusB = 100; // percent
    float aspect = 0, angle = 0;
    IrisShape irisShape = IRIS_HEXAGON;
    float irisRotation = 0;
    float dirtStrength = 0;
    lumen::AberrationTarget caTarget = lumen::AberrationTarget::Off;
    float caAmount = 100, caPosition = 0, caDirection = 135, caScale = 1.5f, caRotation = 0, caDistortion = 0, caSkew = 0;
    float caBlur = 0, caIterations = 3, caHue = 0;
    SpectrumMode caSpectrum = SPECTRUM_RAINBOW;
    A_u_char caColor1[3] = {255, 0, 0}, caColor2[3] = {0, 255, 0}, caColor3[3] = {0, 0, 255};
    lumen::ToneMap toneMap = lumen::ToneMap::None;
    lumen::ToneMapMethod toneMapMethod = lumen::ToneMapMethod::PreserveHue;
    float whitePoint = 4, toneMapMix = 100;
    lumen::BlendMode blend = lumen::BlendMode::Screen;
    float glowOpacity = 100, sourceOpacity = 100;
};

const std::vector<Preset>& Presets() {
    static const std::vector<Preset> presets = [] {
        std::vector<Preset> v;
        Preset p;
        v.push_back(p); // Deep Glow Match (all defaults)

        p = Preset(); p.name = "Soft Bloom";
        p.radius = 900; p.exposure = 0; p.falloff = 25; p.threshold = 40; p.thresholdSmooth = 50; p.whiteness = 15;
        v.push_back(p);

        p = Preset(); p.name = "Neon";
        p.radius = 250; p.exposure = 1.5f; p.falloff = 55; p.whiteness = 70; p.saturationBias = 40;
        v.push_back(p);

        p = Preset(); p.name = "Hot Core";
        p.radius = 150; p.exposure = 2; p.falloff = 75; p.threshold = 20; p.whiteness = 90;
        p.blend = lumen::BlendMode::Add;
        v.push_back(p);

        p = Preset(); p.name = "Anamorphic Streak";
        p.radius = 700; p.falloff = 35; p.threshold = 30; p.whiteness = 40; p.aspect = 90;
        p.tint[0] = 140; p.tint[1] = 190; p.tint[2] = 255; p.tintAmount = 60;
        v.push_back(p);

        p = Preset(); p.name = "Dreamy Haze";
        p.radius = 1500; p.exposure = -0.5f; p.falloff = 15; p.threshold = 30; p.thresholdSmooth = 60;
        p.whiteness = 10; p.tint[0] = 255;
        p.caTarget = lumen::AberrationTarget::Glow; p.caScale = 4; p.caIterations = 12; p.caBlur = 20; p.tint[1] = 205; p.tint[2] = 155; p.tintAmount = 40;
        v.push_back(p);

        p = Preset(); p.name = "Glow Behind Colored Objects";
        p.radius = 400; p.falloff = 45; p.tintAmount = 100; p.blend = lumen::BlendMode::Behind;
        v.push_back(p);

        p = Preset(); p.name = "Lens Fringe";
        p.radius = 600; p.falloff = 40; p.threshold = 20;
        p.caTarget = lumen::AberrationTarget::GlowAndLayer; p.caScale = 1.5f; p.caIterations = 12;
        v.push_back(p);

        p = Preset(); p.name = "RGB Glitch Split";
        p.radius = 300; p.falloff = 50;
        p.caTarget = lumen::AberrationTarget::GlowAndLayer; p.caPosition = 12; p.caDirection = 0; p.caScale = 0; p.caIterations = 3;
        v.push_back(p);

        p = Preset(); p.name = "Prism Spectrum";
        p.radius = 700; p.exposure = 1.5f; p.falloff = 35; p.whiteness = 0;
        p.caTarget = lumen::AberrationTarget::Glow; p.caScale = 8; p.caDistortion = 10; p.caIterations = 32; p.caBlur = 8;
        p.caSpectrum = SPECTRUM_CUSTOM; // pick your own: warm red -> gold -> blue to start
        p.caColor1[0] = 255; p.caColor1[1] = 70; p.caColor1[2] = 40;
        p.caColor2[0] = 255; p.caColor2[1] = 230; p.caColor2[2] = 120;
        p.caColor3[0] = 70; p.caColor3[1] = 120; p.caColor3[2] = 255;
        v.push_back(p);

        p = Preset(); p.name = "Duotone Halo";
        p.radius = 800; p.exposure = 1.5f; p.falloff = 35; p.whiteness = 0; p.tintMode = lumen::TintMode::InnerOuter;
        p.tint[0] = 255; p.tint[1] = 200; p.tint[2] = 90; p.tintOuter[0] = 60; p.tintOuter[1] = 110; p.tintOuter[2] = 255;
        p.tintAmount = 100;
        v.push_back(p);

        p = Preset(); p.name = "Cinematic HDR (ACES)";
        p.exposure = 2.5f; p.falloff = 40; p.blend = lumen::BlendMode::Add; p.whiteness = 0;
        p.toneMap = lumen::ToneMap::AcesFilmic; p.toneMapMethod = lumen::ToneMapMethod::PreserveHue; p.whitePoint = 6;
        v.push_back(p);

        p = Preset(); p.name = "Bokeh Lights (Iris)";
        p.glowMode = lumen::GlowMode::Iris; p.irisShape = IRIS_HEXAGON; p.radius = 250; p.exposure = 3;
        p.falloff = 30; p.threshold = 50; p.whiteness = 10;
        v.push_back(p);

        p = Preset(); p.name = "Dirty Lens";
        p.radius = 1200; p.exposure = 1.5f; p.falloff = 25; p.threshold = 30; p.dirtStrength = 70;
        v.push_back(p);
        return v;
    }();
    return presets;
}

constexpr A_long kPresetCustom = 1;  // popup value of "Custom"
constexpr A_long kPresetDefault = 2; // popup value of the first preset (Deep Glow Match)
const char* const kPresetMenu = "Custom|Deep Glow Match|Soft Bloom|Neon|Hot Core|Anamorphic Streak|Dreamy Haze|"
                                "Glow Behind Colored Objects|Lens Fringe|RGB Glitch Split|Prism Spectrum|Duotone Halo|"
                                "Cinematic HDR (ACES)|Bokeh Lights (Iris)|Dirty Lens";

// ------------------------------------------------------------------ commands

PF_Err About(PF_InData* in_data, PF_OutData* out_data) {
    (void)in_data;
    std::snprintf(out_data->return_msg, sizeof(out_data->return_msg),
                  "%s v%d.%d.%d\r%s\r\rStart from a Preset, then adjust Radius and Exposure.\r"
                  "View > Highlights shows exactly what will glow. Iris, Lens Dirt and Tone Mapping\r"
                  "live in their own sections.",
                  LG_NAME, LG_MAJOR, LG_MINOR, LG_BUGFIX, LG_DESCRIPTION);
    return PF_Err_NONE;
}

PF_Err GlobalSetup(PF_OutData* out_data) {
    out_data->my_version = PF_VERSION(LG_MAJOR, LG_MINOR, LG_BUGFIX, LG_STAGE, LG_BUILD);
    out_data->out_flags = LG_OUT_FLAGS;
    out_data->out_flags2 = LG_OUT_FLAGS2;
    return PF_Err_NONE;
}

PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data) {
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;
    const PF_ParamFlags sup = PF_ParamFlag_SUPERVISE; // edits switch the preset to Custom
    const Preset d;                                   // defaults = Deep Glow Match
    const int numPresets = int(Presets().size());

    AEFX_CLR_STRUCT(def);
    def.flags = PF_ParamFlag_SUPERVISE | PF_ParamFlag_CANNOT_TIME_VARY;
    PF_ADD_POPUP("Preset", numPresets + 1, kPresetDefault, kPresetMenu, ID_PRESET);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("View", 3, int(lumen::View::Final), "Final|Glow Only|Highlights", ID_VIEW);
    AEFX_CLR_STRUCT(def);
    def.flags = sup;
    PF_ADD_POPUP("Glow Mode", 2, int(d.glowMode), "Exponential|Iris (Bokeh)", ID_GLOW_MODE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Glow", ID_GLOW_START);
    PF_ADD_FLOAT_SLIDERX("Radius", 0, 8000, 0, 1500, d.radius, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, sup,
                         ID_RADIUS);
    PF_ADD_FLOAT_SLIDERX("Exposure", -10, 10, -4, 4, d.exposure, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE,
                         sup, ID_EXPOSURE);
    PF_ADD_FLOAT_SLIDERX("Falloff", 0, 100, 0, 100, d.falloff, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, sup,
                         ID_FALLOFF);
    PF_ADD_FLOAT_SLIDERX("Threshold", 0, 1000, 0, 100, d.threshold, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT,
                         sup, ID_THRESHOLD);
    PF_ADD_FLOAT_SLIDERX("Threshold Smoothness", 0, 100, 0, 100, d.thresholdSmooth, PF_Precision_TENTHS,
                         PF_ValueDisplayFlag_PERCENT, sup, ID_THRESHOLD_SMOOTH);
    PF_ADD_FLOAT_SLIDERX("Saturation Bias", -100, 100, -100, 100, d.saturationBias, PF_Precision_TENTHS,
                         PF_ValueDisplayFlag_NONE, sup, ID_SATURATION_BIAS);
    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Mask", ID_MASK_START);
    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("Mask Layer", PF_LayerDefault_NONE, ID_MASK_LAYER);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Mode", 2, MASK_ALPHA, "Alpha|Luminance", ID_MASK_MODE);
    PF_ADD_CHECKBOXX("Invert", FALSE, 0, ID_MASK_INVERT);
    PF_ADD_FLOAT_SLIDERX("Feather", 0, 2000, 0, 200, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0,
                         ID_MASK_FEATHER);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_MASK_END);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_GLOW_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Color", ID_COLOR_START);
    AEFX_CLR_STRUCT(def);
    def.flags = sup;
    PF_ADD_POPUP("Tint Mode", 2, int(d.tintMode), "Single Color|Inner / Outer", ID_TINT_MODE);
    AEFX_CLR_STRUCT(def);
    def.flags = sup;
    PF_ADD_COLOR("Tint Color", d.tint[0], d.tint[1], d.tint[2], ID_TINT_COLOR);
    AEFX_CLR_STRUCT(def);
    def.flags = sup;
    PF_ADD_COLOR("Outer Color", d.tintOuter[0], d.tintOuter[1], d.tintOuter[2], ID_TINT_OUTER);
    PF_ADD_FLOAT_SLIDERX("Tint Amount", 0, 100, 0, 100, d.tintAmount, PF_Precision_TENTHS,
                         PF_ValueDisplayFlag_PERCENT, sup, ID_TINT_AMOUNT);
    PF_ADD_FLOAT_SLIDERX("Whiteness", 0, 100, 0, 100, d.whiteness, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT,
                         sup, ID_WHITENESS);
    PF_ADD_FLOAT_SLIDERX("Red Radius", 0, 400, 25, 200, d.radiusR, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT,
                         sup, ID_RADIUS_R);
    PF_ADD_FLOAT_SLIDERX("Green Radius", 0, 400, 25, 200, d.radiusG, PF_Precision_TENTHS,
                         PF_ValueDisplayFlag_PERCENT, sup, ID_RADIUS_G);
    PF_ADD_FLOAT_SLIDERX("Blue Radius", 0, 400, 25, 200, d.radiusB, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT,
                         sup, ID_RADIUS_B);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_COLOR_END);

    AEFX_CLR_STRUCT(def);
    def.flags = PF_ParamFlag_START_COLLAPSED;
    PF_ADD_TOPIC("Chromatic Aberration", ID_CA_START);
    AEFX_CLR_STRUCT(def);
    def.flags = sup;
    PF_ADD_POPUP("Apply To", 3, int(d.caTarget), "Off|Glow|Glow + Layer", ID_CA_TARGET);
    PF_ADD_FLOAT_SLIDERX("Amount", 0, 1000, 0, 200, d.caAmount, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT,
                         sup, ID_CA_AMOUNT);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POINT("Center", 50, 50, FALSE, ID_CA_CENTER);
    PF_ADD_FLOAT_SLIDERX("Position", -500, 500, 0, 50, d.caPosition, PF_Precision_HUNDREDTHS,
                         PF_ValueDisplayFlag_NONE, sup, ID_CA_POSITION);
    AEFX_CLR_STRUCT(def);
    def.flags = sup;
    PF_ADD_ANGLE("Direction", d.caDirection, ID_CA_DIRECTION);
    PF_ADD_FLOAT_SLIDERX("Scale", -50, 50, -10, 10, d.caScale, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_PERCENT,
                         sup, ID_CA_SCALE);
    AEFX_CLR_STRUCT(def);
    def.flags = sup;
    PF_ADD_ANGLE("Rotation", d.caRotation, ID_CA_ROTATION);
    PF_ADD_FLOAT_SLIDERX("Lens Distortion", -100, 100, -50, 50, d.caDistortion, PF_Precision_TENTHS,
                         PF_ValueDisplayFlag_PERCENT, sup, ID_CA_DISTORTION);
    PF_ADD_FLOAT_SLIDERX("Skew", -50, 50, -10, 10, d.caSkew, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_PERCENT,
                         sup, ID_CA_SKEW);
    PF_ADD_FLOAT_SLIDERX("Blur", 0, 500, 0, 50, d.caBlur, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, sup,
                         ID_CA_BLUR);
    PF_ADD_FLOAT_SLIDERX("Iterations", 3, 64, 3, 32, d.caIterations, PF_Precision_INTEGER, PF_ValueDisplayFlag_NONE,
                         sup, ID_CA_ITERATIONS);
    AEFX_CLR_STRUCT(def);
    def.flags = sup;
    PF_ADD_ANGLE("Hue Offset", d.caHue, ID_CA_HUE);
    AEFX_CLR_STRUCT(def);
    def.flags = sup;
    PF_ADD_POPUP("Spectrum", 2, int(d.caSpectrum), "Rainbow|Custom Colors", ID_CA_SPECTRUM);
    AEFX_CLR_STRUCT(def);
    def.flags = sup;
    PF_ADD_COLOR("Spectrum Color 1", d.caColor1[0], d.caColor1[1], d.caColor1[2], ID_CA_COLOR1);
    AEFX_CLR_STRUCT(def);
    def.flags = sup;
    PF_ADD_COLOR("Spectrum Color 2", d.caColor2[0], d.caColor2[1], d.caColor2[2], ID_CA_COLOR2);
    AEFX_CLR_STRUCT(def);
    def.flags = sup;
    PF_ADD_COLOR("Spectrum Color 3", d.caColor3[0], d.caColor3[1], d.caColor3[2], ID_CA_COLOR3);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Edges", 2, 1, "Repeat Edge Pixels|Transparent", ID_CA_EDGES);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Alpha", 3, 1, "Aberrate Alpha|Preserve Alpha|QCA Look", ID_CA_ALPHA);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_CA_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Shape", ID_SHAPE_START);
    PF_ADD_FLOAT_SLIDERX("Aspect Ratio", -100, 100, -100, 100, d.aspect, PF_Precision_TENTHS,
                         PF_ValueDisplayFlag_NONE, sup, ID_ASPECT);
    AEFX_CLR_STRUCT(def);
    def.flags = sup;
    PF_ADD_ANGLE("Angle", d.angle, ID_ANGLE);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_SHAPE_END);

    AEFX_CLR_STRUCT(def);
    def.flags = PF_ParamFlag_START_COLLAPSED;
    PF_ADD_TOPIC("Iris (Glow Mode: Iris)", ID_IRIS_START);
    AEFX_CLR_STRUCT(def);
    def.flags = sup;
    PF_ADD_POPUP("Iris Shape", 4, int(d.irisShape), "Circle|Hexagon|Octagon|Custom Layer", ID_IRIS_SHAPE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("Iris Layer", PF_LayerDefault_NONE, ID_IRIS_LAYER);
    AEFX_CLR_STRUCT(def);
    def.flags = sup;
    PF_ADD_ANGLE("Iris Rotation", d.irisRotation, ID_IRIS_ROTATION);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_IRIS_END);

    AEFX_CLR_STRUCT(def);
    def.flags = PF_ParamFlag_START_COLLAPSED;
    PF_ADD_TOPIC("Lens Dirt", ID_DIRT_START);
    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("Dirt Layer", PF_LayerDefault_NONE, ID_DIRT_LAYER);
    PF_ADD_FLOAT_SLIDERX("Dirt Strength", 0, 100, 0, 100, d.dirtStrength, PF_Precision_TENTHS,
                         PF_ValueDisplayFlag_PERCENT, sup, ID_DIRT_STRENGTH);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_DIRT_END);

    AEFX_CLR_STRUCT(def);
    def.flags = PF_ParamFlag_START_COLLAPSED;
    PF_ADD_TOPIC("Tone Mapping", ID_TONE_START);
    AEFX_CLR_STRUCT(def);
    def.flags = sup;
    PF_ADD_POPUP("Tone Map", 4, int(d.toneMap), "None|Reinhard|Hable Filmic|ACES Filmic", ID_TONEMAP);
    AEFX_CLR_STRUCT(def);
    def.flags = sup;
    PF_ADD_POPUP("Method", 2, int(d.toneMapMethod), "Per Channel|Preserve Hue", ID_TONEMAP_METHOD);
    PF_ADD_FLOAT_SLIDERX("White Point", 1, 64, 1, 16, d.whitePoint, PF_Precision_HUNDREDTHS,
                         PF_ValueDisplayFlag_NONE, sup, ID_WHITE_POINT);
    PF_ADD_FLOAT_SLIDERX("Tone Map Mix", 0, 100, 0, 100, d.toneMapMix, PF_Precision_TENTHS,
                         PF_ValueDisplayFlag_PERCENT, sup, ID_TONEMAP_MIX);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_TONE_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Compositing", ID_COMP_START);
    AEFX_CLR_STRUCT(def);
    def.flags = sup;
    PF_ADD_POPUP("Blend Mode", 3, int(d.blend), "Add|Screen|Behind", ID_BLEND);
    PF_ADD_FLOAT_SLIDERX("Glow Opacity", 0, 100, 0, 100, d.glowOpacity, PF_Precision_TENTHS,
                         PF_ValueDisplayFlag_PERCENT, sup, ID_OPACITY);
    PF_ADD_FLOAT_SLIDERX("Source Opacity", 0, 100, 0, 100, d.sourceOpacity, PF_Precision_TENTHS,
                         PF_ValueDisplayFlag_PERCENT, sup, ID_SOURCE_OPACITY);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_COMP_END);

    AEFX_CLR_STRUCT(def);
    def.flags = PF_ParamFlag_START_COLLAPSED;
    PF_ADD_TOPIC("Advanced", ID_ADVANCED_START);
    PF_ADD_FLOAT_SLIDERX("Gamma Correction", 1, 3, 1, 3, 1, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0,
                         ID_GAMMA);
    PF_ADD_CHECKBOXX("Dither", TRUE, 0, ID_DITHER);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_ADVANCED_END);

    out_data->num_params = LG_NUM_PARAMS;
    return err;
}

// Fills every preset-controlled parameter from a preset.
void ApplyPreset(PF_ParamDef* params[], const Preset& p) {
    auto changed = [&](int index) { params[index]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE; };
    auto setFloat = [&](int index, float v) {
        params[index]->u.fs_d.value = v;
        changed(index);
    };
    auto setPopup = [&](int index, A_long v) {
        params[index]->u.pd.value = v;
        changed(index);
    };
    auto setColor = [&](int index, const A_u_char c[3]) {
        PF_Pixel& px = params[index]->u.cd.value;
        px.alpha = 255;
        px.red = c[0];
        px.green = c[1];
        px.blue = c[2];
        changed(index);
    };
    auto setAngle = [&](int index, float degrees) {
        params[index]->u.ad.value = PF_Fixed(std::lround(degrees * 65536.0f));
        changed(index);
    };

    setPopup(LG_GLOW_MODE, A_long(p.glowMode));
    setFloat(LG_RADIUS, p.radius);
    setFloat(LG_EXPOSURE, p.exposure);
    setFloat(LG_FALLOFF, p.falloff);
    setFloat(LG_THRESHOLD, p.threshold);
    setFloat(LG_THRESHOLD_SMOOTH, p.thresholdSmooth);
    setFloat(LG_SATURATION_BIAS, p.saturationBias);
    setPopup(LG_TINT_MODE, A_long(p.tintMode));
    setColor(LG_TINT_COLOR, p.tint);
    setColor(LG_TINT_OUTER, p.tintOuter);
    setFloat(LG_TINT_AMOUNT, p.tintAmount);
    setFloat(LG_WHITENESS, p.whiteness);
    setFloat(LG_RADIUS_R, p.radiusR);
    setFloat(LG_RADIUS_G, p.radiusG);
    setFloat(LG_RADIUS_B, p.radiusB);
    setFloat(LG_ASPECT, p.aspect);
    setAngle(LG_ANGLE, p.angle);
    setPopup(LG_IRIS_SHAPE, A_long(p.irisShape));
    setAngle(LG_IRIS_ROTATION, p.irisRotation);
    setFloat(LG_DIRT_STRENGTH, p.dirtStrength);
    setPopup(LG_CA_TARGET, A_long(p.caTarget));
    setFloat(LG_CA_AMOUNT, p.caAmount);
    setFloat(LG_CA_POSITION, p.caPosition);
    setAngle(LG_CA_DIRECTION, p.caDirection);
    setFloat(LG_CA_SCALE, p.caScale);
    setAngle(LG_CA_ROTATION, p.caRotation);
    setFloat(LG_CA_DISTORTION, p.caDistortion);
    setFloat(LG_CA_SKEW, p.caSkew);
    setFloat(LG_CA_BLUR, p.caBlur);
    setFloat(LG_CA_ITERATIONS, p.caIterations);
    setAngle(LG_CA_HUE, p.caHue);
    setPopup(LG_CA_SPECTRUM, A_long(p.caSpectrum));
    setColor(LG_CA_COLOR1, p.caColor1);
    setColor(LG_CA_COLOR2, p.caColor2);
    setColor(LG_CA_COLOR3, p.caColor3);
    setPopup(LG_TONEMAP, A_long(p.toneMap));
    setPopup(LG_TONEMAP_METHOD, A_long(p.toneMapMethod));
    setFloat(LG_WHITE_POINT, p.whitePoint);
    setFloat(LG_TONEMAP_MIX, p.toneMapMix);
    setPopup(LG_BLEND, A_long(p.blend));
    setFloat(LG_OPACITY, p.glowOpacity);
    setFloat(LG_SOURCE_OPACITY, p.sourceOpacity);
}

// Turning chromatic aberration on should always show something: if every
// distortion control is zero, start from a subtle radial fringe.
constexpr float kDefaultAberrationScale = 1.5f;

void EnsureVisibleAberration(PF_ParamDef* params[]) {
    if (params[LG_CA_TARGET]->u.pd.value == A_long(lumen::AberrationTarget::Off)) return;
    const int sliders[] = {LG_CA_POSITION, LG_CA_SCALE, LG_CA_DISTORTION, LG_CA_SKEW, LG_CA_BLUR};
    for (int index : sliders)
        if (params[index]->u.fs_d.value != 0.0) return;
    if (params[LG_CA_ROTATION]->u.ad.value != 0) return;
    params[LG_CA_SCALE]->u.fs_d.value = kDefaultAberrationScale;
    params[LG_CA_SCALE]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
}

PF_Err UserChangedParam(PF_ParamDef* params[], const PF_UserChangedParamExtra* extra) {
    const std::vector<Preset>& presets = Presets();
    if (extra->param_index == LG_CA_TARGET) EnsureVisibleAberration(params);
    if (extra->param_index == LG_PRESET) {
        const A_long choice = params[LG_PRESET]->u.pd.value;
        if (choice >= kPresetDefault && choice < kPresetDefault + A_long(presets.size()))
            ApplyPreset(params, presets[size_t(choice - kPresetDefault)]);
    } else if (params[LG_PRESET]->u.pd.value != kPresetCustom) {
        params[LG_PRESET]->u.pd.value = kPresetCustom;
        params[LG_PRESET]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
    }
    return PF_Err_NONE;
}

// --------------------------------------------------------------- parameters

struct RenderSettings {
    lumen::GlowSettings glow;
    bool dither = true;
    bool wantDirt = false;      // Dirt Strength > 0: check out the Dirt Layer
    bool wantIrisLayer = false; // Iris mode with Custom Layer: check out the Iris Layer
    float irisRotation = 0.0f;
    float downsample = 1.0f;
    float caCenterX = 0.0f, caCenterY = 0.0f; // aberration center, layer pixels (downsampled)
    bool maskLuminance = false;
};

PF_Err ReadSettings(PF_InData* in_data, RenderSettings& rs) {
    PF_Err err = PF_Err_NONE;
    PF_ParamDef p;
    lumen::GlowSettings& s = rs.glow;

    auto checkout = [&](A_long index) -> PF_Err {
        AEFX_CLR_STRUCT(p);
        return PF_CHECKOUT_PARAM(in_data, index, in_data->current_time, in_data->time_step, in_data->time_scale, &p);
    };
    auto checkin = [&]() {
        const PF_Err e = PF_CHECKIN_PARAM(in_data, &p);
        if (!err) err = e;
    };
    auto readFloat = [&](A_long index, float& out, float scale) {
        if (err || (err = checkout(index)) != PF_Err_NONE) return;
        out = float(p.u.fs_d.value) * scale;
        checkin();
    };
    auto readPopup = [&](A_long index, A_long& out) {
        if (err || (err = checkout(index)) != PF_Err_NONE) return;
        out = p.u.pd.value;
        checkin();
    };
    auto readCheckbox = [&](A_long index, bool& out) {
        if (err || (err = checkout(index)) != PF_Err_NONE) return;
        out = p.u.bd.value != 0;
        checkin();
    };
    auto readColor = [&](A_long index, float out[3]) {
        if (err || (err = checkout(index)) != PF_Err_NONE) return;
        out[0] = p.u.cd.value.red / 255.0f;
        out[1] = p.u.cd.value.green / 255.0f;
        out[2] = p.u.cd.value.blue / 255.0f;
        checkin();
    };
    auto readAngle = [&](A_long index, float& out) {
        if (err || (err = checkout(index)) != PF_Err_NONE) return;
        out = float(double(p.u.ad.value) / 65536.0);
        checkin();
    };
    auto clampPopup = [](A_long v, A_long lo, A_long hi, A_long dflt) { return (v >= lo && v <= hi) ? v : dflt; };

    readFloat(LG_RADIUS, s.radius, 1.0f);
    readFloat(LG_EXPOSURE, s.exposure, 1.0f);
    readFloat(LG_FALLOFF, s.falloff, 0.01f);
    readFloat(LG_THRESHOLD, s.threshold, 0.01f);
    readFloat(LG_THRESHOLD_SMOOTH, s.thresholdSmooth, 0.01f);
    readFloat(LG_SATURATION_BIAS, s.saturationBias, 0.01f);
    readColor(LG_TINT_COLOR, s.tint);
    readColor(LG_TINT_OUTER, s.tintOuter);
    readFloat(LG_TINT_AMOUNT, s.tintAmount, 0.01f);
    readFloat(LG_WHITENESS, s.whiteness, 0.01f);
    readFloat(LG_RADIUS_R, s.radiusMul[0], 0.01f);
    readFloat(LG_RADIUS_G, s.radiusMul[1], 0.01f);
    readFloat(LG_RADIUS_B, s.radiusMul[2], 0.01f);
    readFloat(LG_ASPECT, s.aspect, 0.01f);
    readAngle(LG_ANGLE, s.angleDegrees);
    readAngle(LG_IRIS_ROTATION, rs.irisRotation);
    readFloat(LG_DIRT_STRENGTH, s.dirtStrength, 0.01f);
    readFloat(LG_WHITE_POINT, s.whitePoint, 1.0f);
    readFloat(LG_TONEMAP_MIX, s.toneMapMix, 0.01f);
    readFloat(LG_OPACITY, s.opacity, 0.01f);
    readFloat(LG_SOURCE_OPACITY, s.sourceOpacity, 0.01f);
    readFloat(LG_GAMMA, s.gamma, 1.0f);
    readCheckbox(LG_DITHER, rs.dither);
    {
        A_long maskMode = MASK_ALPHA;
        readPopup(LG_MASK_MODE, maskMode);
        rs.maskLuminance = maskMode == MASK_LUMINANCE;
        readCheckbox(LG_MASK_INVERT, s.maskInvert);
        readFloat(LG_MASK_FEATHER, s.maskFeather, 1.0f);
    }

    lumen::Aberration& ab = s.aberration;
    float iterations = 3;
    readFloat(LG_CA_AMOUNT, ab.amount, 0.01f);
    // Position + Direction: the warm end of the spectrum moves `position`
    // pixels toward Direction (AE angles: 0 = right, 90 = up), the cool end
    // the opposite way. The engine shifts sample t by -t * shift, and red is
    // t = -1, so shift points opposite to where red goes.
    float position = 0.0f, direction = 135.0f;
    readFloat(LG_CA_POSITION, position, 1.0f);
    readAngle(LG_CA_DIRECTION, direction);
    {
        const float rad = direction * 3.14159265f / 180.0f;
        ab.shiftX = -position * std::cos(rad);
        ab.shiftY = position * std::sin(rad); // image y points down
    }
    readFloat(LG_CA_SCALE, ab.scale, 1.0f);
    readAngle(LG_CA_ROTATION, ab.rotationDegrees);
    readFloat(LG_CA_DISTORTION, ab.distortion, 1.0f);
    readFloat(LG_CA_SKEW, ab.skew, 1.0f);
    readFloat(LG_CA_BLUR, ab.blur, 1.0f);
    readFloat(LG_CA_ITERATIONS, iterations, 1.0f);
    readAngle(LG_CA_HUE, ab.hueOffsetDegrees);
    readColor(LG_CA_COLOR1, ab.spectrumColors[0]);
    readColor(LG_CA_COLOR2, ab.spectrumColors[1]);
    readColor(LG_CA_COLOR3, ab.spectrumColors[2]);
    ab.iterations = int(std::lround(iterations));
    if (!err && (err = checkout(LG_CA_CENTER)) == PF_Err_NONE) {
        // Point values arrive in this layer's (already downsampled) pixel space.
        rs.caCenterX = float(double(p.u.td.x_value) / 65536.0);
        rs.caCenterY = float(double(p.u.td.y_value) / 65536.0);
        checkin();
    }
    A_long caTarget = 1, caEdges = 1, caAlpha = 1, caSpectrum = SPECTRUM_RAINBOW;
    readPopup(LG_CA_SPECTRUM, caSpectrum);
    ab.customSpectrum = caSpectrum == SPECTRUM_CUSTOM;
    readPopup(LG_CA_TARGET, caTarget);
    readPopup(LG_CA_EDGES, caEdges);
    readPopup(LG_CA_ALPHA, caAlpha);
    ab.target = lumen::AberrationTarget(clampPopup(caTarget, 1, 3, 1));
    ab.repeatEdges = caEdges != 2;
    ab.preserveAlpha = caAlpha == 2;
    ab.qcaLook = caAlpha == 3; // darker soft edges, matching Quick Chromatic Aberration

    A_long glowMode = 1, tintMode = 1, irisShape = IRIS_HEXAGON, toneMap = 1, method = 2, blend = 2, view = 1;
    readPopup(LG_GLOW_MODE, glowMode);
    readPopup(LG_TINT_MODE, tintMode);
    readPopup(LG_IRIS_SHAPE, irisShape);
    readPopup(LG_TONEMAP, toneMap);
    readPopup(LG_TONEMAP_METHOD, method);
    readPopup(LG_BLEND, blend);
    readPopup(LG_VIEW, view);
    s.glowMode = lumen::GlowMode(clampPopup(glowMode, 1, 2, 1));
    s.tintMode = lumen::TintMode(clampPopup(tintMode, 1, 2, 1));
    s.toneMap = lumen::ToneMap(clampPopup(toneMap, 1, 4, 1));
    s.toneMapMethod = lumen::ToneMapMethod(clampPopup(method, 1, 2, 2));
    s.blend = lumen::BlendMode(clampPopup(blend, 1, 3, 2));
    s.view = lumen::View(clampPopup(view, 1, 3, 1));
    irisShape = clampPopup(irisShape, IRIS_CIRCLE, IRIS_CUSTOM, IRIS_HEXAGON);

    if (s.glowMode == lumen::GlowMode::Iris) {
        // Built-in apertures are made here; a custom layer is sampled at render
        // time (a circle stands in until then, which has the same footprint).
        const int sides = irisShape == IRIS_HEXAGON ? 6 : irisShape == IRIS_OCTAGON ? 8 : 0;
        s.iris = lumen::makeIrisKernel(sides, rs.irisRotation, kIrisKernelSize);
        rs.wantIrisLayer = irisShape == IRIS_CUSTOM;
    }
    rs.wantDirt = s.dirtStrength > 0.0f;

    // Radius is in full-resolution pixels; scale it for preview downsampling
    // so the glow looks the same at Half or Quarter resolution.
    const double sx = double(in_data->downsample_x.num) / double(std::max<A_long>(1, in_data->downsample_x.den));
    const double sy = double(in_data->downsample_y.num) / double(std::max<A_long>(1, in_data->downsample_y.den));
    rs.downsample = float(0.5 * (sx + sy));
    s.radius *= rs.downsample;

    // Aberration distances are in full-resolution pixels too. Reach bounds how
    // far it can move pixels (for buffer expansion): shifts and blur directly,
    // the scale-like transforms relative to the layer's size.
    s.maskFeather *= rs.downsample;
    ab.shiftX *= rs.downsample;
    ab.shiftY *= rs.downsample;
    ab.blur *= rs.downsample;
    const float halfDiag = 0.5f * std::sqrt(float(in_data->width) * float(in_data->width) +
                                            float(in_data->height) * float(in_data->height));
    ab.reach = std::fabs(ab.amount) * (std::sqrt(ab.shiftX * ab.shiftX + ab.shiftY * ab.shiftY) + ab.blur +
                                       (std::fabs(ab.scale) + std::fabs(ab.distortion) + std::fabs(ab.skew)) * 0.01f * halfDiag +
                                       std::fabs(ab.rotationDegrees) * 0.0175f * halfDiag);
    return err;
}

// -------------------------------------------------------------------- rects

bool IsEmpty(const PF_LRect& r) { return r.right <= r.left || r.bottom <= r.top; }

PF_LRect Expanded(PF_LRect r, A_long by) {
    if (IsEmpty(r)) return r;
    r.left -= by;
    r.top -= by;
    r.right += by;
    r.bottom += by;
    return r;
}

PF_LRect Intersect(const PF_LRect& a, const PF_LRect& b) {
    PF_LRect r;
    r.left = std::max(a.left, b.left);
    r.top = std::max(a.top, b.top);
    r.right = std::min(a.right, b.right);
    r.bottom = std::min(a.bottom, b.bottom);
    if (IsEmpty(r)) r.left = r.top = r.right = r.bottom = 0;
    return r;
}

PF_LRect Union(const PF_LRect& a, const PF_LRect& b) {
    if (IsEmpty(a)) return b;
    if (IsEmpty(b)) return a;
    PF_LRect r;
    r.left = std::min(a.left, b.left);
    r.top = std::min(a.top, b.top);
    r.right = std::max(a.right, b.right);
    r.bottom = std::max(a.bottom, b.bottom);
    return r;
}

// A world's footprint in layer coordinates.
PF_LRect WorldRect(const PF_EffectWorld* w) {
    PF_LRect r;
    r.left = w->origin_x;
    r.top = w->origin_y;
    r.right = w->origin_x + w->width;
    r.bottom = w->origin_y + w->height;
    return r;
}

// ------------------------------------------------------------- smart render

// Carried from pre-render to render.
struct PreRenderData {
    bool dirt = false;        // Dirt Layer checked out
    bool iris = false;        // Iris Layer checked out
    bool mask = false;        // Mask Layer checked out
    A_long layerWidth = 0;    // this layer's original size (full resolution)
    A_long layerHeight = 0;
};

void DeletePreRenderData(void* data) { delete static_cast<PreRenderData*>(data); }

// Checks out a layer parameter in full (it's sampled as a texture, not
// aligned pixel-for-pixel). Returns true if the layer has any pixels.
bool CheckoutLayerParam(PF_InData* in_data, PF_PreRenderExtra* extra, A_long index, PF_Err& err) {
    if (err) return false;
    PF_RenderRequest req = extra->input->output_request;
    req.rect.left = req.rect.top = -32000;
    req.rect.right = req.rect.bottom = 32000;
    PF_CheckoutResult result;
    AEFX_CLR_STRUCT(result);
    err = extra->cb->checkout_layer(in_data->effect_ref, index, index, &req, in_data->current_time, in_data->time_step,
                                    in_data->time_scale, &result);
    return !err && !IsEmpty(result.result_rect);
}

// The glow reaches `margin` pixels past the layer's content, so ask AE for
// that much extra input and allow the output to grow by the same amount.
// (AE still clips the request to the comp frame, as it does for any effect.)
PF_Err PreRender(PF_InData* in_data, PF_OutData* out_data, PF_PreRenderExtra* extra) {
    (void)out_data;
    PF_Err err = PF_Err_NONE;
    RenderSettings rs;
    ERR(ReadSettings(in_data, rs));
    if (err) return err;

    const A_long margin = lumen::glowMargin(rs.glow);
    PF_RenderRequest req = extra->input->output_request;
    req.rect = Expanded(req.rect, margin);
    req.preserve_rgb_of_zero_alpha = FALSE;

    PF_CheckoutResult in_result;
    AEFX_CLR_STRUCT(in_result);
    ERR(extra->cb->checkout_layer(in_data->effect_ref, LG_INPUT, LG_INPUT, &req, in_data->current_time,
                                  in_data->time_step, in_data->time_scale, &in_result));
    if (err) return err;

    extra->output->result_rect = Intersect(Expanded(in_result.result_rect, margin), extra->input->output_request.rect);
    extra->output->max_result_rect = Expanded(in_result.max_result_rect, margin);
    extra->output->solid = FALSE;

    PreRenderData* data = new PreRenderData;
    data->layerWidth = in_result.ref_width;
    data->layerHeight = in_result.ref_height;
    if (rs.wantDirt) data->dirt = CheckoutLayerParam(in_data, extra, LG_DIRT_LAYER, err);
    if (rs.wantIrisLayer) data->iris = CheckoutLayerParam(in_data, extra, LG_IRIS_LAYER, err);
    data->mask = CheckoutLayerParam(in_data, extra, LG_MASK_LAYER, err); // empty (false) when no layer is picked
    extra->output->pre_render_data = data;
    extra->output->delete_pre_render_data_func = DeletePreRenderData;

    LG_LOG("PRE radius=%.1f exp=%.2f margin=%d dirt=%d iris=%d", rs.glow.radius, rs.glow.exposure, int(margin),
           int(data->dirt), int(data->iris));
    return err;
}

// Dither: random rounding (a uniform offset of +-0.5 LSB before rounding).
// Fractional values - soft glow gradients - round up or down at random, which
// breaks up banding; values that are already exact, like solid source pixels,
// are never changed.
inline float DitherNoise(A_long x, A_long y, A_long frame, int channel) {
    uint32_t h = uint32_t(x) * 0x8da6b343u ^ uint32_t(y) * 0xd8163841u ^ uint32_t(frame) * 0xcb1ab31fu ^
                 uint32_t(channel) * 0x165667b1u;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return float(h & 0xffffff) / float(0x1000000) - 0.5f;
}

template <typename Pixel> struct Channel;
template <> struct Channel<PF_Pixel8> {
    static constexpr bool kQuantized = true;
    static float in(A_u_char v) { return float(v) / 255.0f; }
    static A_u_char out(float v, float noise) {
        return A_u_char(std::floor(std::min(1.0f, std::max(0.0f, v)) * 255.0f + 0.5f + noise));
    }
};
template <> struct Channel<PF_Pixel16> {
    static constexpr bool kQuantized = true;
    static float in(A_u_short v) { return float(v) / float(PF_MAX_CHAN16); }
    static A_u_short out(float v, float noise) {
        const float q = std::floor(std::min(1.0f, std::max(0.0f, v)) * float(PF_MAX_CHAN16) + 0.5f + noise);
        return A_u_short(std::min(float(PF_MAX_CHAN16), std::max(0.0f, q)));
    }
};
template <> struct Channel<PF_PixelFloat> {
    static constexpr bool kQuantized = false;
    static float in(PF_FpShort v) { return float(v); }
    static PF_FpShort out(float v, float) { return PF_FpShort(v); } // 32 bpc keeps values above 1, no dither needed
};

template <typename Pixel> Pixel* Row(const PF_EffectWorld* w, A_long y) {
    return reinterpret_cast<Pixel*>(reinterpret_cast<char*>(w->data) + y * w->rowbytes);
}

// AE worlds hold straight (unpremultiplied) color; the engine works premultiplied.
template <typename Pixel> void ReadWorld(const PF_EffectWorld* w, const PF_LRect& bounds, lumen::ImageRGBA& img) {
    using C = Channel<Pixel>;
    const A_long dx = w->origin_x - bounds.left;
    const A_long dy = w->origin_y - bounds.top;
    for (A_long y = 0; y < w->height; ++y) {
        const Pixel* row = Row<Pixel>(w, y);
        for (A_long x = 0; x < w->width; ++x) {
            const Pixel& p = row[x];
            const float a = C::in(p.alpha);
            img.at(int(x + dx), int(y + dy)) = lumen::RGBA{C::in(p.red) * a, C::in(p.green) * a, C::in(p.blue) * a, a};
        }
    }
}

template <typename Pixel>
void WriteWorld(const lumen::ImageRGBA& img, const PF_LRect& bounds, PF_EffectWorld* w, bool dither, A_long frame) {
    using C = Channel<Pixel>;
    const bool useDither = dither && C::kQuantized;
    const A_long dx = w->origin_x - bounds.left;
    const A_long dy = w->origin_y - bounds.top;
    for (A_long y = 0; y < w->height; ++y) {
        Pixel* row = Row<Pixel>(w, y);
        for (A_long x = 0; x < w->width; ++x) {
            const lumen::RGBA& c = img.at(int(x + dx), int(y + dy));
            const float a = std::min(1.0f, std::max(0.0f, c.a));
            const float inv = a > 0.0f ? 1.0f / a : 0.0f;
            const A_long lx = x + w->origin_x, ly = y + w->origin_y; // layer coords: noise is stable across tiles
            auto n = [&](int ch) { return useDither ? DitherNoise(lx, ly, frame, ch) : 0.0f; };
            Pixel& p = row[x];
            p.alpha = C::out(a, n(0));
            p.red = C::out(c.r * inv, n(1));
            p.green = C::out(c.g * inv, n(2));
            p.blue = C::out(c.b * inv, n(3));
        }
    }
}

// Lens dirt as a luminance map over the working area: the dirt layer is
// stretched across this layer's original bounds.
template <typename Pixel>
void BuildDirtMap(const PF_EffectWorld* dirtWorld, const PF_LRect& bounds, float layerW, float layerH,
                  std::vector<float>& out) {
    using C = Channel<Pixel>;
    const int w = int(bounds.right - bounds.left), h = int(bounds.bottom - bounds.top);
    out.assign(size_t(w) * size_t(h), 0.0f);
    if (layerW <= 0 || layerH <= 0 || dirtWorld->width <= 0 || dirtWorld->height <= 0) return;
    auto lum = [&](A_long x, A_long y) {
        x = std::min<A_long>(dirtWorld->width - 1, std::max<A_long>(0, x));
        y = std::min<A_long>(dirtWorld->height - 1, std::max<A_long>(0, y));
        const Pixel& p = Row<Pixel>(dirtWorld, y)[x];
        const float a = C::in(p.alpha);
        return (0.2126f * C::in(p.red) + 0.7152f * C::in(p.green) + 0.0722f * C::in(p.blue)) * a;
    };
    for (int y = 0; y < h; ++y) {
        const float v = std::min(1.0f, std::max(0.0f, (float(bounds.top + y) + 0.5f) / layerH));
        const float fy = v * float(dirtWorld->height) - 0.5f;
        const A_long y0 = A_long(std::floor(fy));
        const float ty = fy - float(y0);
        for (int x = 0; x < w; ++x) {
            const float u = std::min(1.0f, std::max(0.0f, (float(bounds.left + x) + 0.5f) / layerW));
            const float fx = u * float(dirtWorld->width) - 0.5f;
            const A_long x0 = A_long(std::floor(fx));
            const float tx = fx - float(x0);
            const float top = lum(x0, y0) + (lum(x0 + 1, y0) - lum(x0, y0)) * tx;
            const float bot = lum(x0, y0 + 1) + (lum(x0 + 1, y0 + 1) - lum(x0, y0 + 1)) * tx;
            out[size_t(y) * size_t(w) + size_t(x)] = top + (bot - top) * ty;
        }
    }
}

// Glow mask over the working area, aligned in layer coordinates (like Set
// Matte): the mask layer's pixel at layer (x, y) masks this layer's (x, y).
template <typename Pixel>
void BuildMaskMap(const PF_EffectWorld* maskWorld, const PF_LRect& bounds, bool luminance, std::vector<float>& out) {
    using C = Channel<Pixel>;
    const int w = int(bounds.right - bounds.left), h = int(bounds.bottom - bounds.top);
    out.assign(size_t(w) * size_t(h), 0.0f);
    for (int y = 0; y < h; ++y) {
        const A_long my = bounds.top + y - maskWorld->origin_y;
        if (my < 0 || my >= maskWorld->height) continue;
        const Pixel* row = Row<Pixel>(maskWorld, my);
        for (int x = 0; x < w; ++x) {
            const A_long mx = bounds.left + x - maskWorld->origin_x;
            if (mx < 0 || mx >= maskWorld->width) continue;
            const Pixel& p = row[mx];
            const float a = C::in(p.alpha);
            const float v = luminance ? a * (0.2126f * C::in(p.red) + 0.7152f * C::in(p.green) + 0.0722f * C::in(p.blue)) : a;
            out[size_t(y) * size_t(w) + size_t(x)] = std::min(1.0f, std::max(0.0f, v));
        }
    }
}

template <typename Pixel>
void RenderWorlds(const PF_EffectWorld* input, PF_EffectWorld* output, RenderSettings& rs, A_long frame,
                  const PF_EffectWorld* dirtWorld, const PF_EffectWorld* irisWorld, const PF_EffectWorld* maskWorld,
                  const PreRenderData* prd) {
    const PF_LRect outRect = WorldRect(output);
    const PF_LRect bounds = input ? Union(WorldRect(input), outRect) : outRect;
    lumen::ImageRGBA src(int(bounds.right - bounds.left), int(bounds.bottom - bounds.top)), dst;
    if (input) ReadWorld<Pixel>(input, bounds, src);

    std::vector<float> mask;
    if (maskWorld) {
        BuildMaskMap<Pixel>(maskWorld, bounds, rs.maskLuminance, mask);
        rs.glow.mask = &mask;
    }

    std::vector<float> dirt;
    if (dirtWorld && prd) {
        BuildDirtMap<Pixel>(dirtWorld, bounds, float(prd->layerWidth) * rs.downsample,
                            float(prd->layerHeight) * rs.downsample, dirt);
        rs.glow.dirt = &dirt;
    }
    if (irisWorld) {
        lumen::ImageRGBA irisImg(int(irisWorld->width), int(irisWorld->height));
        ReadWorld<Pixel>(irisWorld, WorldRect(irisWorld), irisImg);
        rs.glow.iris = lumen::irisKernelFromImage(irisImg, rs.irisRotation, kIrisKernelSize);
    }

    rs.glow.aberration.centerX = rs.caCenterX - float(bounds.left);
    rs.glow.aberration.centerY = rs.caCenterY - float(bounds.top);
    lumen::renderGlow(src, dst, rs.glow);
    WriteWorld<Pixel>(dst, bounds, output, rs.dither, frame);
}

PF_Err SmartRender(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extra) {
    (void)out_data;
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    PF_EffectWorld* input = nullptr;
    PF_EffectWorld* output = nullptr;
    PF_EffectWorld* dirtWorld = nullptr;
    PF_EffectWorld* irisWorld = nullptr;
    PF_EffectWorld* maskWorld = nullptr;
    const PreRenderData* prd = static_cast<const PreRenderData*>(extra->input->pre_render_data);

    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, LG_INPUT, &input));
    if (prd && prd->dirt) ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, LG_DIRT_LAYER, &dirtWorld));
    if (prd && prd->iris) ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, LG_IRIS_LAYER, &irisWorld));
    if (prd && prd->mask) ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, LG_MASK_LAYER, &maskWorld));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &output));

    RenderSettings rs;
    ERR(ReadSettings(in_data, rs));

    if (!err && output && output->width > 0 && output->height > 0) {
        PF_PixelFormat format = PF_PixelFormat_INVALID;
        const void* suite = nullptr;
        ERR(in_data->pica_basicP->AcquireSuite(kPFWorldSuite, kPFWorldSuiteVersion2, &suite));
        PF_WorldSuite2* worldSuite = static_cast<PF_WorldSuite2*>(const_cast<void*>(suite));
        if (!err && worldSuite) {
            ERR(worldSuite->PF_GetPixelFormat(output, &format));
            in_data->pica_basicP->ReleaseSuite(kPFWorldSuite, kPFWorldSuiteVersion2);
        }
        const A_long frame = in_data->time_step != 0 ? in_data->current_time / in_data->time_step : 0;
        if (!err) {
            switch (format) {
            case PF_PixelFormat_ARGB128:
                RenderWorlds<PF_PixelFloat>(input, output, rs, frame, dirtWorld, irisWorld, maskWorld, prd);
                break;
            case PF_PixelFormat_ARGB64:
                RenderWorlds<PF_Pixel16>(input, output, rs, frame, dirtWorld, irisWorld, maskWorld, prd);
                break;
            case PF_PixelFormat_ARGB32:
                RenderWorlds<PF_Pixel8>(input, output, rs, frame, dirtWorld, irisWorld, maskWorld, prd);
                break;
            default: err = PF_Err_BAD_CALLBACK_PARAM; break;
            }
        }
    }

    if (prd && prd->mask) ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, LG_MASK_LAYER));
    if (prd && prd->iris) ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, LG_IRIS_LAYER));
    if (prd && prd->dirt) ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, LG_DIRT_LAYER));
    ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, LG_INPUT));
    return err;
}

} // namespace

// -------------------------------------------------------------- entry points

extern "C" DllExport PF_Err PluginDataEntryFunction2(PF_PluginDataPtr inPtr,
                                                    PF_PluginDataCB2 inPluginDataCallBackPtr,
                                                    SPBasicSuite* inSPBasicSuitePtr, const char* inHostName,
                                                    const char* inHostVersion) {
    (void)inSPBasicSuitePtr;
    (void)inHostName;
    (void)inHostVersion;
    PF_Err result = PF_Err_INVALID_CALLBACK; // the SDK macro assigns to `result`
    result = PF_REGISTER_EFFECT_EXT2(inPtr, inPluginDataCallBackPtr, LG_NAME, LG_MATCH_NAME, LG_CATEGORY,
                                     AE_RESERVED_INFO, "EffectMain", LG_SUPPORT_URL);
    return result;
}

extern "C" DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                                       PF_LayerDef* output, void* extra) {
    (void)output;
    PF_Err err = PF_Err_NONE;
    try {
        switch (cmd) {
        case PF_Cmd_ABOUT: err = About(in_data, out_data); break;
        case PF_Cmd_GLOBAL_SETUP: err = GlobalSetup(out_data); break;
        case PF_Cmd_PARAMS_SETUP: err = ParamsSetup(in_data, out_data); break;
        case PF_Cmd_USER_CHANGED_PARAM:
            err = UserChangedParam(params, static_cast<const PF_UserChangedParamExtra*>(extra));
            break;
        case PF_Cmd_SMART_PRE_RENDER:
            err = PreRender(in_data, out_data, static_cast<PF_PreRenderExtra*>(extra));
            break;
        case PF_Cmd_SMART_RENDER:
            err = SmartRender(in_data, out_data, static_cast<PF_SmartRenderExtra*>(extra));
            break;
        default: break;
        }
    } catch (const std::bad_alloc&) {
        err = PF_Err_OUT_OF_MEMORY;
    } catch (...) {
        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    return err;
}
