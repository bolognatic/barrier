#include "CMSWindowsPrimaryScreen.h"
#include "CMSWindowsClipboard.h"
#include "CServer.h"
#include "CSynergyHook.h"
#include "XSynergy.h"
#include "CThread.h"
#include "CLog.h"
#include <assert.h>
#include <string.h>

//
// CMSWindowsPrimaryScreen
//

CMSWindowsPrimaryScreen::CMSWindowsPrimaryScreen() :
								m_server(NULL),
								m_active(false),
								m_window(NULL),
								m_nextClipboardWindow(NULL),
								m_clipboardOwner(NULL),
								m_hookLibrary(NULL),
								m_mark(0),
								m_markReceived(0)
{
	// do nothing
}

CMSWindowsPrimaryScreen::~CMSWindowsPrimaryScreen()
{
	assert(m_window      == NULL);
	assert(m_hookLibrary == NULL);
}

static CString s_log;
static CString s_logMore;
static HWND s_debug = NULL;
static HWND s_debugLog = NULL;
static DWORD s_thread = 0;
static BOOL CALLBACK WINAPI debugProc(HWND, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_INITDIALOG:
		return TRUE;

	case WM_CLOSE:
		PostQuitMessage(0);
		return TRUE;

	case WM_APP:
		if (!s_logMore.empty()) {
			if (s_log.size() > 20000)
				s_log = s_logMore;
			else
				s_log += s_logMore;
			s_logMore = "";
			SendMessage(s_debugLog, WM_SETTEXT, FALSE, (LPARAM)(LPCTSTR)s_log.c_str());
			SendMessage(s_debugLog, EM_SETSEL, s_log.size(), s_log.size());
			SendMessage(s_debugLog, EM_SCROLLCARET, 0, 0);
		}
		return TRUE;
	}
	return FALSE;
}
static void debugOutput(const char* msg)
{
	s_logMore += msg;
	PostMessage(s_debug, WM_APP, 0, 0);
}

void					CMSWindowsPrimaryScreen::run()
{
CLog::setOutputter(&debugOutput);
	doRun();
CLog::setOutputter(NULL);
}

void					CMSWindowsPrimaryScreen::stop()
{
	doStop();
}

void					CMSWindowsPrimaryScreen::open(CServer* server)
{
	assert(m_server == NULL);
	assert(server   != NULL);

	// set the server
	m_server = server;

	// get keyboard state
	updateKeys();

	// open the display
	openDisplay();

	// enter the screen
	doEnter();
}

void					CMSWindowsPrimaryScreen::close()
{
	assert(m_server != NULL);

	// close the display
	closeDisplay();

	// done with server
	m_server = NULL;
}

void					CMSWindowsPrimaryScreen::enter(SInt32 x, SInt32 y)
{
	log((CLOG_INFO "entering primary at %d,%d", x, y));
	assert(m_active == true);

	// do non-warp enter stuff
	doEnter();

	// warp to requested location
	warpCursor(x, y);
}

void					CMSWindowsPrimaryScreen::doEnter()
{
	// release the capture
	ReleaseCapture();

	// hide our window and restore the foreground window
	SetForegroundWindow(m_lastActive);
	ShowWindow(m_window, SW_HIDE);

	// set the zones that should cause a jump
	SInt32 w, h;
	getScreenSize(&w, &h);
	SetZoneFunc setZone = (SetZoneFunc)GetProcAddress(
											m_hookLibrary, "setZone");
	setZone(m_server->getActivePrimarySides(), w, h, getJumpZoneSize());

	// all messages prior to now are invalid
	nextMark();

	// not active anymore
	m_active = false;
}

void					CMSWindowsPrimaryScreen::leave()
{
	log((CLOG_INFO "leaving primary"));
	assert(m_active == false);

	// all messages prior to now are invalid
	nextMark();

	// remember the active window before we leave
	m_lastActive = GetForegroundWindow();

	// show our window and put it in the foreground
	ShowWindow(m_window, SW_SHOW);
	SetForegroundWindow(m_window);

	// capture the cursor so we don't lose keyboard input
	SetCapture(m_window);

	// relay all mouse and keyboard events
	SetRelayFunc setRelay = (SetRelayFunc)GetProcAddress(
											m_hookLibrary, "setRelay");
	setRelay();

	// warp the mouse to the center of the screen
	SInt32 w, h;
	getScreenSize(&w, &h);
	warpCursor(w >> 1, h >> 1);

	// warp is also invalid
	nextMark();

	// local client now active
	m_active = true;

	// if we think we own the clipboard but we don't then somebody
	// grabbed the clipboard on this screen without us knowing.
	// tell the server that this screen grabbed the clipboard.
	//
	// this works around bugs in the clipboard viewer chain.
	// sometimes NT will simply never send WM_DRAWCLIPBOARD
	// messages for no apparent reason and rebooting fixes the
	// problem.  since we don't want a broken clipboard until the
	// next reboot we do this double check.  clipboard ownership
	// won't be reflected on other screens until we leave but at
	// least the clipboard itself will work.
	HWND clipboardOwner = GetClipboardOwner();
	if (m_clipboardOwner != clipboardOwner) {
		try {
			m_clipboardOwner = clipboardOwner;
			if (m_clipboardOwner != m_window) {
				m_server->grabClipboard(kClipboardClipboard);
				m_server->grabClipboard(kClipboardSelection);
			}
		}
		catch (XBadClient&) {
			// ignore
		}
	}
}

