#include <string.h>
#ifndef _WIN32
#include <strings.h> // strcasecmp; gcc14-era toolchains (flatpak 24.08 SDK) error on the implicit declaration
#endif
#include <ctype.h>
#include <stdio.h>  // snprintf/sscanf; SDL3's SDL_stdinc.h no longer includes it for us
#include <stdlib.h> // abs/atoi; SDL3's SDL_stdinc.h no longer includes it for us
#include <SDL3/SDL.h>
#include <PR/ultratypes.h>
#include <PR/os_thread.h>
#include <PR/os_cont.h>
#include "platform.h"
#include "input.h"
#include "video.h"
#include "config.h"
#include "utils.h"
#include "system.h"
#include "fs.h"
#include "net/net.h"
// Needed for the MPOPTION_CONTROLLERS_ONLY gate below — pulls in g_Vars and
// g_MpSetup so we can drop kbd/mouse input mid-match when the host (or any
// client) flips the option.
#include "bss.h"
#include "data.h"
#include "constants.h"
#include "types.h"

#define CONTROLLERDB_FNAME "gamecontrollerdb.txt"

#define MAX_BIND_STR 256

#define TRIG_THRESHOLD (30 * 256)
#define DEFAULT_DEADZONE 4096
#define DEFAULT_DEADZONE_RY 6144

#define WHEEL_UP_MASK SDL_BUTTON_MASK(VK_MOUSE_WHEEL_UP - VK_MOUSE_BEGIN + 1)
#define WHEEL_DN_MASK SDL_BUTTON_MASK(VK_MOUSE_WHEEL_DN - VK_MOUSE_BEGIN + 1)

#define CURSOR_HIDE_THRESHOLD 1
#define CURSOR_HIDE_TIME 3000000 // us

static SDL_Gamepad *pads[INPUT_MAX_CONTROLLERS];

#define CONTROLLERCFG_DEFAULT { \
	.rumbleOn = 0, \
	.rumbleScale = 0.5f, \
	.axisMap = { \
		{ SDL_GAMEPAD_AXIS_LEFTX,  SDL_GAMEPAD_AXIS_LEFTY  }, \
		{ SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY }, \
	}, \
	.sens = { 1.f, 1.f, 1.f, 1.f }, \
	.deadzone = { DEFAULT_DEADZONE, DEFAULT_DEADZONE, DEFAULT_DEADZONE, DEFAULT_DEADZONE_RY }, \
	.stickCButtons = 0, \
	.swapSticks = 1, \
	.deviceIndex = -1, \
	.cancelCButtons = 0, \
	.trigRumble = 1, \
}

static struct controllercfg {
	s32 rumbleOn;
	f32 rumbleScale;
	u32 axisMap[2][2];
	f32 sens[4];
	s32 deadzone[4];
	s32 stickCButtons;
	s32 swapSticks;
	s32 deviceIndex;
	s32 cancelCButtons;
	s32 trigRumble;     // Input.PlayerN.TriggerRumble: mirror rumble onto impulse triggers
	s32 hasTrigRumble;  // runtime capability, not config-bound
	s32 hasLED;         // runtime capability, not config-bound
	s32 hasGyro;        // runtime capability, not config-bound
} padsCfg[INPUT_MAX_CONTROLLERS] = {
	CONTROLLERCFG_DEFAULT,
	CONTROLLERCFG_DEFAULT,
	CONTROLLERCFG_DEFAULT,
	CONTROLLERCFG_DEFAULT
};

static u32 binds[MAXCONTROLLERS][CK_TOTAL_COUNT][INPUT_MAX_BINDS];
static char bindStrs[MAXCONTROLLERS][CK_TOTAL_COUNT][MAX_BIND_STR];

static s32 fakeControllers = 0;
static s32 firstController = 0;
static s32 connectedMask = 0;

// SDL3 removed all device-index APIs in favour of instance IDs; keep an
// enumeration array so the rest of this file (and the persisted
// Input.PlayerN.ControllerIndex config values) can keep using stable
// positional indices. Refreshed on joystick add/remove events.
static SDL_JoystickID *joyIds = NULL;
static s32 numJoysticks = 0;

static void inputRefreshJoyList(void)
{
	if (joyIds) {
		SDL_free(joyIds);
	}
	int n = 0;
	joyIds = SDL_GetJoysticks(&n);
	numJoysticks = joyIds ? n : 0;
}

static s32 useHIDAPI = 1;
static s32 useRawInput = 0;

// Input.GamepadLED: tint RGB-LED pads (DualShock4/DualSense lightbar) with a
// per-player colour and flash red on low health (SDL3 SDL_SetGamepadLED)
static s32 padLEDEnabled = 1;

// PlayStation-convention player colours
static const u8 padLEDColours[INPUT_MAX_CONTROLLERS][3] = {
	{ 0x00, 0x00, 0xff }, // player 1: blue
	{ 0xff, 0x00, 0x00 }, // player 2: red
	{ 0x00, 0xff, 0x00 }, // player 3: green
	{ 0xff, 0x00, 0xff }, // player 4: pink
};

// last 0x00RRGGBB sent to each pad so we only push HID reports on change;
// 0xffffffff = unknown/forced resend
static u32 padLEDState[INPUT_MAX_CONTROLLERS] = { 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff };

// next time the LED tick is allowed to run; /padtest led pushes this out so a
// manually set test colour isn't immediately repainted
static u64 padLEDNextUpdate = 0;

#define LED_UPDATE_INTERVAL_US 100000   // poll player state at 10Hz
#define LED_FLASH_PERIOD_US    250000   // low-health flash half-period
#define LED_LOW_HEALTH_FRAC    0.25f

// Gyro aim (SDL3 gamepad sensors; pad 1 / player 1 only). The event watcher
// integrates angular velocity (rad/s) against the sensor's own timestamps;
// inputUpdateGyro converts the integral to a per-frame aim delta in the same
// degree units inputMouseGetScaledDelta returns, where it is merged.
static s32 gyroAimEnabled = 0;  // Input.GyroAim
static f32 gyroSensX = 1.f;     // Input.GyroSpeedX (negative = inverted)
static f32 gyroSensY = 1.f;     // Input.GyroSpeedY (negative = inverted)
static f32 gyroAccX = 0.f;      // integrated pitch, radians (event watcher)
static f32 gyroAccY = 0.f;      // integrated yaw, radians (event watcher)
static u64 gyroLastTimestamp = 0; // last sensor event timestamp, ns (0 = none)
static f32 gyroDX = 0.f;        // this frame's aim delta, degrees
static f32 gyroDY = 0.f;

// Conversion from integrated radians to mouse-delta units, empirically
// calibrated (DualSense, real pad vs camera) so sens 1.0 = 1:1 between
// physical pad rotation and camera rotation. The raw rad->deg conversion
// (57.3) overshoots ~3.3x through the mouse-delta consumer scaling.
#define GYRO_UNIT_SCALE (57.29578f * 0.3f)
#define GYRO_MAX_EVENT_DT 0.5f  // ignore integration gaps longer than this (s)

static s32 mouseEnabled = 1;
static s32 mouseX, mouseY;
static s32 mouseDX, mouseDY;
static u32 mouseButtons;
static s32 mouseWheel = 0;

static s32 mouseLocked = 0;
static s32 mouseLockMode = MLOCK_AUTO;
static s32 mouseGrab = 1;
static u64 mouseCursorTime = 0;
static s32 mouseShowCursor = 1;

static f32 mouseSensX = 2.5f;
static f32 mouseSensY = 2.5f;

static s32 lastKey = 0;
// Which physical device the player most recently used: 0 = keyboard/mouse,
// 1 = gamepad. Updated in the event watcher; read by chaos Button Thief
// (inputLastSourceWasPad) so it only steals binds that exist for the device
// in hand (movement is discrete C-buttons on kb/mouse but an analog stick on
// a pad, where stealing a C-button does nothing).
static s32 lastSourceWasPad = 0;
static char lastChar = 0;
static s32 textInput = 0;
// One-frame cooldown applied after text input ends. While text input was
// active, inputReadController returns button=0 for every pad. As soon as
// it ends, the next frame's read sees whatever keys are still held (most
// notably Enter, which the user just pressed to submit the keyboard) as
// a 0->1 edge — so joyGetButtonsPressedThisFrame fires START_BUTTON and
// the MPSETUP root interprets it as "press Start to begin match". This
// counter keeps the masked state alive long enough for those held keys
// to be observed as steady-state instead of fresh presses.
static s32 textInputCooldown = 0;

static char *clipboardText = NULL;

static const char *ckNames[CK_TOTAL_COUNT] = {
	"R_CBUTTONS",
	"L_CBUTTONS",
	"D_CBUTTONS",
	"U_CBUTTONS",
	"R_TRIG",
	"L_TRIG",
	"X_BUTTON",
	"Y_BUTTON",
	"R_JPAD",
	"L_JPAD",
	"D_JPAD",
	"U_JPAD",
	"START_BUTTON",
	"Z_TRIG",
	"B_BUTTON",
	"A_BUTTON",
	"STICK_XNEG",
	"STICK_XPOS",
	"STICK_YNEG",
	"STICK_YPOS",
	"ACCEPT_BUTTON",
	"CANCEL_BUTTON",
	"CK_0040",
	"CK_0080",
	"CK_0100",
	"CK_0200",
	"CK_0400",
	"CK_0800",
	"CK_1000",
	"CK_2000",
	"CK_4000",
	"CK_8000"
};

static const char *vkPunctNames[] = {
	"MINUS", "EQUALS", "LEFTBRACKET", "RIGHTBRACKET", "BACKSLASH",
	"HASH", "SEMICOLON", "APOSTROPHE", "GRAVE", "COMMA", "PERIOD", "SLASH"
};

static const char *vkMouseNames[] = {
	"MOUSE_LEFT",
	"MOUSE_MIDDLE",
	"MOUSE_RIGHT",
	"MOUSE_X1",
	"MOUSE_X2",
	"MOUSE_WHEEL_UP",
	"MOUSE_WHEEL_DN",
};

