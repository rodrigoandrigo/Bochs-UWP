/////////////////////////////////////////////////////////////////////////
// UWP DirectX display backend.
/////////////////////////////////////////////////////////////////////////

#define BX_PLUGGABLE

#include "bochs.h"
#include "iodev/iodev.h"
#include "gui.h"
#include "plugin.h"
#include "param_names.h"
#include "uwp_dx_bridge.h"

#if BX_WITH_UWP_DX

#include <algorithm>
#include <deque>
#include <mutex>
#include <stdint.h>
#include <vector>

#include "icon_bochs.h"

void bx_gui_c::key_event(Bit32u key)
{
  DEV_kbd_gen_scancode(key);
}

class bx_uwp_dx_gui_c : public bx_gui_c {
public:
  bx_uwp_dx_gui_c(void) :
    fb_width(0),
    fb_height(0),
    framebuffer_dirty(false),
    dirty_x0(0),
    dirty_y0(0),
    dirty_x1(0),
    dirty_y1(0),
    mouse_absxy(false),
    mouse_buttons(0),
    ctrl_l_down(false),
    ctrl_r_down(false),
    alt_l_down(false),
    alt_r_down(false) {}
  DECLARE_GUI_VIRTUAL_METHODS()
  DECLARE_GUI_NEW_VIRTUAL_METHODS()
  virtual void set_mouse_mode_absxy(bool mode);
  virtual void draw_char(Bit8u ch, Bit8u fc, Bit8u bc, Bit16u xc, Bit16u yc,
                         Bit8u fw, Bit8u fh, Bit8u fx, Bit8u fy,
                         bool gfxcharw9, Bit8u cs, Bit8u ce, bool curs,
                         bool font2);
  void enqueue_native_key(unsigned native_key, int pressed);
  void enqueue_pointer(int x, int y, int z, unsigned buttons, int absolute);
  void enqueue_focus(int focused);
  void enqueue_mouse_capture_request(int enabled);
  void enqueue_shutdown();

private:
  enum uwp_dx_event_type {
    UWP_DX_EVENT_KEY,
    UWP_DX_EVENT_POINTER,
    UWP_DX_EVENT_FOCUS,
    UWP_DX_EVENT_SHUTDOWN,
    UWP_DX_EVENT_MOUSE_CAPTURE_REQUEST
  };
  struct uwp_dx_event {
    uwp_dx_event_type type;
    unsigned native_key;
    int pressed;
    int x;
    int y;
    int z;
    unsigned buttons;
    int absolute;
    int focused;
    int capture_enabled;
  };

  void resize_framebuffer(unsigned x, unsigned y);
  Bit32u palette_bgra(Bit8u index) const;
  bool source_is_indexed() const;
  void copy_indexed_tile(Bit8u *tile, unsigned x, unsigned y,
                         unsigned w, unsigned h);
  void copy_bgra_tile(Bit8u *tile, unsigned x, unsigned y,
                      unsigned w, unsigned h);
  void update_indexed_rect(unsigned x, unsigned y, unsigned w, unsigned h);
  void update_all_indexed_pixels();
  void mark_dirty_rect(unsigned x, unsigned y, unsigned w, unsigned h);
  void mark_full_frame_dirty();
  void enqueue_event(const uwp_dx_event &event);
  Bit32u map_native_key(unsigned native_key, int pressed);
  void update_modifier_state(Bit32u bx_key, int pressed);
  bool handle_mouse_toggle_key(Bit32u bx_key, int pressed);
  bool handle_mouse_toggle_buttons(unsigned buttons);
  void release_keyboard_state();

  std::vector<uint32_t> framebuffer;
  std::vector<Bit8u> indexed_framebuffer;
  unsigned fb_width;
  unsigned fb_height;
  bool framebuffer_dirty;
  unsigned dirty_x0;
  unsigned dirty_y0;
  unsigned dirty_x1;
  unsigned dirty_y1;
  bool mouse_absxy;
  unsigned mouse_buttons;
  bool ctrl_l_down;
  bool ctrl_r_down;
  bool alt_l_down;
  bool alt_r_down;
  std::mutex event_mutex;
  std::deque<uwp_dx_event> event_queue;
};

static bx_uwp_dx_gui_c *theGui = NULL;
IMPLEMENT_GUI_PLUGIN_CODE(uwp_dx)

#define LOG_THIS theGui->

