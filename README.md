# Lumen Glow

A physically based glow effect for After Effects, built as a native C++ plugin.
It uses the same "bloom pyramid" method as Deep Glow: the bright parts of the
layer are shrunk level by level, blurred, scaled back up and summed with an
inverse-square falloff. Big radii cost almost nothing extra (~20 ms for a 1080p
frame on the CPU at any radius), and it works at 8, 16 and 32 bpc with
Multi-Frame Rendering.

## Using it

1. Apply **Effect > Stylize > Lumen Glow**. It starts on the **Deep Glow Match**
   preset: the same Radius and Exposure numbers give the same glow as Deep Glow.
2. Pick a **Preset** for a starting look, then adjust **Radius** (size) and
   **Exposure** (brightness). Changing any control flips the Preset menu to
   *Custom*, so it always describes what you're seeing.
3. Use **View > Highlights** to see exactly what will glow while you set
   **Threshold**; switch back to **Final** when done.

## Controls

| Group | Control | What it does |
|---|---|---|
| | Preset | Deep Glow Match, Soft Bloom, Neon, Hot Core, Anamorphic Streak, Dreamy Haze, Glow Behind Colored Objects, Lens Fringe, RGB Glitch Split, Prism Spectrum, Duotone Halo, Cinematic HDR (ACES), Bokeh Lights (Iris), Dirty Lens. |
| | View | Final, Glow Only (for comping it yourself), Highlights (threshold preview). |
| | Glow Mode | Exponential (smooth inverse-square glow) or Iris (Bokeh): the glow takes the shape of a lens aperture. |
| Glow | Radius | Glow size in pixels, on Deep Glow's scale (default 550). Adapts to preview resolution. |
| | Exposure | Brightness in stops, on Deep Glow's scale (default 1). +1 doubles, -1 halves. |
| | Falloff | 42% matches Deep Glow. Lower = bigger soft halo, higher = tighter hot core. |
| | Threshold, Threshold Smoothness | Only pixels brighter than Threshold glow; Smoothness softens the cutoff. Above 100% for HDR. |
| | Saturation Bias | Negative glows desaturated, positive glows extra-saturated. |
| Glow > Mask | Mask Layer | Only the parts of this layer inside the chosen layer glow (the glow still spreads past the mask). Aligned in layer space, like Set Matte. |
| | Mode, Invert | Use the mask layer's Alpha or Luminance; Invert glows everywhere except inside it. |
| | Feather | Softens the mask's edges by this many pixels: no mask feather or blur needed on the mask layer. |
| Color | Tint Mode | Single Color, or Inner / Outer: the core takes Tint Color and the halo Outer Color. |
| | Tint Color, Outer Color, Tint Amount | Recolor the glow (not the layer). |
| | Whiteness | Burns the hottest parts of the glow toward white. |
| | Red / Green / Blue Radius | Per-channel radius multipliers (100% = Radius). |
| Chromatic Aberration | Apply To | Off, Glow, or Glow + Layer (the layer itself is aberrated too). Turning it on with everything at 0 starts you at Scale 1.5%. |
| | Amount | Master strength for every transform below (100% = as set). |
| | Center | Lens center; drag it in the comp viewer. |
| | Position, Direction | Split the colors apart by Position pixels: warm toward Direction (default 135° = top-left), cool the opposite way. Like Quick Chromatic Aberration's Position. |
| | Scale | Radial fringing that grows toward the frame edges, like a real lens. |
| | Rotation, Lens Distortion, Skew | Spectrum ends rotate, barrel/pincushion, or skew in opposite directions. |
| | Blur | Softens the outer colors of the spectrum; the middle stays sharp. |
| | Iterations | 3 = classic RGB split; more = a smooth rainbow spectrum (slower). |
| | Hue Offset | Rotates which colors fringe which way: the full 360 degrees (Rainbow spectrum). |
| | Spectrum, Spectrum Color 1/2/3 | Rainbow (hue wheel) or Custom Colors: the split blends Color 1 -> 2 -> 3. With Scale, Color 3 is the outer fringe; with Position, Colors 1 and 3 go opposite ways. |
| | Edges, Alpha | Repeat Edge Pixels or Transparent; Alpha: Aberrate, Preserve, or QCA Look (darker soft edges, matching Quick Chromatic Aberration). |
| Shape | Aspect Ratio, Angle | Stretch the glow along Angle (positive) or across it (negative): anamorphic streaks. |
| Iris | Iris Shape | Circle, Hexagon, Octagon, or Custom Layer (any layer's luminance becomes the aperture). |
| | Iris Layer, Iris Rotation | The layer used by Custom Layer, and the aperture's rotation. |
| Lens Dirt | Dirt Layer, Dirt Strength | A dirt/smudge texture stretched over the layer; the glow lights up its specks. |
| Tone Mapping | Tone Map | None, Reinhard, Hable Filmic, ACES Filmic: compress hot (HDR) results into range. |
| | Method | Per Channel (highlights burn to white) or Preserve Hue (saturated highlights stay colorful). |
| | White Point, Tone Map Mix | Brightness that maps to pure white; blend with the un-tone-mapped result. |
| Compositing | Blend Mode | Screen (default, like Deep Glow), Add (hotter, can clip; pair with Tone Mapping), Behind (glow behind the layer). |
| | Glow Opacity, Source Opacity | Fade the glow, or fade the original layer while keeping its glow. |
| Advanced | Gamma Correction | 1 = off. 2.2 = glow in linear light (physically correct, softer tails). |
| | Dither | Breaks up banding in 8/16 bpc. Never alters solid pixels. |

Recipes:
- Red text, white glow: preset **Glow Behind Colored Objects**.
- Light streaks: preset **Anamorphic Streak**, then turn Angle.
- Bokeh from city lights / particles: preset **Bokeh Lights (Iris)**; raise Threshold so only the lights glow.
- Custom bokeh: Glow Mode Iris, Iris Shape Custom Layer, pick a shape or photo layer (turn its visibility off).
- Lens dirt: preset **Dirty Lens**, then pick a dirt texture as the Dirt Layer (turn its visibility off).
- Subtle realism for CG/motion graphics: Chromatic Aberration > Apply To Glow + Layer, Scale 1-2%, Iterations 8-12.
- Very hot HDR glows: Blend Mode Add + Tone Map ACES Filmic, Method Preserve Hue to keep colors.

## Building

You need the After Effects SDK (free, from Adobe's developer site; sign in with
your Adobe account) and CMake 3.21 or newer.

### macOS (one universal build for Apple Silicon and Intel)

1. Install Xcode from the App Store, then `sudo xcode-select -s /Applications/Xcode.app`.
2. `brew install cmake`
3. `./build_mac.sh /path/to/AfterEffectsSDK`

Output: `build/Release/LumenGlow.plugin`

### Windows (x64)

Needs Visual Studio 2022 with "Desktop development with C++". From a
"Developer Command Prompt for VS 2022":

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DAE_SDK_DIR=C:/path/to/AfterEffectsSDK
cmake --build build --config Release
```

Output: `build\Release\LumenGlow.aex`. It's linked against the static C++
runtime, so render nodes don't need the Visual C++ redistributable.

## Installing (every artist machine and every render node)

Copy the plugin into the shared MediaCore folder, which every installed
version of After Effects reads:

- macOS: `/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/`
- Windows: `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\`

Restart After Effects. The effect appears under **Effect > Stylize > Lumen Glow**.

macOS note: the build is ad-hoc signed. If you copy it to other Macs over the
network or a download, clear the quarantine flag once per machine:

```
sudo xattr -dr com.apple.quarantine "/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/LumenGlow.plugin"
```

For wide distribution, sign it with a Developer ID certificate and notarize it.

Render farm: install the same plugin version on every node. A node without it
renders the layer without the glow, and aerender logs a missing-effect warning.

## First-run checklist

1. The effect shows up under Effect > Stylize and the About box says v2.5.0.
2. 8, 16 and 32 bpc projects all render without errors.
3. Glow extends past the edges of a text layer (no hard rectangle).
4. Half/Quarter preview resolution looks the same as Full, just softer.
5. A project saved on Mac renders on a Windows node, and vice versa.
6. Side-by-side with Deep Glow at default settings, the glows should match.
   Calibration lives in `kRadiusScale` and `kBaseGainStops` in
   `src/GlowCore.cpp`; `tests/profile_fit.cpp` measures the falloff curve.

## Project layout

```
src/GlowCore.*          glow engine (no After Effects dependencies)
src/LumenGlow.cpp       AE plugin: parameters, Smart Render, pixel formats
src/LumenGlowPiPL.r     plugin property list (how AE discovers the effect)
src/LumenGlow_Version.h name, match name, version and flags (shared by both)
tests/glow_preview.cpp  renders test images with the engine and times it
mac/, win/              platform build helpers
```

The engine test builds without the SDK:

```
clang++ -std=c++17 -O2 -Isrc src/GlowCore.cpp tests/glow_preview.cpp -o glow_preview
./glow_preview .        # writes glow_preview.ppm and prints timings
```

Don't change `LG_MATCH_NAME` after projects use the effect: After Effects uses
it to find the effect in saved projects.