static const char *vkJoyNames[] = {
	"JOY1_A",
	"JOY1_B",
	"JOY1_X",
	"JOY1_Y",
	"JOY1_BACK",
	"JOY1_GUIDE",
	"JOY1_START",
	"JOY1_LSTICK",
	"JOY1_RSTICK",
	"JOY1_LSHOULDER",
	"JOY1_RSHOULDER",
	"JOY1_DPAD_UP",
	"JOY1_DPAD_DOWN",
	"JOY1_DPAD_LEFT",
	"JOY1_DPAD_RIGHT",
	"JOY1_BUTTON_15",
	"JOY1_BUTTON_16",
	"JOY1_BUTTON_17",
	"JOY1_BUTTON_18",
	"JOY1_BUTTON_19",
	"JOY1_TOUCHPAD",
	"JOY1_BUTTON_21",
	"JOY1_BUTTON_22",
	"JOY1_BUTTON_23",
	"JOY1_BUTTON_24",
	"JOY1_BUTTON_25",
	"JOY1_BUTTON_26",
	"JOY1_BUTTON_27",
	"JOY1_BUTTON_28",
	"JOY1_BUTTON_29",
	"JOY1_LTRIGGER",
	"JOY1_RTRIGGER",
};

static char vkNames[VK_TOTAL_COUNT][64];

static s8 vkPrevState[VK_TOTAL_COUNT];

void inputSetDefaultKeyBinds(s32 cidx, s32 n64mode)
{
	// TODO: make VK constants for all these
	static const u32 pckbbinds[][3] = {
		{ CK_B,             SDL_SCANCODE_E,      0                   },
		{ CK_X,             SDL_SCANCODE_R,      0                   },
		{ CK_RTRIG,         VK_MOUSE_RIGHT,      SDL_SCANCODE_Z      },
		{ CK_LTRIG,         SDL_SCANCODE_F,      SDL_SCANCODE_X      },
		{ CK_ZTRIG,         VK_MOUSE_LEFT,       SDL_SCANCODE_SPACE  },
		{ CK_START,         SDL_SCANCODE_TAB,    0                   },
		{ CK_DPAD_D,        SDL_SCANCODE_Q,      VK_MOUSE_MIDDLE     },
		{ CK_DPAD_U,        0,                   0                   },
		{ CK_Y,             VK_MOUSE_WHEEL_DN,   0                   },
		{ CK_DPAD_L,        VK_MOUSE_WHEEL_UP,   0                   },
		{ CK_C_D,           SDL_SCANCODE_S,      0                   },
		{ CK_C_U,           SDL_SCANCODE_W,      0                   },
		{ CK_C_R,           SDL_SCANCODE_D,      0                   },
		{ CK_C_L,           SDL_SCANCODE_A,      0                   },
		{ CK_STICK_XNEG,    SDL_SCANCODE_LEFT,   0                   },
		{ CK_STICK_XPOS,    SDL_SCANCODE_RIGHT,  0                   },
		{ CK_STICK_YNEG,    SDL_SCANCODE_DOWN,   0                   },
		{ CK_STICK_YPOS,    SDL_SCANCODE_UP,     0                   },
		{ CK_4000,          SDL_SCANCODE_LSHIFT, 0                   },
		{ CK_2000,          SDL_SCANCODE_LCTRL,  0                   },
		{ CK_ACCEPT,        SDL_SCANCODE_RETURN, SDL_SCANCODE_E      },
		{ CK_CANCEL,        VK_MOUSE_RIGHT,      0                   },
	};

	static const u32 pcjoybinds[][2] = {
		{ CK_A,      SDL_GAMEPAD_BUTTON_SOUTH          },
		{ CK_X,      SDL_GAMEPAD_BUTTON_WEST           },
		{ CK_Y,      SDL_GAMEPAD_BUTTON_NORTH          },
		{ CK_DPAD_L, SDL_GAMEPAD_BUTTON_EAST,          },
		{ CK_DPAD_D, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER  },
		{ CK_LTRIG,  SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER },
		{ CK_RTRIG,  VK_JOY1_LTRIG - VK_JOY1_BEGIN     },
		{ CK_ZTRIG,  VK_JOY1_RTRIG - VK_JOY1_BEGIN     },
		{ CK_START,  SDL_GAMEPAD_BUTTON_START          },
		{ CK_C_D,    SDL_GAMEPAD_BUTTON_DPAD_DOWN      },
		{ CK_C_U,    SDL_GAMEPAD_BUTTON_DPAD_UP        },
		{ CK_C_R,    SDL_GAMEPAD_BUTTON_DPAD_RIGHT     },
		{ CK_C_L,    SDL_GAMEPAD_BUTTON_DPAD_LEFT      },
		{ CK_ACCEPT, SDL_GAMEPAD_BUTTON_SOUTH          },
		{ CK_CANCEL, SDL_GAMEPAD_BUTTON_EAST           },
		{ CK_8000,   SDL_GAMEPAD_BUTTON_LEFT_STICK     },
	};

	static const u32 n64kbbinds[][3] = {
		{ CK_A,          SDL_SCANCODE_Q,      0                  },
		{ CK_B,          SDL_SCANCODE_E,      0                  },
		{ CK_RTRIG,      VK_MOUSE_RIGHT,      SDL_SCANCODE_LALT  },
		{ CK_LTRIG,      SDL_SCANCODE_F,      0                  },
		{ CK_ZTRIG,      VK_MOUSE_LEFT,       SDL_SCANCODE_SPACE },
		{ CK_START,      SDL_SCANCODE_RETURN, 0                  },
		{ CK_C_D,        SDL_SCANCODE_S,      0                  },
		{ CK_C_U,        SDL_SCANCODE_W,      0                  },
		{ CK_C_R,        SDL_SCANCODE_D,      0                  },
		{ CK_C_L,        SDL_SCANCODE_A,      0                  },
		{ CK_DPAD_L,     SDL_SCANCODE_LEFT,   0                  },
		{ CK_DPAD_R,     SDL_SCANCODE_RIGHT,  0                  },
		{ CK_DPAD_D,     SDL_SCANCODE_DOWN,   0                  },
		{ CK_DPAD_U,     SDL_SCANCODE_UP,     0                  },
		{ CK_STICK_YNEG, SDL_SCANCODE_K,      0                  },
		{ CK_STICK_YPOS, SDL_SCANCODE_I,      0                  },
		{ CK_STICK_XNEG, SDL_SCANCODE_J,      0                  },
		{ CK_STICK_XPOS, SDL_SCANCODE_L,      0                  },
	};

	static const u32 n64joybinds[][2] = {
		{ CK_A,      SDL_GAMEPAD_BUTTON_SOUTH          },
		{ CK_B,      SDL_GAMEPAD_BUTTON_EAST           },
		{ CK_LTRIG,  SDL_GAMEPAD_BUTTON_LEFT_SHOULDER  },
		{ CK_RTRIG,  SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER },
		{ CK_ZTRIG,  VK_JOY1_RTRIG - VK_JOY1_BEGIN     },
		{ CK_START,  SDL_GAMEPAD_BUTTON_START          },
		{ CK_DPAD_D, SDL_GAMEPAD_BUTTON_DPAD_DOWN      },
		{ CK_DPAD_U, SDL_GAMEPAD_BUTTON_DPAD_UP        },
		{ CK_DPAD_L, SDL_GAMEPAD_BUTTON_DPAD_LEFT      },
		{ CK_DPAD_R, SDL_GAMEPAD_BUTTON_DPAD_RIGHT     },
	};

	memset(binds[cidx], 0, sizeof(binds[cidx]));

	const u32 (*kbbinds)[3];
	const u32 (*joybinds)[2];
	u32 numkbbinds;
	u32 numjoybinds;
	if (n64mode) {
		kbbinds = n64kbbinds;
		joybinds = n64joybinds;
		numkbbinds = sizeof(n64kbbinds) / sizeof(n64kbbinds[0]);
		numjoybinds = sizeof(n64joybinds) / sizeof(n64joybinds[0]);
	} else {
		kbbinds = pckbbinds;
		joybinds = pcjoybinds;
		numkbbinds = sizeof(pckbbinds) / sizeof(pckbbinds[0]);
		numjoybinds = sizeof(pcjoybinds) / sizeof(pcjoybinds[0]);
	}

	if (cidx == 0) {
		for (u32 i = 0; i < numkbbinds; ++i) {
			for (s32 j = 1; j < 3; ++j) {
				if (kbbinds[i][j]) {
					inputKeyBind(cidx, kbbinds[i][0], j - 1, kbbinds[i][j]);
				}
			}
		}
	}

	for (u32 i = 0; i < numjoybinds; ++i) {
		inputKeyBind(cidx, joybinds[i][0], -1, VK_JOY_BEGIN + cidx * INPUT_MAX_CONTROLLER_BUTTONS + joybinds[i][1]);
	}
}

static inline s32 inputDeviceIndexFromId(const SDL_JoystickID id) {
	for (s32 jidx = 0; jidx < numJoysticks; ++jidx) {
		if (joyIds[jidx] == id) {
			return jidx;
		}
	}
	return -1;
}

static inline SDL_JoystickID inputControllerGetId(SDL_Gamepad *ctrl)
{
	return SDL_GetJoystickID(SDL_GetGamepadJoystick(ctrl));
}

