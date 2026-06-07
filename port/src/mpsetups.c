#include "types.h"
#include "constants.h"
#include "bss.h"
#include "game/pak.h"
#include "game/filelist.h"
#include "game/menu.h"
#include "game/savebuffer.h"
#include "game/mplayer/mplayer.h"
#include <string.h>
#include "fs.h"
#include "system.h"
#include "config.h"
#include "mpsetups.h"

/*
MP Setup File Format
	# header
	[version{1}]
	[defaultsetup{1}]
	[numsetups{1}]
	# setups
	[setup_1{80}]
	...
	[setup_n{80}]
	# weapon presets (v2+)
	[numweaponpresets{1}]
	[preset_1{sizeof(struct mpweaponpreset)}]
	...
	[preset_m{sizeof(struct mpweaponpreset)}]
 */

#define MPSETUP_VERSION 10 // v10: + scenario high bit (1 bit tail — scenarios 8-15, e.g. Race; block now 100% FULL). v9: + global Lives mode/count (5 bits). v7: + paintclaimtime (4 bits). v6: portoptions folded into options' high 32 bits

#define MPSETUP_EXPORTDIR "$S/exported/"
#define MPSETUP_FILENAME "mpsetups"
#define MPSETUP_FILENAME_EXP "mpsetups-ext"

#define MPSETUP_OP_DEFAULT 0
#define MPSETUP_OP_IMPORT 1
#define MPSETUP_OP_EXPORT 2

#define MPSETUP_IMPORT_OVERWRITE 0
#define MPSETUP_IMPORT_ADD 1

#define MPSETUP_IMPORT_CONFLICT 0x1313

extern struct menudialogdef g_MpSaveSetupNameMenuDialog;

s16 g_MpCurrentSetup = -1;
struct mpsetupfile g_MpSetupFile;

u8 g_MpWeaponPresetCount = 0;
struct mpweaponpreset g_MpWeaponPresets[MPWEAPONPRESET_MAXENTRIES];

static struct mpsetupfile g_ImportMpSetupFile;
static u64 g_MpImportExportFilter[2];

static char g_StatusText[128];
static char g_TitleImportExportDialog[20];
static char g_LabelSetDefault[20] = "Set Default\n";

/* extended mpsetup menu definitions */

static MenuItemHandlerResult menuhandlerRenameSetup(s32 operation, struct menuitem *item, union handlerdata *data);
static MenuItemHandlerResult menuhandlerDeleteSetup(s32 operation, struct menuitem *item, union handlerdata *data);
static MenuItemHandlerResult menuhandlerImportOrExportSettings(s32 operation, struct menuitem *item, union handlerdata *data);
static MenuItemHandlerResult menuhandlerImportAction(s32 operation, struct menuitem *item, union handlerdata *data);
static MenuItemHandlerResult menuhandlerOpenImportExportDialog(s32 operation, struct menuitem *item, union handlerdata *data);
static MenuItemHandlerResult menuhandlerSelectSetupHandler(s32 operation, struct menuitem *item, union handlerdata *data);
static MenuItemHandlerResult menuhandlerSetupRename(s32 operation, struct menuitem *item, union handlerdata *data);
static MenuItemHandlerResult menuhandlerSetupDelete(s32 operation, struct menuitem *item, union handlerdata *data);
static MenuItemHandlerResult menuhandlerSetupSetDefault(s32 operation, struct menuitem *item, union handlerdata *data);

static struct menuitem g_StatusOkMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING,
		(uintptr_t)g_StatusText,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_SELECTABLE_CENTRE,
		L_OPTIONS_347, // "OK"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_StatusOkDialog = {
	MENUDIALOGTYPE_SUCCESS,
	L_OPTIONS_345, // "Cool!"
	g_StatusOkMenuItems,
	NULL,
	MENUDIALOGFLAG_DISABLEBANNER,
	NULL,
};

static struct menuitem g_StatusErrorMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING,
		(uintptr_t)g_StatusText,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_SELECTABLE_CENTRE,
		L_OPTIONS_347, // "OK"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

/* public */
struct menudialogdef g_StatusErrorDialog = {
		MENUDIALOGTYPE_DANGER,
		L_OPTIONS_277, // "Failed"
		g_StatusErrorMenuItems,
		NULL,
		MENUDIALOGFLAG_DISABLEBANNER,
		NULL,
};