void					CMSWindowsPrimaryScreen::warpCursor(SInt32 x, SInt32 y)
{
	SInt32 w, h;
	getScreenSize(&w, &h);
	mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE,
								(DWORD)((65535.99 * x) / (w - 1)),
								(DWORD)((65535.99 * y) / (h - 1)),
								0, 0);
}

void					CMSWindowsPrimaryScreen::setClipboard(
								ClipboardID id, const IClipboard* src)
{
	assert(m_window != NULL);

	CMSWindowsClipboard dst(m_window);
	CClipboard::copy(&dst, src);
}

void					CMSWindowsPrimaryScreen::grabClipboard(ClipboardID id)
{
	assert(m_window != NULL);

	CMSWindowsClipboard clipboard(m_window);
	if (clipboard.open(0)) {
		clipboard.close();
	}
}

void					CMSWindowsPrimaryScreen::getSize(
								SInt32* width, SInt32* height) const
{
	getScreenSize(width, height);
}

SInt32					CMSWindowsPrimaryScreen::getJumpZoneSize() const
{
	return 1;
}

void					CMSWindowsPrimaryScreen::getClipboard(
								ClipboardID id, IClipboard* dst) const
{
	assert(m_window != NULL);

	CMSWindowsClipboard src(m_window);
	CClipboard::copy(dst, &src);
}

KeyModifierMask			CMSWindowsPrimaryScreen::getToggleMask() const
{
	KeyModifierMask mask;
	if ((m_keys[VK_CAPITAL] & 0x01) != 0)
		mask |= KeyModifierCapsLock;
	if ((m_keys[VK_NUMLOCK] & 0x01) != 0)
		mask |= KeyModifierNumLock;
	if ((m_keys[VK_SCROLL] & 0x01) != 0)
		mask |= KeyModifierScrollLock;
	return mask;
}

#include "resource.h" // FIXME

void					CMSWindowsPrimaryScreen::onOpenDisplay()
{
	assert(m_window == NULL);
	assert(m_server != NULL);

// create debug dialog
s_thread = GetCurrentThreadId();;
s_debug = CreateDialog(getInstance(), MAKEINTRESOURCE(IDD_SYNERGY), NULL, &debugProc);
s_debugLog = ::GetDlgItem(s_debug, IDC_LOG);
CLog::setOutputter(&debugOutput);
ShowWindow(s_debug, SW_SHOWNORMAL);

	// initialize clipboard owner to current owner.  we don't want
	// to take ownership of the clipboard just by starting up.
	m_clipboardOwner = GetClipboardOwner();

	// create the window
	m_window = CreateWindowEx(WS_EX_TOPMOST |
								WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
								(LPCTSTR)getClass(), "Synergy",
								WS_POPUP,
								0, 0, 1, 1, NULL, NULL,
								getInstance(),
								NULL);
	assert(m_window != NULL);

	// install our clipboard snooper
	m_nextClipboardWindow = SetClipboardViewer(m_window);

	// load the hook library
	bool hooked = false;
	m_hookLibrary = LoadLibrary("synrgyhk");
	if (m_hookLibrary != NULL) {
		// install input hooks
		InstallFunc install = (InstallFunc)GetProcAddress(
											m_hookLibrary, "install"); 
		if (install != NULL) {
			hooked = (install(m_window) != 0);
		}
	}
	if (!hooked) {
		ChangeClipboardChain(m_window, m_nextClipboardWindow);
		m_nextClipboardWindow = NULL;
		DestroyWindow(m_window);
		m_window = NULL;
		// FIXME -- throw
	}

	// initialize marks
	m_mark         = 0;
	m_markReceived = 0;
	nextMark();
}

void					CMSWindowsPrimaryScreen::onCloseDisplay()
{
	assert(m_window != NULL);

	// uninstall input hooks
	UninstallFunc uninstall = (UninstallFunc)GetProcAddress(
											m_hookLibrary, "uninstall"); 
	if (uninstall != NULL) {
		uninstall();
	}

	// done with hook library
	FreeLibrary(m_hookLibrary);
	m_hookLibrary = NULL;

	// remove clipboard snooper
	ChangeClipboardChain(m_window, m_nextClipboardWindow);
	m_nextClipboardWindow = NULL;

	// destroy window
	DestroyWindow(m_window);
	m_window = NULL;

CLog::setOutputter(NULL);
DestroyWindow(s_debug);
s_debug = NULL;
s_thread = 0;
}