static inline void inputInitController(const s32 cidx, const s32 jidx)
{
	// SDL3 replaced SDL_GameControllerHasRumble() with a gamepad property
	const SDL_PropertiesID props = SDL_GetGamepadProperties(pads[cidx]);
	padsCfg[cidx].rumbleOn = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
	padsCfg[cidx].hasTrigRumble = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false);
	padsCfg[cidx].hasLED = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false);
	padsCfg[cidx].hasGyro = SDL_GamepadHasSensor(pads[cidx], SDL_SENSOR_GYRO);

	sysLogPrintf(LOG_NOTE, "input: pad %d caps: rumble=%d trigrumble=%d rgbled=%d gyro=%d",
		cidx, padsCfg[cidx].rumbleOn, padsCfg[cidx].hasTrigRumble, padsCfg[cidx].hasLED, padsCfg[cidx].hasGyro);

	// gyro aim reads player 1's pad only
	if (cidx == 0 && padsCfg[cidx].hasGyro && gyroAimEnabled) {
		SDL_SetGamepadSensorEnabled(pads[cidx], SDL_SENSOR_GYRO, true);
		gyroAccX = gyroAccY = 0.f;
		gyroLastTimestamp = 0;
	}

	// make the LEDs on the controller indicate which player it's for
	SDL_SetGamepadPlayerIndex(pads[cidx], cidx);

	// tint the RGB LED (lightbar) with the player colour right away
	padLEDState[cidx] = 0xffffffff;
	if (padsCfg[cidx].hasLED && padLEDEnabled) {
		const u8 *c = padLEDColours[cidx];
		if (SDL_SetGamepadLED(pads[cidx], c[0], c[1], c[2])) {
			padLEDState[cidx] = ((u32)c[0] << 16) | ((u32)c[1] << 8) | c[2];
		} else {
			sysLogPrintf(LOG_NOTE, "input: SDL_SetGamepadLED failed for pad %d: %s", cidx, SDL_GetError());
		}
	}

	// remember the joystick index
	padsCfg[cidx].deviceIndex = jidx;

	connectedMask |= (1 << cidx);

	sysLogPrintf(LOG_NOTE, "input: assigned controller '%d: (%s)' (id %d) to player %d",
		jidx, SDL_GetGamepadName(pads[cidx]), inputControllerGetId(pads[cidx]), cidx);

	SDL_Joystick* joy = SDL_GetGamepadJoystick(pads[cidx]);
	if (joy) {
		char guidStr[1024] = "";
		SDL_GUID guid = SDL_GetJoystickGUID(joy);
		SDL_GUIDToString(guid, guidStr, sizeof(guidStr));
		sysLogPrintf(LOG_NOTE, "input: GUID for controller %d: %s", jidx, guidStr);
	}
}

static inline void inputCloseController(const s32 cidx)
{
	sysLogPrintf(LOG_NOTE, "input: removed controller '%d: (%s)' (id %d) from player %d",
		padsCfg[cidx].deviceIndex, SDL_GetGamepadName(pads[cidx]), inputControllerGetId(pads[cidx]), cidx);

	// reset player LEDs
	SDL_SetGamepadPlayerIndex(pads[cidx], -1);

	SDL_CloseGamepad(pads[cidx]);

	pads[cidx] = NULL;
	padsCfg[cidx].rumbleOn = 0;
	padsCfg[cidx].hasTrigRumble = 0;
	padsCfg[cidx].hasLED = 0;
	padsCfg[cidx].hasGyro = 0;
	padLEDState[cidx] = 0xffffffff;
	if (cidx == 0) {
		gyroAccX = gyroAccY = gyroDX = gyroDY = 0.f;
		gyroLastTimestamp = 0;
	}

	if (cidx) {
		connectedMask &= ~(1 << cidx);
	}
}

static inline s32 inputControllerGetIndex(SDL_Gamepad *ctrl)
{
	if (ctrl) {
		for (s32 i = 0; i < INPUT_MAX_CONTROLLERS; ++i) {
			if (pads[i] == ctrl) {
				return i;
			}
		}
	}
	return -1;
}

static inline s32 inputControllerGetIndexByDeviceIndex(const s32 jidx)
{
	for (s32 cidx = 0; cidx < INPUT_MAX_CONTROLLERS; ++cidx) {
		if (pads[cidx] && padsCfg[cidx].deviceIndex == jidx) {
			return cidx;
		}
	}
	return -1;
}

static inline s32 inputControllerGetIndexById(const SDL_JoystickID jid)
{
	for (s32 cidx = 0; cidx < INPUT_MAX_CONTROLLERS; ++cidx) {
		if (pads[cidx]) {
			if (inputControllerGetId(pads[cidx]) == jid) {
				return cidx;
			}
		}
	}
	return -1;
}

static inline void inputCloseAllControllers(void)
{
	for (s32 cidx = 0; cidx < INPUT_MAX_CONTROLLERS; ++cidx) {
		if (pads[cidx]) {
			inputCloseController(cidx);
			pads[cidx] = NULL;
		}
	}

	connectedMask = 1; // always report first controller as connected
}

static inline s32 inputTryController(const s32 cidx, const s32 jidx)
{
	if (!pads[cidx]) {
		pads[cidx] = SDL_OpenGamepad(joyIds[jidx]);
		if (pads[cidx]) {
			inputInitController(cidx, jidx);
			return 1;
		}
	}
	return 0;
}

static inline void inputInitAllControllers(void)
{
	SDL_UpdateGamepads();

	inputRefreshJoyList();

	connectedMask = 1; // always report first controller as connected

	// first try to assign the controllers that we had last time
	// we're still free to check by device index before any controller device events fire
	for (s32 cidx = 0; cidx < INPUT_MAX_CONTROLLERS; ++cidx) {
		const s32 jidx = padsCfg[cidx].deviceIndex;
		if (jidx >= 0 && jidx < numJoysticks) {
			if (SDL_IsGamepad(joyIds[jidx]) && inputControllerGetIndexByDeviceIndex(jidx) < 0) {
				// using the full assign function in case user sets same index for several players
				if (inputTryController(cidx, jidx)) {
					// success
					continue;
				}
			}
			// nothing was there, forget it
			padsCfg[cidx].deviceIndex = -1;
		}
	}

	// now try autofilling the rest, starting with firstController
	for (s32 jidx = 0; jidx < numJoysticks; ++jidx) {
		if (SDL_IsGamepad(joyIds[jidx]) && inputControllerGetIndexByDeviceIndex(jidx) < 0) {
			for (s32 cidx = firstController; cidx < INPUT_MAX_CONTROLLERS; ++cidx) {
				if (inputTryController(cidx, jidx)) {
					break;
				}
			}
		}
	}

	const s32 overrideMask = (1 << fakeControllers) - 1;
	if (overrideMask) {
		connectedMask = overrideMask;
	}
}

// Confine the OS cursor to the window while it has focus (Input.MouseGrab).
// This is the free-cursor half of "mouse lock when the window is active":
// relative mouse mode already contains the cursor during gameplay, but in
// menus the cursor is free and can wander onto another monitor, where a
// click deactivates the game. SDL manages the grab per-focus (released on
// focus loss, re-applied on regain); the explicit FOCUS_GAINED re-assert
// below covers boot order and any state SDL dropped while unfocused.
static void inputApplyMouseGrab(void)
{
	SDL_Window *wnd = (SDL_Window *)videoGetWindowHandle();
	if (wnd) {
		SDL_SetWindowMouseGrab(wnd, mouseGrab && mouseEnabled);
	}
}

