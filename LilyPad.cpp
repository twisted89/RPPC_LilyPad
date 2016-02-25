/*  LilyPad - Pad plugin for PS2 Emulator
 *  Copyright (C) 2002-2014  PCSX2 Dev Team/ChickenLiver
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the
 *  terms of the GNU Lesser General Public License as published by the Free
 *  Software Found- ation, either version 3 of the License, or (at your option)
 *  any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with PCSX2.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "Global.h"

// For escape timer, so as not to break GSDX+DX9.
#include <time.h>
#include "resource.h"
#include "InputManager.h"
#include "Config.h"

#define PADdefs

#include "DeviceEnumerator.h"
#ifdef _MSC_VER
#include "WndProcEater.h"
#endif
#include "KeyboardQueue.h"
#include "svnrev.h"
#include "DualShock4.h"
#include "HidDevice.h"

#define WMA_FORCE_UPDATE (WM_APP + 0x537)
#define FORCE_UPDATE_WPARAM ((WPARAM)0x74328943)
#define FORCE_UPDATE_LPARAM ((LPARAM)0x89437437)

// LilyPad version.
#define VERSION ((0<<8) | 1 | (0<<24))

#ifdef __linux__
Display *GSdsp;
Window  GSwin;
#else
HINSTANCE hInst;
HWND hWnd;
HWND hWndTop;

WndProcEater hWndGSProc;
WndProcEater hWndTopProc;

// ButtonProc is used mostly by the Config panel for eating the procedures of the
// button with keyboard focus.
WndProcEater hWndButtonProc;
#endif

// Keeps the various sources for Update polling (PADpoll, PADupdate, etc) from wreaking
// havoc on each other...
#ifdef __linux__
static std::mutex updateLock;
#else
CRITICAL_SECTION updateLock;
#endif

// Used to toggle mouse listening.
u8 miceEnabled;

// 2 when both pads are initialized, 1 for one pad, etc.
int openCount = 0;

int activeWindow = 0;
#ifdef _MSC_VER
int windowThreadId = 0;
int updateQueued = 0;
#endif

u32 bufSize = 0;
unsigned char outBuf[50];
unsigned char inBuf[50];

//		windowThreadId = GetWindowThreadProcessId(hWnd, 0);

#define MODE_DIGITAL 0x41
#define MODE_ANALOG 0x73
#define MODE_DS2_NATIVE 0x79

#ifdef _MSC_VER
int IsWindowMaximized(HWND hWnd) {
	RECT rect;
	if (GetWindowRect(hWnd, &rect)) {
		POINT p;
		p.x = rect.left;
		p.y = rect.top;
		MONITORINFO info;
		memset(&info, 0, sizeof(info));
		info.cbSize = sizeof(info);
		HMONITOR hMonitor;
		if ((hMonitor = MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY)) &&
			GetMonitorInfo(hMonitor, &info) &&
			memcmp(&info.rcMonitor, &rect, sizeof(rect)) == 0) {
			return 1;
		}
	}
	return 0;
}
#endif

void DEBUG_TEXT_OUT(const char *text) {
#ifdef _MSC_VER
	if (config.debug) {
		HANDLE hFile = CreateFileA("logs\\padLog.txt", FILE_APPEND_DATA, FILE_SHARE_READ, 0, OPEN_ALWAYS, 0, 0);
		if (hFile != INVALID_HANDLE_VALUE) {
			DWORD junk;
			WriteFile(hFile, text, strlen(text), &junk, 0);
			CloseHandle(hFile);;
		}
	}
#endif
}

void DEBUG_NEW_SET() {
#ifdef _MSC_VER
	if (config.debug && bufSize > 1) {
		HANDLE hFile = CreateFileA("logs\\padLog.txt", FILE_APPEND_DATA, FILE_SHARE_READ, 0, OPEN_ALWAYS, 0, 0);
		if (hFile != INVALID_HANDLE_VALUE) {
			u32 i;
			char temp[1500];
			char *end = temp;
			sprintf(end, "%02X (%02X) ", inBuf[0], inBuf[1]);
			end += 8;
			for (i = 2; i < bufSize; i++) {
				sprintf(end, "%02X ", inBuf[i]);
				end += 3;
			}
			end[-1] = '\n';
			sprintf(end, "%02X (%02X) ", outBuf[0], outBuf[1]);
			end += 8;
			for (i = 2; i < bufSize; i++) {
				sprintf(end, "%02X ", outBuf[i]);
				end += 3;
			}
			end[-1] = '\n';
			end++[0] = '\n';
			DWORD junk;
			WriteFile(hFile, temp, end - temp, &junk, 0);
			CloseHandle(hFile);;
		}
	}
	bufSize = 0;
#endif
}

inline void DEBUG_IN(unsigned char c) {
	if (bufSize < sizeof(inBuf) - 1) inBuf[bufSize] = c;
}
inline void DEBUG_OUT(unsigned char c) {
	if (bufSize < sizeof(outBuf) - 1) outBuf[bufSize++] = c;
}

struct Stick {
	int horiz;
	int vert;
};

// Sum of states of all controls for a pad (Not including toggles).
struct ButtonSum {
	int buttons[14];
	Stick sticks[3];
};

#define PAD_SAVE_STATE_VERSION	3

// Freeze data, for a single pad.  Basically has all pad state that
// a PS2 can set.
struct PadFreezeData {
	// Digital / Analog / DS2 Native
	u8 mode;

	u8 modeLock;

	// In config mode
	u8 config;

	u8 vibrate[8];
	u8 umask[2];

	// Vibration indices.
	u8 vibrateI[2];

	// Last vibration value sent to controller.
	// Only used so as not to call vibration
	// functions when old and new values are both 0.
	u8 currentVibrate[2];

	// Next vibrate val to send to controller.  If next and current are
	// both 0, nothing is sent to the controller.  Otherwise, it's sent
	// on every update.
	u8 nextVibrate[2];
};

class Pad : public PadFreezeData {
public:
	// Current pad state.
	ButtonSum sum;
	// State of locked buttons.  Already included by sum, used
	// as initial value of sum.
	ButtonSum lockedSum;

	// Flags for which controls (buttons or axes) are locked, if any.
	DWORD lockedState;

	// Used to keep track of which pads I'm running.
	// Note that initialized pads *can* be disabled.
	// I keep track of state of non-disabled non-initialized
	// pads, but should never be asked for their state.
	u8 initialized;

	// Set to 1 if the state of this pad has been updated since its state
	// was last queried.
	char stateUpdated;

	// initialized and not disabled (and mtap state for slots > 0).
	u8 enabled;
} pads[2][4];

// Active slots for each port.
int slots[2];
// Which ports we're running on.
int portInitialized[2];

// Force value to be from 0 to 255.
u8 Cap(int i) {
	if (i < 0) return 0;
	if (i > 255) return 255;
	return (u8)i;
}

// Force value to be from -127 to 127.
u8 CapS(int i) {
	if (i < -127) return -127;
	if (i > 127) return 127;
	return (u8)i;
}

inline void ReleaseModifierKeys() {
	QueueKeyEvent(VK_SHIFT, KEYRELEASE);
	QueueKeyEvent(VK_MENU, KEYRELEASE);
	QueueKeyEvent(VK_CONTROL, KEYRELEASE);
}

// RefreshEnabledDevices() enables everything that can potentially
// be bound to, as well as the "Ignore keyboard" device.
//
// This enables everything that input should be read from while the
// emulator is running.  Takes into account  mouse and focus state
// and which devices have bindings for enabled pads.  Releases
// keyboards if window is not focused.  Releases game devices if
// background monitoring is not checked.
// And releases games if not focused and config.background is not set.
void UpdateEnabledDevices(int updateList = 0) {
	// Enable all devices I might want.  Can ignore the rest.
	RefreshEnabledDevices(updateList);
	// Figure out which pads I'm getting input for.
	for (int port = 0; port < 2; port++) {
		for (int slot = 0; slot < 4; slot++) {
			if (slot && !config.multitap[port]) {
				pads[port][slot].enabled = 0;
			}
			else {
				pads[port][slot].enabled = pads[port][slot].initialized && config.padConfigs[port][slot].type != DisabledPad;
			}
		}
	}
	for (int i = 0; i < dm->numDevices; i++) {
		Device *dev = dm->devices[i];

		if (!dev->enabled) continue;
		if (!dev->attached) {
			dm->DisableDevice(i);
			continue;
		}

		// Disable ignore keyboard if don't have focus or there are no keys to ignore.
		if (dev->api == IGNORE_KEYBOARD) {
			if ((!config.vistaVolume && (config.keyboardApi == NO_API || !dev->pads[0][0].numBindings)) || !activeWindow) {
				dm->DisableDevice(i);
			}
			continue;
		}
		// Keep for PCSX2 keyboard shotcuts, unless unfocused.
		if (dev->type == KEYBOARD) {
			if (!activeWindow) dm->DisableDevice(i);
		}
		// Keep for cursor hiding consistency, unless unfocused.
		// miceEnabled tracks state of mouse enable/disable button, not if mouse API is set to disabled.
		else if (dev->type == MOUSE) {
			if (!activeWindow) dm->DisableDevice(i);
		}
		else if (!activeWindow && !config.background) dm->DisableDevice(i);
		else {
			int numActiveBindings = 0;
			for (int port = 0; port < 2; port++) {
				for (int slot = 0; slot < 4; slot++) {
					if (pads[port][slot].enabled) {
						numActiveBindings += dev->pads[port][slot].numBindings + dev->pads[port][slot].numFFBindings;
					}
				}
			}
			if (!numActiveBindings)
				dm->DisableDevice(i);
		}
	}
}

#ifdef _MSC_VER
BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD fdwReason, void* lpvReserved) {
	hInst = hInstance;
	if (fdwReason == DLL_PROCESS_ATTACH) {
		InitializeCriticalSection(&updateLock);

		DisableThreadLibraryCalls(hInstance);
	}
	else if (fdwReason == DLL_PROCESS_DETACH) {
		while (openCount)
			PADclose();
		PADshutdown();
		UninitHid();
		UninitLibUsb();
		DeleteCriticalSection(&updateLock);
	}
	return 1;
}
#endif

void AddForce(ButtonSum *sum, u8 cmd, int delta = 255) {
	if (!delta) return;
	if (cmd < 30) {
		sum->buttons[cmd - 0x10] += delta;
	}
	// D-pad.  Command numbering is based on ordering of digital values.
	else if (cmd < 34) {
		if (cmd == 0x30) {
			sum->sticks[0].vert -= delta;
		}
		else if (cmd == 31) {
			sum->sticks[0].horiz += delta;
		}
		else if (cmd == 32) {
			sum->sticks[0].vert += delta;
		}
		else if (cmd == 33) {
			sum->sticks[0].horiz -= delta;
		}
	}
	// Left stick.
	else if (cmd < 38) {
		if (cmd == 34) {
			sum->sticks[2].vert -= delta;
		}
		else if (cmd == 35) {
			sum->sticks[2].horiz += delta;
		}
		else if (cmd == 36) {
			sum->sticks[2].vert += delta;
		}
		else if (cmd == 37) {
			sum->sticks[2].horiz -= delta;
		}
	}
	// Right stick.
	else if (cmd < 42) {
		if (cmd == 38) {
			sum->sticks[1].vert -= delta;
		}
		else if (cmd == 39) {
			sum->sticks[1].horiz += delta;
		}
		else if (cmd == 40) {
			sum->sticks[1].vert += delta;
		}
		else if (cmd == 41) {
			sum->sticks[1].horiz -= delta;
		}
	}
}

int ProcessButtonBinding(Binding *b, ButtonSum *sum, int value) {
	if (value < b->deadZone || !value) return 0;

	if (config.turboKeyHack == 1){ // send a tabulator keypress to emulator
		//printf("%x\n", b->command);
		if (b->command == 0x11){ // L3 button
			static unsigned int LastCheck = 0;
			unsigned int t = timeGetTime();
			if (t - LastCheck < 300) return 0;
			QueueKeyEvent(VK_TAB, KEYPRESS);
			LastCheck = t;
		}
	}

	int sensitivity = b->sensitivity;
	if (sensitivity < 0) {
		sensitivity = -sensitivity;
		value = (1 << 16) - value;
	}
	if (value < 0) return 0;

	/* Note:  Value ranges of FULLY_DOWN, and sensitivity of
	 *  BASE_SENSITIVITY corresponds to an axis/button being exactly fully down.
	 *  Math in next line takes care of those two conditions, rounding as necessary.
	 *  Done using __int64s because overflows will occur when
	 *  sensitivity > BASE_SENSITIVITY and/or value > FULLY_DOWN.  Latter only happens
	 *  for relative axis.
	 */
	int force = (int)((((sensitivity*(255 * (__int64)value)) + BASE_SENSITIVITY / 2) / BASE_SENSITIVITY + FULLY_DOWN / 2) / FULLY_DOWN);
	AddForce(sum, b->command, force);

	return 1;
}