bool					CMSWindowsPrimaryScreen::onPreTranslate(MSG* msg)
{
if (IsDialogMessage(s_debug, msg)) {
	return true;
}

	// handle event
	switch (msg->message) {
	case SYNERGY_MSG_MARK:
		m_markReceived = msg->wParam;
		return true;

	case SYNERGY_MSG_KEY:
		// ignore if not at current mark
		if (m_mark == m_markReceived) {
			KeyModifierMask mask;
			const KeyID key = mapKey(msg->wParam, msg->lParam, &mask);
			if (key != kKeyNone) {
				if ((msg->lParam & 0x80000000) == 0) {
					// key press
					const SInt32 repeat = (SInt32)(msg->lParam & 0xffff);
					if (repeat >= 2) {
						log((CLOG_DEBUG1 "event: key repeat key=%d mask=0x%04x count=%d", key, mask, repeat));
						m_server->onKeyRepeat(key, mask, repeat);
					}
					else {
						log((CLOG_DEBUG1 "event: key press key=%d mask=0x%04x", key, mask));
						m_server->onKeyDown(key, mask);
					}

					// update key state
					updateKey(msg->wParam, true);
				}
				else {
					// key release
					log((CLOG_DEBUG1 "event: key release key=%d mask=0x%04x", key, mask));
					m_server->onKeyUp(key, mask);

					// update key state
					updateKey(msg->wParam, false);
				}
			}

		}
		return true;

	case SYNERGY_MSG_MOUSE_BUTTON:
		// ignore if not at current mark
		if (m_mark == m_markReceived) {
			const ButtonID button = mapButton(msg->wParam);
			switch (msg->wParam) {
			case WM_LBUTTONDOWN:
			case WM_MBUTTONDOWN:
			case WM_RBUTTONDOWN:
				log((CLOG_DEBUG1 "event: button press button=%d", button));
				if (button != kButtonNone) {
					m_server->onMouseDown(button);
				}
				break;

			case WM_LBUTTONUP:
			case WM_MBUTTONUP:
			case WM_RBUTTONUP:
				log((CLOG_DEBUG1 "event: button release button=%d", button));
				if (button != kButtonNone) {
					m_server->onMouseUp(button);
				}
				break;
			}
		}
		return true;

	case SYNERGY_MSG_MOUSE_MOVE:
		// ignore if not at current mark
		if (m_mark == m_markReceived) {
			SInt32 x = (SInt32)msg->wParam;
			SInt32 y = (SInt32)msg->lParam;
			if (!m_active) {
				log((CLOG_DEBUG2 "event: inactive move %d,%d", x, y));
				m_server->onMouseMovePrimary(x, y);
			}
			else {
				log((CLOG_DEBUG2 "event: active move %d,%d", x, y));

				// get screen size
				SInt32 w, h;
				getScreenSize(&w, &h);

				// get center pixel
				w >>= 1;
				h >>= 1;

				// ignore and discard message if motion is to center of
				// screen.  those are caused by us warping the mouse.
				if (x != w || y != h) {
					// get mouse deltas
					x -= w;
					y -= h;

					// warp mouse back to center
					warpCursor(w, h);

					// send motion
					m_server->onMouseMoveSecondary(x, y);
				}
			}
		}
		return true;
	}

	return false;
}

LRESULT					CMSWindowsPrimaryScreen::onEvent(
								HWND hwnd, UINT msg,
								WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	// FIXME -- handle display changes
	case WM_PAINT:
		ValidateRect(hwnd, NULL);
		return 0;

	case WM_DRAWCLIPBOARD:
		log((CLOG_DEBUG "clipboard was taken"));

		// first pass it on
		SendMessage(m_nextClipboardWindow, msg, wParam, lParam);

		// now notify server that somebody changed the clipboard.
		// skip that if we're the new owner.
		try {
			m_clipboardOwner = GetClipboardOwner();
			if (m_clipboardOwner != m_window) {
				m_server->grabClipboard(kClipboardClipboard);
				m_server->grabClipboard(kClipboardSelection);
			}
		}
		catch (XBadClient&) {
			// ignore.  this can happen if we receive this event
			// before we've fully started up.
		}
		return 0;

	case WM_CHANGECBCHAIN:
		if (m_nextClipboardWindow == (HWND)wParam)
			m_nextClipboardWindow = (HWND)lParam;
		else
			SendMessage(m_nextClipboardWindow, msg, wParam, lParam);
		return 0;
	}
	return DefWindowProc(hwnd, msg, wParam, lParam);
}

void					CMSWindowsPrimaryScreen::nextMark()
{
	assert(m_window != NULL);

	PostMessage(m_window, SYNERGY_MSG_MARK, ++m_mark, 0);
}