// NOTE: must return SDL3's real 1-byte bool, spelled _Bool here because
// types.h #defines `bool` to s32 (which would mismatch SDL_EventFilter)
static _Bool inputEventFilter(void *data, SDL_Event *event)
{
	switch (event->type) {
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			// window became active: re-assert cursor confinement and, if the
			// game holds the mouse (gameplay), relative capture
			inputApplyMouseGrab();
			if (mouseLocked) {
				inputLockMouse(1);
			}
			break;
		case SDL_EVENT_GAMEPAD_ADDED:
			// NOTE: in SDL3 `which` is an instance ID, not a device index
			inputRefreshJoyList();
			for (s32 i = firstController; i < INPUT_MAX_CONTROLLERS; ++i) {
				if (!pads[i]) {
					pads[i] = SDL_OpenGamepad(event->gdevice.which);
					if (pads[i]) {
						inputInitController(i, inputDeviceIndexFromId(event->gdevice.which));
					}
					break;
				}
			}
			break;

		case SDL_EVENT_GAMEPAD_REMOVED: {
			SDL_Gamepad *ctrl = SDL_GetGamepadFromID(event->gdevice.which);
			const s32 idx = inputControllerGetIndex(ctrl);
			if (idx >= 0) {
				inputCloseController(idx);
				padsCfg[idx].deviceIndex = -1;
			}
			break;
		}

		case SDL_EVENT_JOYSTICK_ADDED:
		case SDL_EVENT_JOYSTICK_REMOVED:
			inputRefreshJoyList(); // joystick count has changed
			break;

		case SDL_EVENT_MOUSE_WHEEL:
			// wheel.y is a float in SDL3; consumers only test the sign
			mouseWheel = (event->wheel.y > 0.f) - (event->wheel.y < 0.f);
			if (!lastKey && mouseWheel) {
				lastKey = (mouseWheel < 0) + VK_MOUSE_WHEEL_UP;
			}
			lastSourceWasPad = 0;
			break;

		case SDL_EVENT_MOUSE_BUTTON_DOWN:
			if (!lastKey) {
				lastKey = VK_MOUSE_BEGIN - 1 + event->button.button;
			}
			lastSourceWasPad = 0;
			break;

		case SDL_EVENT_MOUSE_MOTION:
			lastSourceWasPad = 0;
			break;

		case SDL_EVENT_KEY_DOWN:
			if (!lastKey) {
				lastKey = VK_KEYBOARD_BEGIN + event->key.scancode;
			}
			lastSourceWasPad = 0;
			break;

		case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
			if (!lastKey) {
				lastKey = VK_JOY1_BEGIN + event->gbutton.button;
				SDL_Gamepad *ctrl = SDL_GetGamepadFromID(event->gbutton.which);
				const s32 idx = inputControllerGetIndex(ctrl);
				if (idx >= 0) {
					lastKey += idx * INPUT_MAX_CONTROLLER_BUTTONS;
				}
			}
			lastSourceWasPad = 1;
			break;

		case SDL_EVENT_GAMEPAD_AXIS_MOTION:
			if (!lastKey) {
				if (event->gaxis.axis >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER && event->gaxis.value > TRIG_THRESHOLD) {
					lastKey = VK_JOY1_LTRIG + (event->gaxis.axis - SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
					SDL_Gamepad *ctrl = SDL_GetGamepadFromID(event->gaxis.which);
					const s32 idx = inputControllerGetIndex(ctrl);
					if (idx >= 0) {
						lastKey += idx * INPUT_MAX_CONTROLLER_BUTTONS;
					}
				}
			}
			// Only a real stick/trigger push flips the source (small idle jitter
			// past a deadzone shouldn't claim the player picked up the pad).
			if (event->gaxis.value > 8000 || event->gaxis.value < -8000) {
				lastSourceWasPad = 1;
			}
			break;

		case SDL_EVENT_TEXT_INPUT:
			if (!lastChar && event->text.text[0] && (u8)event->text.text[0] < 0x80) {
				lastChar = event->text.text[0];
			}
			break;

		case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
			// gyro aim: integrate angular velocity (rad/s) against the
			// sensor's own timestamps for frame-rate-independent precision;
			// consumed once per frame by inputUpdateGyro. Pad 1 only.
			if (event->gsensor.sensor == SDL_SENSOR_GYRO && pads[0] &&
					event->gsensor.which == inputControllerGetId(pads[0])) {
				if (gyroLastTimestamp) {
					const f32 dt = (f32)(event->gsensor.sensor_timestamp - gyroLastTimestamp) * 1e-9f;
					if (dt > 0.f && dt < GYRO_MAX_EVENT_DT) {
						gyroAccX += event->gsensor.data[0] * dt; // pitch
						gyroAccY += event->gsensor.data[1] * dt; // yaw
					}
				}
				gyroLastTimestamp = event->gsensor.sensor_timestamp;
			}
			break;

		default:
			break;
	}

	return true;
}

static inline void inputGetScancodeName(const SDL_Scancode sc, char *out, size_t len)
{
		const char *scname = SDL_GetScancodeName(sc);
		if (scname) {
			strncpy(out, scname, len - 1);
			for (u32 i = 0; i < len && out[i]; ++i) {
				if (out[i] == ' ') {
					out[i] = '_';
				} else {
					out[i] = toupper(out[i]);
				}
			}
		} else {
			snprintf(out, len, "KEY%d", (s32)sc);
		}
}

static inline void inputInitKeyNames(void)
{
	for (SDL_Scancode key = SDL_SCANCODE_A; key <= SDL_SCANCODE_SPACE; ++key) {
		inputGetScancodeName(key, vkNames[key], sizeof(vkNames[key]));
	}

	// special characters
	for (SDL_Scancode key = SDL_SCANCODE_MINUS; key < SDL_SCANCODE_CAPSLOCK; ++key) {
		strcpy(vkNames[key], vkPunctNames[key - SDL_SCANCODE_MINUS]);
	}

	for (SDL_Scancode key = SDL_SCANCODE_CAPSLOCK; key <= SDL_SCANCODE_NUMLOCKCLEAR; ++key) {
		inputGetScancodeName(key, vkNames[key], sizeof(vkNames[key]));
	}

	// keypad names
	strcpy(vkNames[SDL_SCANCODE_KP_DIVIDE], "KP_DIVIDE");
	strcpy(vkNames[SDL_SCANCODE_KP_MULTIPLY], "KP_MULTIPLY");
	strcpy(vkNames[SDL_SCANCODE_KP_MINUS], "KP_MINUS");
	strcpy(vkNames[SDL_SCANCODE_KP_PLUS], "KP_PLUS");
	strcpy(vkNames[SDL_SCANCODE_KP_ENTER], "KP_ENTER");
	strcpy(vkNames[SDL_SCANCODE_KP_PERIOD], "KP_PERIOD");
	strcpy(vkNames[SDL_SCANCODE_KP_EQUALS], "KP_EQUALS");
	for (SDL_Scancode key = SDL_SCANCODE_KP_1; key < SDL_SCANCODE_KP_0; ++key) {
		char tmp[8] = "KP_1";
		tmp[3] = '1' + (key - SDL_SCANCODE_KP_1);
		strcpy(vkNames[key], tmp);
	}

	for (SDL_Scancode key = SDL_SCANCODE_LCTRL; key <= SDL_SCANCODE_RGUI; ++key) {
		inputGetScancodeName(key, vkNames[key], sizeof(vkNames[key]));
	}

	// mouse names
	for (u32 vk = VK_MOUSE_BEGIN; vk < VK_JOY1_BEGIN; ++vk) {
		strcpy(vkNames[vk], vkMouseNames[vk - VK_MOUSE_BEGIN]);
	}

	// joystick names
	for (u32 vk = VK_JOY1_BEGIN; vk < VK_TOTAL_COUNT; ++vk) {
		const u32 jidx = (vk - VK_JOY1_BEGIN) / INPUT_MAX_CONTROLLER_BUTTONS;
		const u32 jbtn = (vk - VK_JOY1_BEGIN) % INPUT_MAX_CONTROLLER_BUTTONS;
		strcpy(vkNames[vk], vkJoyNames[jbtn]);
		vkNames[vk][3] = '1' + jidx;
	}
}

void inputSaveBinds(void)
{
	char *bindstr;

	for (s32 i = 0; i < MAXCONTROLLERS; ++i) {
		for (u32 ck = 0; ck < CK_TOTAL_COUNT; ++ck) {
			bindstr = bindStrs[i][ck];
			bindstr[0] = '\0';
			for (s32 b = 0; b < INPUT_MAX_BINDS; ++b) {
				if (binds[i][ck][b]) {
					if (b) {
						strncat(bindstr, ", ", MAX_BIND_STR - 1);
					}
					strncat(bindstr, inputGetKeyName(binds[i][ck][b]), MAX_BIND_STR - 1);
				}
			}
			if (!bindstr[0]) {
				strcpy(bindstr, "NONE");
			}
		}
	}
}

static inline void inputParseBindString(const s32 ctrl, const u32 ck, char *bindstr)
{
	if (!bindstr[0]) {
		// empty string, keep defaults
		return;
	}

	// unbind all first
	memset(binds[ctrl][ck], 0, sizeof(binds[ctrl][ck]));

	if (!strcasecmp(bindstr, "NONE")) {
		// explicitly nothing bound
		return;
	}

	const char *tok = strtok(bindstr, ", ");
	while (tok) {
		if (tok[0]) {
			const s32 vk = inputGetKeyByName(tok);
			if (vk > 0) {
				inputKeyBind(ctrl, ck, -1, vk);
			}
		}
		tok = strtok(NULL, ", ");
	}
}

static inline void inputLoadBinds(void)
{
	for (s32 i = 0; i < MAXCONTROLLERS; ++i) {
		for (u32 ck = 0; ck < CK_TOTAL_COUNT; ++ck) {
			inputParseBindString(i, ck, bindStrs[i][ck]);
		}
	}
}

s32 inputInit(void)
{
	if (g_NetDedicatedMode == 1) {
		// Headless dedicated: no input devices, no event watcher. inputUpdate
		// short-circuits below; inputKeyPressed reads SDL keyboard state which
		// SDL returns as an empty buffer when SDL_INIT_VIDEO isn't up.
		sysLogPrintf(LOG_NOTE, "input: headless dedicated server, skipping init");
		return 0;
	}

	// Set SDL hints before initializing the controller subsystem.
	// SDL3 dropped all the version gates; hints that may not survive future
	// SDL3 releases stay wrapped in #ifdef (they are just string macros).
	if (useHIDAPI) {
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAM, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_WII, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAMDECK, "1");
#ifdef SDL_HINT_JOYSTICK_ENHANCED_REPORTS
		// successor of the SDL2 PS4/PS5 _RUMBLE hints: enables rumble and
		// motion sensors for PS4/5 pads connected via bluetooth
		SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");
#endif
	}
	if (useRawInput) {
		SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "1");
#ifdef SDL_HINT_JOYSTICK_RAWINPUT_CORRELATE_XINPUT
		SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT_CORRELATE_XINPUT, "1");
#endif
	}

	if (!SDL_WasInit(SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC)) {
		SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC);
	}

	// try to load controller db from an external file in the save folder
	// NOTE: SDL3 expects SDL3-format mapping lines; SDL2-era db files may not apply
	if (fsFileSize("$S/" CONTROLLERDB_FNAME)) {
		const char *dbpath = fsFullPath("$S/" CONTROLLERDB_FNAME);
		const s32 dbcount = SDL_AddGamepadMappingsFromFile(dbpath);
		if (dbcount >= 0) {
			sysLogPrintf(LOG_NOTE, "input: added %d controller mappings from %s", dbcount, dbpath);
		}
	}

	inputInitAllControllers();

	// since the main event loop is elsewhere, we can receive some events we need using a watcher
	SDL_AddEventWatch(inputEventFilter, NULL);

	inputInitKeyNames();

	for (s32 i = 0; i < INPUT_MAX_CONTROLLERS; ++i) {
		inputSetDefaultKeyBinds(i, 0);
	}

	if (mouseLockMode != MLOCK_AUTO) {
		inputLockMouse(mouseLockMode);
	}

	// videoInit ran just before us, so the window exists; the FOCUS_GAINED
	// watcher keeps this asserted from here on
	inputApplyMouseGrab();

	// update the axis maps
	// NOTE: by default sticks get swapped for 1.2: "right stick" here means left stick on your controller
	for (s32 i = 0; i < INPUT_MAX_CONTROLLERS; ++i) {
		inputControllerSetSticksSwapped(i, padsCfg[i].swapSticks);
	}

	inputLoadBinds();

	return connectedMask;
}

// True while the active match has MPOPTION_CONTROLLERS_ONLY set. Gates the
// per-controller bind reads (so kbd/mouse bound to a CK is ignored) and the
// mouse delta accessors (so mouse aim is suppressed). Only takes effect
// during a running match — pre-match menus still accept kbd/mouse so the
// host can flip the option in the first place. Each machine reads its own
// local g_MpSetup, but the option is synced via SVC_STAGE_START so every
// client honours the host's choice once the match starts.
static inline bool inputControllersOnlyActive(void)
{
	return g_Vars.mplayerisrunning && (g_MpSetup.options & MPOPTION_CONTROLLERS_ONLY);
}

static inline s32 inputBindPressed(const s32 idx, const u32 ck)
{
	const bool ctrlOnly = inputControllersOnlyActive();
	for (s32 i = 0; i < INPUT_MAX_BINDS; ++i) {
		const u32 vk = binds[idx][ck][i];
		if (!vk) {
			continue;
		}
		// In controllers-only mode, ignore any bind that's a keyboard or
		// mouse key. Joystick binds live at vk >= VK_JOY_BEGIN, so this
		// preserves the controller-side of every CK even when a CK has
		// mixed kbd+controller binds.
		if (ctrlOnly && vk < VK_JOY_BEGIN) {
			continue;
		}
		if (inputKeyPressed(vk)) {
			return 1;
		}
	}
	return 0;
}