// Restricts d-pad/analog stick values to be from -255 to 255 and button values to be from 0 to 255.
// With D-pad in DS2 native mode, the negative and positive ranges are both independently from 0 to 255,
// which is why I use 9 bits of all sticks.  For left and right sticks, I have to remove a bit before sending.
void CapSum(ButtonSum *sum) {
	int i;
	for (i = 0; i < 3; i++) {
#ifdef __linux__
		int div = std::max(abs(sum->sticks[i].horiz), abs(sum->sticks[i].vert));
#else
		int div = max(abs(sum->sticks[i].horiz), abs(sum->sticks[i].vert));
#endif
		if (div > 255) {
			sum->sticks[i].horiz = sum->sticks[i].horiz * 255 / div;
			sum->sticks[i].vert = sum->sticks[i].vert * 255 / div;
		}
	}
	for (i = 0; i < 12; i++) {
		sum->buttons[i] = Cap(sum->buttons[i]);
	}
}

void CapSumRP(RPPadDataS* RPpad) {

	int div = max(abs(RPpad->leftJoyX), abs(RPpad->leftJoyY));

	if (div > 32767) {
		RPpad->leftJoyX = RPpad->leftJoyX * 32767 / div;
		RPpad->leftJoyY = RPpad->leftJoyY * 32767 / div;
	}

	div = max(abs(RPpad->rightJoyX), abs(RPpad->rightJoyY));

	if (div > 65535) {
		RPpad->rightJoyX = RPpad->rightJoyX * 32767 / div;
		RPpad->rightJoyY = RPpad->rightJoyY * 32767 / div;
	}
}

// Counter similar to stateUpdated for each pad, except used for PADkeyEvent instead.
// Only matters when GS thread updates is disabled (Just like summed pad values
// for pads beyond the first slot).

// Values, in order, correspond to PADkeyEvent, PADupdate(0), PADupdate(1), and
// WndProc(WMA_FORCE_UPDATE).  Last is always 0.
char padReadKeyUpdated[4] = { 0, 0, 0, 0 };

#define LOCK_DIRECTION 2
#define LOCK_BUTTONS 4
#define LOCK_BOTH 1

#ifdef _MSC_VER
struct EnterScopedSection
{
	CRITICAL_SECTION& m_cs;