static const KeyID		g_virtualKey[] =
{
	/* 0x00 */ kKeyNone,	// reserved
	/* 0x01 */ kKeyNone,	// VK_LBUTTON
	/* 0x02 */ kKeyNone,	// VK_RBUTTON
	/* 0x03 */ 0xff6b,		// VK_CANCEL		XK_Break
	/* 0x04 */ kKeyNone,	// VK_MBUTTON
	/* 0x05 */ kKeyNone,	// undefined
	/* 0x06 */ kKeyNone,	// undefined
	/* 0x07 */ kKeyNone,	// undefined
	/* 0x08 */ 0xff08,		// VK_BACK			XK_Backspace
	/* 0x09 */ 0xff09,		// VK_TAB			VK_Tab
	/* 0x0a */ kKeyNone,	// undefined
	/* 0x0b */ kKeyNone,	// undefined
	/* 0x0c */ 0xff0b,		// VK_CLEAR			XK_Clear
	/* 0x0d */ 0xff0d,		// VK_RETURN		XK_Return
	/* 0x0e */ kKeyNone,	// undefined
	/* 0x0f */ kKeyNone,	// undefined
	/* 0x10 */ 0xffe1,		// VK_SHIFT			XK_Shift_L
	/* 0x11 */ 0xffe3,		// VK_CONTROL		XK_Control_L
	/* 0x12 */ 0xffe9,		// VK_MENU			XK_Alt_L
	/* 0x13 */ 0xff13,		// VK_PAUSE			XK_Pause
	/* 0x14 */ 0xffe5,		// VK_CAPITAL		XK_Caps_Lock
	/* 0x15 */ kKeyNone,	// VK_KANA			
	/* 0x16 */ kKeyNone,	// VK_HANGUL		
	/* 0x17 */ kKeyNone,	// VK_JUNJA			
	/* 0x18 */ kKeyNone,	// VK_FINAL			
	/* 0x19 */ kKeyNone,	// VK_KANJI			
	/* 0x1a */ kKeyNone,	// undefined
	/* 0x1b */ 0xff1b,		// VK_ESCAPE		XK_Escape
	/* 0x1c */ kKeyNone,	// VK_CONVERT		
	/* 0x1d */ kKeyNone,	// VK_NONCONVERT	
	/* 0x1e */ kKeyNone,	// VK_ACCEPT		
	/* 0x1f */ kKeyNone,	// VK_MODECHANGE	
	/* 0x20 */ 0x0020,		// VK_SPACE			XK_space
	/* 0x21 */ 0xff55,		// VK_PRIOR			XK_Prior
	/* 0x22 */ 0xff56,		// VK_NEXT			XK_Next
	/* 0x23 */ 0xff57,		// VK_END			XK_End
	/* 0x24 */ 0xff50,		// VK_HOME			XK_Home
	/* 0x25 */ 0xff51,		// VK_LEFT			XK_Left
	/* 0x26 */ 0xff52,		// VK_UP			XK_Up
	/* 0x27 */ 0xff53,		// VK_RIGHT			XK_Right
	/* 0x28 */ 0xff54,		// VK_DOWN			XK_Down
	/* 0x29 */ 0xff60,		// VK_SELECT		XK_Select
	/* 0x2a */ kKeyNone,	// VK_PRINT			
	/* 0x2b */ 0xff62,		// VK_EXECUTE		XK_Execute
	/* 0x2c */ 0xff61,		// VK_SNAPSHOT		XK_Print
	/* 0x2d */ 0xff63,		// VK_INSERT		XK_Insert
	/* 0x2e */ 0xffff,		// VK_DELETE		XK_Delete
	/* 0x2f */ 0xff6a,		// VK_HELP			XK_Help
	/* 0x30 */ kKeyNone,	// VK_0				XK_0
	/* 0x31 */ kKeyNone,	// VK_1				XK_1
	/* 0x32 */ kKeyNone,	// VK_2				XK_2
	/* 0x33 */ kKeyNone,	// VK_3				XK_3
	/* 0x34 */ kKeyNone,	// VK_4				XK_4
	/* 0x35 */ kKeyNone,	// VK_5				XK_5
	/* 0x36 */ kKeyNone,	// VK_6				XK_6
	/* 0x37 */ kKeyNone,	// VK_7				XK_7
	/* 0x38 */ kKeyNone,	// VK_8				XK_8
	/* 0x39 */ kKeyNone,	// VK_9				XK_9
	/* 0x3a */ kKeyNone,	// undefined
	/* 0x3b */ kKeyNone,	// undefined
	/* 0x3c */ kKeyNone,	// undefined
	/* 0x3d */ kKeyNone,	// undefined
	/* 0x3e */ kKeyNone,	// undefined
	/* 0x3f */ kKeyNone,	// undefined
	/* 0x40 */ kKeyNone,	// undefined
	/* 0x41 */ kKeyNone,	// VK_A				XK_A
	/* 0x42 */ kKeyNone,	// VK_B				XK_B
	/* 0x43 */ kKeyNone,	// VK_C				XK_C
	/* 0x44 */ kKeyNone,	// VK_D				XK_D
	/* 0x45 */ kKeyNone,	// VK_E				XK_E
	/* 0x46 */ kKeyNone,	// VK_F				XK_F
	/* 0x47 */ kKeyNone,	// VK_G				XK_G
	/* 0x48 */ kKeyNone,	// VK_H				XK_H
	/* 0x49 */ kKeyNone,	// VK_I				XK_I
	/* 0x4a */ kKeyNone,	// VK_J				XK_J
	/* 0x4b */ kKeyNone,	// VK_K				XK_K
	/* 0x4c */ kKeyNone,	// VK_L				XK_L
	/* 0x4d */ kKeyNone,	// VK_M				XK_M
	/* 0x4e */ kKeyNone,	// VK_N				XK_N
	/* 0x4f */ kKeyNone,	// VK_O				XK_O
	/* 0x50 */ kKeyNone,	// VK_P				XK_P
	/* 0x51 */ kKeyNone,	// VK_Q				XK_Q
	/* 0x52 */ kKeyNone,	// VK_R				XK_R
	/* 0x53 */ kKeyNone,	// VK_S				XK_S
	/* 0x54 */ kKeyNone,	// VK_T				XK_T
	/* 0x55 */ kKeyNone,	// VK_U				XK_U
	/* 0x56 */ kKeyNone,	// VK_V				XK_V
	/* 0x57 */ kKeyNone,	// VK_W				XK_W
	/* 0x58 */ kKeyNone,	// VK_X				XK_X
	/* 0x59 */ kKeyNone,	// VK_Y				XK_Y
	/* 0x5a */ kKeyNone,	// VK_Z				XK_Z
	/* 0x5b */ 0xffe7,		// VK_LWIN			XK_Meta_L
	/* 0x5c */ 0xffe8,		// VK_RWIN			XK_Meta_R
	/* 0x5d */ 0xff67,		// VK_APPS			XK_Menu
	/* 0x5e */ kKeyNone,	// undefined
	/* 0x5f */ kKeyNone,	// undefined
	/* 0x60 */ 0xffb0,		// VK_NUMPAD0		XK_KP_0
	/* 0x61 */ 0xffb1,		// VK_NUMPAD1		XK_KP_1
	/* 0x62 */ 0xffb2,		// VK_NUMPAD2		XK_KP_2
	/* 0x63 */ 0xffb3,		// VK_NUMPAD3		XK_KP_3
	/* 0x64 */ 0xffb4,		// VK_NUMPAD4		XK_KP_4
	/* 0x65 */ 0xffb5,		// VK_NUMPAD5		XK_KP_5
	/* 0x66 */ 0xffb6,		// VK_NUMPAD6		XK_KP_6
	/* 0x67 */ 0xffb7,		// VK_NUMPAD7		XK_KP_7
	/* 0x68 */ 0xffb8,		// VK_NUMPAD8		XK_KP_8
	/* 0x69 */ 0xffb9,		// VK_NUMPAD9		XK_KP_9
	/* 0x6a */ 0xffaa,		// VK_MULTIPLY		XK_KP_Multiply
	/* 0x6b */ 0xffab,		// VK_ADD			XK_KP_Add
	/* 0x6c */ 0xffac,		// VK_SEPARATOR		XK_KP_Separator
	/* 0x6d */ 0xffad,		// VK_SUBTRACT		XK_KP_Subtract
	/* 0x6e */ 0xffae,		// VK_DECIMAL		XK_KP_Decimal
	/* 0x6f */ 0xffaf,		// VK_DIVIDE		XK_KP_Divide
	/* 0x70 */ 0xffbe,		// VK_F1			XK_F1
	/* 0x71 */ 0xffbf,		// VK_F2			XK_F2
	/* 0x72 */ 0xffc0,		// VK_F3			XK_F3
	/* 0x73 */ 0xffc1,		// VK_F4			XK_F4
	/* 0x74 */ 0xffc2,		// VK_F5			XK_F5
	/* 0x75 */ 0xffc3,		// VK_F6			XK_F6
	/* 0x76 */ 0xffc4,		// VK_F7			XK_F7
	/* 0x77 */ 0xffc5,		// VK_F8			XK_F8
	/* 0x78 */ 0xffc6,		// VK_F9			XK_F9
	/* 0x79 */ 0xffc7,		// VK_F10			XK_F10
	/* 0x7a */ 0xffc8,		// VK_F11			XK_F11
	/* 0x7b */ 0xffc9,		// VK_F12			XK_F12
	/* 0x7c */ 0xffca,		// VK_F13			XK_F13
	/* 0x7d */ 0xffcb,		// VK_F14			XK_F14
	/* 0x7e */ 0xffcc,		// VK_F15			XK_F15
	/* 0x7f */ 0xffcd,		// VK_F16			XK_F16
	/* 0x80 */ 0xffce,		// VK_F17			XK_F17
	/* 0x81 */ 0xffcf,		// VK_F18			XK_F18
	/* 0x82 */ 0xffd0,		// VK_F19			XK_F19
	/* 0x83 */ 0xffd1,		// VK_F20			XK_F20
	/* 0x84 */ 0xffd2,		// VK_F21			XK_F21
	/* 0x85 */ 0xffd3,		// VK_F22			XK_F22
	/* 0x86 */ 0xffd4,		// VK_F23			XK_F23
	/* 0x87 */ 0xffd5,		// VK_F24			XK_F24
	/* 0x88 */ kKeyNone,	// unassigned
	/* 0x89 */ kKeyNone,	// unassigned
	/* 0x8a */ kKeyNone,	// unassigned
	/* 0x8b */ kKeyNone,	// unassigned
	/* 0x8c */ kKeyNone,	// unassigned
	/* 0x8d */ kKeyNone,	// unassigned
	/* 0x8e */ kKeyNone,	// unassigned
	/* 0x8f */ kKeyNone,	// unassigned
	/* 0x90 */ 0xff7f,		// VK_NUMLOCK		XK_Num_Lock
	/* 0x91 */ 0xff14,		// VK_SCROLL		XK_Scroll_Lock
	/* 0x92 */ kKeyNone,	// unassigned
	/* 0x93 */ kKeyNone,	// unassigned
	/* 0x94 */ kKeyNone,	// unassigned
	/* 0x95 */ kKeyNone,	// unassigned
	/* 0x96 */ kKeyNone,	// unassigned
	/* 0x97 */ kKeyNone,	// unassigned
	/* 0x98 */ kKeyNone,	// unassigned
	/* 0x99 */ kKeyNone,	// unassigned
	/* 0x9a */ kKeyNone,	// unassigned
	/* 0x9b */ kKeyNone,	// unassigned
	/* 0x9c */ kKeyNone,	// unassigned
	/* 0x9d */ kKeyNone,	// unassigned
	/* 0x9e */ kKeyNone,	// unassigned
	/* 0x9f */ kKeyNone,	// unassigned
	/* 0xa0 */ 0xffe1,		// VK_LSHIFT		XK_Shift_L
	/* 0xa1 */ 0xffe2,		// VK_RSHIFT		XK_Shift_R
	/* 0xa2 */ 0xffe3,		// VK_LCONTROL		XK_Control_L
	/* 0xa3 */ 0xffe4,		// VK_RCONTROL		XK_Control_R
	/* 0xa4 */ 0xffe9,		// VK_LMENU			XK_Alt_L
	/* 0xa5 */ 0xffea,		// VK_RMENU			XK_Alt_R
	/* 0xa6 */ kKeyNone,	// unassigned
	/* 0xa7 */ kKeyNone,	// unassigned
	/* 0xa8 */ kKeyNone,	// unassigned
	/* 0xa9 */ kKeyNone,	// unassigned
	/* 0xaa */ kKeyNone,	// unassigned
	/* 0xab */ kKeyNone,	// unassigned
	/* 0xac */ kKeyNone,	// unassigned
	/* 0xad */ kKeyNone,	// unassigned
	/* 0xae */ kKeyNone,	// unassigned
	/* 0xaf */ kKeyNone,	// unassigned
	/* 0xb0 */ kKeyNone,	// unassigned
	/* 0xb1 */ kKeyNone,	// unassigned
	/* 0xb2 */ kKeyNone,	// unassigned
	/* 0xb3 */ kKeyNone,	// unassigned
	/* 0xb4 */ kKeyNone,	// unassigned
	/* 0xb5 */ kKeyNone,	// unassigned
	/* 0xb6 */ kKeyNone,	// unassigned
	/* 0xb7 */ kKeyNone,	// unassigned
	/* 0xb8 */ kKeyNone,	// unassigned
	/* 0xb9 */ kKeyNone,	// unassigned
	/* 0xba */ kKeyNone,	// OEM specific
	/* 0xbb */ kKeyNone,	// OEM specific
	/* 0xbc */ kKeyNone,	// OEM specific
	/* 0xbd */ kKeyNone,	// OEM specific
	/* 0xbe */ kKeyNone,	// OEM specific
	/* 0xbf */ kKeyNone,	// OEM specific
	/* 0xc0 */ kKeyNone,	// OEM specific
	/* 0xc1 */ kKeyNone,	// unassigned
	/* 0xc2 */ kKeyNone,	// unassigned
	/* 0xc3 */ kKeyNone,	// unassigned
	/* 0xc4 */ kKeyNone,	// unassigned
	/* 0xc5 */ kKeyNone,	// unassigned
	/* 0xc6 */ kKeyNone,	// unassigned
	/* 0xc7 */ kKeyNone,	// unassigned
	/* 0xc8 */ kKeyNone,	// unassigned
	/* 0xc9 */ kKeyNone,	// unassigned
	/* 0xca */ kKeyNone,	// unassigned
	/* 0xcb */ kKeyNone,	// unassigned
	/* 0xcc */ kKeyNone,	// unassigned
	/* 0xcd */ kKeyNone,	// unassigned
	/* 0xce */ kKeyNone,	// unassigned
	/* 0xcf */ kKeyNone,	// unassigned
	/* 0xd0 */ kKeyNone,	// unassigned
	/* 0xd1 */ kKeyNone,	// unassigned
	/* 0xd2 */ kKeyNone,	// unassigned
	/* 0xd3 */ kKeyNone,	// unassigned
	/* 0xd4 */ kKeyNone,	// unassigned
	/* 0xd5 */ kKeyNone,	// unassigned
	/* 0xd6 */ kKeyNone,	// unassigned
	/* 0xd7 */ kKeyNone,	// unassigned
	/* 0xd8 */ kKeyNone,	// unassigned
	/* 0xd9 */ kKeyNone,	// unassigned
	/* 0xda */ kKeyNone,	// unassigned
	/* 0xdb */ kKeyNone,	// OEM specific
	/* 0xdc */ kKeyNone,	// OEM specific
	/* 0xdd */ kKeyNone,	// OEM specific
	/* 0xde */ kKeyNone,	// OEM specific
	/* 0xdf */ kKeyNone,	// OEM specific
	/* 0xe0 */ kKeyNone,	// OEM specific
	/* 0xe1 */ kKeyNone,	// OEM specific
	/* 0xe2 */ kKeyNone,	// OEM specific
	/* 0xe3 */ kKeyNone,	// OEM specific
	/* 0xe4 */ kKeyNone,	// OEM specific
	/* 0xe5 */ kKeyNone,	// unassigned
	/* 0xe6 */ kKeyNone,	// OEM specific
	/* 0xe7 */ kKeyNone,	// unassigned
	/* 0xe8 */ kKeyNone,	// unassigned
	/* 0xe9 */ kKeyNone,	// OEM specific
	/* 0xea */ kKeyNone,	// OEM specific
	/* 0xeb */ kKeyNone,	// OEM specific
	/* 0xec */ kKeyNone,	// OEM specific
	/* 0xed */ kKeyNone,	// OEM specific
	/* 0xee */ kKeyNone,	// OEM specific
	/* 0xef */ kKeyNone,	// OEM specific
	/* 0xf0 */ kKeyNone,	// OEM specific
	/* 0xf1 */ kKeyNone,	// OEM specific
	/* 0xf2 */ kKeyNone,	// OEM specific
	/* 0xf3 */ kKeyNone,	// OEM specific
	/* 0xf4 */ kKeyNone,	// OEM specific
	/* 0xf5 */ kKeyNone,	// OEM specific
	/* 0xf6 */ kKeyNone,	// VK_ATTN			
	/* 0xf7 */ kKeyNone,	// VK_CRSEL			
	/* 0xf8 */ kKeyNone,	// VK_EXSEL			
	/* 0xf9 */ kKeyNone,	// VK_EREOF			
	/* 0xfa */ kKeyNone,	// VK_PLAY			
	/* 0xfb */ kKeyNone,	// VK_ZOOM			
	/* 0xfc */ kKeyNone,	// reserved
	/* 0xfd */ kKeyNone,	// VK_PA1			
	/* 0xfe */ kKeyNone,	// VK_OEM_CLEAR		
	/* 0xff */ kKeyNone		// reserved
};