// Chaos "XBLA mode" (pd.deadzone): a runtime deadzone floor in raw axis units
// (0..32768). When larger than the user's configured per-axis deadzone it
// wins; 0 = off. Set via inputSetChaosDeadzone from the Lua chaos bindings.
static s32 chaosDeadzone = 0;

// Chaos Button Thief: 1 if the player's most recent input came from a gamepad.
s32 inputLastSourceWasPad(void)
{
	return lastSourceWasPad;
}

void inputSetChaosDeadzone(s32 dz)
{
	if (dz < 0) {
		dz = 0;
	} else if (dz > 31000) {
		dz = 31000; // never a fully dead stick
	}
	chaosDeadzone = dz;
}

static inline s32 inputAxisScale(s32 x, const s32 deadzoneCfg, const f32 scale)
{
	const s32 deadzone = (chaosDeadzone > deadzoneCfg) ? chaosDeadzone : deadzoneCfg;

	if (abs(x) < deadzone) {
		return 0;
	} else {
		// rescale to fit the non-deadzone range
		if (x < 0) {
			x += deadzone;
		} else {
			x -= deadzone;
		}
		x = x * 32768 / (32768 - deadzone);
		// scale with sensitivity
		x *= scale;
		return (x > 32767) ? 32767 : ((x < -32768) ? -32768 : x);
	}
}

s32 inputReadController(s32 idx, OSContPad *npad)
{
	if (idx < 0 || idx >= INPUT_MAX_CONTROLLERS  || !npad) {
		return -1;
	}

	npad->button = 0;

	// While text input is active we already mask. After it ends we keep
	// masking until the keys that drove the text input (Enter / Escape)
	// are physically released, otherwise the joystick edge detector reads
	// the still-held key as a fresh 0->1 transition on the next real
	// sample and fires START_BUTTON / B_BUTTON at the dialog underneath
	// (e.g. menu.c:5243 reads inputs.start and pushes the Ready dialog
	// which starts the Combat Sim match). Once the keys are released the
	// next real read sees 0 and the prior masked sample was also 0, so no
	// spurious edge fires.
	if (textInput || textInputCooldown > 0) {
		npad->stick_x = 0;
		npad->stick_y = 0;
		npad->rstick_x = 0;
		npad->rstick_y = 0;
		if (!textInput && !inputKeyPressed(VK_RETURN) && !inputKeyPressed(VK_ESCAPE)) {
			textInputCooldown = 0;
		}
		return 0;
	}

	for (u32 i = 0; i < CONT_NUM_BUTTONS; ++i) {
		if (inputBindPressed(idx, i)) {
			npad->button |= 1U << i;
		}
	}

	const s32 xdiff = (inputBindPressed(idx, CK_STICK_XPOS) - inputBindPressed(idx, CK_STICK_XNEG));
	const s32 ydiff = (inputBindPressed(idx, CK_STICK_YPOS) - inputBindPressed(idx, CK_STICK_YNEG));
	npad->stick_x = xdiff < 0 ? -0x80 : (xdiff > 0 ? 0x7F : 0);
	npad->stick_y = ydiff < 0 ? -0x80 : (ydiff > 0 ? 0x7F : 0);

	const struct controllercfg *cfg = &padsCfg[idx];

	if (cfg->cancelCButtons) {
		// opposite C buttons cancel each other out
		if ((npad->button & (L_CBUTTONS | R_CBUTTONS)) == (L_CBUTTONS | R_CBUTTONS)) {
			npad->button &= ~(L_CBUTTONS | R_CBUTTONS);
		}
		if ((npad->button & (U_CBUTTONS | D_CBUTTONS)) == (U_CBUTTONS | D_CBUTTONS)) {
			npad->button &= ~(U_CBUTTONS | D_CBUTTONS);
		}
	}

	if (!pads[idx]) {
		return 0;
	}

	s32 leftX = SDL_GetGamepadAxis(pads[idx], cfg->axisMap[0][0]);
	s32 leftY = SDL_GetGamepadAxis(pads[idx], cfg->axisMap[0][1]);
	s32 rightX = SDL_GetGamepadAxis(pads[idx], cfg->axisMap[1][0]);
	s32 rightY = SDL_GetGamepadAxis(pads[idx], cfg->axisMap[1][1]);

	leftX = inputAxisScale(leftX, cfg->deadzone[cfg->axisMap[0][0]], cfg->sens[cfg->axisMap[0][0]]);
	leftY = inputAxisScale(leftY, cfg->deadzone[cfg->axisMap[0][1]], cfg->sens[cfg->axisMap[0][1]]);
	rightX = inputAxisScale(rightX, cfg->deadzone[cfg->axisMap[1][0]], cfg->sens[cfg->axisMap[1][0]]);
	rightY = inputAxisScale(rightY, cfg->deadzone[cfg->axisMap[1][1]], cfg->sens[cfg->axisMap[1][1]]);

	if (!npad->stick_x && leftX) {
		npad->stick_x = leftX / 0x100;
	}

	s32 stickY = -leftY / 0x100;
	if (!npad->stick_y && stickY) {
		npad->stick_y = (stickY == 128) ? 127 : stickY;
	}

	if (cfg->stickCButtons) {
		// rstick emulates C buttons
		if (rightX < -0x4000) npad->button |= L_CBUTTONS;
		if (rightX > +0x4000) npad->button |= R_CBUTTONS;
		if (rightY < -0x4000) npad->button |= U_CBUTTONS;
		if (rightY > +0x4000) npad->button |= D_CBUTTONS;
		npad->rstick_x = 0;
		npad->rstick_y = 0;
	} else {
		// rstick is an analog input
		if (rightX) {
			npad->rstick_x = rightX / 0x100;
		}
		s32 rStickY = -rightY / 0x100;
		if (rStickY) {
			npad->rstick_y = (rStickY == 128) ? 127 : rStickY;
		}
	}

	return 0;
}

static inline void inputUpdateMouse(void)
{
	// SDL3 reports mouse state in floats; carry the fractional part of the
	// relative deltas over to the next frame so slow movement isn't lost to
	// truncation
	static f32 mouseRemX, mouseRemY;

	f32 fmx = 0.f, fmy = 0.f;
	mouseButtons = SDL_GetMouseState(&fmx, &fmy);

	// window coords are in points on macOS/Wayland HiDPI windows; map them to
	// pixel space so inputMouseGetPosition's mapping against videoGetWidth()
	// (pixels) stays 1:1. Density is 1.0 on Windows, making this a no-op.
	SDL_Window *vwnd = (SDL_Window *)videoGetWindowHandle();
	f32 pd = vwnd ? SDL_GetWindowPixelDensity(vwnd) : 1.f;
	if (pd <= 0.f) {
		pd = 1.f;
	}

	const s32 mx = (s32)(fmx * pd);
	const s32 my = (s32)(fmy * pd);

	if (mouseWheel > 0) {
		mouseButtons |= WHEEL_UP_MASK;
	} else if (mouseWheel < 0) {
		mouseButtons |= WHEEL_DN_MASK;
	}

	mouseWheel = 0;

	f32 fdx = 0.f, fdy = 0.f;
	SDL_GetRelativeMouseState(&fdx, &fdy);
	if (mouseLocked) {
		mouseRemX += fdx;
		mouseRemY += fdy;
		mouseDX = (s32)mouseRemX;
		mouseDY = (s32)mouseRemY;
		mouseRemX -= mouseDX;
		mouseRemY -= mouseDY;
	} else {
		mouseDX = mx - mouseX;
		mouseDY = my - mouseY;
	}

	mouseX = mx;
	mouseY = my;

	// if MLOCK_AUTO is enabled, disable cursor if mouse is unlocked
	// and we haven't moved it for a few seconds
	if (mouseLockMode == MLOCK_AUTO && !mouseLocked) {
		if (abs(mouseDX) > CURSOR_HIDE_THRESHOLD || abs(mouseDY) > CURSOR_HIDE_THRESHOLD) {
			if (!mouseShowCursor) {
				inputMouseShowCursor(1);
			}
		} else if (sysGetMicroseconds() > mouseCursorTime) {
			if (mouseShowCursor) {
				inputMouseShowCursor(0);
			}
		}
	}
}

// Refresh pad RGB LEDs (lightbars): base = player slot colour, flashing red
// while that local player is alive on low health. Rate-limited so we don't
// spam HID output reports; sends only when the colour actually changes.
// Reads g_Vars.players directly — same precedent as the
// MPOPTION_CONTROLLERS_ONLY gate above (input.c already links game state).
static inline void inputUpdatePadLEDs(void)
{
	const u64 now = sysGetMicroseconds();
	if (now < padLEDNextUpdate) {
		return;
	}
	padLEDNextUpdate = now + LED_UPDATE_INTERVAL_US;

	for (s32 i = 0; i < INPUT_MAX_CONTROLLERS; ++i) {
		if (!pads[i] || !padsCfg[i].hasLED) {
			continue;
		}

		u8 r = padLEDColours[i][0];
		u8 g = padLEDColours[i][1];
		u8 b = padLEDColours[i][2];

		// local player i drives pad i; flash red while alive on low health
		struct player *pl = g_Vars.players[i];
		if (pl && !pl->isdead && pl->bondhealth < LED_LOW_HEALTH_FRAC) {
			const s32 phase = (now / LED_FLASH_PERIOD_US) & 1;
			r = phase ? 0xff : 0x20;
			g = 0;
			b = 0;
		}

		const u32 rgb = ((u32)r << 16) | ((u32)g << 8) | b;
		if (rgb != padLEDState[i]) {
			if (SDL_SetGamepadLED(pads[i], r, g, b)) {
				padLEDState[i] = rgb;
			}
		}
	}
}

// Convert the radians the event watcher integrated since last frame into this
// frame's aim delta, in the same degree units the mouse path produces.
// Sensitivity is applied here so live /gyro sens changes take effect at once.
// Signs: SDL gyro +Y = pad turning left, +X = pad pitching up; mouse +dx =
// look right, +dy = look down — hence both negations. Negative GyroSpeed
// values invert.
static inline void inputUpdateGyro(void)
{
	gyroDX = -gyroAccY * GYRO_UNIT_SCALE * gyroSensX;
	gyroDY = -gyroAccX * GYRO_UNIT_SCALE * gyroSensY;
	gyroAccX = 0.f;
	gyroAccY = 0.f;
}