static struct menuitem g_RenameSetupItems[] = {
#if VERSION != VERSION_JPN_FINAL
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING,
		(uintptr_t)"Enter the setup name:\n",
		0,
		NULL,
	},
#endif
	{
		MENUITEMTYPE_KEYBOARD,
		18,
		0,
		0,
		1,
		menuhandlerRenameSetup,
	},
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_RenameSetupDialog = {
		MENUDIALOGTYPE_DEFAULT,
		(uintptr_t)"Setup Name:\n",
		g_RenameSetupItems,
		NULL,
		MENUDIALOGFLAG_LITERAL_TEXT,
		NULL,
};

static struct menuitem g_DeleteSetupItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING,
		(uintptr_t)"Delete Setup?\n",
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_SELECTABLE_CENTRE,
		L_OPTIONS_385, // "No"
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE,
		L_OPTIONS_386, // "Yes"
		0,
		menuhandlerDeleteSetup,
	},
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_DeleteSetupDialog = {
		MENUDIALOGTYPE_DANGER,
		(uintptr_t)"Delete Setup\n",
		g_DeleteSetupItems,
		NULL,
		MENUDIALOGFLAG_LITERAL_TEXT,
		NULL,
};

static struct menuitem g_ImportExportItems[] = {
	{
		MENUITEMTYPE_LIST,
		0,
		MENUITEMFLAG_LOCKABLEMINOR | MENUITEMFLAG_LABEL_CUSTOMCOLOUR,
		140,
		0x0000004d,
		menuhandlerImportOrExportSettings,
	},
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_ImportExportDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t) g_TitleImportExportDialog,
	g_ImportExportItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static struct menuitem g_ManageImportExportItems[] = {
	{
		MENUITEMTYPE_SELECTABLE,
		MPSETUP_OP_IMPORT,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Import Settings\n",
		0,
		menuhandlerOpenImportExportDialog,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		MPSETUP_OP_EXPORT,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Export Settings\n",
		0,
		menuhandlerOpenImportExportDialog,
	},
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_ManageImportExportDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t) "Import/Export\n",
	g_ManageImportExportItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static struct menuitem g_MpManageSettingsListItems[] = {
	{
		MENUITEMTYPE_LIST,
		0,
		MENUITEMFLAG_LABEL_CUSTOMCOLOUR,
		160,
		0x00000042,
		menuhandlerSelectSetupHandler,
	},
	{ MENUITEMTYPE_END },

};

/* public */
struct menudialogdef g_ManageSettingsDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t) "Manage Settings\n",
	g_MpManageSettingsListItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	&g_ManageImportExportDialog,
};

static struct menuitem g_ManageSetupItems[] = {
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Rename\n",
		0,
		menuhandlerSetupRename,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Delete\n",
		0,
		menuhandlerSetupDelete,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)g_LabelSetDefault,
		0,
		menuhandlerSetupSetDefault,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_ManageSetupDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Manage Setup",
	g_ManageSetupItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static struct menuitem g_ImportOverrideItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING,
		(uintptr_t) "How to resolve setups\nwith the same name?\n",
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		MPSETUP_IMPORT_ADD,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		(uintptr_t) "Add\n",
		0,
		menuhandlerImportAction,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		MPSETUP_IMPORT_OVERWRITE,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		(uintptr_t) "Overwrite\n",
		0,
		menuhandlerImportAction,
	},
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_ImportOverrideDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Name Conflicts\n",
	g_ImportOverrideItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/* common utils */