KeyID					CMSWindowsPrimaryScreen::mapKey(
								WPARAM vkCode, LPARAM info,
								KeyModifierMask* maskOut)
{
	// note:  known microsoft bugs
	//  Q72583 -- MapVirtualKey() maps keypad keys incorrectly
	//    95,98: num pad vk code -> invalid scan code
	//    95,98,NT4: num pad scan code -> bad vk code except
	//      SEPARATOR, MULTIPLY, SUBTRACT, ADD

	static const KeyID XK_Multi_key = 0xff20;

	assert(maskOut != NULL);

	// map modifier key
	KeyModifierMask mask = 0;
	if (((m_keys[VK_LSHIFT] |
		  m_keys[VK_RSHIFT] |
		  m_keys[VK_SHIFT]) & 0x80) != 0)
		mask |= KeyModifierShift;
	if (((m_keys[VK_LCONTROL] |
		  m_keys[VK_RCONTROL] |
		  m_keys[VK_CONTROL]) & 0x80) != 0)
		mask |= KeyModifierControl;
	if (((m_keys[VK_LMENU] |
		  m_keys[VK_RMENU] |
		  m_keys[VK_MENU]) & 0x80) != 0)
		mask |= KeyModifierAlt;
	if (((m_keys[VK_LWIN] |
		  m_keys[VK_RWIN]) & 0x80) != 0)
		mask |= KeyModifierMeta;
	if ((m_keys[VK_CAPITAL] & 0x01) != 0)
		mask |= KeyModifierCapsLock;
	if ((m_keys[VK_NUMLOCK] & 0x01) != 0)
		mask |= KeyModifierNumLock;
	if ((m_keys[VK_SCROLL] & 0x01) != 0)
		mask |= KeyModifierScrollLock;
	*maskOut = mask;

	// get the scan code
	UINT scanCode = static_cast<UINT>((info & 0xff0000) >> 16);

	// convert virtual key to one that distinguishes between left and
	// right for keys that have left/right versions.  known scan codes
	// that don't have left/right versions are passed through unchanged.
	// unknown scan codes return 0.
	UINT vkCode2 = MapVirtualKey(scanCode, 3);

	// work around bug Q72583 (bad num pad conversion in MapVirtualKey())
	if (vkCode >= VK_NUMPAD0 && vkCode <= VK_DIVIDE)
		vkCode2 = vkCode;

	// MapVirtualKey() appears to map VK_LWIN, VK_RWIN, VK_APPS to
	// some other meaningless virtual key.  work around that bug.
	else if (vkCode >= VK_LWIN && vkCode <= VK_APPS)
		vkCode2 = vkCode;

	// sadly, win32 will not distinguish between the left and right
	// control and alt keys using the above function.  however, we
	// can check for those:  if bit 24 of info is set then the key
	// is a "extended" key, such as the right control and right alt
	// keys.
	if ((info & 0x1000000) != 0) {
		switch (vkCode2) {
		case VK_LCONTROL:
			vkCode2 = VK_RCONTROL;
			break;

		case VK_LMENU:
			vkCode2 = VK_RMENU;
			break;
		}
	}

	// use left/right distinguishing virtual key
	vkCode = vkCode2;
	log((CLOG_DEBUG1 "key vk=%d scan=%d", vkCode, scanCode));

	// handle some keys via table lookup
	KeyID id = g_virtualKey[vkCode];
	if (id != kKeyNone) {
		return id;
	}

	// check for dead keys
	if (MapVirtualKey(vkCode, 2) >= 0x8000) {
		return XK_Multi_key;
	}

	// ToAscii() maps ctrl+letter to the corresponding control code
	// and ctrl+backspace to delete.  if we've got a control code or
	// delete then do ToAscii() again but without the control state.
	// ToAscii() interprets the control modifier state which we don't
	// want.  so save the control state then clear it.
	BYTE lControl       = m_keys[VK_LCONTROL];
	BYTE rControl       = m_keys[VK_RCONTROL];
	BYTE control        = m_keys[VK_CONTROL];
	m_keys[VK_LCONTROL] = 0;
	m_keys[VK_RCONTROL] = 0;
	m_keys[VK_CONTROL]  = 0;

	// convert to ascii
	WORD ascii;
	int result = ToAscii(vkCode, scanCode, m_keys, &ascii, 0);

	// restore control state
	m_keys[VK_LCONTROL] = lControl;
	m_keys[VK_RCONTROL] = rControl;
	m_keys[VK_CONTROL]  = control;

	// if result is less than zero then it was a dead key.  that key
	// is remembered by the keyboard which we don't want.  remove it
	// by calling ToAscii() again with arbitrary arguments.
	if (result < 0) {
		ToAscii(vkCode, scanCode, m_keys, &ascii, 0);
		return XK_Multi_key;
	}

	// if result is 1 then the key was succesfully converted
	else if (result == 1) {
		return static_cast<KeyID>(ascii & 0x00ff);
	}

	// if result is 2 then a previous dead key could not be composed.
	// put the old dead key back.
	else if (result == 2) {
		// get the scan code of the dead key and the shift state
		// required to generate it.
		vkCode = VkKeyScan(ascii & 0x00ff);

		// set shift state required to generate key
		BYTE keys[256];
		memset(keys, 0, sizeof(keys));
		if (vkCode & 0x0100)
			keys[VK_SHIFT] = 0x80;
		if (vkCode & 0x0100)
			keys[VK_CONTROL] = 0x80;
		if (vkCode & 0x0100)
			keys[VK_MENU] = 0x80;

		// strip shift state off of virtual key code
		vkCode &= 0x00ff;

		// get the scan code for the key
		scanCode = MapVirtualKey(vkCode, 0);

		// put it back
		ToAscii(vkCode, scanCode, keys, &ascii, 0);
		return XK_Multi_key;
	}

	// cannot convert key
	return kKeyNone;
}