static Bit32u uwp_dx_scan_to_bx(unsigned scan_code, bool extended)
{
  if (extended) {
    switch (scan_code) {
      case 0x1c: return BX_KEY_KP_ENTER;
      case 0x1d: return BX_KEY_CTRL_R;
      case 0x20: return BX_KEY_POWER_CALC;
      case 0x32: return BX_KEY_INT_HOME;
      case 0x35: return BX_KEY_KP_DIVIDE;
      case 0x37: return BX_KEY_PRINT;
      case 0x38: return BX_KEY_ALT_R;
      case 0x45: return BX_KEY_NUM_LOCK;
      case 0x46: return BX_KEY_CTRL_BREAK;
      case 0x47: return BX_KEY_HOME;
      case 0x48: return BX_KEY_UP;
      case 0x49: return BX_KEY_PAGE_UP;
      case 0x4b: return BX_KEY_LEFT;
      case 0x4d: return BX_KEY_RIGHT;
      case 0x4f: return BX_KEY_END;
      case 0x50: return BX_KEY_DOWN;
      case 0x51: return BX_KEY_PAGE_DOWN;
      case 0x52: return BX_KEY_INSERT;
      case 0x53: return BX_KEY_DELETE;
      case 0x5b: return BX_KEY_WIN_L;
      case 0x5c: return BX_KEY_WIN_R;
      case 0x5d: return BX_KEY_MENU;
      case 0x5e: return BX_KEY_POWER_POWER;
      case 0x5f: return BX_KEY_POWER_SLEEP;
      case 0x63: return BX_KEY_POWER_WAKE;
      case 0x65: return BX_KEY_INT_SEARCH;
      case 0x66: return BX_KEY_INT_FAV;
      case 0x68: return BX_KEY_INT_STOP;
      case 0x69: return BX_KEY_INT_FORWARD;
      case 0x6a: return BX_KEY_INT_BACK;
      case 0x6b: return BX_KEY_POWER_MYCOMP;
      case 0x6c: return BX_KEY_INT_MAIL;
    }

    return BX_KEY_UNHANDLED;
  }

  switch (scan_code) {
    case 0x01: return BX_KEY_ESC;
    case 0x02: return BX_KEY_1;
    case 0x03: return BX_KEY_2;
    case 0x04: return BX_KEY_3;
    case 0x05: return BX_KEY_4;
    case 0x06: return BX_KEY_5;
    case 0x07: return BX_KEY_6;
    case 0x08: return BX_KEY_7;
    case 0x09: return BX_KEY_8;
    case 0x0a: return BX_KEY_9;
    case 0x0b: return BX_KEY_0;
    case 0x0c: return BX_KEY_MINUS;
    case 0x0d: return BX_KEY_EQUALS;
    case 0x0e: return BX_KEY_BACKSPACE;
    case 0x0f: return BX_KEY_TAB;
    case 0x10: return BX_KEY_Q;
    case 0x11: return BX_KEY_W;
    case 0x12: return BX_KEY_E;
    case 0x13: return BX_KEY_R;
    case 0x14: return BX_KEY_T;
    case 0x15: return BX_KEY_Y;
    case 0x16: return BX_KEY_U;
    case 0x17: return BX_KEY_I;
    case 0x18: return BX_KEY_O;
    case 0x19: return BX_KEY_P;
    case 0x1a: return BX_KEY_LEFT_BRACKET;
    case 0x1b: return BX_KEY_RIGHT_BRACKET;
    case 0x1c: return BX_KEY_ENTER;
    case 0x1d: return BX_KEY_CTRL_L;
    case 0x1e: return BX_KEY_A;
    case 0x1f: return BX_KEY_S;
    case 0x20: return BX_KEY_D;
    case 0x21: return BX_KEY_F;
    case 0x22: return BX_KEY_G;
    case 0x23: return BX_KEY_H;
    case 0x24: return BX_KEY_J;
    case 0x25: return BX_KEY_K;
    case 0x26: return BX_KEY_L;
    case 0x27: return BX_KEY_SEMICOLON;
    case 0x28: return BX_KEY_SINGLE_QUOTE;
    case 0x29: return BX_KEY_GRAVE;
    case 0x2a: return BX_KEY_SHIFT_L;
    case 0x2b: return BX_KEY_BACKSLASH;
    case 0x2c: return BX_KEY_Z;
    case 0x2d: return BX_KEY_X;
    case 0x2e: return BX_KEY_C;
    case 0x2f: return BX_KEY_V;
    case 0x30: return BX_KEY_B;
    case 0x31: return BX_KEY_N;
    case 0x32: return BX_KEY_M;
    case 0x33: return BX_KEY_COMMA;
    case 0x34: return BX_KEY_PERIOD;
    case 0x35: return BX_KEY_SLASH;
    case 0x36: return BX_KEY_SHIFT_R;
    case 0x37: return BX_KEY_KP_MULTIPLY;
    case 0x38: return BX_KEY_ALT_L;
    case 0x39: return BX_KEY_SPACE;
    case 0x3a: return BX_KEY_CAPS_LOCK;
    case 0x3b: return BX_KEY_F1;
    case 0x3c: return BX_KEY_F2;
    case 0x3d: return BX_KEY_F3;
    case 0x3e: return BX_KEY_F4;
    case 0x3f: return BX_KEY_F5;
    case 0x40: return BX_KEY_F6;
    case 0x41: return BX_KEY_F7;
    case 0x42: return BX_KEY_F8;
    case 0x43: return BX_KEY_F9;
    case 0x44: return BX_KEY_F10;
    case 0x45: return BX_KEY_NUM_LOCK;
    case 0x46: return BX_KEY_SCRL_LOCK;
    case 0x47: return BX_KEY_KP_HOME;
    case 0x48: return BX_KEY_KP_UP;
    case 0x49: return BX_KEY_KP_PAGE_UP;
    case 0x4a: return BX_KEY_KP_SUBTRACT;
    case 0x4b: return BX_KEY_KP_LEFT;
    case 0x4c: return BX_KEY_KP_5;
    case 0x4d: return BX_KEY_KP_RIGHT;
    case 0x4e: return BX_KEY_KP_ADD;
    case 0x4f: return BX_KEY_KP_END;
    case 0x50: return BX_KEY_KP_DOWN;
    case 0x51: return BX_KEY_KP_PAGE_DOWN;
    case 0x52: return BX_KEY_KP_INSERT;
    case 0x53: return BX_KEY_KP_DELETE;
    case 0x56: return BX_KEY_LEFT_BACKSLASH;
    case 0x57: return BX_KEY_F11;
    case 0x58: return BX_KEY_F12;
    case 0x73: return BX_KEY_SLASH;
    case 0x7e: return BX_KEY_KP_DELETE;
  }

  return BX_KEY_UNHANDLED;
}

