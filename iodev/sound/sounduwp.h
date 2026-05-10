/////////////////////////////////////////////////////////////////////////
// UWP lowlevel sound bridge.
/////////////////////////////////////////////////////////////////////////

#ifndef BX_SOUND_UWP_H
#define BX_SOUND_UWP_H

#ifdef __cplusplus
extern "C" {
#endif

void bx_uwp_audio_host_open(unsigned sample_rate, unsigned bits,
                            unsigned channels);
void bx_uwp_audio_host_submit(const void *data, unsigned length,
                              unsigned sample_rate, unsigned bits,
                              unsigned channels);
void bx_uwp_audio_host_close(void);
int bx_uwp_audio_host_input_open(unsigned sample_rate, unsigned bits,
                                 unsigned channels);
int bx_uwp_audio_host_input_get(void *data, unsigned length);
void bx_uwp_audio_host_input_close(void);
int bx_uwp_midi_host_open(const char *device);
int bx_uwp_midi_host_ready(void);
int bx_uwp_midi_host_send(unsigned delta, unsigned command,
                          unsigned length, const unsigned char *data);
void bx_uwp_midi_host_close(void);

#ifdef __cplusplus
}
#endif

#endif