	EnterScopedSection(CRITICAL_SECTION& cs) : m_cs(cs) {
		EnterCriticalSection(&m_cs);
	}

	~EnterScopedSection() {
		LeaveCriticalSection(&m_cs);
	}
};
#endif

int clamp(int min, int val, int max)
{
	if (val < min)
		return min;
	if (val > max)
		return max;
	return val;
}


static double mouse2axis(int which, s_mouse_control* mc, double x, double y, double exp, double multiplier, double dead_zone, e_shape shape, e_mouse_mode mode)
{

	double z = 0;
	double dz = dead_zone;
	double motion_residue = 0;
	double ztrunk = 0;
	double val = 0;
	int min_axis, max_axis;
	int new_state;
	double frequency_scale = 1.1250;
	int axis = 0;;

	max_axis = 127;
	min_axis = -max_axis;

	//Output("Axis: %d val: %d res: %.4f zt: %.4f X: %.4f Y: %.4f Exp: %.4f Mul: %.4f Dz: %.4f\n", which, axis, motion_residue, ztrunk, x, y, exp, multiplier, dead_zone);

	if (which == 0)
	{
		val = x * frequency_scale;
		if (x && y && shape == E_SHAPE_CIRCLE)
		{
			dz = dz*cos(atan(fabs(y / x)));
			//printf("1: %.4f\n", dz);
		}
	}
	else if (which == 1)
	{
		val = y * frequency_scale;
		if (x && y && shape == E_SHAPE_CIRCLE)
		{
			dz = dz*sin(atan(fabs(y / x)));
			//printf("2: %.4f\n", dz);
		}
	}

	if (val != 0)
	{
		z = multiplier * (val / fabs(val)) * pow(fabs(val), exp);
		//printf("3: %.4f\n", z);
		/*
		* Subtract the first position to the dead zone (useful for high multipliers).
		*/
		dz = dz - multiplier;// * pow(1, exp);
		//printf("4: %.4f\n", dz);
	}

	if (mode == E_MOUSE_MODE_AIMING)
	{
		if (z > 0)
		{
			axis = dz + z;
			/*
			* max axis position => no residue
			*/
			if (axis < max_axis)
			{
				ztrunk = axis - dz;
				//printf("5: %.4f\n", ztrunk);
			}
		}
		else if (z < 0)
		{
			axis = z - dz;
			/*
			* max axis position => no residue
			*/
			if (axis > min_axis)
			{
				ztrunk = axis + dz;
				//printf("6: %.4f\n", ztrunk);
			}
		}
		else axis = 0;
	}
	else //E_MOUSE_MODE_DRIVING
	{
		axis = axis + z;
		if (axis > 0 && axis < dz)
		{
			axis -= (2 * dz);
		}
		if (axis < 0 && axis > -dz)
		{
			axis += (2 * dz);
		}
	}

	axis = clamp(min_axis, axis, max_axis);

	if (val != 0 && ztrunk != 0)
	{
		//printf("ztrunk: %.4f\n", ztrunk);
		/*
		* Compute the motion that wasn't applied due to the double to integer conversion.
		*/
		motion_residue = (val / fabs(val)) * (fabs(val) - pow(fabs(ztrunk) / multiplier, 1 / exp));
		if (fabs(motion_residue) < 0.0039)//allow 256 subpositions
		{
			motion_residue = 0;
		}
		//printf("motion_residue: %.4f\n", motion_residue);
	}
	//Output("Axis: %d val: %d res: %.4f zt: %.4f X: %.4f Y: %.4f Exp: %.4f Mul: %.4f Dz: %.4f\n", which, axis, motion_residue, ztrunk, x, y, exp, multiplier, dead_zone);

	if (which == 0)
		mc->residue_x = motion_residue;
	else if (which == 1)
		mc->residue_y = motion_residue;

	//Output("val: %d\n", axis);
	return axis;
}

void Output(const char* szFormat, ...)
{
	char szBuff[1024];
	va_list arg;
	va_start(arg, szFormat);
	_vsnprintf(szBuff, sizeof(szBuff), szFormat, arg);
	va_end(arg);

	OutputDebugStringA(szBuff);
}