static Bit32u uwp_dx_virtual_key_to_bx(unsigned native_key)
{
  if ((native_key >= 'A') && (native_key <= 'Z')) {
    return BX_KEY_A + native_key - 'A';
  }
  if ((native_key >= '0') && (native_key <= '9')) {
    return BX_KEY_0 + native_key - '0';
  }
  if ((native_key >= 0x70) && (native_key <= 0x7b)) {
    return BX_KEY_F1 + native_key - 0x70;
  }

  switch (native_key) {
    case 0x08: return BX_KEY_BACKSPACE;
    case 0x09: return BX_KEY_TAB;
    case 0x0d: return BX_KEY_ENTER;
    case 0x1000: return BX_KEY_KP_ENTER;
    case 0x10: return BX_KEY_SHIFT_L;
    case 0x11: return BX_KEY_CTRL_L;
    case 0x12: return BX_KEY_ALT_L;
    case 0x13: return BX_KEY_PAUSE;
    case 0x14: return BX_KEY_CAPS_LOCK;
    case 0x1b: return BX_KEY_ESC;
    case 0x20: return BX_KEY_SPACE;
    case 0x21: return BX_KEY_PAGE_UP;
    case 0x22: return BX_KEY_PAGE_DOWN;
    case 0x23: return BX_KEY_END;
    case 0x24: return BX_KEY_HOME;
    case 0x25: return BX_KEY_LEFT;
    case 0x26: return BX_KEY_UP;
    case 0x27: return BX_KEY_RIGHT;
    case 0x28: return BX_KEY_DOWN;
    case 0x2a: return BX_KEY_PRINT;
    case 0x2c: return BX_KEY_PRINT;
    case 0x2d: return BX_KEY_INSERT;
    case 0x2e: return BX_KEY_DELETE;
    case 0x5b: return BX_KEY_WIN_L;
    case 0x5c: return BX_KEY_WIN_R;
    case 0x5d: return BX_KEY_MENU;
    case 0x5e: return BX_KEY_POWER_POWER;
    case 0x5f: return BX_KEY_POWER_SLEEP;
    case 0x60: return BX_KEY_KP_INSERT;
    case 0x61: return BX_KEY_KP_END;
    case 0x62: return BX_KEY_KP_DOWN;
    case 0x63: return BX_KEY_KP_PAGE_DOWN;
    case 0x64: return BX_KEY_KP_LEFT;
    case 0x65: return BX_KEY_KP_5;
    case 0x66: return BX_KEY_KP_RIGHT;
    case 0x67: return BX_KEY_KP_HOME;
    case 0x68: return BX_KEY_KP_UP;
    case 0x69: return BX_KEY_KP_PAGE_UP;
    case 0x6a: return BX_KEY_KP_MULTIPLY;
    case 0x6b: return BX_KEY_KP_ADD;
    case 0x6d: return BX_KEY_KP_SUBTRACT;
    case 0x6e: return BX_KEY_KP_DELETE;
    case 0x6f: return BX_KEY_KP_DIVIDE;
    case 0x90: return BX_KEY_NUM_LOCK;
    case 0x91: return BX_KEY_SCRL_LOCK;
    case 0xa0: return BX_KEY_SHIFT_L;
    case 0xa1: return BX_KEY_SHIFT_R;
    case 0xa2: return BX_KEY_CTRL_L;
    case 0xa3: return BX_KEY_CTRL_R;
    case 0xa4: return BX_KEY_ALT_L;
    case 0xa5: return BX_KEY_ALT_R;
    case 0xa6: return BX_KEY_INT_BACK;
    case 0xa7: return BX_KEY_INT_FORWARD;
    case 0xa9: return BX_KEY_INT_STOP;
    case 0xaa: return BX_KEY_INT_SEARCH;
    case 0xab: return BX_KEY_INT_FAV;
    case 0xac: return BX_KEY_INT_HOME;
    case 0xb4: return BX_KEY_INT_MAIL;
    case 0xb6: return BX_KEY_POWER_MYCOMP;
    case 0xb7: return BX_KEY_POWER_CALC;
    case 0xba: return BX_KEY_SEMICOLON;
    case 0xbb: return BX_KEY_EQUALS;
    case 0xbc: return BX_KEY_COMMA;
    case 0xbd: return BX_KEY_MINUS;
    case 0xbe: return BX_KEY_PERIOD;
    case 0xbf: return BX_KEY_SLASH;
    case 0xc0: return BX_KEY_GRAVE;
    case 0xdb: return BX_KEY_LEFT_BRACKET;
    case 0xdc: return BX_KEY_BACKSLASH;
    case 0xdd: return BX_KEY_RIGHT_BRACKET;
    case 0xde: return BX_KEY_SINGLE_QUOTE;
    case 0xc1: return BX_KEY_SLASH;
    case 0xc2: return BX_KEY_KP_DELETE;
    case 0xe2: return BX_KEY_LEFT_BACKSLASH;
    case 0xe3: return BX_KEY_POWER_WAKE;
  }

  return BX_KEY_UNHANDLED;
}