static s32 mpsetupDeserialize(FILE *f, struct mpsetupfile *setupfile)
{
	s32 rx = 0;

	rx += fread(&setupfile->version, sizeof(setupfile->version), 1, f);
	rx += fread(&setupfile->defaultsetup, sizeof(setupfile->defaultsetup), 1, f);
	rx += fread(&setupfile->numsetups, sizeof(setupfile->numsetups), 1, f);

	for (int i = 0; i < setupfile->numsetups; ++i) {
		rx += fread(setupfile->setups[i].bytes, sizeof(setupfile->setups[i].bytes), 1, f);
	}

	// v2+ tail: custom weapon presets. Only populated when this is the
	// default setupfile (g_MpSetupFile) — import/export of weapon presets
	// is intentionally skipped to keep the export format symmetric with
	// the existing per-setup checkbox UI.
	if (setupfile == &g_MpSetupFile) {
		g_MpWeaponPresetCount = 0;
		if (setupfile->version >= 2) {
			u8 npresets = 0;
			if (fread(&npresets, sizeof(npresets), 1, f) == 1) {
				if (npresets > MPWEAPONPRESET_MAXENTRIES) {
					npresets = MPWEAPONPRESET_MAXENTRIES;
				}
				for (u8 i = 0; i < npresets; ++i) {
					if (fread(&g_MpWeaponPresets[i], sizeof(g_MpWeaponPresets[i]), 1, f) != 1) {
						break;
					}
					// defensive: terminate name in case the file was hand-edited
					g_MpWeaponPresets[i].name[MPWEAPONPRESET_MAXNAME] = '\0';
					g_MpWeaponPresetCount = i + 1;
				}
			}
		}
	}

	return rx;
}

static s32 mpsetupSerialize(FILE *f, struct mpsetupfile *setupfile)
{
	s32 wx = 0;

	wx += fwrite(&setupfile->version, sizeof(setupfile->version), 1, f);
	wx += fwrite(&setupfile->defaultsetup, sizeof(setupfile->defaultsetup), 1, f);
	wx += fwrite(&setupfile->numsetups, sizeof(setupfile->numsetups), 1, f);

	for (int i = 0; i < setupfile->numsetups; ++i) {
		wx += fwrite(setupfile->setups[i].bytes, sizeof(setupfile->setups[i].bytes), 1, f);
	}

	// v2+ tail: only attach weapon presets when serialising the active
	// setupfile. The export path uses a temporary mpsetupfile (no preset
	// roundtrip) and the import path consumes one without the tail.
	if (setupfile == &g_MpSetupFile && setupfile->version >= 2) {
		fwrite(&g_MpWeaponPresetCount, sizeof(g_MpWeaponPresetCount), 1, f);
		for (u8 i = 0; i < g_MpWeaponPresetCount; ++i) {
			fwrite(&g_MpWeaponPresets[i], sizeof(g_MpWeaponPresets[i]), 1, f);
		}
		wx += 1;
	}

	return wx;
}

static FILE *mpsetupOpenFile(bool write, u8 op) {
	const char *filename = fsFullPath("$S/" MPSETUP_FILENAME ".bin");

	if (op == MPSETUP_OP_EXPORT) {
		// create export directory if it doesn't exist
		if (fsFileSize(MPSETUP_EXPORTDIR) < 0) {
			if (fsCreateDir(MPSETUP_EXPORTDIR) != 0) {
				return NULL;
			}
		}
		filename = fsFullPath(MPSETUP_EXPORTDIR MPSETUP_FILENAME_EXP ".bin");
	} else if (op == MPSETUP_OP_IMPORT) {
		// same name as export but different folder
		filename = fsFullPath("$S/" MPSETUP_FILENAME_EXP ".bin");
	}

	FILE *f;

	if (fsFileSize(filename) < 0) {
		// setup file doesn't exist: create one
		f = fsFileOpenWrite(filename);
		fsFileFree(f);
	}

	if (write) {
		f = fsFileOpenWrite(filename);
	} else {
		f = fsFileOpenRead(filename);
	}

	if (f == NULL) {
		sysLogPrintf(LOG_ERROR, "Unable to open mp setup file");
		return NULL;
	}

	return f;
}

static s32 mpsetupSaveFile(u8 op, struct mpsetupfile *setupfile)
{
	FILE *f = mpsetupOpenFile(true, op);
	if (f == NULL) {
		return -1;
	}

	s32 nwritten = mpsetupSerialize(f, setupfile);
	if (nwritten < 1) {
		fsFileFree(f);
		sysLogPrintf(LOG_ERROR, "Unable to write the MP setup file");
		snprintf(g_StatusText, sizeof(g_StatusText), "Unable to write the setup file\n");
		return -1;
	}

	fsFileFree(f);
	return 0;
}