void inputUpdate(void)
{
	if (g_NetDedicatedMode == 1) {
		return;
	}

	SDL_UpdateGamepads();

	if (mouseEnabled) {
		inputUpdateMouse();
	}

	if (padLEDEnabled) {
		inputUpdatePadLEDs();
	}

	if (gyroAimEnabled) {
		inputUpdateGyro();
	}
}

s32 inputControllerConnected(s32 idx)
{
	if (idx < 0 || idx >= INPUT_MAX_CONTROLLERS) {
		return 0;
	}
	return pads[idx] || (connectedMask & (1 << idx));
}

s32 inputRumbleSupported(s32 idx)
{
	if (idx < 0 || idx >= INPUT_MAX_CONTROLLERS) {
		return 0;
	}
	return padsCfg[idx].rumbleOn;
}

void inputRumble(s32 idx, f32 strength, f32 time)
{
	if (idx < 0 || idx >= INPUT_MAX_CONTROLLERS || !pads[idx]) {
		return;
	}

	if (padsCfg[idx].rumbleScale <= 0.f) {
		return;
	}

	if (padsCfg[idx].rumbleOn) {
		strength *= padsCfg[idx].rumbleScale;
		if (strength <= 0.f) {
			strength = 0.f;
			time = 0.f;
		} else {
			strength *= 65535.f;
			time *= 1000.f;
		}
		SDL_RumbleGamepad(pads[idx], (u16)strength, (u16)strength, (u32)time);
		// mirror onto the impulse triggers (Xbox One/Series pads) if present
		if (padsCfg[idx].trigRumble && padsCfg[idx].hasTrigRumble) {
			SDL_RumbleGamepadTriggers(pads[idx], (u16)strength, (u16)strength, (u32)time);
		}
	}
}

f32 inputRumbleGetStrength(s32 cidx)
{
	return padsCfg[cidx].rumbleScale;
}

void inputRumbleSetStrength(s32 cidx, f32 val)
{
	padsCfg[cidx].rumbleScale = val;
}

s32 inputControllerMask(void)
{
	return connectedMask;
}

s32 inputControllerGetSticksSwapped(s32 cidx)
{
	return padsCfg[cidx].swapSticks;
}

void inputControllerSetSticksSwapped(s32 cidx, s32 swapped)
{
	padsCfg[cidx].swapSticks = swapped;
	if (swapped) {
		padsCfg[cidx].axisMap[0][0] = SDL_GAMEPAD_AXIS_RIGHTX;
		padsCfg[cidx].axisMap[0][1] = SDL_GAMEPAD_AXIS_RIGHTY;
		padsCfg[cidx].axisMap[1][0] = SDL_GAMEPAD_AXIS_LEFTX;
		padsCfg[cidx].axisMap[1][1] = SDL_GAMEPAD_AXIS_LEFTY;
	} else {
		padsCfg[cidx].axisMap[0][0] = SDL_GAMEPAD_AXIS_LEFTX;
		padsCfg[cidx].axisMap[0][1] = SDL_GAMEPAD_AXIS_LEFTY;
		padsCfg[cidx].axisMap[1][0] = SDL_GAMEPAD_AXIS_RIGHTX;
		padsCfg[cidx].axisMap[1][1] = SDL_GAMEPAD_AXIS_RIGHTY;
	}
}

s32 inputControllerGetDualAnalog(s32 cidx)
{
	return !padsCfg[cidx].stickCButtons;
}

void inputControllerSetDualAnalog(s32 cidx, s32 enable)
{
	padsCfg[cidx].stickCButtons = !enable;
}

s32 inputControllerGetCancelCButtons(s32 cidx)
{
	return padsCfg[cidx].cancelCButtons;
}

void inputControllerSetCancelCButtons(s32 cidx, s32 cancel)
{
	padsCfg[cidx].cancelCButtons = cancel;
}

f32 inputControllerGetAxisScale(s32 cidx, s32 stick, s32 axis)
{
	return padsCfg[cidx].sens[stick * 2 + axis];
}

void inputControllerSetAxisScale(s32 cidx, s32 stick, s32 axis, f32 value)
{
	padsCfg[cidx].sens[stick * 2 + axis] = value;
}

f32 inputControllerGetAxisDeadzone(s32 cidx, s32 stick, s32 axis)
{
	return (f32)padsCfg[cidx].deadzone[stick * 2 + axis] / 32767.f;
}

void inputControllerSetAxisDeadzone(s32 cidx, s32 stick, s32 axis, f32 value)
{
	padsCfg[cidx].deadzone[stick * 2 + axis] = value * 32767.f;
}

s32 inputGetConnectedControllers(s32 *out)
{
	s32 count = 0;

	for (s32 jidx = 0; jidx < numJoysticks; ++jidx) {
		if (SDL_IsGamepad(joyIds[jidx])) {
			if (out && count < INPUT_MAX_CONNECTED_CONTROLLERS) {
				out[count] = joyIds[jidx];
			}
			++count;
		}
	}

	return count;
}

s32 inputGetAssignedControllerId(s32 cidx)
{
	if (cidx < 0 || cidx >= INPUT_MAX_CONTROLLERS) {
		return -1;
	}

	if (pads[cidx] == NULL) {
		return -1;
	}

	return inputControllerGetId(pads[cidx]);
}

const char *inputGetConnectedControllerName(s32 id)
{
	static char fullName[256];

	if (id < 0) {
		return "Invalid";
	}

	const s32 jidx = inputDeviceIndexFromId(id);
	if (jidx < 0) {
		return "Invalid";
	}

	const char *name = SDL_GetGamepadNameForID(joyIds[jidx]);
	if (!name || !name[0]) {
		name = "Unnamed Controller";
	}

	snprintf(fullName, sizeof(fullName), "%d: %s", jidx, name);

	// replace non-ascii chars with spaces
	for (char *p = fullName; *p; ++p) {
		if ((u32)*p >= 0x7f) {
			*p = ' ';
		}
	}

	return fullName;
}

s32 inputAssignController(s32 cidx, s32 id)
{
	if (cidx < 0 || cidx >= INPUT_MAX_CONTROLLERS) {
		return 0;
	}

	if (id < 0) {
		// close current controller, if any
		if (pads[cidx]) {
			inputCloseController(cidx);
			return 1;
		}
		return 0;
	}

	const s32 jidx = inputDeviceIndexFromId(id);
	if (jidx < 0 || jidx >= numJoysticks || !SDL_IsGamepad(joyIds[jidx])) {
		return 0;
	}

	// try to unassign any other instances of this controller
	for (s32 i = 0; i < INPUT_MAX_CONTROLLERS; ++i) {
		if (pads[i] && inputControllerGetId(pads[i]) == id) {
			inputCloseController(i);
			pads[i] = NULL;
			padsCfg[i].deviceIndex = -1;
		}
	}

	SDL_Gamepad *newpad = SDL_OpenGamepad(joyIds[jidx]);
	if (!newpad) {
		return 0;
	}

	if (pads[cidx]) {
		inputCloseController(cidx);
	}

	pads[cidx] = newpad;
	inputInitController(cidx, id);

	return 1;
}

void inputKeyBind(s32 idx, u32 ck, s32 bind, u32 vk)
{
	if (idx < 0 || idx >= INPUT_MAX_CONTROLLERS || bind >= INPUT_MAX_BINDS || ck >= CK_TOTAL_COUNT) {
		return;
	}

	if (bind < 0) {
		for (s32 i = 0; i < INPUT_MAX_BINDS; ++i) {
			if (binds[idx][ck][i] == 0) {
				bind = i;
				break;
			}
		}
		if (bind < 0) {
			bind = INPUT_MAX_BINDS - 1; // just overwrite last
		}
	}

	binds[idx][ck][bind] = vk;
}

const u32 *inputKeyGetBinds(s32 idx, u32 ck)
{
	if (idx < 0 || idx >= INPUT_MAX_CONTROLLERS || ck >= CK_TOTAL_COUNT) {
		return NULL;
	}
	return binds[idx][ck];
}

s32 inputKeyPressed(u32 vk)
{
	if (vk >= VK_KEYBOARD_BEGIN && vk < VK_MOUSE_BEGIN) {
		// SDL3 returns a 1-byte bool array; can't spell `bool` here because
		// types.h #defines it to s32 (4-byte stride would read garbage)
		const u8 *state = (const u8 *)SDL_GetKeyboardState(NULL);
		return state[vk - VK_KEYBOARD_BEGIN];
	}

	if (vk >= VK_MOUSE_BEGIN && vk < VK_JOY_BEGIN) {
		return (mouseButtons & SDL_BUTTON_MASK(vk - VK_MOUSE_BEGIN + 1)) != 0;
	}

	if (vk >= VK_JOY_BEGIN && vk < VK_TOTAL_COUNT) {
		vk -= VK_JOY_BEGIN;
		const s32 idx = vk / INPUT_MAX_CONTROLLER_BUTTONS;
		if (idx < 0 || idx >= INPUT_MAX_CONTROLLERS || !pads[idx]) {
			return 0;
		}
		vk = vk % INPUT_MAX_CONTROLLER_BUTTONS;
		// triggers
		if (vk == 30 || vk == 31) {
			const s32 trig = SDL_GAMEPAD_AXIS_LEFT_TRIGGER + vk - 30;
			return SDL_GetGamepadAxis(pads[idx], trig) > TRIG_THRESHOLD;
		}
		return SDL_GetGamepadButton(pads[idx], vk);
	}

	return 0;
}

s32 inputKeyJustPressed(u32 vk)
{
	const s8 pressed = inputKeyPressed(vk);
	const s32 result = pressed && !vkPrevState[vk];
	vkPrevState[vk] = pressed;
	return result;
}

static inline u32 inputContToContKey(const u32 cont)
{
	if (cont == 0) {
		return 0;
	}
	// just a log2 to convert CONT_* to their indices
	return 32 - __builtin_clz(cont - 1);
}

