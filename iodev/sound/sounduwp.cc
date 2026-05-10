/////////////////////////////////////////////////////////////////////////
// UWP lowlevel sound driver.
/////////////////////////////////////////////////////////////////////////

#define BX_PLUGGABLE

#include "bochs.h"
#include "plugin.h"
#include "pc_system.h"

#if BX_SUPPORT_SOUNDLOW && BX_HAVE_SOUND_UWP

#include "soundlow.h"
#include "sounduwp.h"

#define LOG_THIS
#define SOUNDUWP_PACKETS_PER_SEC 20

PLUGIN_ENTRY_FOR_SND_MODULE(uwp)
{
  if (mode == PLUGIN_PROBE) {
    return (int)PLUGTYPE_SND;
  }
  return 0;
}

class bx_sound_uwp_c : public bx_sound_lowlevel_c {
public:
  bx_sound_uwp_c() : bx_sound_lowlevel_c("uwp") {}
  virtual ~bx_sound_uwp_c() {}

  virtual bx_soundlow_waveout_c* get_waveout();
  virtual bx_soundlow_wavein_c* get_wavein();
  virtual bx_soundlow_midiout_c* get_midiout();
} bx_sound_uwp;

class bx_soundlow_waveout_uwp_c : public bx_soundlow_waveout_c {
public:
  bx_soundlow_waveout_uwp_c() : bx_soundlow_waveout_c(), opened(false) {}
  virtual ~bx_soundlow_waveout_uwp_c() {}

  virtual int openwaveoutput(const char *wavedev);
  virtual int set_pcm_params(bx_pcm_param_t *param);
  virtual int get_packetsize();
  virtual int output(int length, Bit8u data[]);
  virtual int closewaveoutput();
private:
  bool opened;
};

class bx_soundlow_wavein_uwp_c : public bx_soundlow_wavein_c {
public:
  bx_soundlow_wavein_uwp_c();
  virtual ~bx_soundlow_wavein_uwp_c();

  virtual int openwaveinput(const char *wavedev, sound_record_handler_t rh);
  virtual int startwaverecord(bx_pcm_param_t *param);
  virtual int getwavepacket(int length, Bit8u data[]);
  virtual int stopwaverecord();

private:
  bool opened;
  bool recording;
  bx_pcm_param_t wavein_param;
};

class bx_soundlow_midiout_uwp_c : public bx_soundlow_midiout_c {
public:
  bx_soundlow_midiout_uwp_c();
  virtual ~bx_soundlow_midiout_uwp_c();

  virtual int openmidioutput(const char *mididev);
  virtual int midiready();
  virtual int sendmidicommand(int delta, int command, int length, Bit8u data[]);
  virtual int closemidioutput();

private:
  bool opened;
};

bx_soundlow_waveout_c* bx_sound_uwp_c::get_waveout()
{
  if (waveout == NULL) {
    waveout = new bx_soundlow_waveout_uwp_c();
  }
  return waveout;
}

bx_soundlow_wavein_c* bx_sound_uwp_c::get_wavein()
{
  if (wavein == NULL) {
    wavein = new bx_soundlow_wavein_uwp_c();
  }
  return wavein;
}

bx_soundlow_midiout_c* bx_sound_uwp_c::get_midiout()
{
  if (midiout == NULL) {
    midiout = new bx_soundlow_midiout_uwp_c();
  }
  return midiout;
}

int bx_soundlow_waveout_uwp_c::openwaveoutput(const char *wavedev)
{
  UNUSED(wavedev);
  set_pcm_params(&real_pcm_param);
  pcm_callback_id = register_wave_callback(this, pcm_callback);
  start_resampler_thread();
  start_mixer_thread();
  opened = true;
  return BX_SOUNDLOW_OK;
}

int bx_soundlow_waveout_uwp_c::set_pcm_params(bx_pcm_param_t *param)
{
  real_pcm_param = *param;
  if (real_pcm_param.samplerate == 0) {
    real_pcm_param = default_pcm_param;
  }
  bx_uwp_audio_host_open(real_pcm_param.samplerate,
                         real_pcm_param.bits,
                         real_pcm_param.channels);
  return BX_SOUNDLOW_OK;
}

int bx_soundlow_waveout_uwp_c::get_packetsize()
{
  return real_pcm_param.samplerate * real_pcm_param.channels *
    (real_pcm_param.bits / 8) / SOUNDUWP_PACKETS_PER_SEC;
}

int bx_soundlow_waveout_uwp_c::output(int length, Bit8u data[])
{
  if (!opened || (length <= 0) || (data == NULL)) {
    return BX_SOUNDLOW_OK;
  }
  bx_uwp_audio_host_submit(data, (unsigned)length,
                           real_pcm_param.samplerate,
                           real_pcm_param.bits,
                           real_pcm_param.channels);
  BX_MSLEEP(1000 / SOUNDUWP_PACKETS_PER_SEC);
  return BX_SOUNDLOW_OK;
}