void UpdateRP(unsigned int port, unsigned int slot, RPPadDataS* RPpad){
	// Lock prior to timecheck code to avoid pesky race conditions.

#ifdef __linux__
	std::lock_guard<std::mutex> lock(updateLock);
#else
	EnterScopedSection padlock(updateLock);
#endif
	static unsigned int LastCheck = 0;
	unsigned int t = timeGetTime();
	if (t - LastCheck < 15 || !openCount) return;

	LastCheck = t;

#ifdef __linux__
	InitInfo info = {
		0, 0, GSdsp, GSwin
	};
#else
	InitInfo info = {
		0, 0, hWndTop, &hWndGSProc
	};
#endif

	RPpad->axisLXUpdate = false;
	RPpad->axisLYUpdate = false;
	RPpad->axisRXUpdate = false;
	RPpad->axisRYUpdate = false;
	RPpad->btnUpdate = false;

#ifdef _MSC_VER
	if (windowThreadId != GetCurrentThreadId()) {
		//if (!updateQueued) {
		//	updateQueued = 1;
		//	PostMessage(hWnd, WMA_FORCE_UPDATE, FORCE_UPDATE_WPARAM, FORCE_UPDATE_LPARAM);
		//}
		return;
	}
#endif

	dm->Update(&info);
	for (int i = 0; i < dm->numDevices; i++) {
		Device *dev = dm->devices[i];
		// Skip both disabled devices and inactive enabled devices.
		// Shouldn't be any of the latter, in general, but just in case...
		if (!dev->active) continue;
		if (config.padConfigs[port][slot].type == DisabledPad || !pads[port][slot].initialized) continue;
		for (int j = 0; j < dev->pads[port][slot].numBindings; j++) {
			Binding *b = dev->pads[port][slot].bindings + j;
			int cmd = b->command;
			int sensitivity = b->sensitivity;
			int state = dev->virtualControlState[b->controlIndex];
			double dz = (double)b->deadZone / BASE_SENSITIVITY;
			if (cmd > 0x0F) {

				if (dev->isMouse && cmd > 33){
					double exp = (double)b->Exponent / BASE_SENSITIVITY;
					double sen = (double)b->sensitivity / BASE_SENSITIVITY;
					if (((cmd == 35 || cmd == 39) && dev->mc.x > 0) || ((cmd == 37 || cmd == 41) && dev->mc.x < 0))
						state = abs(mouse2axis(0, &dev->mc, dev->mc.x, dev->mc.y, exp/*0.85*/, sen/*15*/, dz/*0*/, E_SHAPE_CIRCLE, E_MOUSE_MODE_AIMING));
					else if (((cmd == 34 || cmd == 38) && dev->mc.y < 0) || ((cmd == 36 || cmd == 40) && dev->mc.y > 0))
						state = abs(mouse2axis(1, &dev->mc, dev->mc.x, dev->mc.y, exp, sen, dz, E_SHAPE_CIRCLE, E_MOUSE_MODE_AIMING));
					else
						continue;
				}
				else{
					if (sensitivity < 0) {
						sensitivity = -sensitivity;
						state = (1 << 16) - state;
					}
				}

				if (!dev->isMouse)
					state = (int)((((sensitivity*(127 * (__int64)state)) + BASE_SENSITIVITY / 2) / BASE_SENSITIVITY + FULLY_DOWN / 2) / FULLY_DOWN);

				dev->virtualControlState[b->controlIndex] = state;

				if (state == dev->oldVirtualControlState[b->controlIndex]) continue;

				int btnVal = (1 << (cmd - 0x10));

				if (state > dz){

					if (cmd < 34) {
						if ((RPpad->buttonStatus & btnVal) == 0){
							RPpad->buttonStatus ^= btnVal;
							RPpad->btnUpdate = true;
						}

					}
					// Left stick.
					else if (cmd < 38) {
						if (cmd == 34){
							RPpad->leftJoyY = -state;
							RPpad->axisLYUpdate = true;
						}
						else if (cmd == 35){
							RPpad->leftJoyX = state;
							RPpad->axisLXUpdate = true;
						}
						else if (cmd == 36){
							RPpad->leftJoyY = state;
							RPpad->axisLYUpdate = true;
						}
						else if (cmd == 37){
							RPpad->leftJoyX = -state;
							RPpad->axisLXUpdate = true;
						}
					}
					// Right stick.
					else if (cmd < 42) {
						//Output("cmd: %d	val: %d\n", cmd, state);
						if (cmd == 38){
							RPpad->rightJoyY = -state;
							RPpad->axisRYUpdate = true;
						}
						else if (cmd == 39){
							RPpad->rightJoyX = state;
							RPpad->axisRXUpdate = true;
						}
						else if (cmd == 40){
							RPpad->rightJoyY = state;
							RPpad->axisRYUpdate = true;
						}
						else if (cmd == 41){
							RPpad->rightJoyX = -state;
							RPpad->axisRXUpdate = true;
						}
					}
				}
				else{
					if (cmd < 34) {
						if ((RPpad->buttonStatus & btnVal) > 0)
						{
							RPpad->buttonStatus ^= btnVal;
							RPpad->btnUpdate = true;
						}

					}
					// Left stick.
					else if (cmd < 38) {
						if (cmd == 34 || cmd == 36){
							if (!RPpad->axisLYUpdate){
								RPpad->leftJoyY = 0;
								RPpad->axisLYUpdate = true;
							}
						}
						else if (cmd == 35 || cmd == 37){
							if (!RPpad->axisLXUpdate){
								RPpad->leftJoyX = 0;
								RPpad->axisLXUpdate = true;
							}
						}
					}
					// Right stick.
					else if (cmd < 42) {
						if (cmd == 38 || cmd == 40){
							if (!RPpad->axisRYUpdate){
								RPpad->rightJoyY = 0;
								RPpad->axisRYUpdate = true;
							}
						}
						else if (cmd == 39 || cmd == 41){
							if (!RPpad->axisRXUpdate){
								RPpad->rightJoyX = 0;
								RPpad->axisRXUpdate = true;
							}
						}
					}

				}
				//else if ((state >> 15) && !(dev->oldVirtualControlState[b->controlIndex] >> 15)) {
				//	if (cmd == 0x0F) {
				//		miceEnabled = !miceEnabled;
				//		UpdateEnabledDevices();
				//	}
				//	else if (cmd == 0x0C) {
				//		lockStateChanged[port][slot] |= LOCK_BUTTONS;
				//	}
				//	else if (cmd == 0x0E) {
				//		lockStateChanged[port][slot] |= LOCK_DIRECTION;
				//	}
				//	else if (cmd == 0x0D) {
				//		lockStateChanged[port][slot] |= LOCK_BOTH;
				//	}
				//	else if (cmd == 0x28) {
				//		if (!pads[port][slot].modeLock) {
				//			if (pads[port][slot].mode != MODE_DIGITAL)
				//				pads[port][slot].mode = MODE_DIGITAL;
				//			else
				//				pads[port][slot].mode = MODE_ANALOG;
				//		}
				//	}
				//}
			}
		}
	}
	dm->PostRead();

	CapSumRP(RPpad);

	for (int motor = 0; motor < 2; motor++) {
		// TODO:  Probably be better to send all of these at once.
		if (pads[port][slot].nextVibrate[motor] | pads[port][slot].currentVibrate[motor]) {
			pads[port][slot].currentVibrate[motor] = pads[port][slot].nextVibrate[motor];
			dm->SetEffect(port, slot, motor, pads[port][slot].nextVibrate[motor]);
		}
	}
}

void Update(unsigned int port, unsigned int slot) {
	char *stateUpdated;
	if (port < 2) {
		stateUpdated = &pads[port][slot].stateUpdated;
	}
	else if (port < 6) {
		stateUpdated = padReadKeyUpdated + port - 2;
	}
	else return;

	// Lock prior to timecheck code to avoid pesky race conditions.
#ifdef __linux__
	std::lock_guard<std::mutex> lock(updateLock);
#else
	EnterScopedSection padlock(updateLock);
#endif

	static unsigned int LastCheck = 0;
	unsigned int t = timeGetTime();
	if (t - LastCheck < 15 || !openCount) return;

#ifdef _MSC_VER
	if (windowThreadId != GetCurrentThreadId()) {
		if (stateUpdated[0] < 0) {
			if (!updateQueued) {
				updateQueued = 1;
				PostMessage(hWnd, WMA_FORCE_UPDATE, FORCE_UPDATE_WPARAM, FORCE_UPDATE_LPARAM);
			}
		}
		return;
	}
#endif

	LastCheck = t;
}

void CALLBACK PADupdate(int port) {
	Update(port + 3, 0);
}

inline void SetVibrate(int port, int slot, int motor, u8 val) {
	pads[port][slot].nextVibrate[motor] = val;
}

u32 CALLBACK PS2EgetLibType(void) {
	ps2e = 1;
	return PS2E_LT_PAD;
}

u32 CALLBACK PS2EgetLibVersion2(u32 type) {
	ps2e = 1;
	if (type == PS2E_LT_PAD)
		return (PS2E_PAD_VERSION << 16) | VERSION;
	return 0;
}

#ifdef _MSC_VER
// Used in about and config screens.
void GetNameAndVersionString(wchar_t *out) {
#ifdef NO_CRT
	wsprintfW(out, L"LilyPad %i.%i.%i", (VERSION >> 8) & 0xFF, VERSION & 0xFF, (VERSION >> 24) & 0xFF);
#elif defined(PCSX2_DEBUG)
	wsprintfW(out, L"LilyPad Debug %i.%i.%i (%lld)", (VERSION >> 8) & 0xFF, VERSION & 0xFF, (VERSION >> 24) & 0xFF, SVN_REV);
#else
	wsprintfW(out, L"LilyPad PS4 %i.%i.%i", (VERSION >> 8) & 0xFF, VERSION & 0xFF, (VERSION >> 24) & 0xFF);
#endif
}
#endif

char* CALLBACK PSEgetLibName() {
#ifdef NO_CRT
	return "LilyPad";
#elif defined(PCSX2_DEBUG)
	static char version[50];
	sprintf(version, "LilyPad Debug (%lld)", SVN_REV);
	return version;
#else
	static char version[50];
	sprintf(version, "LilyPad (%lld)", SVN_REV);
	return version;
#endif
}

char* CALLBACK PS2EgetLibName(void) {
	ps2e = 1;
	return PSEgetLibName();
}

//void CALLBACK PADgsDriverInfo(GSdriverInfo *info) {
//	info=info;
//}

void CALLBACK PADshutdown() {
	DEBUG_TEXT_OUT("LilyPad shutdown.\n\n");
	for (int i = 0; i < 8; i++)
		pads[i & 1][i >> 1].initialized = 0;
	portInitialized[0] = portInitialized[1] = 0;
	UnloadConfigs();
}

inline void StopVibrate() {
	for (int i = 0; i < 8; i++) {
		SetVibrate(i & 1, i >> 1, 0, 0);
		SetVibrate(i & 1, i >> 1, 1, 0);
	}
}