static s32 mpsetupLoadFile(struct mpsetupfile *setupfile, u8 op)
{
	FILE *f = mpsetupOpenFile(false, op);
	if (f == NULL) {
		return -1;
	}

	mpsetupDeserialize(f, setupfile);

	if (op == MPSETUP_OP_DEFAULT && setupfile->defaultsetup > 0) {
		mpsetupLoadSetup(setupfile->defaultsetup - 1);
	}

	fsFileFree(f);

	return 0;
}

static s32 mpsetupImportFile(u8 op, u8 skipOverlap)
{
	if (!skipOverlap) {
		// check for names overlap
		u8 overlap = false;
		for (int i = 0; i < g_ImportMpSetupFile.numsetups; ++i) {
			for (int j = 0; j < g_MpSetupFile.numsetups; ++j) {
				if (strcmp(g_ImportMpSetupFile.setups[i].bytes, g_MpSetupFile.setups[j].bytes) == 0) {
					overlap = true;
					return MPSETUP_IMPORT_CONFLICT;
				}
			}
		}
	}

	for (int i = 0; i < g_ImportMpSetupFile.numsetups; ++i) {
		s16 overlapIdx = -1;
		for (int j = g_MpSetupFile.numsetups-1; j >= 0; --j) {
			if (strcmp(g_ImportMpSetupFile.setups[i].bytes, g_MpSetupFile.setups[j].bytes) == 0) {
				overlapIdx = j;
				break;
			}
		}

		s16 importIdx = overlapIdx;
		if (overlapIdx < 0 || op == MPSETUP_IMPORT_ADD) {
			if (g_MpSetupFile.numsetups == MPSETUP_MAXSETUPS) {
				snprintf(g_StatusText, sizeof(g_StatusText), "Number of setups exceeds %d\n", MPSETUP_MAXSETUPS);
				return -1;
			}
			importIdx = g_MpSetupFile.numsetups++;
		}

		memcpy(g_MpSetupFile.setups[importIdx].bytes, g_ImportMpSetupFile.setups[i].bytes, MPSETUP_BLOCKSIZE);
	}

	return mpsetupSaveCurrentFile();
}

static s32 mpsetupExportFile(void)
{
	struct mpsetupfile expMpSetupFile;
	expMpSetupFile.numsetups = 0;
	expMpSetupFile.defaultsetup = 0;
	expMpSetupFile.version = MPSETUP_VERSION;

	u8 maxsetups = MPSETUP_MAXSETUPS;
	maxsetups = MIN(maxsetups, g_MpSetupFile.numsetups);
	for (int i = 0; i < maxsetups; ++i) {
		u8 bank = i < 64 ? 0 : 1;
		u8 bit = i - bank * 64;
		if (g_MpImportExportFilter[bank] & (1 << bit)) {
			u8 n = expMpSetupFile.numsetups;
			expMpSetupFile.numsetups++;
			char *name = g_MpSetupFile.setups[i].bytes;
			memcpy(expMpSetupFile.setups[n].bytes, g_MpSetupFile.setups[i].bytes, MPSETUP_BLOCKSIZE);
			expMpSetupFile.numsetups = n + 1;
		}
	}

	s32 err = mpsetupSaveFile(MPSETUP_OP_EXPORT, &expMpSetupFile);
	if (err) {
		snprintf(g_StatusText, sizeof(g_StatusText), "Unable to write\nsetup file\n");
	}

	return err;
}

static s32 mpsetupDelete(void)
{
	s32 slotindex = g_Menus[g_MpPlayerNum].mpsetup.slotindex;
	for (int i = slotindex; i < g_MpSetupFile.numsetups - 1; ++i) {
		u8* dst = g_MpSetupFile.setups[i].bytes;
		u8* src = g_MpSetupFile.setups[i+1].bytes;
		memcpy(dst, src, MPSETUP_BLOCKSIZE);
	}

	if (g_MpCurrentSetup > slotindex) {
		g_MpCurrentSetup--;
	}
	else if (g_MpCurrentSetup == slotindex) {
		g_MpCurrentSetup = -1;
	}

	if (g_MpSetupFile.defaultsetup > slotindex + 1) {
		 g_MpSetupFile.defaultsetup--;
	}
	else if (g_MpSetupFile.defaultsetup == slotindex + 1) {
		g_MpSetupFile.defaultsetup = 0;
	}

	g_MpSetupFile.numsetups--;
	return mpsetupSaveCurrentFile();
}

