/* LumenGlowPiPL.r - plug-in property list. AE reads this to discover the
 * effect before loading its code. Mac: compiled by Rez. Windows: run through
 * cl /EP and the SDK's PiPLtool (see win/build_pipl.bat).
 * Values must match GlobalSetup in LumenGlow.cpp; they come from
 * LumenGlow_Version.h so they can't drift. */

#include "AEConfig.h"
#include "AE_EffectVers.h"
#include "LumenGlow_Version.h"

#ifndef AE_OS_WIN
	#include "AE_General.r"
#endif

resource 'PiPL' (16000) {
	{
		Kind {
			AEEffect
		},
		Name {
			LG_NAME
		},
		Category {
			LG_CATEGORY
		},
#ifdef AE_OS_WIN
	#if defined(AE_PROC_ARM64)
		CodeWinARM64 {"EffectMain"},
	#else
		CodeWin64X86 {"EffectMain"},
	#endif
#else
		CodeMacARM64 {"EffectMain"},
		CodeMacIntel64 {"EffectMain"},
#endif
		AE_PiPL_Version {
			2,
			0
		},
		AE_Effect_Spec_Version {
			PF_PLUG_IN_VERSION,
			PF_PLUG_IN_SUBVERS
		},
		AE_Effect_Version {
			LG_VERSION_NUM
		},
		AE_Effect_Info_Flags {
			0
		},
		AE_Effect_Global_OutFlags {
			LG_OUT_FLAGS
		},
		AE_Effect_Global_OutFlags_2 {
			LG_OUT_FLAGS2
		},
		AE_Effect_Match_Name {
			LG_MATCH_NAME
		},
		AE_Reserved_Info {
			0
		},
		AE_Effect_Support_URL {
			LG_SUPPORT_URL
		}
	}
};
