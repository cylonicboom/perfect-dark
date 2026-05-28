#ifndef _IN_MPSETUPS_H
#define _IN_MPSETUPS_H

#include "types.h"

s32 mpsetupLoadCurrentFile(void);
s32 mpsetupSaveCurrentFile(void);
void mpsetupLoadSetup(s32 slotindex);
s32 mpsetupSaveSetup(s32 slotindex, u8 savefile);
void mpsetupCopyAllFromPak(void);
void mpProfileLoadFromPak(void);
void mpProfileSave(void);

extern s32 g_MpProfileHead;
extern s32 g_MpProfileBody;

// Saved Combat Sim custom weapon preset. Serialized as the v2 tail of
// mpsetups.bin; held in g_MpWeaponPresets[] at runtime. weapons[] mirrors
// the encoding used by g_MpSetup.weapons (challenge-aware mpweapon indices,
// see mpSetWeaponSlot). slotfnflags[] is a per-slot bitmask of FNFLAG_*
// values gating bgunPrimary/SecondaryFunctionDisabled at runtime.
struct mpweaponpreset {
	char name[MPWEAPONPRESET_MAXNAME + 1];
	u8 weapons[NUM_MPWEAPONSLOTS];
	u8 slotfnflags[NUM_MPWEAPONSLOTS];
};

extern u8 g_MpWeaponPresetCount;
extern struct mpweaponpreset g_MpWeaponPresets[MPWEAPONPRESET_MAXENTRIES];

// Per-slot function-mode flags applied to the active Combat Sim loadout.
// Bit 0 = primary disabled, bit 1 = secondary disabled (never both set —
// the menu UI prevents that). Consulted by bgunPrimary/Secondary
// FunctionDisabled at runtime. Reset to zero whenever a non-Custom
// weapon set is applied; populated when a saved preset is loaded.
extern u8 g_MpSlotFnFlags[NUM_MPWEAPONSLOTS];

s32 mpWeaponPresetFind(const char *name);
s32 mpWeaponPresetAdd(const char *name, const u8 *weapons, const u8 *slotfnflags);
void mpWeaponPresetReplace(s32 idx, const u8 *weapons, const u8 *slotfnflags);
void mpWeaponPresetRename(s32 idx, const char *newname);
void mpWeaponPresetDelete(s32 idx);

#endif