/* menu handlers */

// Rename setup dialog
static MenuItemHandlerResult menuhandlerRenameSetup(s32 operation, struct menuitem *item, union handlerdata *data)
{
	char *name = data->keyboard.string;
	s32 slotindex = g_Menus[g_MpPlayerNum].mpsetup.slotindex;
	struct setupblock *setup = &g_MpSetupFile.setups[slotindex];
	s32 err;

	switch (operation) {
	case MENUOP_GETTEXT:
		strcpy(name, setup->bytes);
		break;
	case MENUOP_SETTEXT:
		strcpy(g_MpSetup.name, name);
		break;
	case MENUOP_SET:
		strcpy(setup->bytes, name);
		err = mpsetupSaveSetup(slotindex, true);
		if (!err) {
			menuPopDialog();
		} else {
			// TODO
			// menuPushDialog(&g_FilemgrFileSavedMenuDialog);
		}
		break;
	}

	return 0;
}

// Delete setup dialog
static MenuItemHandlerResult menuhandlerDeleteSetup(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		s32 err = mpsetupDelete();
		if (!err) {
			menuPopDialog();
			menuPopDialog();
		} else {
			// TODO
		}
	}

	return 0;
}

// Import (or Export) settings dialog
static MenuItemHandlerResult menuhandlerImportOrExportSettings(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *labels[] = {
		"Select All",
		"Select None",
		"Import",
		"Export",
	};

	s32 op = g_Menus[g_MpPlayerNum].mpsetup.unke24;

	struct mpsetupfile *setupfile = op == MPSETUP_OP_IMPORT ? &g_ImportMpSetupFile : &g_MpSetupFile;

	u8 numsetups = setupfile->numsetups;
	u8 numitems = numsetups + 3;
	u8 bank = data->list.value < 64 ? 0 : 1;
	u8 bit = data->list.value - bank * 64;
	u8 hasselection = (g_MpImportExportFilter[0] | g_MpImportExportFilter[1]) != 0;
	u8 ofs;
	s32 err;
	u32 colour;
	s32 index;

	switch (operation) {
	case MENUOP_GETCOLOUR:
		colour = data->list.unk04;
		if (data->list.value == numitems - 1) {
			data->list.unk04 = hasselection ? colour : (colour & 0xffffff00) | 0x66;
		}
		break;

	case MENUOP_GETOPTIONCOUNT:
		data->list.value = numitems;
		break;

	case MENUOP_GETOPTIONTEXT:
		if (data->list.value < numsetups) {
			return (uintptr_t) setupfile->setups[data->list.value].bytes;
		} else {
			// creates an offset to select the "Export" or "Import" label for the last item
			ofs = (data->list.value == numitems - 1) ? op - 1 : 0;
			return (intptr_t)labels[data->list.value - numsetups + ofs];
		}

	case MENUOP_SET:
		if (data->list.value < setupfile->numsetups) {
			g_MpImportExportFilter[bank] ^= (1 << bit);
		} else {
			index = data->list.value - setupfile->numsetups;

			switch (index) {
			// Select All
			case 0:
				g_MpImportExportFilter[0] = g_MpImportExportFilter[1] = -1;
				break;
			// Select None
			case 1:
				g_MpImportExportFilter[0] = g_MpImportExportFilter[1] = 0;
				break;
			// Export (or Import)
			case 2:
				if (hasselection) {
					err = op == MPSETUP_OP_IMPORT ? mpsetupImportFile(0, false) : mpsetupExportFile();
					if (!err) {
						menuPopDialog();
						if (op == MPSETUP_OP_IMPORT) {
							// back to the 'Manage Settings' screen
							menuPopDialog();
							menuPushDialog(&g_ManageSettingsDialog);
						} else {
							snprintf(g_StatusText, sizeof(g_StatusText), "File %s.bin\nwritten to the folder 'exported'\n", MPSETUP_FILENAME_EXP);
							menuPushDialog(&g_StatusOkDialog);
						}
					} else {
						if (err == MPSETUP_IMPORT_CONFLICT) {
							menuPopDialog();
							menuPushDialog(&g_ImportOverrideDialog);
						} else {
							menuPushDialog(&g_StatusErrorDialog);
						}
					}
				}
				break;
			}
		}
		break;

	case MENUOP_GETSELECTEDINDEX:
		data->list.value = 0x000fffff;
		break;

	case MENUOP_GETLISTITEMCHECKBOX:
		if (data->list.value < numsetups) {
			data->list.unk04 = (g_MpImportExportFilter[bank] & (1 << bit));
		}
		break;
	}

	return 0;
}

