// USER internals shared with the display and MMSYSTEM layers.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "recomp/win16/gdi.hpp"

namespace win16 {

constexpr uint16_t WM_CREATE = 0x0001, WM_DESTROY = 0x0002, WM_MOVE = 0x0003, WM_SIZE = 0x0005,
                   WM_ACTIVATE = 0x0006, WM_SETFOCUS = 0x0007, WM_KILLFOCUS = 0x0008,
                   WM_ENABLE = 0x000A, WM_SETTEXT = 0x000C, WM_GETTEXT = 0x000D,
                   WM_GETTEXTLENGTH = 0x000E, WM_PAINT = 0x000F, WM_CLOSE = 0x0010,
                   WM_QUIT = 0x0012, WM_ERASEBKGND = 0x0014, WM_SHOWWINDOW = 0x0018,
                   WM_CTLCOLOR = 0x0019, WM_ACTIVATEAPP = 0x001C, WM_SETCURSOR = 0x0020,
                   WM_MOUSEACTIVATE = 0x0021, WM_GETMINMAXINFO = 0x0024, WM_DRAWITEM = 0x002B,
                   WM_MEASUREITEM = 0x002C, WM_SETFONT = 0x0030, WM_GETFONT = 0x0031,
                   WM_WINDOWPOSCHANGED = 0x0047, WM_NCCREATE = 0x0081, WM_NCDESTROY = 0x0082,
                   WM_NCHITTEST = 0x0084, WM_GETDLGCODE = 0x0087, WM_KEYDOWN = 0x0100,
                   WM_KEYUP = 0x0101, WM_CHAR = 0x0102, WM_SYSKEYDOWN = 0x0104,
                   WM_SYSKEYUP = 0x0105, WM_COMMAND = 0x0111, WM_SYSCOMMAND = 0x0112,
                   WM_TIMER = 0x0113, WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201,
                   WM_LBUTTONUP = 0x0202, WM_LBUTTONDBLCLK = 0x0203, WM_RBUTTONDOWN = 0x0204,
                   WM_RBUTTONUP = 0x0205, WM_PARENTNOTIFY = 0x0210, MM_MCINOTIFY = 0x03B9;

// Send a message to a window (calls its window procedure synchronously).
uint32_t send_message(uint16_t hwnd, uint16_t msg, uint16_t wParam, uint32_t lParam);
void post_message(uint16_t hwnd, uint16_t msg, uint16_t wParam, uint32_t lParam);
// Call a far procedure in the program (callback) with Pascal arguments.
uint32_t call_app(uint32_t proc, const uint16_t* words, int nwords);

// Host-side input, fed by the display layer (screen coordinates).
void input_mouse_move(int x, int y);
void input_mouse_button(int x, int y, int button, bool down);
void input_key(int vk, bool down);
void input_char(int ch);
void input_quit();

// Current cursor image for the display layer (nullptr: none).
struct CursorImage {
    Surface image;     // 0xAARRGGBB
    int hotX = 0, hotY = 0;
};
const CursorImage* current_cursor();
void cursor_position(int* x, int* y);

// Called while the program waits for messages: present the screen, pump SDL.
// `blockMs`: how long the host may sleep waiting for input.
void host_pump(int blockMs);
// A text box gained or lost the focus: on touch devices the web page opens the
// on-screen keyboard (no effect elsewhere).
void host_text_input(bool active);

void user_init();
void dump_windows();   // log visible windows (debugging)

// ---- window internals for the predefined control classes (controls.cpp) ----
struct WndInfo {
    uint16_t parent = 0, id = 0;
    uint32_t style = 0;
    int w = 0, h = 0;
    bool enabled = true, visible = false;
};
bool wnd_info(uint16_t h, WndInfo& out);
std::string wnd_text(uint16_t h);
void wnd_set_text(uint16_t h, const std::string& text);
void wnd_invalidate(uint16_t h);
uint16_t wnd_paint_begin(uint16_t h);   // DC for the client area; validates it
void wnd_paint_end(uint16_t hdc);
uint16_t wnd_focus();
void wnd_set_focus(uint16_t h);
uint16_t wnd_capture();
void wnd_set_capture(uint16_t h);
std::vector<uint16_t> wnd_children(uint16_t h);   // front to back
uint32_t wnd_default_proc(uint16_t h, uint16_t msg, uint16_t wp, uint32_t lp);
void screen_to_client(uint16_t h, int* x, int* y);
Rect wnd_visible_rect(uint16_t h);   // visible client area in screen coordinates
// Screen rectangles of visible top-level windows stacked above h's top-level window.
std::vector<Rect> wnd_occluders(uint16_t h);

// controls.cpp: registers the native window procedures; returns the far
// procedure address of a predefined class ("BUTTON", ...) or 0.
void controls_init();
uint32_t control_proc(const std::string& cls);

}  // namespace win16