inline void ResetVibrate(int port, int slot) {
	SetVibrate(port, slot, 0, 0);
	SetVibrate(port, slot, 1, 0);
	((int*)(pads[port][slot].vibrate))[0] = 0xFFFFFF5A;
	((int*)(pads[port][slot].vibrate))[1] = 0xFFFFFFFF;
}

void ResetPad(int port, int slot) {
	// Lines before memset currently don't do anything useful,
	// but allow this function to be called at any time.

	// Need to backup, so can be called at any point.
	u8 enabled = pads[port][slot].enabled;

	// Currently should never do anything.
	SetVibrate(port, slot, 0, 0);
	SetVibrate(port, slot, 1, 0);

	memset(&pads[port][slot], 0, sizeof(pads[0][0]));
	pads[port][slot].mode = MODE_DIGITAL;
	pads[port][slot].umask[0] = pads[port][slot].umask[1] = 0xFF;
	// Sets up vibrate variable.
	ResetVibrate(port, slot);
	if (config.padConfigs[port][slot].autoAnalog && !ps2e) {
		pads[port][slot].mode = MODE_ANALOG;
	}
	pads[port][slot].initialized = 1;

	pads[port][slot].enabled = enabled;
}


struct QueryInfo {
	u8 port;
	u8 slot;
	u8 lastByte;
	u8 currentCommand;
	u8 numBytes;
	u8 queryDone;
	u8 response[42];
} query = { 0, 0, 0, 0, 0, 0xFF, 0xF3 };

#ifdef _MSC_VER
int saveStateIndex = 0;
#endif

s32 CALLBACK PADinit(u32 flags) {
	// Note:  Won't load settings if already loaded.
	if (LoadSettings() < 0) {
		return -1;
	}
	int port = (flags & 3);
	if (port == 3) {
		if (PADinit(1) == -1) return -1;
		return PADinit(2);
	}

#if defined(PCSX2_DEBUG) && defined(_MSC_VER)
	int tmpFlag = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
	tmpFlag |= _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF;
	_CrtSetDbgFlag(tmpFlag);
#endif

	port--;

	for (int i = 0; i < 4; i++) {
		ResetPad(port, i);
	}
	slots[port] = 0;
	portInitialized[port] = 1;

	query.lastByte = 1;
	query.numBytes = 0;
	ClearKeyQueue();
#ifdef __linux__
	R_ClearKeyQueue();
#endif
	// Just in case, when resuming emulation.
	ReleaseModifierKeys();

	DEBUG_TEXT_OUT("LilyPad initialized\n\n");
	return 0;
}



// Note to self:  Has to be a define for the sizeof() to work right.
// Note to self 2: All are the same size, anyways, except for longer full DS2 response
//   and shorter digital mode response.
#define SET_RESULT(a) { \
	memcpy(query.response+2, a, sizeof(a)); \
	query.numBytes = 2+sizeof(a); \
			}

#define SET_FINAL_RESULT(a) {			  \
	memcpy(query.response+2, a, sizeof(a));\
	query.numBytes = 2+sizeof(a);		  \
	query.queryDone = 1;			  \
			}

static const u8 ConfigExit[7] = { 0x5A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
//static const u8 ConfigExit[7] = {0x5A, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};

static const u8 noclue[7] = { 0x5A, 0x00, 0x00, 0x02, 0x00, 0x00, 0x5A };
static u8 queryMaskMode[7] = { 0x5A, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x5A };
//static const u8 DSNonNativeMode[7] = {0x5A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const u8 setMode[7] = { 0x5A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

// DS2
static const u8 queryModelDS2[7] = { 0x5A, 0x03, 0x02, 0x00, 0x02, 0x01, 0x00 };
// DS1
static const u8 queryModelDS1[7] = { 0x5A, 0x01, 0x02, 0x00, 0x02, 0x01, 0x00 };

static const u8 queryAct[2][7] = { { 0x5A, 0x00, 0x00, 0x01, 0x02, 0x00, 0x0A },
{ 0x5A, 0x00, 0x00, 0x01, 0x01, 0x01, 0x14 } };

static const u8 queryComb[7] = { 0x5A, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00 };

static const u8 queryMode[7] = { 0x5A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };


static const u8 setNativeMode[7] = { 0x5A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A };

#ifdef _MSC_VER
// Implements a couple of the hacks that affect whatever top-level window
// the GS viewport belongs to (title, screensaver)
ExtraWndProcResult TitleHackWndProc(HWND hWndTop, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT *output) {
	switch (uMsg) {
	case WM_SETTEXT:
		if (config.saveStateTitle) {
			wchar_t text[200];
			int len;
			if (IsWindowUnicode(hWndTop)) {
				len = wcslen((wchar_t*)lParam);
				if (len < sizeof(text) / sizeof(wchar_t)) wcscpy(text, (wchar_t*)lParam);
			}
			else {
				len = MultiByteToWideChar(CP_ACP, 0, (char*)lParam, -1, text, sizeof(text) / sizeof(wchar_t));
			}
			if (len > 0 && len < 150 && !wcsstr(text, L" | State(Lilypad) ")) {
				wsprintfW(text + len, L" | State(Lilypad) %i", saveStateIndex);
				SetWindowText(hWndTop, text);
				return NO_WND_PROC;
			}
		}
		break;
	case WM_SYSCOMMAND:
		if ((wParam == SC_SCREENSAVE || wParam == SC_MONITORPOWER) && config.disableScreenSaver)
			return NO_WND_PROC;
		break;
	default:
		break;
	}
	return CONTINUE_BLISSFULLY;
}

// All that's needed to force hiding the cursor in the proper thread.
// Could have a special case elsewhere, but this make sure it's called
// only once, rather than repeatedly.
ExtraWndProcResult HideCursorProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT *output) {
	ShowCursor(0);
	return CONTINUE_BLISSFULLY_AND_RELEASE_PROC;
}

// Useful sequence before changing into active/inactive state.
// Handles hooking/unhooking of mouse and KB and also mouse cursor visibility.
// towardsActive==true indicates we're gaining activity (on focus etc), false is for losing activity (on close, kill focus, etc).
void PrepareActivityState(bool towardsActive) {
	if (!towardsActive)
		ReleaseModifierKeys();
	else{
		if (config.forceHide) {
			hWndGSProc.Eat(HideCursorProc, 0);
		}
	}

	activeWindow = towardsActive;
	UpdateEnabledDevices();
}

// responsible for monitoring device addition/removal, focus changes, and viewport closures.
ExtraWndProcResult StatusWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT *output) {
	switch (uMsg) {
	case WMA_FORCE_UPDATE:
		if (wParam == FORCE_UPDATE_WPARAM && lParam == FORCE_UPDATE_LPARAM) {
			if (updateQueued) {
				updateQueued = 0;
				Update(5, 0);
			}
			return NO_WND_PROC;
		}
	case WM_DEVICECHANGE:
		if (wParam == DBT_DEVNODES_CHANGED) {
			UpdateEnabledDevices(1);
		}
		break;
	case WM_ACTIVATE:
		// Release any buttons PCSX2 may think are down when
		// losing/gaining focus.
		// Note - I never managed to see this case entered, but SET/KILLFOCUS are entered. - avih 2014-04-16
		PrepareActivityState(LOWORD(wParam) != WA_INACTIVE);
		break;
	case WM_CLOSE:
		if (config.closeHacks & 2) {
			ExitProcess(0);
			return NO_WND_PROC;
		}
		else{
			QueueKeyEvent(VK_ESCAPE, KEYPRESS);
			return NO_WND_PROC;
		}
		break;
	case WM_DESTROY:
		QueueKeyEvent(VK_ESCAPE, KEYPRESS);
		break;
	case WM_KILLFOCUS:
		PrepareActivityState(false);
		break;
	case WM_SETFOCUS:
		PrepareActivityState(true);
		break;
	default:
		break;
	}
	return CONTINUE_BLISSFULLY;
}