// Import: overwrite dialog
static MenuItemHandlerResult menuhandlerImportAction(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		s32 err = mpsetupImportFile(item->param, true);
		if (!err) {
			menuPopDialog();
			menuPushDialog(&g_ManageSettingsDialog);
		} else {
			menuPushDialog(&g_StatusErrorDialog);
		}
	}

	return 0;
}

// Manage Import/Export settings dialog
static MenuItemHandlerResult menuhandlerOpenImportExportDialog(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_SET:
		if (item->param == MPSETUP_OP_IMPORT) {
			strcpy(g_TitleImportExportDialog, "Import Settings\n");
			if (fsFileSize("$S/" MPSETUP_FILENAME_EXP ".bin") < 0) {
				snprintf(
					g_StatusText, sizeof(g_StatusText),
					"No import file found.\n"
					"Place the file %s.bin\n"
					"Next to your %s.bin file\n",
					MPSETUP_FILENAME_EXP, MPSETUP_FILENAME
				);
				menuPushDialog(&g_StatusErrorDialog);
				return 0;
			}

			mpsetupLoadFile(&g_ImportMpSetupFile, MPSETUP_OP_IMPORT);
		} else {
			strcpy(g_TitleImportExportDialog, "Export Settings\n");
		}
		g_Menus[g_MpPlayerNum].mpsetup.unke24 = item->param;
		g_MpImportExportFilter[0] = g_MpImportExportFilter[1] = -1;
		menuPushDialog(&g_ImportExportDialog);
		break;
	}

	return 0;
}

