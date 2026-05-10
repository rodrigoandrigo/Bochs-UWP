/////////////////////////////////////////////////////////////////////////
// UWP DirectX bridge contract for the Bochs GUI backend.
/////////////////////////////////////////////////////////////////////////

#ifndef BX_UWP_DX_BRIDGE_H
#define BX_UWP_DX_BRIDGE_H

#define BX_UWP_DX_NATIVE_KEY_SCANCODE 0x2000u
#define BX_UWP_DX_NATIVE_KEY_EXTENDED 0x0100u
#define BX_UWP_DX_NATIVE_KEY_CODE_MASK 0x00ffu

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*bx_uwp_dx_native_key_cb)(unsigned native_key, int pressed);
typedef void (*bx_uwp_dx_pointer_cb)(int x, int y, int z, unsigned buttons,
                                     int absolute);
typedef void (*bx_uwp_dx_focus_cb)(int focused);
typedef void (*bx_uwp_dx_shutdown_cb)(void);
typedef void (*bx_uwp_dx_mouse_capture_cb)(int enabled);

typedef struct bx_uwp_dx_sink_t {
  bx_uwp_dx_native_key_cb key_event;
  bx_uwp_dx_pointer_cb pointer_event;
  bx_uwp_dx_focus_cb focus_event;
  bx_uwp_dx_shutdown_cb shutdown_event;
  bx_uwp_dx_mouse_capture_cb mouse_capture_event;
} bx_uwp_dx_sink_t;

void bx_uwp_dx_host_set_sink(const bx_uwp_dx_sink_t *sink);
void bx_uwp_dx_host_configure(unsigned width, unsigned height,
                              unsigned pitch, unsigned bpp);
void bx_uwp_dx_host_update_rect(const void *bgra, unsigned source_pitch,
                                unsigned x, unsigned y,
                                unsigned width, unsigned height);
void bx_uwp_dx_host_present(void);
void bx_uwp_dx_host_clear(unsigned bgra);
void bx_uwp_dx_host_set_mouse_capture(int enabled);
void bx_uwp_dx_host_set_mouse_mode(int absolute);
void bx_uwp_dx_host_set_status_text(const char *text);

#ifdef __cplusplus
}
#endif

#endif