char restoreFullScreen = 0;
// This hack sends ALT+ENTER to the window to toggle fullscreen.
// PCSX2 doesn't need it (it exits full screen on ESC on its own).
DWORD WINAPI MaximizeWindowThreadProc(void *lpParameter) {
	Sleep(100);
	keybd_event(VK_LMENU, MapVirtualKey(VK_LMENU, MAPVK_VK_TO_VSC), 0, 0);
	keybd_event(VK_RETURN, MapVirtualKey(VK_RETURN, MAPVK_VK_TO_VSC), 0, 0);
	Sleep(10);
	keybd_event(VK_RETURN, MapVirtualKey(VK_RETURN, MAPVK_VK_TO_VSC), KEYEVENTF_KEYUP, 0);
	keybd_event(VK_LMENU, MapVirtualKey(VK_LMENU, MAPVK_VK_TO_VSC), KEYEVENTF_KEYUP, 0);
	return 0;
}
#endif

void CALLBACK PADconfigure() {
	if (openCount) {
		return;
	}
	Configure();
}

#ifdef _MSC_VER
DWORD WINAPI RenameWindowThreadProc(void *lpParameter) {
	wchar_t newTitle[200];
	if (hWndTop) {
		int len = GetWindowTextW(hWndTop, newTitle, 200);
		if (len > 0 && len < 199) {
			wchar_t *end;
			if (end = wcsstr(newTitle, L" | State ")) *end = 0;
			SetWindowTextW(hWndTop, newTitle);
		}
	}
	return 0;
}

void SaveStateChanged() {
	if (config.saveStateTitle) {
		// GSDX only checks its window's message queue at certain points or something, so
		// have to do this in another thread to prevent deadlock.
		HANDLE hThread = CreateThread(0, 0, RenameWindowThreadProc, 0, 0, 0);
		if (hThread) CloseHandle(hThread);
	}
}
#endif

s32 CALLBACK PADopen(void *pDsp) {
	if (openCount++) return 0;
	DEBUG_TEXT_OUT("LilyPad opened\n\n");

	miceEnabled = !config.mouseUnfocus;
#ifdef _MSC_VER
	if (!hWnd) {
		if (IsWindow((HWND)pDsp)) {
			hWnd = (HWND)pDsp;
		}
		else if (pDsp && !IsBadReadPtr(pDsp, 4) && IsWindow(*(HWND*)pDsp)) {
			hWnd = *(HWND*)pDsp;
		}
		else {
			openCount = 0;
			MessageBoxA(GetActiveWindow(),
				"Invalid Window handle passed to LilyPad.\n"
				"\n"
				"Either your emulator or gs plugin is buggy,\n"
				"Despite the fact the emulator is about to\n"
				"blame LilyPad for failing to initialize.",
				"Non-LilyPad Error", MB_OK | MB_ICONERROR);
			return -1;
		}
		hWndTop = hWnd;
		while (GetWindowLong(hWndTop, GWL_STYLE) & WS_CHILD)
			hWndTop = GetParent(hWndTop);

		if (!hWndGSProc.SetWndHandle(hWnd)) {
			openCount = 0;
			return -1;
		}

		// Implements most hacks, as well as enabling/disabling mouse
		// capture when focus changes.
		updateQueued = 0;
		hWndGSProc.Eat(StatusWndProc, 0);

		if (hWnd != hWndTop) {
			if (!hWndTopProc.SetWndHandle(hWndTop)) {
				openCount = 0;
				return -1;
			}
			hWndTopProc.Eat(TitleHackWndProc, 0);
		}
		else
			hWndGSProc.Eat(TitleHackWndProc, 0);

		if (config.forceHide) {
			hWndGSProc.Eat(HideCursorProc, 0);
		}
		SaveStateChanged();

		windowThreadId = GetWindowThreadProcessId(hWndTop, 0);
	}

	if (restoreFullScreen) {
		if (!IsWindowMaximized(hWndTop)) {
			HANDLE hThread = CreateThread(0, 0, MaximizeWindowThreadProc, hWndTop, 0, 0);
			if (hThread) CloseHandle(hThread);
		}
		restoreFullScreen = 0;
	}
#endif
	for (int port = 0; port < 2; port++) {
		for (int slot = 0; slot < 4; slot++) {
			memset(&pads[port][slot].sum, 0, sizeof(pads[port][slot].sum));
			memset(&pads[port][slot].lockedSum, 0, sizeof(pads[port][slot].lockedSum));
			pads[port][slot].lockedState = 0;
		}
	}

#ifdef _MSC_VER
	// I'd really rather use this line, but GetActiveWindow() does not have complete specs.
	// It *seems* to return null when no window from this thread has focus, but the
	// Microsoft specs seem to imply it returns the window from this thread that would have focus,
	// if any window did (topmost in this thread?).  Which isn't what I want, and doesn't seem
	// to be what it actually does.
	// activeWindow = GetActiveWindow() == hWnd;

	// activeWindow = (GetAncestor(hWnd, GA_ROOT) == GetAncestor(GetForegroundWindow(), GA_ROOT));
#else
	// Not used so far
	GSdsp = *(Display**)pDsp;
	GSwin = (Window)*(((uptr*)pDsp) + 1);
#endif
	activeWindow = miceEnabled;

	UpdateEnabledDevices();
	return 0;
}

void CALLBACK PADclose() {
	if (openCount && !--openCount) {
		DEBUG_TEXT_OUT("LilyPad closed\n\n");
#ifdef _MSC_VER
		updateQueued = 0;
		hWndGSProc.Release();
		hWndTopProc.Release();
		dm->ReleaseInput();
		hWnd = 0;
		hWndTop = 0;
#else
		R_ClearKeyQueue();
#endif
		ClearKeyQueue();
	}
}

u8 CALLBACK PADstartPoll(int port) {
	DEBUG_NEW_SET();
	port--;
	if ((unsigned int)port <= 1 && pads[port][slots[port]].enabled) {
		query.queryDone = 0;
		query.port = port;
		query.slot = slots[port];
		query.numBytes = 2;
		query.lastByte = 0;
		DEBUG_IN(port);
		DEBUG_OUT(0xFF);
		DEBUG_IN(slots[port]);
		DEBUG_OUT(pads[port][slots[port]].enabled);
		return 0xFF;
	}
	else {
		query.queryDone = 1;
		query.numBytes = 0;
		query.lastByte = 1;
		DEBUG_IN(0);
		DEBUG_OUT(0);
		DEBUG_IN(port);
		DEBUG_OUT(0);
		return 0;
	}
}

inline int IsDualshock4(u8 port, u8 slot) {
	return config.padConfigs[query.port][query.slot].type == Dualshock4Pad;
}