static Bit32u uwp_dx_native_key_to_bx(unsigned native_key)
{
  if (native_key & BX_UWP_DX_NATIVE_KEY_SCANCODE) {
    Bit32u bx_key = uwp_dx_scan_to_bx(
      native_key & BX_UWP_DX_NATIVE_KEY_CODE_MASK,
      (native_key & BX_UWP_DX_NATIVE_KEY_EXTENDED) != 0);
    if (bx_key != BX_KEY_UNHANDLED) {
      return bx_key;
    }
  }

  return uwp_dx_virtual_key_to_bx(native_key);
}

static void uwp_dx_sink_key_event(unsigned native_key, int pressed)
{
  if (theGui != NULL) {
    theGui->enqueue_native_key(native_key, pressed);
  }
}

static void uwp_dx_sink_pointer_event(int x, int y, int z, unsigned buttons,
                                      int absolute)
{
  if (theGui != NULL) {
    theGui->enqueue_pointer(x, y, z, buttons, absolute);
  }
}

static void uwp_dx_sink_focus_event(int focused)
{
  if (theGui != NULL) {
    theGui->enqueue_focus(focused);
  }
}

static void uwp_dx_sink_shutdown_event(void)
{
  if (theGui != NULL) {
    theGui->enqueue_shutdown();
  }
}

static void uwp_dx_sink_mouse_capture_event(int enabled)
{
  if (theGui != NULL) {
    theGui->enqueue_mouse_capture_request(enabled);
  }
}

static const bx_uwp_dx_sink_t uwp_dx_sink = {
  uwp_dx_sink_key_event,
  uwp_dx_sink_pointer_event,
  uwp_dx_sink_focus_event,
  uwp_dx_sink_shutdown_event,
  uwp_dx_sink_mouse_capture_event
};

Bit32u bx_uwp_dx_gui_c::palette_bgra(Bit8u index) const
{
  return 0xff000000 |
         ((Bit32u)palette[index].red << 16) |
         ((Bit32u)palette[index].green << 8) |
         (Bit32u)palette[index].blue;
}

bool bx_uwp_dx_gui_c::source_is_indexed() const
{
  return (!guest_textmode && (guest_bpp == 8));
}

void bx_uwp_dx_gui_c::copy_indexed_tile(Bit8u *tile, unsigned x, unsigned y,
                                        unsigned w, unsigned h)
{
  unsigned src_pitch = x_tilesize;
  for (unsigned row = 0; row < h; row++) {
    const Bit8u *src = tile + row * src_pitch;
    Bit8u *idx_dst = indexed_framebuffer.empty() ? NULL :
      &indexed_framebuffer[(y + row) * fb_width + x];
    Bit32u *bgra_dst = &framebuffer[(y + row) * fb_width + x];
    for (unsigned col = 0; col < w; col++) {
      Bit8u index = src[col];
      if (idx_dst != NULL) {
        idx_dst[col] = index;
      }
      bgra_dst[col] = palette_bgra(index);
    }
  }
}

void bx_uwp_dx_gui_c::copy_bgra_tile(Bit8u *tile, unsigned x, unsigned y,
                                     unsigned w, unsigned h)
{
  unsigned src_pitch = x_tilesize * 4;
  for (unsigned row = 0; row < h; row++) {
    memcpy(&framebuffer[(y + row) * fb_width + x],
           tile + row * src_pitch, w * 4);
  }
}

