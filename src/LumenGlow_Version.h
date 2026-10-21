/* LumenGlow_Version.h - identity, version and flags.
 *
 * Shared by the C++ code and the PiPL resource (LumenGlowPiPL.r), which is
 * compiled by Rez/PiPLtool and can't evaluate the SDK's enums. So the values
 * are plain numbers here, and LumenGlow.cpp static_asserts each one against
 * the SDK definition: if any of them is wrong the build fails instead of AE
 * warning about a mismatch at load time.
 *
 * Plain C preprocessor only (no C++), because Rez reads this file too.
 */

#ifndef LUMENGLOW_VERSION_H
#define LUMENGLOW_VERSION_H

#define LG_NAME          "Lumen Glow"
#define LG_MATCH_NAME    "LMNG Lumen Glow"   /* never change once projects use it */
#define LG_CATEGORY      "Stylize"
#define LG_DESCRIPTION   "Physically based glow: iris bokeh, lens dirt, tone mapping, spectral chromatic aberration."
#define LG_SUPPORT_URL   "https://example.com/lumenglow"

#define LG_MAJOR   2
#define LG_MINOR   5
#define LG_BUGFIX  0
#define LG_STAGE   3   /* PF_Stage_RELEASE */
#define LG_BUILD   1

/* PF_VERSION(2, 5, 0, PF_Stage_RELEASE, 1) = (2 << 19) | (5 << 15) | (3 << 9) | 1 */
#define LG_VERSION_NUM   1213953

/* PF_OutFlag_DEEP_COLOR_AWARE */
#define LG_OUT_FLAGS     0x02000000

/* PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG | PF_OutFlag2_REVEALS_ZERO_ALPHA |
   PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_FLOAT_COLOR_AWARE |
   PF_OutFlag2_SUPPORTS_THREADED_RENDERING */
#define LG_OUT_FLAGS2    0x08001488

#endif