u8 CALLBACK PADpoll(u8 value) {
	DEBUG_IN(value);
	if (query.lastByte + 1 >= query.numBytes) {
		DEBUG_OUT(0);
		return 0;
	}
	if (query.lastByte && query.queryDone) {
		DEBUG_OUT(query.response[1 + query.lastByte]);
		return query.response[++query.lastByte];
	}

	int i;
	Pad *pad = &pads[query.port][query.slot];
	if (query.lastByte == 0) {
		query.lastByte++;
		query.currentCommand = value;
		switch (value) {
			// CONFIG_MODE
		case 0x43:
			if (pad->config) {
				// In config mode.  Might not actually be leaving it.
				SET_RESULT(ConfigExit);
				DEBUG_OUT(0xF3);
				return 0xF3;
			}
			// READ_DATA_AND_VIBRATE
		case 0x42:
			query.response[2] = 0x5A;
			{
				Update(query.port, query.slot);
				ButtonSum *sum = &pad->sum;

				u8 b1 = 0xFF, b2 = 0xFF;
				for (i = 0; i < 4; i++) {
					b1 -= (sum->buttons[i]   > 0) << i;
				}
				for (i = 0; i < 8; i++) {
					b2 -= (sum->buttons[i + 4] > 0) << i;
				}
				b1 -= ((sum->sticks[0].vert < 0) << 4);
				b1 -= ((sum->sticks[0].horiz > 0) << 5);
				b1 -= ((sum->sticks[0].vert > 0) << 6);
				b1 -= ((sum->sticks[0].horiz < 0) << 7);
				query.response[3] = b1;
				query.response[4] = b2;

				query.numBytes = 5;
				if (pad->mode != MODE_DIGITAL) {
					query.response[5] = Cap((sum->sticks[1].horiz + 255) / 2);
					query.response[6] = Cap((sum->sticks[1].vert + 255) / 2);
					query.response[7] = Cap((sum->sticks[2].horiz + 255) / 2);
					query.response[8] = Cap((sum->sticks[2].vert + 255) / 2);

					query.numBytes = 9;
					if (pad->mode != MODE_ANALOG) {
						// Good idea?  No clue.
						//query.response[3] &= pad->mask[0];
						//query.response[4] &= pad->mask[1];

						// Each value is from -255 to 255, so have to use cap to convert
						// negative values to 0.
						query.response[9] = Cap(sum->sticks[0].horiz);
						query.response[10] = Cap(-sum->sticks[0].horiz);
						query.response[11] = Cap(-sum->sticks[0].vert);
						query.response[12] = Cap(sum->sticks[0].vert);

						// No need to cap these, already done int CapSum().
						query.response[13] = (unsigned char)sum->buttons[8];
						query.response[14] = (unsigned char)sum->buttons[9];
						query.response[15] = (unsigned char)sum->buttons[10];
						query.response[16] = (unsigned char)sum->buttons[11];
						query.response[17] = (unsigned char)sum->buttons[6];
						query.response[18] = (unsigned char)sum->buttons[7];
						query.response[19] = (unsigned char)sum->buttons[4];
						query.response[20] = (unsigned char)sum->buttons[5];
						query.numBytes = 21;
					}
				}
			}

			query.lastByte = 1;
			DEBUG_OUT(pad->mode);
			return pad->mode;
			// SET_VREF_PARAM
		case 0x40:
			SET_FINAL_RESULT(noclue);
			break;
			// QUERY_DS2_ANALOG_MODE
		case 0x41:
			// Right?  Wrong?  No clue.
			if (pad->mode == MODE_DIGITAL) {
				queryMaskMode[1] = queryMaskMode[2] = queryMaskMode[3] = 0;
				queryMaskMode[6] = 0x00;
			}
			else {
				queryMaskMode[1] = pad->umask[0];
				queryMaskMode[2] = pad->umask[1];
				queryMaskMode[3] = 0x03;
				// Not entirely sure about this.
				//queryMaskMode[3] = 0x01 | (pad->mode == MODE_DS2_NATIVE)*2;
				queryMaskMode[6] = 0x5A;
			}
			SET_FINAL_RESULT(queryMaskMode);
			break;
			// SET_MODE_AND_LOCK
		case 0x44:
			SET_RESULT(setMode);
			ResetVibrate(query.port, query.slot);
			break;
			// QUERY_MODEL_AND_MODE
		case 0x45:
			if (IsDualshock4(query.port, query.slot)) {
				SET_FINAL_RESULT(queryModelDS2)
			}
			else {
				SET_FINAL_RESULT(queryModelDS1);
			}
			// Not digital mode.
			query.response[5] = (pad->mode & 0xF) != 1;
			break;
			// QUERY_ACT
		case 0x46:
			SET_RESULT(queryAct[0]);
			break;
			// QUERY_COMB
		case 0x47:
			SET_FINAL_RESULT(queryComb);
			break;
			// QUERY_MODE
		case 0x4C:
			SET_RESULT(queryMode);
			break;
			// VIBRATION_TOGGLE
		case 0x4D:
			memcpy(query.response + 2, pad->vibrate, 7);
			query.numBytes = 9;
			ResetVibrate(query.port, query.slot);
			break;
			// SET_DS2_NATIVE_MODE
		case 0x4F:
			if (IsDualshock4(query.port, query.slot)) {
				SET_RESULT(setNativeMode);
			}
			else {
				SET_FINAL_RESULT(setNativeMode);
			}
			break;
		default:
			query.numBytes = 0;
			query.queryDone = 1;
			break;
		}
		DEBUG_OUT(0xF3);
		return 0xF3;
	}
	else {
		query.lastByte++;
		switch (query.currentCommand) {
			// READ_DATA_AND_VIBRATE
		case 0x42:
			if (query.lastByte == pad->vibrateI[0]) {
				SetVibrate(query.port, query.slot, 1, 255 * (value & 1));
			}
			else if (query.lastByte == pad->vibrateI[1]) {
				SetVibrate(query.port, query.slot, 0, value);
			}
			break;
			// CONFIG_MODE
		case 0x43:
			if (query.lastByte == 3) {
				query.queryDone = 1;
				pad->config = value;
			}
			break;
			// SET_MODE_AND_LOCK
		case 0x44:
			if (query.lastByte == 3 && value < 2) {
				static const u8 modes[2] = { MODE_DIGITAL, MODE_ANALOG };
				pad->mode = modes[value];
			}
			else if (query.lastByte == 4) {
				if (value == 3) {
					pad->modeLock = 3;
				}
				else {
					pad->modeLock = 0;
					if (pad->mode == MODE_DIGITAL && config.padConfigs[query.port][query.slot].autoAnalog && !ps2e) {
						pad->mode = MODE_ANALOG;
					}
				}
				query.queryDone = 1;
			}
			break;
			// QUERY_ACT
		case 0x46:
			if (query.lastByte == 3) {
				if (value < 2) SET_RESULT(queryAct[value])
					// bunch of 0's
					// else SET_RESULT(setMode);
					query.queryDone = 1;
			}
			break;
			// QUERY_MODE
		case 0x4C:
			if (query.lastByte == 3 && value < 2) {
				query.response[6] = 4 + value * 3;
				query.queryDone = 1;
			}
			// bunch of 0's
			//else data = setMode;
			break;
			// VIBRATION_TOGGLE
		case 0x4D:
			if (query.lastByte >= 3) {
				if (value == 0) {
					pad->vibrateI[0] = (u8)query.lastByte;
				}
				else if (value == 1) {
					pad->vibrateI[1] = (u8)query.lastByte;
				}
				pad->vibrate[query.lastByte - 2] = value;
			}
			break;
			// SET_DS2_NATIVE_MODE
		case 0x4F:
			if (query.lastByte == 3 || query.lastByte == 4) {
				pad->umask[query.lastByte - 3] = value;
			}
			else if (query.lastByte == 5) {
				if (!(value & 1)) {
					pad->mode = MODE_DIGITAL;
				}
				else if (!(value & 2)) {
					pad->mode = MODE_ANALOG;
				}
				else {
					pad->mode = MODE_DS2_NATIVE;
				}
			}
			break;
		default:
			DEBUG_OUT(0);
			return 0;
		}
		DEBUG_OUT(query.response[query.lastByte]);
		return query.response[query.lastByte];
	}
}

// returns: 1 if supports pad1
//			2 if supports pad2
//			3 if both are supported
u32 CALLBACK PADquery() {
	return 3;
}