// Manage settings dialog
static MenuItemHandlerResult menuhandlerSelectSetupHandler(s32 operation, struct menuitem *item, union handlerdata *data)
{
	u32 colour;
	switch (operation) {
	case MENUOP_GETCOLOUR:
		colour = data->list.unk04;
		if (data->list.value + 1 == g_MpSetupFile.defaultsetup) {
			data->list.unk04 = (colour & 0xffff0000) | 0xff;
		}
		break;
	case MENUOP_GETOPTIONCOUNT:
		data->list.value = g_MpSetupFile.numsetups;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (uintptr_t) g_MpSetupFile.setups[data->list.value].bytes;
	case MENUOP_SET:
		g_Menus[g_MpPlayerNum].mpsetup.slotindex = data->list.value;
		if (data->list.value == g_MpSetupFile.defaultsetup - 1) {
			strcpy(g_LabelSetDefault, "Clear Default\n");
		} else {
			strcpy(g_LabelSetDefault, "Set Default\n");
		}
		menuPushDialog(&g_ManageSetupDialog);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->list.value = 0xfffff;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerSetupRename(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		menuPushDialog(&g_RenameSetupDialog);
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerSetupDelete(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		menuPushDialog(&g_DeleteSetupDialog);
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerSetupSetDefault(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		s32 selected = g_Menus[g_MpPlayerNum].mpsetup.slotindex;
		// clicked on "clear default"
		if (selected == g_MpSetupFile.defaultsetup - 1) {
			g_MpSetupFile.defaultsetup = 0;
			strcpy(g_LabelSetDefault, "Set Default\n");
		} else {
			g_MpSetupFile.defaultsetup = selected + 1;
			strcpy(g_LabelSetDefault, "Clear Default\n");
		}

		mpsetupSaveCurrentFile();
	}

	return 0;
}

/* weapon preset helpers (port-only) */

s32 mpWeaponPresetFind(const char *name)
{
	if (name == NULL || name[0] == '\0') {
		return -1;
	}
	for (u8 i = 0; i < g_MpWeaponPresetCount; ++i) {
		if (strcmp(g_MpWeaponPresets[i].name, name) == 0) {
			return i;
		}
	}
	return -1;
}

s32 mpWeaponPresetAdd(const char *name, const u8 *weapons, const u8 *slotfnflags)
{
	if (g_MpWeaponPresetCount >= MPWEAPONPRESET_MAXENTRIES) {
		return -1;
	}
	struct mpweaponpreset *p = &g_MpWeaponPresets[g_MpWeaponPresetCount];
	strncpy(p->name, name, MPWEAPONPRESET_MAXNAME);
	p->name[MPWEAPONPRESET_MAXNAME] = '\0';
	memcpy(p->weapons, weapons, NUM_MPWEAPONSLOTS);
	memcpy(p->slotfnflags, slotfnflags, NUM_MPWEAPONSLOTS);
	return g_MpWeaponPresetCount++;
}

void mpWeaponPresetReplace(s32 idx, const u8 *weapons, const u8 *slotfnflags)
{
	if (idx < 0 || idx >= g_MpWeaponPresetCount) {
		return;
	}
	memcpy(g_MpWeaponPresets[idx].weapons, weapons, NUM_MPWEAPONSLOTS);
	memcpy(g_MpWeaponPresets[idx].slotfnflags, slotfnflags, NUM_MPWEAPONSLOTS);
}

void mpWeaponPresetRename(s32 idx, const char *newname)
{
	if (idx < 0 || idx >= g_MpWeaponPresetCount || newname == NULL) {
		return;
	}
	strncpy(g_MpWeaponPresets[idx].name, newname, MPWEAPONPRESET_MAXNAME);
	g_MpWeaponPresets[idx].name[MPWEAPONPRESET_MAXNAME] = '\0';
}

void mpWeaponPresetDelete(s32 idx)
{
	if (idx < 0 || idx >= g_MpWeaponPresetCount) {
		return;
	}
	for (u8 i = idx; i + 1 < g_MpWeaponPresetCount; ++i) {
		g_MpWeaponPresets[i] = g_MpWeaponPresets[i + 1];
	}
	g_MpWeaponPresetCount--;
}

/* public functions */

s32 mpsetupLoadCurrentFile(void)
{
	return mpsetupLoadFile(&g_MpSetupFile, MPSETUP_OP_DEFAULT);
}

s32 mpsetupSaveCurrentFile(void)
{
	g_MpSetupFile.version = MPSETUP_VERSION;
	return mpsetupSaveFile(MPSETUP_OP_DEFAULT, &g_MpSetupFile);
}

s32 mpsetupSaveSetup(s32 slotindex, u8 savefile)
{
	struct savebuffer setup;

	// request to add a new setup
	if (slotindex == g_MpSetupFile.numsetups) {
		g_MpCurrentSetup = g_MpSetupFile.numsetups++;
	}

	savebufferClear(&setup);
	mpsetupfileSaveWad(&setup);

	memcpy(g_MpSetupFile.setups[slotindex].bytes, setup.bytes, MPSETUP_BLOCKSIZE);

	return savefile ? mpsetupSaveCurrentFile() : 0;
}

void mpsetupLoadSetup(s32 index)
{
	struct savebuffer buffer;
	savebufferClear(&buffer);
	struct setupblock *block = &g_MpSetupFile.setups[index];
	memcpy(&buffer.bytes, block->bytes, MPSETUP_BLOCKSIZE);
	mpsetupfileLoadWad(&buffer, g_MpSetupFile.version);
	g_MpCurrentSetup = index;
}

void mpsetupCopyAllFromPak(void)
{
	if (fsFileSize("$S/" MPSETUP_FILENAME ".bin") > 0) {
		return;
	}

	filelistCreate(1, FILETYPE_MPSETUP);
	filelistsTick();

	for (int i = 0; i < g_FileLists[1]->numfiles; ++i) {
		struct savebuffer buffer;
		savebufferClear(&buffer);
		struct filelistfile *file = &g_FileLists[1]->files[i];
		s32 device = pakFindBySerial(file->deviceserial);
		s32 err = pakReadBodyAtGuid(device, file->fileid, buffer.bytes, 0);

		if (err != 0) {
			sysLogPrintf(LOG_ERROR, "Unable to read pak. device %d fileid %d deviceserial %d err %d",
				device, file->fileid, file->deviceserial, err);
			continue;
		}

		mpsetupfileLoadWad(&buffer, 0);

		// save the file when writing the last setup
		u8 savefile = i == g_FileLists[1]->numfiles - 1;
		mpsetupSaveSetup(g_MpSetupFile.numsetups, savefile);
	}

	// to reset the mp setup
	mpInit(false);
}

extern char g_MpProfileName[];

// Persist player 0's Combat Sim profile name to pd.ini. Called only from the
// "Load Player" success path so we never overwrite the ini with an mpInit
// default (e.g. "Player 1") from the Agent file picker.
void mpProfileSave(void)
{
	const struct mpplayerconfig *cfg = &g_PlayerConfigsArray[0];
	if (cfg->base.name[0] == '\0') {
		return;
	}
	strncpy(g_MpProfileName, cfg->base.name, MAX_PLAYERNAME - 1);
	g_MpProfileName[MAX_PLAYERNAME - 1] = '\0';
	size_t len = strlen(g_MpProfileName);
	if (len > 0 && g_MpProfileName[len - 1] == '\n') {
		g_MpProfileName[len - 1] = '\0';
	}
	g_MpProfileHead = cfg->base.mpheadnum;
	g_MpProfileBody = cfg->base.mpbodynum;
	configSave(CONFIG_PATH);
}

void mpProfileLoadFromPak(void)
{
	if (!g_MpProfileName[0]) {
		return;
	}

	// Skip if the profile is already loaded — fileguid being set means a real
	// pak load has happened and the runtime config is still valid. If mpInit
	// has cleared it (Agent file picker, etc.), fileguid drops to 0 and we
	// re-enumerate / reload on the next menu open.
	if (g_PlayerConfigsArray[0].fileguid.fileid != 0) {
		return;
	}

	filelistCreate(2, FILETYPE_MPPLAYER);
	filelistsTick();

	struct filelist *list = g_FileLists[2];
	if (!list || list->numfiles <= 0) {
		return;
	}

	for (int i = 0; i < list->numfiles; ++i) {
		struct filelistfile *file = &list->files[i];
		s32 device = pakFindBySerial(file->deviceserial);
		if (device < 0) {
			continue;
		}

		// file->name holds the first 16 bytes of the pak file body (populated
		// by filelistUpdate during filelistsTick). mpplayerfileGetOverview
		// decodes the player name from those bytes.
		char namebuf[MAX_PLAYERNAME];
		u32 playtime;
		mpplayerfileGetOverview(file->name, namebuf, &playtime);

		// Strip the trailing \n delimiter the game appends to all player names.
		size_t len = strlen(namebuf);
		if (len > 0 && namebuf[len - 1] == '\n') {
			namebuf[len - 1] = '\0';
		}

		if (strcmp(namebuf, g_MpProfileName) == 0) {
			// Use the same load path as the in-game "Load Player" menu so the
			// loaded profile (handicap, fileguid, etc.) is set up identically.
			if (mpplayerfileLoad(0, device, file->fileid, file->deviceserial) == 0) {
				const u8 newhead = g_PlayerConfigsArray[0].base.mpheadnum;
				const u8 newbody = g_PlayerConfigsArray[0].base.mpbodynum;
				// Persist head/body to pd.ini so the title-screen laptop scene
				// can render the player's character on the very first frame
				// of the next session (before this hook has a chance to run).
				// Only writes when the cached values actually change to avoid
				// hammering the ini on every main-menu re-entry.
				if (g_MpProfileHead != (s32)newhead || g_MpProfileBody != (s32)newbody) {
					g_MpProfileHead = newhead;
					g_MpProfileBody = newbody;
					configSave(CONFIG_PATH);
				}
				sysLogPrintf(LOG_NOTE, "mp profile '%s' auto-loaded (head=%d body=%d)",
						g_MpProfileName, g_MpProfileHead, g_MpProfileBody);
			}
			return;
		}
	}

	sysLogPrintf(LOG_NOTE, "mp profile '%s' not found in pak (%d files scanned)",
			g_MpProfileName, list->numfiles);
}