s32 inputButtonPressed(s32 idx, u32 contbtn)
{
	if (idx < 0 || idx >= INPUT_MAX_CONTROLLERS) {
		return 0;
	}

	return inputBindPressed(idx, inputContToContKey(contbtn));
}

void inputLockMouse(s32 lock)
{
	mouseLocked = !!lock;
	// relative mouse mode is per-window in SDL3; NULL when headless/early-init
	SDL_Window *wnd = (SDL_Window *)videoGetWindowHandle();
	if (wnd) {
		SDL_SetWindowRelativeMouseMode(wnd, mouseLocked);
	}
}

s32 inputMouseIsLocked(void)
{
	return mouseLocked;
}

s32 inputMouseGetPosition(s32 *x, s32 *y)
{
	// In headless dedicated, videoGetWidth/Height return 0 (gfx_init was
	// skipped). Avoid the integer divide-by-zero that menu input handlers
	// trigger when they poll mouse position with no video subsystem.
	const s32 vw = videoGetWidth();
	const s32 vh = videoGetHeight();
	if (vw <= 0 || vh <= 0) {
		if (x) *x = 0;
		if (y) *y = 0;
		return 0;
	}
	if (x) *x = mouseX * videoGetNativeWidth() / vw;
	if (y) *y = mouseY * videoGetNativeHeight() / vh;
	return (mouseDX != 0 || mouseDY != 0);
}

void inputMouseGetRawDelta(s32 *dx, s32 *dy)
{
	if (dx) *dx = mouseDX;
	if (dy) *dy = mouseDY;
}

void inputMouseGetScaledDelta(f32* dx, f32* dy)
{
		f32 mdx = 0.f, mdy = 0.f;

		// Suppress mouse aim when controllers-only mode is on; gameplay
		// callers (bondmove camera, eyespy, etc.) read zeros and fall back
		// to the right stick. Done here rather than at the call sites so
		// every consumer is covered without per-site edits.
		if (mouseLocked && !inputControllersOnlyActive()) {
				mdx = mouseSensX * ((f32)mouseDX / 3.5f) * 0.022f;
				mdy = mouseSensY * ((f32)mouseDY / 3.5f) * 0.022f;
		}
		// Gyro aim rides the same gameplay gate (mouseLocked = in-game, not
		// in menus/console) but is controller input, so it is deliberately
		// NOT suppressed by MPOPTION_CONTROLLERS_ONLY.
		if (mouseLocked && gyroAimEnabled) {
				mdx += gyroDX;
				mdy += gyroDY;
		}
		if (dx) *dx = mdx;
		if (dy) *dy = mdy;
}

void inputMouseGetAbsScaledDelta(f32* dx, f32* dy)
{
		f32 mdx = 0.f, mdy = 0.f;

		if (mouseLocked && !inputControllersOnlyActive()) {
				mdx = fabsf(mouseSensX) * ((f32)mouseDX / 3.5f) * 0.022f;
				mdy = fabsf(mouseSensY) * ((f32)mouseDY / 3.5f) * 0.022f;
		}
		if (dx) *dx = mdx;
		if (dy) *dy = mdy;
}

void inputMouseGetSpeed(f32 *x, f32 *y)
{
	*x = mouseSensX;
	*y = mouseSensY;
}

void inputMouseSetSpeed(f32 x, f32 y)
{
	mouseSensX = x;
	mouseSensY = y;
}

s32 inputMouseIsEnabled(void)
{
	return mouseEnabled;
}

void inputMouseEnable(s32 enabled)
{
	mouseEnabled = !!enabled;
	if (!mouseEnabled && mouseLockMode != MLOCK_ON && mouseLocked) {
		inputLockMouse(0);
	}
	// grab follows mouseEnabled so controller-only players keep a free cursor
	inputApplyMouseGrab();
}

s32 inputAutoLockMouse(s32 wantlock)
{
	if (mouseEnabled && mouseLockMode == MLOCK_AUTO) {
		inputLockMouse(wantlock);
		return 1;
	}
	return 0;
}

void inputMouseShowCursor(s32 show)
{
	mouseShowCursor = !!show;
	if (mouseShowCursor) {
		SDL_ShowCursor();
	} else {
		SDL_HideCursor();
	}
	if (show) {
		mouseCursorTime = sysGetMicroseconds() + CURSOR_HIDE_TIME;
	}
}

s32 inputGetMouseLockMode(void)
{
	return mouseLockMode;
}

void inputSetMouseLockMode(s32 lockmode)
{
	mouseLockMode = lockmode;
	if (lockmode == MLOCK_ON) {
		inputLockMouse(1);
	} else {
		inputLockMouse(0);
	}
}

const char *inputGetContKeyName(u32 ck)
{
	if (ck >= CK_TOTAL_COUNT) {
		return "";
	}
	return ckNames[ck];
}

s32 inputGetContKeyByName(const char *name)
{
	for (u32 i = 0; i < CK_TOTAL_COUNT; ++i) {
		if (!strcmp(name, ckNames[i])) {
			return i;
		}
	}
	sysLogPrintf(LOG_WARNING, "unknown bind name: `%s`", name);
	return -1;
}

const char *inputGetKeyName(s32 vk)
{
	if (vk < 0 || vk >= VK_TOTAL_COUNT) {
		vk = 0;
	}
	if (!vkNames[vk][0]) {
		snprintf(vkNames[vk], sizeof(vkNames[vk]), "UNKNOWN%d", vk);
	}
	return vkNames[vk];
}

s32 inputGetKeyByName(const char *name)
{
	s32 start = 0;
	s32 end = 0;

	if (!strncmp(name, "JOY", 3) && isdigit(name[3])) {
		const s32 idx = name[3] - '1';
		if (idx >= 0 && idx < INPUT_MAX_CONTROLLERS) {
			start = VK_JOY1_BEGIN + idx * INPUT_MAX_CONTROLLER_BUTTONS;
			end = start + INPUT_MAX_CONTROLLER_BUTTONS;
		}
	} else if (!strncmp(name, "MOUSE", 5)) {
		start = VK_MOUSE_BEGIN;
		end = VK_JOY1_BEGIN;
	} else if (!strncmp(name, "UNKNOWN", 7) && isdigit(name[7])) {
		const s32 key = atoi(name + 7);
		if (key >= 0 && key < VK_TOTAL_COUNT) {
			return key;
		}
	} else {
		end = VK_MOUSE_BEGIN;
	}

	for (s32 i = start; i < end; ++i) {
		if (!strcmp(vkNames[i], name)) {
			return i;
		}
	}

	sysLogPrintf(LOG_WARNING, "unknown key name: `%s`", name);

	return -1;
}

void inputClearLastKey(void)
{
	lastKey = 0;
}

s32 inputGetLastKey(void)
{
	return lastKey;
}

void inputStartTextInput(void)
{
	lastChar = 0;
	lastKey = 0;
	textInput = 1;
	// text input is per-window in SDL3
	SDL_Window *wnd = (SDL_Window *)videoGetWindowHandle();
	if (wnd) {
		SDL_StartTextInput(wnd);
	}
}

void inputClearLastTextChar(void)
{
	lastChar = 0;
}

char inputGetLastTextChar(void)
{
	return lastChar;
}

static inline s32 filterChar(const char ch)
{
	return isalnum(ch) || ch == ' ' || ch == '?' || ch == '!' || ch == '.';
}

s32 inputTextHandler(char *out, const u32 outSize, s32 *curCol, s32 oskCharsOnly)
{
	const s32 ctrlHeld = inputGetKeyModState() & KM_CTRL;

	if (!ctrlHeld) {
		const char chr = inputGetLastTextChar();
		inputClearLastTextChar();
		const s32 valid = chr && (oskCharsOnly ? filterChar(chr) : isprint(chr));
		if (valid) {
			if (*curCol < outSize - 1) {
				out[(*curCol)++] = chr;
				out[*curCol] = '\0';
			}
		}
	}

	const s32 key = inputGetLastKey();
	inputClearLastKey();
	if (ctrlHeld && (key == VK_A + ('v' - 'a'))) {
		// CTRL+V; paste from clipboard
		const char *clip = inputGetClipboard();
		if (clip) {
			const s32 remain = outSize - *curCol - 1;
			inputClearClipboard();
			*curCol += snprintf(out + *curCol, remain, "%s", clip);
			if (*curCol > outSize) {
				*curCol = outSize;
			}
		}
	} else if (key == VK_BACKSPACE) {
		if (*curCol) {
			out[--*curCol] = '\0';
		} else {
			out[0] = '\0';
		}
	} else if (key == VK_ESCAPE) {
		return -1;
	} else if (key == VK_RETURN || key == (VK_KEYBOARD_BEGIN + SDL_SCANCODE_KP_ENTER)) {
		// Enter / numpad-Enter submits the entered text. Callers treat a
		// positive return as "submit" (e.g. console command, menu text field)
		// and a negative return as "cancel" (ESC).
		return 1;
	}

	return 0;
}

void inputClearClipboard(void)
{
	if (clipboardText) {
		SDL_free(clipboardText);
		clipboardText = NULL;
	}
}

const char *inputGetClipboard(void)
{
	if (!clipboardText) {
		char *text = SDL_GetClipboardText();
		if (text) {
			clipboardText = text;
			// remove non-printable and multibyte chars
			for (; *text; ++text) {
				if ((u8)*text < 0x20 || (u8)*text >= 0x7F) {
					*text = '?';
				}
			}
		}
	}
	return clipboardText;
}

void inputStopTextInput(void)
{
	SDL_Window *wnd = (SDL_Window *)videoGetWindowHandle();
	if (wnd) {
		SDL_StopTextInput(wnd);
	}
	textInput = 0;
	// Mask one more frame so the held Enter (or any other key still down
	// from typing) reads as 0->0 rather than 0->1 in the joystick edge
	// detector. See textInputCooldown declaration for the full story.
	textInputCooldown = 1;
	lastKey = 0;
	lastChar = 0;
}

s32 inputIsTextInputActive(void)
{
	return textInput;
}

u32 inputGetKeyModState(void)
{
	return SDL_GetModState();
}