#ifdef _MSC_VER
INT_PTR CALLBACK AboutDialogProc(HWND hWndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	if (uMsg == WM_INITDIALOG) {
		wchar_t idString[100];
		GetNameAndVersionString(idString);
		SetDlgItemTextW(hWndDlg, IDC_VERSION, idString);
	}
	else if (uMsg == WM_COMMAND && (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)) {
		EndDialog(hWndDlg, 0);
		return 1;
	}
	return 0;
}
#endif


void CALLBACK PADabout() {
#ifdef _MSC_VER
	DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUT), 0, AboutDialogProc);
#endif
}

s32 CALLBACK PADtest() {
	return 0;
}

keyEvent* CALLBACK PADkeyEvent() {
	// If running both pads, ignore every other call.  So if two keys pressed in same interval...
	static char eventCount = 0;
	eventCount++;
	if (eventCount < openCount) {
		return 0;
	}
	eventCount = 0;

	//Update(2, 0);
	static keyEvent ev;
	if (!GetQueuedKeyEvent(&ev)) return 0;

#ifdef _MSC_VER
	static char shiftDown = 0;
	static char altDown = 0;
	if (miceEnabled && (ev.key == VK_ESCAPE || (int)ev.key == -2) && ev.evt == KEYPRESS) {
		// Disable mouse/KB hooks on escape (before going into paused mode).
		// This is a hack, since PADclose (which is called on pause) should enevtually also deactivate the
		// mouse/kb capture. In practice, WindowsMessagingMouse::Deactivate is called from PADclose, but doesn't
		// manage to release the mouse, maybe due to the thread from which it's called or some
		// state or somehow being too late.
		// This explicitly triggers inactivity (releasing mouse/kb hooks) before PCSX2 starts to close the plugins.
		// Regardless, the mouse/kb hooks will get re-enabled on resume if required without need for further hacks.

		PrepareActivityState(false);
	}

	if ((ev.key == VK_ESCAPE || (int)ev.key == -2) && ev.evt == KEYPRESS && config.escapeFullscreenHack) {
		static int t;
		if ((int)ev.key != -2 && IsWindowMaximized(hWndTop)) {
			t = timeGetTime();
			QueueKeyEvent(-2, KEYPRESS);
			HANDLE hThread = CreateThread(0, 0, MaximizeWindowThreadProc, 0, 0, 0);
			if (hThread) CloseHandle(hThread);
			restoreFullScreen = 1;
			return 0;
		}
		if (ev.key != VK_ESCAPE) {
			if (timeGetTime() - t < 1000) {
				QueueKeyEvent(-2, KEYPRESS);
				return 0;
			}
		}
		ev.key = VK_ESCAPE;
	}

	//if (ev.key == VK_F2 && ev.evt == KEYPRESS) {
	//	saveStateIndex += 1 - 2 * shiftDown;
	//	saveStateIndex = (saveStateIndex + 10) % 10;
	//	SaveStateChanged();
	//}

	// So don't change skip mode on alt-F4.
	if (ev.key == VK_F4 && altDown) {
		return 0;
	}

	if (ev.key == VK_LSHIFT || ev.key == VK_RSHIFT || ev.key == VK_SHIFT) {
		ev.key = VK_SHIFT;
		shiftDown = (ev.evt == KEYPRESS);
	}
	else if (ev.key == VK_LCONTROL || ev.key == VK_RCONTROL) {
		ev.key = VK_CONTROL;
	}
	else if (ev.key == VK_LMENU || ev.key == VK_RMENU || ev.key == VK_SHIFT) {
		ev.key = VK_MENU;
		altDown = (ev.evt == KEYPRESS);
	}
#endif
	return &ev;
}

struct PadPluginFreezeData {
	char format[8];
	// Currently all different versions are incompatible.
	// May split into major/minor with some compatibility rules.
	u32 version;
	// So when loading, know which plugin's settings I'm loading.
	// Not a big deal.  Use a static variable when saving to figure it out.
	u8 port;
	// active slot for port
	u8 slot[2];
	PadFreezeData padData[2][4];
	QueryInfo query;
};

s32 CALLBACK PADfreeze(int mode, freezeData *data) {
	if (!data)
	{
		printf("LilyPad savestate null pointer!\n");
		return -1;
	}

	if (mode == FREEZE_SIZE) {
		data->size = sizeof(PadPluginFreezeData);
	}
	else if (mode == FREEZE_LOAD) {
		PadPluginFreezeData &pdata = *(PadPluginFreezeData*)(data->data);
		StopVibrate();
		if (data->size != sizeof(PadPluginFreezeData) ||
			pdata.version != PAD_SAVE_STATE_VERSION ||
			strcmp(pdata.format, "PadMode")) return 0;

		if (pdata.port >= 2) return 0;

		query = pdata.query;
		if (pdata.query.slot < 4) {
			query = pdata.query;
		}

		// Tales of the Abyss - pad fix
		// - restore data for both ports
		for (int port = 0; port < 2; port++) {
			for (int slot = 0; slot < 4; slot++) {
				u8 mode = pdata.padData[port][slot].mode;

				if (mode != MODE_DIGITAL && mode != MODE_ANALOG && mode != MODE_DS2_NATIVE) {
					break;
				}

				// Not sure if the cast is strictly necessary, but feel safest with it there...
				*(PadFreezeData*)&pads[port][slot] = pdata.padData[port][slot];
			}

			if (pdata.slot[port] < 4)
				slots[port] = pdata.slot[port];
		}
	}
	else if (mode == FREEZE_SAVE) {
		if (data->size != sizeof(PadPluginFreezeData)) return 0;
		PadPluginFreezeData &pdata = *(PadPluginFreezeData*)(data->data);


		// Tales of the Abyss - pad fix
		// - PCSX2 only saves port0 (save #1), then port1 (save #2)

		memset(&pdata, 0, sizeof(pdata));
		strcpy(pdata.format, "PadMode");
		pdata.version = PAD_SAVE_STATE_VERSION;
		pdata.port = 0;
		pdata.query = query;

		for (int port = 0; port < 2; port++) {
			for (int slot = 0; slot < 4; slot++) {
				pdata.padData[port][slot] = pads[port][slot];
			}

			pdata.slot[port] = slots[port];
		}
	}
	else return -1;
	return 0;
}

u32 CALLBACK PADreadPort1(RPPadDataS* RPpad) {

	if (openCount)
	{
		UpdateRP(0, slots[0], RPpad);
		return 0;
	}
	return 1;
}

u32 CALLBACK PADreadPort2(RPPadDataS* RPpad) {
	PADstartPoll(2);
	PADpoll(0x42);
	memcpy(pads, query.response + 1, 7);
	memset(pads + 7, 0, sizeof(PadDataS) - 7);
	return 0;
}

u32 CALLBACK PSEgetLibType() {
	return 8;
}

u32 CALLBACK PSEgetLibVersion() {
	return (VERSION & 0xFFFFFF);
}

s32 CALLBACK PADqueryMtap(u8 port) {
	port--;
	if (port > 1) return 0;
	return config.multitap[port];
}

s32 CALLBACK PADsetSlot(u8 port, u8 slot) {
	port--;
	slot--;
	if (port > 1 || slot > 3) {
		return 0;
	}
	// Even if no pad there, record the slot, as it is the active slot regardless.
	slots[port] = slot;
	// First slot always allowed.
	// return pads[port][slot].enabled | !slot;
	return 1;
}

// Little funkiness to handle rounding floating points to ints without the C runtime.
// Unfortunately, means I can't use /GL optimization option when NO_CRT is defined.
#ifdef NO_CRT
extern "C" long _cdecl _ftol();
extern "C" long _cdecl _ftol2_sse() {
	return _ftol();
}
extern "C" long _cdecl _ftol2() {
	return _ftol();
}
#endif