void bx_uwp_dx_gui_c::update_indexed_rect(unsigned x, unsigned y,
                                          unsigned w, unsigned h)
{
  if (indexed_framebuffer.empty()) {
    return;
  }
  if (x >= fb_width || y >= fb_height) {
    return;
  }
  w = ((x + w) > fb_width) ? (fb_width - x) : w;
  h = ((y + h) > fb_height) ? (fb_height - y) : h;
  for (unsigned row = 0; row < h; row++) {
    const Bit8u *src = &indexed_framebuffer[(y + row) * fb_width + x];
    Bit32u *dst = &framebuffer[(y + row) * fb_width + x];
    for (unsigned col = 0; col < w; col++) {
      dst[col] = palette_bgra(src[col]);
    }
  }
}

void bx_uwp_dx_gui_c::update_all_indexed_pixels()
{
  update_indexed_rect(0, 0, fb_width, fb_height);
}

void bx_uwp_dx_gui_c::enqueue_event(const uwp_dx_event &event)
{
  std::lock_guard<std::mutex> lock(event_mutex);
  event_queue.push_back(event);
}

void bx_uwp_dx_gui_c::enqueue_native_key(unsigned native_key, int pressed)
{
  uwp_dx_event event = {};
  event.type = UWP_DX_EVENT_KEY;
  event.native_key = native_key;
  event.pressed = pressed;
  enqueue_event(event);
}

void bx_uwp_dx_gui_c::enqueue_pointer(int x, int y, int z, unsigned buttons,
                                      int absolute)
{
  uwp_dx_event event = {};
  event.type = UWP_DX_EVENT_POINTER;
  event.x = x;
  event.y = y;
  event.z = z;
  event.buttons = buttons;
  event.absolute = absolute;
  enqueue_event(event);
}

void bx_uwp_dx_gui_c::enqueue_focus(int focused)
{
  uwp_dx_event event = {};
  event.type = UWP_DX_EVENT_FOCUS;
  event.focused = focused;
  enqueue_event(event);
}

void bx_uwp_dx_gui_c::enqueue_mouse_capture_request(int enabled)
{
  uwp_dx_event event = {};
  event.type = UWP_DX_EVENT_MOUSE_CAPTURE_REQUEST;
  event.capture_enabled = enabled;
  enqueue_event(event);
}

void bx_uwp_dx_gui_c::enqueue_shutdown()
{
  uwp_dx_event event = {};
  event.type = UWP_DX_EVENT_SHUTDOWN;
  enqueue_event(event);
}

void bx_uwp_dx_gui_c::update_modifier_state(Bit32u bx_key, int pressed)
{
  bool down = (pressed != 0);
  switch (bx_key) {
    case BX_KEY_CTRL_L:
      ctrl_l_down = down;
      break;
    case BX_KEY_CTRL_R:
      ctrl_r_down = down;
      break;
    case BX_KEY_ALT_L:
      alt_l_down = down;
      break;
    case BX_KEY_ALT_R:
      alt_r_down = down;
      break;
  }
}

bool bx_uwp_dx_gui_c::handle_mouse_toggle_key(Bit32u bx_key, int pressed)
{
  Bit32u toggle_key = 0;
  switch (bx_key) {
    case BX_KEY_CTRL_L:
    case BX_KEY_CTRL_R:
      toggle_key = BX_MT_KEY_CTRL;
      break;
    case BX_KEY_ALT_L:
    case BX_KEY_ALT_R:
      toggle_key = BX_MT_KEY_ALT;
      break;
    case BX_KEY_F10:
      toggle_key = BX_MT_KEY_F10;
      break;
    case BX_KEY_F12:
      toggle_key = BX_MT_KEY_F12;
      break;
    case BX_KEY_G:
      toggle_key = BX_MT_KEY_G;
      break;
  }

  if (toggle_key == 0) {
    return false;
  }

  bool toggle = mouse_toggle_check(toggle_key, pressed != 0);
  if (toggle) {
    toggle_mouse_enable();
  }
  return toggle;
}

bool bx_uwp_dx_gui_c::handle_mouse_toggle_buttons(unsigned buttons)
{
  struct button_toggle_t {
    unsigned mask;
    Bit32u toggle_key;
  };
  static const button_toggle_t button_toggles[] = {
    { 0x01, BX_MT_LBUTTON },
    { 0x02, BX_MT_RBUTTON },
    { 0x04, BX_MT_MBUTTON }
  };

  bool toggled = false;
  unsigned changed = mouse_buttons ^ buttons;
  for (unsigned i = 0; i < sizeof(button_toggles) / sizeof(button_toggles[0]); i++) {
    if ((changed & button_toggles[i].mask) == 0) {
      continue;
    }
    bool pressed = (buttons & button_toggles[i].mask) != 0;
    if (mouse_toggle_check(button_toggles[i].toggle_key, pressed)) {
      toggled = true;
    }
  }

  mouse_buttons = buttons;
  if (toggled) {
    toggle_mouse_enable();
  }
  return toggled;
}