ButtonID				CMSWindowsPrimaryScreen::mapButton(
								WPARAM button) const
{
	switch (button) {
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
		return kButtonLeft;

	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
		return kButtonMiddle;

	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
		return kButtonRight;

	default:
		return kButtonNone;
	}
}

void					CMSWindowsPrimaryScreen::updateKeys()
{
	// clear key state
	memset(m_keys, 0, sizeof(m_keys));

	// we only care about the modifier key states
	m_keys[VK_LSHIFT]   = GetKeyState(VK_LSHIFT);
	m_keys[VK_RSHIFT]   = GetKeyState(VK_RSHIFT);
	m_keys[VK_SHIFT]    = GetKeyState(VK_SHIFT);
	m_keys[VK_LCONTROL] = GetKeyState(VK_LCONTROL);
	m_keys[VK_RCONTROL] = GetKeyState(VK_RCONTROL);
	m_keys[VK_CONTROL]  = GetKeyState(VK_CONTROL);
	m_keys[VK_LMENU]    = GetKeyState(VK_LMENU);
	m_keys[VK_RMENU]    = GetKeyState(VK_RMENU);
	m_keys[VK_MENU]     = GetKeyState(VK_MENU);
	m_keys[VK_LWIN]     = GetKeyState(VK_LWIN);
	m_keys[VK_RWIN]     = GetKeyState(VK_RWIN);
	m_keys[VK_APPS]     = GetKeyState(VK_APPS);
	m_keys[VK_CAPITAL]  = GetKeyState(VK_CAPITAL);
	m_keys[VK_NUMLOCK]  = GetKeyState(VK_NUMLOCK);
	m_keys[VK_SCROLL]   = GetKeyState(VK_SCROLL);
}