int bx_soundlow_waveout_uwp_c::closewaveoutput()
{
  if (opened) {
    bx_uwp_audio_host_close();
    opened = false;
  }
  return BX_SOUNDLOW_OK;
}

bx_soundlow_wavein_uwp_c::bx_soundlow_wavein_uwp_c()
  : bx_soundlow_wavein_c(), opened(false), recording(false)
{
  memset(&wavein_param, 0, sizeof(wavein_param));
}

bx_soundlow_wavein_uwp_c::~bx_soundlow_wavein_uwp_c()
{
  stopwaverecord();
  bx_uwp_audio_host_input_close();
}

int bx_soundlow_wavein_uwp_c::openwaveinput(const char *wavedev, sound_record_handler_t rh)
{
  UNUSED(wavedev);
  record_handler = rh;
  if (rh != NULL) {
    record_timer_index = DEV_register_timer(this, record_timer_handler, 1, 1, 0, "sounduwp");
  }
  opened = true;
  recording = false;
  memset(&wavein_param, 0, sizeof(wavein_param));
  return BX_SOUNDLOW_OK;
}

int bx_soundlow_wavein_uwp_c::startwaverecord(bx_pcm_param_t *param)
{
  if (!opened || param == NULL) {
    return BX_SOUNDLOW_ERR;
  }

  Bit8u shift = 0;
  if (param->bits == 16) shift++;
  if (param->channels == 2) shift++;
  record_packet_size = (param->samplerate / 10) << shift;
  if (record_packet_size <= 0 || record_packet_size > BX_SOUNDLOW_WAVEPACKETSIZE) {
    record_packet_size = BX_SOUNDLOW_WAVEPACKETSIZE;
  }

  if (memcmp(param, &wavein_param, sizeof(bx_pcm_param_t)) != 0) {
    wavein_param = *param;
    if (bx_uwp_audio_host_input_open(param->samplerate, param->bits, param->channels) != 0) {
      recording = false;
      return BX_SOUNDLOW_ERR;
    }
  }

  recording = true;
  if (record_timer_index != BX_NULL_TIMER_HANDLE) {
    Bit64u timer_val = (Bit64u)record_packet_size * 1000000 / (param->samplerate << shift);
    bx_pc_system.activate_timer(record_timer_index, (Bit32u)timer_val, 1);
  }
  return BX_SOUNDLOW_OK;
}

int bx_soundlow_wavein_uwp_c::getwavepacket(int length, Bit8u data[])
{
  if (data == NULL || length <= 0) {
    return BX_SOUNDLOW_OK;
  }

  if (!recording || bx_uwp_audio_host_input_get(data, (unsigned)length) != 0) {
    memset(data, 0, length);
  }
  return BX_SOUNDLOW_OK;
}

int bx_soundlow_wavein_uwp_c::stopwaverecord()
{
  if (record_timer_index != BX_NULL_TIMER_HANDLE) {
    bx_pc_system.deactivate_timer(record_timer_index);
  }
  recording = false;
  return BX_SOUNDLOW_OK;
}

bx_soundlow_midiout_uwp_c::bx_soundlow_midiout_uwp_c()
  : bx_soundlow_midiout_c(), opened(false)
{
}

bx_soundlow_midiout_uwp_c::~bx_soundlow_midiout_uwp_c()
{
  closemidioutput();
}

int bx_soundlow_midiout_uwp_c::openmidioutput(const char *mididev)
{
  opened = (bx_uwp_midi_host_open(mididev) == 0);
  return opened ? BX_SOUNDLOW_OK : BX_SOUNDLOW_ERR;
}

int bx_soundlow_midiout_uwp_c::midiready()
{
  return (opened && bx_uwp_midi_host_ready() == 0) ? BX_SOUNDLOW_OK : BX_SOUNDLOW_ERR;
}

int bx_soundlow_midiout_uwp_c::sendmidicommand(int delta, int command, int length, Bit8u data[])
{
  if (!opened) {
    return BX_SOUNDLOW_ERR;
  }
  return bx_uwp_midi_host_send((unsigned)delta, (unsigned)command,
    (unsigned)length, data) == 0 ? BX_SOUNDLOW_OK : BX_SOUNDLOW_ERR;
}

int bx_soundlow_midiout_uwp_c::closemidioutput()
{
  if (opened) {
    bx_uwp_midi_host_close();
    opened = false;
  }
  return BX_SOUNDLOW_OK;
}

#endif