// /padtest console command — debug aid for the SDL3 gamepad extras
// (PORT_SDL3_EXTRAS.md). Subcommands:
//   caps          log capability properties + connection for connected pads
//   led R G B     set pad 1's LED directly (LED tick is held off for 5s)
//   rumble S MS   raw body rumble on pad 1, S = 0..65535 (bypasses RumbleScale)
//   trig S MS     raw trigger rumble on pad 1, S = 0..65535
//   hp            log the health frac the LED low-health flash reads
void inputPadTest(const char *arg)
{
	if (strncmp(arg, "caps", 4) == 0) {
		s32 found = 0;
		for (s32 i = 0; i < INPUT_MAX_CONTROLLERS; ++i) {
			if (!pads[i]) {
				continue;
			}
			SDL_Joystick *joy = SDL_GetGamepadJoystick(pads[i]);
			const s32 wireless = joy ?
				(SDL_GetJoystickConnectionState(joy) == SDL_JOYSTICK_CONNECTION_WIRELESS) : -1;
			sysLogPrintf(LOG_CHAT, "pad%d '%s' rumble=%d trig=%d rgbled=%d gyro=%d wireless=%d",
				i + 1, SDL_GetGamepadName(pads[i]), padsCfg[i].rumbleOn,
				padsCfg[i].hasTrigRumble, padsCfg[i].hasLED, padsCfg[i].hasGyro, wireless);
			++found;
		}
		if (!found) {
			sysLogPrintf(LOG_CHAT, "no pads connected");
		}
	} else if (strncmp(arg, "led", 3) == 0) {
		s32 r = 255, g = 0, b = 0;
		sscanf(arg + 3, "%d %d %d", &r, &g, &b);
		if (pads[0]) {
			const s32 ok = SDL_SetGamepadLED(pads[0], (u8)r, (u8)g, (u8)b);
			padLEDState[0] = 0xffffffff;
			padLEDNextUpdate = sysGetMicroseconds() + 5000000; // hold the test colour 5s
			sysLogPrintf(LOG_CHAT, "pad1 LED(%d,%d,%d): %s", r, g, b, ok ? "OK" : SDL_GetError());
		} else {
			sysLogPrintf(LOG_CHAT, "no pad 1");
		}
	} else if (strncmp(arg, "rumble", 6) == 0) {
		s32 s = 65535, ms = 1000;
		sscanf(arg + 6, "%d %d", &s, &ms);
		if (pads[0]) {
			const s32 ok = SDL_RumbleGamepad(pads[0], (u16)s, (u16)s, (u32)ms);
			sysLogPrintf(LOG_CHAT, "pad1 rumble(%d, %dms): %s", s, ms, ok ? "OK" : SDL_GetError());
		} else {
			sysLogPrintf(LOG_CHAT, "no pad 1");
		}
	} else if (strncmp(arg, "trig", 4) == 0) {
		s32 s = 65535, ms = 1000;
		sscanf(arg + 4, "%d %d", &s, &ms);
		if (pads[0]) {
			const s32 ok = SDL_RumbleGamepadTriggers(pads[0], (u16)s, (u16)s, (u32)ms);
			sysLogPrintf(LOG_CHAT, "pad1 trig(%d, %dms): %s", s, ms, ok ? "OK" : SDL_GetError());
		} else {
			sysLogPrintf(LOG_CHAT, "no pad 1");
		}
	} else if (strncmp(arg, "hp", 2) == 0) {
		struct player *pl = g_Vars.players[0];
		if (pl) {
			sysLogPrintf(LOG_CHAT, "p1 bondhealth=%.3f isdead=%d (LED flashes below %.2f)",
				pl->bondhealth, (s32)pl->isdead, LED_LOW_HEALTH_FRAC);
		} else {
			sysLogPrintf(LOG_CHAT, "p1 player not present");
		}
	} else {
		sysLogPrintf(LOG_CHAT, "usage: /padtest caps | led R G B | rumble S MS | trig S MS | hp");
	}
}

void inputGyroEnable(s32 enable)
{
	gyroAimEnabled = !!enable;
	gyroAccX = gyroAccY = gyroDX = gyroDY = 0.f;
	gyroLastTimestamp = 0;
	if (pads[0] && padsCfg[0].hasGyro) {
		SDL_SetGamepadSensorEnabled(pads[0], SDL_SENSOR_GYRO, gyroAimEnabled);
	}
}

s32 inputGyroIsEnabled(void)
{
	return gyroAimEnabled;
}

s32 inputGyroSupported(s32 idx)
{
	// gyro aim is pad 1 / player 1 only for now
	return idx == 0 && pads[0] && padsCfg[0].hasGyro;
}

void inputGyroGetSpeed(f32 *x, f32 *y)
{
	if (x) *x = gyroSensX;
	if (y) *y = gyroSensY;
}

void inputGyroSetSpeed(f32 x, f32 y)
{
	gyroSensX = x;
	gyroSensY = y;
}

// /gyro console command — gyro aim live control (pad 1 / player 1):
//   /gyro              toggle
//   /gyro on|off       set
//   /gyro sens X [Y]   sensitivity multiplier(s); 1 = 1:1 with the real pad,
//                      negative inverts; Y defaults to X
//   /gyro status       print enable state, pad capability, sens, data rate
void inputGyroCommand(const char *arg)
{
	if (strncmp(arg, "sens", 4) == 0) {
		f32 x = gyroSensX, y = 0.f;
		const s32 n = sscanf(arg + 4, "%f %f", &x, &y);
		if (n >= 1) {
			gyroSensX = x;
			gyroSensY = (n >= 2) ? y : x;
		}
		sysLogPrintf(LOG_CHAT, "gyro sens %.2f %.2f", gyroSensX, gyroSensY);
	} else if (strncmp(arg, "status", 6) == 0) {
		const s32 hasgyro = pads[0] && padsCfg[0].hasGyro;
		const f32 rate = hasgyro ? SDL_GetGamepadSensorDataRate(pads[0], SDL_SENSOR_GYRO) : 0.f;
		sysLogPrintf(LOG_CHAT, "gyro aim %s; pad1 gyro=%d rate=%.0fHz sens=%.2f/%.2f",
			gyroAimEnabled ? "ON" : "OFF", hasgyro, rate, gyroSensX, gyroSensY);
	} else {
		const s32 on = (*arg) ? !(strcmp(arg, "0") == 0 || strcmp(arg, "off") == 0) : !gyroAimEnabled;
		inputGyroEnable(on);
		if (on && (!pads[0] || !padsCfg[0].hasGyro)) {
			sysLogPrintf(LOG_CHAT, "gyro aim ON (but pad 1 has no gyro!)");
		} else {
			sysLogPrintf(LOG_CHAT, "gyro aim %s", on ? "ON" : "OFF");
		}
	}
}

PD_CONSTRUCTOR static void inputConfigInit(void)
{
	configRegisterInt("Input.MouseEnabled", &mouseEnabled, 0, 1);
	configRegisterInt("Input.MouseLockMode", &mouseLockMode, MLOCK_OFF, MLOCK_AUTO);
	configRegisterInt("Input.MouseGrab", &mouseGrab, 0, 1);
	configRegisterFloat("Input.MouseSpeedX", &mouseSensX, -30.f, 30.f);
	configRegisterFloat("Input.MouseSpeedY", &mouseSensY, -30.f, 30.f);
	configRegisterInt("Input.FakeGamepads", &fakeControllers, 0, 4);
	configRegisterInt("Input.FirstGamepadNum", &firstController, 0, 3);
	configRegisterInt("Input.UseHIDAPI", &useHIDAPI, 0, 1);
	configRegisterInt("Input.UseRawInput", &useRawInput, 0, 1);
	configRegisterInt("Input.GamepadLED", &padLEDEnabled, 0, 1);
	configRegisterInt("Input.GyroAim", &gyroAimEnabled, 0, 1);
	configRegisterFloat("Input.GyroSpeedX", &gyroSensX, -30.f, 30.f);
	configRegisterFloat("Input.GyroSpeedY", &gyroSensY, -30.f, 30.f);

	char secname[] = "Input.Player1.Binds";
	char keyname[256] = { 0 };
	for (s32 c = 0; c < MAXCONTROLLERS; ++c) {
		secname[12] = '1' + c;
		secname[13] = '\0';
		configRegisterFloat(strFmt("%s.RumbleScale", secname), &padsCfg[c].rumbleScale, 0.f, 1.f);
		configRegisterInt(strFmt("%s.TriggerRumble", secname), &padsCfg[c].trigRumble, 0, 1);
		configRegisterInt(strFmt("%s.LStickDeadzoneX", secname), &padsCfg[c].deadzone[0], 0, 32767);
		configRegisterInt(strFmt("%s.LStickDeadzoneY", secname), &padsCfg[c].deadzone[1], 0, 32767);
		configRegisterInt(strFmt("%s.RStickDeadzoneX", secname), &padsCfg[c].deadzone[2], 0, 32767);
		configRegisterInt(strFmt("%s.RStickDeadzoneY", secname), &padsCfg[c].deadzone[3], 0, 32767);
		configRegisterFloat(strFmt("%s.LStickScaleX", secname), &padsCfg[c].sens[0], -10.f, 10.f);
		configRegisterFloat(strFmt("%s.LStickScaleY", secname), &padsCfg[c].sens[1], -10.f, 10.f);
		configRegisterFloat(strFmt("%s.RStickScaleX", secname), &padsCfg[c].sens[2], -10.f, 10.f);
		configRegisterFloat(strFmt("%s.RStickScaleY", secname), &padsCfg[c].sens[3], -10.f, 10.f);
		configRegisterInt(strFmt("%s.StickCButtons", secname), &padsCfg[c].stickCButtons, 0, 1);
		configRegisterInt(strFmt("%s.CancelCButtons", secname), &padsCfg[c].cancelCButtons, 0, 1);
		configRegisterInt(strFmt("%s.SwapSticks", secname), &padsCfg[c].swapSticks, 0, 1);
		configRegisterInt(strFmt("%s.ControllerIndex", secname), &padsCfg[c].deviceIndex, -1, 0x7FFFFFFF);
		secname[13] = '.';
		for (u32 ck = 0; ck < CK_TOTAL_COUNT; ++ck) {
			snprintf(keyname, sizeof(keyname), "%s.%s", secname, inputGetContKeyName(ck));
			configRegisterString(keyname, bindStrs[c][ck], MAX_BIND_STR);
		}
	}
}