Bit32u bx_uwp_dx_gui_c::map_native_key(unsigned native_key, int pressed)
{
  Bit32u bx_key = uwp_dx_native_key_to_bx(native_key);
  if (bx_key == BX_KEY_UNHANDLED) {
    return bx_key;
  }

  bool ctrl_down = ctrl_l_down || ctrl_r_down;
  bool alt_down = alt_l_down || alt_r_down;
  if ((bx_key == BX_KEY_PAUSE) && ctrl_down) {
    bx_key = BX_KEY_CTRL_BREAK;
  } else if ((bx_key == BX_KEY_PRINT) && alt_down) {
    bx_key = BX_KEY_ALT_SYSREQ;
  }

  update_modifier_state(bx_key, pressed);
  return bx_key;
}

void bx_uwp_dx_gui_c::release_keyboard_state()
{
  ctrl_l_down = false;
  ctrl_r_down = false;
  alt_l_down = false;
  alt_r_down = false;
  mouse_buttons = 0;
  mouse_toggle_check(BX_MT_KEY_CTRL, false);
  mouse_toggle_check(BX_MT_KEY_ALT, false);
  mouse_toggle_check(BX_MT_KEY_F10, false);
  mouse_toggle_check(BX_MT_KEY_F12, false);
  mouse_toggle_check(BX_MT_KEY_G, false);
  mouse_toggle_check(BX_MT_LBUTTON, false);
  mouse_toggle_check(BX_MT_RBUTTON, false);
  mouse_toggle_check(BX_MT_MBUTTON, false);
  DEV_kbd_release_keys();
}

void bx_uwp_dx_gui_c::resize_framebuffer(unsigned x, unsigned y)
{
  fb_width = x ? x : 1;
  fb_height = y ? y : 1;
  host_xres = (Bit16u)fb_width;
  host_yres = (Bit16u)fb_height;
  host_bpp = source_is_indexed() ? 8 : 32;
  host_pitch = host_xres * (source_is_indexed() ? 1 : 4);
  framebuffer.assign(fb_width * fb_height, 0xff000000);
  if (source_is_indexed()) {
    indexed_framebuffer.assign(fb_width * fb_height, 0);
  } else {
    indexed_framebuffer.clear();
  }
  mark_full_frame_dirty();

  bx_uwp_dx_host_configure(host_xres, host_yres, host_xres * 4, 32);
}

void bx_uwp_dx_gui_c::mark_dirty_rect(unsigned x, unsigned y, unsigned w, unsigned h)
{
  if (framebuffer.empty() || x >= fb_width || y >= fb_height || w == 0 || h == 0) {
    return;
  }
  if (x + w > fb_width) {
    w = fb_width - x;
  }
  if (y + h > fb_height) {
    h = fb_height - y;
  }

  if (!framebuffer_dirty) {
    dirty_x0 = x;
    dirty_y0 = y;
    dirty_x1 = x + w;
    dirty_y1 = y + h;
  } else {
    dirty_x0 = (std::min)(dirty_x0, x);
    dirty_y0 = (std::min)(dirty_y0, y);
    dirty_x1 = (std::max)(dirty_x1, x + w);
    dirty_y1 = (std::max)(dirty_y1, y + h);
  }
  framebuffer_dirty = true;
}

void bx_uwp_dx_gui_c::mark_full_frame_dirty()
{
  mark_dirty_rect(0, 0, fb_width, fb_height);
}

void bx_uwp_dx_gui_c::specific_init(int argc, char **argv, unsigned headerbar_y)
{
  put("UWP_DX");
  UNUSED(headerbar_y);
  UNUSED(bochs_icon_bits);

  new_gfx_api = 1;
  new_text_api = 1;
  dialog_caps = BX_GUI_DLG_RUNTIME | BX_GUI_DLG_SAVE_RESTORE;

  Bit8u flags = BX_GUI_OPT_NOKEYREPEAT | BX_GUI_OPT_HIDE_IPS |
                BX_GUI_OPT_CMDMODE | BX_GUI_OPT_NO_GUI_CONSOLE;
  for (int i = 1; i < argc; i++) {
    if (!parse_common_gui_options(argv[i], flags)) {
      BX_PANIC(("Unknown uwp_dx option '%s'", argv[i]));
    }
  }

  resize_framebuffer(640, 480);
  bx_uwp_dx_host_set_sink(&uwp_dx_sink);
  bx_uwp_dx_host_set_status_text("Bochs UWP DirectX backend ready");
}