void					CMSWindowsPrimaryScreen::updateKey(
								UINT vkCode, bool press)
{
	if (press) {
		switch (vkCode) {
		case VK_LSHIFT:
		case VK_RSHIFT:
		case VK_SHIFT:
			m_keys[vkCode]     |= 0x80;
			m_keys[VK_SHIFT]   |= 0x80;
			break;

		case VK_LCONTROL:
		case VK_RCONTROL:
		case VK_CONTROL:
			m_keys[vkCode]     |= 0x80;
			m_keys[VK_CONTROL] |= 0x80;
			break;

		case VK_LMENU:
		case VK_RMENU:
		case VK_MENU:
			m_keys[vkCode]     |= 0x80;
			m_keys[VK_MENU]    |= 0x80;
			break;

		case VK_LWIN:
		case VK_RWIN:
		case VK_APPS:
			m_keys[vkCode]     |= 0x80;
			break;

		case VK_CAPITAL:
		case VK_NUMLOCK:
		case VK_SCROLL:
			// toggle keys
			m_keys[vkCode]     |= 0x80;
			if ((m_keys[vkCode] & 0x01) == 0) {
				m_keys[vkCode] |= 0x01;
			}
			break;
		}
	}
	else {
		switch (vkCode) {
		case VK_LSHIFT:
		case VK_RSHIFT:
		case VK_SHIFT:
			m_keys[vkCode]     &= ~0x80;
			if (((m_keys[VK_LSHIFT] | m_keys[VK_RSHIFT]) & 0x80) == 0) {
				m_keys[VK_SHIFT] &= ~0x80;
			}
			break;

		case VK_LCONTROL:
		case VK_RCONTROL:
		case VK_CONTROL:
			m_keys[vkCode]     &= ~0x80;
			if (((m_keys[VK_LCONTROL] | m_keys[VK_RCONTROL]) & 0x80) == 0) {
				m_keys[VK_CONTROL] &= ~0x80;
			}
			break;

		case VK_LMENU:
		case VK_RMENU:
		case VK_MENU:
			m_keys[vkCode]     &= ~0x80;
			if (((m_keys[VK_LMENU] | m_keys[VK_RMENU]) & 0x80) == 0) {
				m_keys[VK_MENU] &= ~0x80;
			}
			break;

		case VK_LWIN:
		case VK_RWIN:
		case VK_APPS:
			m_keys[vkCode]     &= ~0x80;
			break;

		case VK_CAPITAL:
		case VK_NUMLOCK:
		case VK_SCROLL:
			// toggle keys
			m_keys[vkCode] &= ~0x80;
			if ((m_keys[vkCode] & 0x01) != 0) {
				m_keys[vkCode] &= ~0x01;
			}
			break;
		}
	}
}