void bx_uwp_dx_gui_c::handle_events(void)
{
  std::deque<uwp_dx_event> pending;
  {
    std::lock_guard<std::mutex> lock(event_mutex);
    pending.swap(event_queue);
  }

  while (!pending.empty()) {
    uwp_dx_event event = pending.front();
    pending.pop_front();

    switch (event.type) {
      case UWP_DX_EVENT_KEY: {
        Bit32u bx_key = map_native_key(event.native_key, event.pressed);
        if (bx_key != BX_KEY_UNHANDLED) {
          if (handle_mouse_toggle_key(bx_key, event.pressed)) {
            break;
          }
          bx_gui_c::key_event(bx_key |
            (event.pressed ? BX_KEY_PRESSED : BX_KEY_RELEASED));
        }
        break;
      }
      case UWP_DX_EVENT_POINTER:
        if (handle_mouse_toggle_buttons(event.buttons)) {
          break;
        }
        DEV_mouse_motion(event.x, event.y, event.z, event.buttons,
                         mouse_absxy ? (event.absolute != 0) : 0);
        break;
      case UWP_DX_EVENT_FOCUS:
        if (!event.focused) {
          release_keyboard_state();
        }
        break;
      case UWP_DX_EVENT_MOUSE_CAPTURE_REQUEST: {
        bool enabled = (event.capture_enabled != 0);
        if (SIM->get_param_bool(BXPN_MOUSE_ENABLED)->get() != enabled) {
          SIM->get_param_bool(BXPN_MOUSE_ENABLED)->set(enabled);
        }
        break;
      }
      case UWP_DX_EVENT_SHUTDOWN:
        bx_exit(0);
        break;
    }
  }
}

void bx_uwp_dx_gui_c::flush(void)
{
  if (framebuffer_dirty && !framebuffer.empty()) {
    unsigned width = dirty_x1 > dirty_x0 ? dirty_x1 - dirty_x0 : fb_width;
    unsigned height = dirty_y1 > dirty_y0 ? dirty_y1 - dirty_y0 : fb_height;
    const uint32_t *src = &framebuffer[dirty_y0 * fb_width + dirty_x0];
    bx_uwp_dx_host_update_rect(src, fb_width * 4, dirty_x0, dirty_y0,
                               width, height);
    framebuffer_dirty = false;
  }
  bx_uwp_dx_host_present();
}

void bx_uwp_dx_gui_c::clear_screen(void)
{
  if (!framebuffer.empty()) {
    std::fill(framebuffer.begin(), framebuffer.end(), 0xff000000);
    mark_full_frame_dirty();
  }
  bx_uwp_dx_host_clear(0xff000000);
}

void bx_uwp_dx_gui_c::text_update(Bit8u *old_text, Bit8u *new_text,
                                  unsigned long cursor_x,
                                  unsigned long cursor_y,
                                  bx_vga_tminfo_t *tm_info)
{
  Bit16u cursor_address = 0xffff;
  if ((cursor_x != 0xffff) && (cursor_y != 0xffff)) {
    cursor_address = (Bit16u)(tm_info->start_address +
      (cursor_y * tm_info->line_offset) + (cursor_x * 2));
  }
  text_update_common(old_text, new_text, cursor_address, tm_info);
}

void bx_uwp_dx_gui_c::draw_char(Bit8u ch, Bit8u fc, Bit8u bc, Bit16u xc,
                                Bit16u yc, Bit8u fw, Bit8u fh, Bit8u fx,
                                Bit8u fy, bool gfxcharw9, Bit8u cs, Bit8u ce,
                                bool curs, bool font2)
{
  if (framebuffer.empty() || (xc >= fb_width) || (yc >= fb_height)) {
    return;
  }
  if ((unsigned)xc + (unsigned)fw > fb_width) {
    fw = (Bit8u)(fb_width - xc);
  }
  if ((unsigned)yc + (unsigned)fh > fb_height) {
    fh = (Bit8u)(fb_height - yc);
  }

  Bit8u *font_ptr = &vga_charmap[font2 ? 1 : 0][(ch << 5) + fy];
  bool dwidth = (guest_fwidth > 9);
  Bit32u fg = palette_bgra(fc);
  Bit32u bg = palette_bgra(bc);

  for (Bit8u row = 0; row < fh; row++, fy++) {
    Bit16u font_row = *font_ptr++;
    if (gfxcharw9) {
      font_row = (font_row << 1) | (font_row & 0x01);
    } else {
      font_row <<= 1;
    }
    if (fx > 0) {
      font_row <<= fx;
    }
    Bit16u mask = (curs && (fy >= cs) && (fy <= ce)) ? 0x100 : 0x00;
    Bit32u *dst = &framebuffer[(yc + row) * fb_width + xc];
    for (Bit8u col = 0; col < fw; col++) {
      *dst++ = ((font_row & 0x100) == mask) ? bg : fg;
      if (!dwidth || (col & 1)) {
        font_row <<= 1;
      }
    }
  }

  mark_dirty_rect(xc, yc, fw, fh);
}

void bx_uwp_dx_gui_c::graphics_tile_update(Bit8u *tile, unsigned x, unsigned y)
{
  if (framebuffer.empty() || (x >= fb_width) || (y >= fb_height)) {
    return;
  }
  unsigned w = ((x + x_tilesize) > fb_width) ? (fb_width - x) : x_tilesize;
  unsigned h = ((y + y_tilesize) > fb_height) ? (fb_height - y) : y_tilesize;

  if (source_is_indexed()) {
    copy_indexed_tile(tile, x, y, w, h);
  } else {
    copy_bgra_tile(tile, x, y, w, h);
  }

  mark_dirty_rect(x, y, w, h);
}

bx_svga_tileinfo_t *bx_uwp_dx_gui_c::graphics_tile_info(bx_svga_tileinfo_t *info)
{
  if (snapshot_mode) {
    return bx_gui_c::graphics_tile_info_common(info);
  }

  if (!info) {
    info = new bx_svga_tileinfo_t;
    if (!info) {
      return NULL;
    }
  }

  info->bpp = source_is_indexed() ? 8 : 32;
  info->pitch = host_pitch;
  if (source_is_indexed()) {
    info->red_shift = 0;
    info->green_shift = 0;
    info->blue_shift = 0;
    info->red_mask = 0;
    info->green_mask = 0;
    info->blue_mask = 0;
    info->is_indexed = 1;
  } else {
    info->red_shift = 24;
    info->green_shift = 16;
    info->blue_shift = 8;
    info->red_mask = 0xff0000;
    info->green_mask = 0x00ff00;
    info->blue_mask = 0x0000ff;
    info->is_indexed = 0;
  }
  info->is_little_endian = 1;
  info->snapshot_mode = snapshot_mode;
  return info;
}

Bit8u *bx_uwp_dx_gui_c::graphics_tile_get(unsigned x, unsigned y,
                                          unsigned *w, unsigned *h)
{
  if (x + x_tilesize > fb_width) {
    *w = fb_width - x;
  } else {
    *w = x_tilesize;
  }

  if (y + y_tilesize > fb_height) {
    *h = fb_height - y;
  } else {
    *h = y_tilesize;
  }

  if (source_is_indexed()) {
    return &indexed_framebuffer[y * fb_width + x];
  }

  return (Bit8u *)&framebuffer[y * fb_width + x];
}

void bx_uwp_dx_gui_c::graphics_tile_update_in_place(unsigned x, unsigned y,
                                                    unsigned w, unsigned h)
{
  if (source_is_indexed()) {
    update_indexed_rect(x, y, w, h);
  }
  mark_dirty_rect(x, y, w, h);
}

bool bx_uwp_dx_gui_c::palette_change(Bit8u index, Bit8u red, Bit8u green,
                                     Bit8u blue)
{
  UNUSED(red);
  UNUSED(green);
  UNUSED(blue);
  if (source_is_indexed()) {
    update_all_indexed_pixels();
    mark_full_frame_dirty();
  }
  UNUSED(index);
  return 1;
}

void bx_uwp_dx_gui_c::dimension_update(unsigned x, unsigned y, unsigned fheight,
                                       unsigned fwidth, unsigned bpp)
{
  guest_textmode = (fheight > 0);
  guest_xres = x;
  guest_yres = y;
  guest_bpp = bpp;
  guest_fheight = (Bit8u)fheight;
  guest_fwidth = (Bit8u)fwidth;
  resize_framebuffer(x, y);
}

unsigned bx_uwp_dx_gui_c::create_bitmap(const unsigned char *bmap,
                                        unsigned xdim, unsigned ydim)
{
  UNUSED(bmap);
  UNUSED(xdim);
  UNUSED(ydim);
  return 0;
}

unsigned bx_uwp_dx_gui_c::headerbar_bitmap(unsigned bmap_id, unsigned alignment,
                                           void (*f)(void))
{
  UNUSED(bmap_id);
  UNUSED(alignment);
  UNUSED(f);
  return 0;
}

void bx_uwp_dx_gui_c::replace_bitmap(unsigned hbar_id, unsigned bmap_id)
{
  UNUSED(hbar_id);
  UNUSED(bmap_id);
}

void bx_uwp_dx_gui_c::show_headerbar(void)
{
}

int bx_uwp_dx_gui_c::get_clipboard_text(Bit8u **bytes, Bit32s *nbytes)
{
  UNUSED(bytes);
  UNUSED(nbytes);
  return 0;
}

int bx_uwp_dx_gui_c::set_clipboard_text(char *snapshot, Bit32u len)
{
  UNUSED(snapshot);
  UNUSED(len);
  return 0;
}

void bx_uwp_dx_gui_c::mouse_enabled_changed_specific(bool val)
{
  bx_uwp_dx_host_set_mouse_capture(val ? 1 : 0);
}

void bx_uwp_dx_gui_c::set_mouse_mode_absxy(bool mode)
{
  mouse_absxy = mode;
  bx_uwp_dx_host_set_mouse_mode(mode ? 1 : 0);
}

void bx_uwp_dx_gui_c::exit(void)
{
  bx_uwp_dx_host_set_sink(NULL);
  bx_uwp_dx_host_set_mouse_capture(0);
  bx_uwp_dx_host_set_mouse_mode(0);
  bx_uwp_dx_host_set_status_text("Bochs UWP DirectX backend stopped");
}

#endif
