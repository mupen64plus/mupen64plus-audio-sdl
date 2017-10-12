/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-sdl-audio - main.c                                        *
 *   Mupen64Plus homepage: http://code.google.com/p/mupen64plus/           *
 *   Copyright (C) 2007-2009 Richard Goedeken                              *
 *   Copyright (C) 2007-2008 Ebenblues                                     *
 *   Copyright (C) 2003 JttL                                               *
 *   Copyright (C) 2002 Hacktarux                                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <SDL.h>
#include <SDL_audio.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "circular_buffer.h"
#include "main.h"
#include "osal_dynamiclib.h"
#include "resamplers/resamplers.h"
#include "volume.h"

#ifdef USE_AUDIORESOURCE
#include <audioresource.h>
#include <glib.h>
#endif

#define M64P_PLUGIN_PROTOTYPES 1
#include "m64p_common.h"
#include "m64p_config.h"
#include "m64p_plugin.h"
#include "m64p_types.h"

/* Default start-time size of primary buffer (in equivalent output samples).
   This is the buffer where audio is loaded after it's extracted from n64's memory.
   This value must be larger than PRIMARY_BUFFER_TARGET */
#define PRIMARY_BUFFER_SIZE 16384

/* this is the buffer fullness level (in equivalent output samples) which is targeted
   for the primary audio buffer (by inserting delays) each time data is received from
   the running N64 program.  This value must be larger than the SECONDARY_BUFFER_SIZE.
   Decreasing this value will reduce audio latency but requires a faster PC to avoid
   choppiness. Increasing this will increase audio latency but reduce the chance of
   drop-outs. The default value 2048 gives a 46ms maximum A/V delay at 44.1khz */
#define PRIMARY_BUFFER_TARGET 2048

/* Size of secondary buffer, in output samples. This is the requested size of SDL's
   hardware buffer, and the size of the mix buffer for doing SDL volume control. The
   SDL documentation states that this should be a power of two between 512 and 8192. */
#define SECONDARY_BUFFER_SIZE 1024

/* This sets default frequency what is used if rom doesn't want to change it.
   Probably only game that needs this is Zelda: Ocarina Of Time Master Quest
   *NOTICE* We should try to find out why Demos' frequencies are always wrong
   They tend to rely on a default frequency, apparently, never the same one ;)*/
#define DEFAULT_FREQUENCY 33600

/* number of bytes per sample */
#define N64_SAMPLE_BYTES 4
#define SDL_SAMPLE_BYTES 4

/* volume mixer types */
#define VOLUME_TYPE_SDL     1
#define VOLUME_TYPE_OSS     2

struct sdl_backend
{
    struct circular_buffer primary_buffer;

    /* Primary buffer size (in output samples) */
    size_t primary_buffer_size;

    /* Primary buffer fullness target (in output samples) */
    size_t target;

    /* Secondary buffer size (in output samples) */
    size_t secondary_buffer_size;

    /* Mixing buffer used for volume control */
    unsigned char* mix_buffer;

    unsigned int last_cb_time;
    unsigned int input_frequency;
    unsigned int output_frequency;
    unsigned int speed_factor;

    unsigned int swap_channels;

    unsigned int audio_sync;

    unsigned int paused_for_sync;

    unsigned int underrun_count;

    /* Resampler */
    void* resampler;
    const struct resampler_interface* iresampler;
};

/* local variables */
static void (*l_DebugCallback)(void *, int, const char *) = NULL;
static void *l_DebugCallContext = NULL;
static int l_PluginInit = 0;
static m64p_handle l_ConfigAudio;
#ifdef USE_AUDIORESOURCE
static audioresource_t *l_audioresource = NULL;
static int l_audioresource_acquired = 0;
#endif

static struct sdl_backend* l_sdl_backend = NULL;

/* Read header for type definition */
static AUDIO_INFO AudioInfo;
// volume to scale the audio by, range of 0..100
// if muted, this holds the volume when not muted
static int VolPercent = 80;
// how much percent to increment/decrement volume by
static int VolDelta = 5;
// the actual volume passed into SDL, range of 0..SDL_MIX_MAXVOLUME
static int VolSDL = SDL_MIX_MAXVOLUME;
// Muted or not
static int VolIsMuted = 0;
//which type of volume control to use
static int VolumeControlType = VOLUME_TYPE_SDL;

static int critical_failure = 0;

/* definitions of pointers to Core config functions */
ptr_ConfigOpenSection      ConfigOpenSection = NULL;
ptr_ConfigDeleteSection    ConfigDeleteSection = NULL;
ptr_ConfigSaveSection      ConfigSaveSection = NULL;
ptr_ConfigSetParameter     ConfigSetParameter = NULL;
ptr_ConfigGetParameter     ConfigGetParameter = NULL;
ptr_ConfigGetParameterHelp ConfigGetParameterHelp = NULL;
ptr_ConfigSetDefaultInt    ConfigSetDefaultInt = NULL;
ptr_ConfigSetDefaultFloat  ConfigSetDefaultFloat = NULL;
ptr_ConfigSetDefaultBool   ConfigSetDefaultBool = NULL;
ptr_ConfigSetDefaultString ConfigSetDefaultString = NULL;
ptr_ConfigGetParamInt      ConfigGetParamInt = NULL;
ptr_ConfigGetParamFloat    ConfigGetParamFloat = NULL;
ptr_ConfigGetParamBool     ConfigGetParamBool = NULL;
ptr_ConfigGetParamString   ConfigGetParamString = NULL;

/* Global functions */
void DebugMessage(int level, const char *message, ...)
{
  char msgbuf[1024];
  va_list args;

  if (l_DebugCallback == NULL)
      return;

  va_start(args, message);
  vsprintf(msgbuf, message, args);

  (*l_DebugCallback)(l_DebugCallContext, level, msgbuf);

  va_end(args);
}

static void my_audio_callback(void* userdata, unsigned char* stream, int len)
{
    struct sdl_backend* sdl_backend = (struct sdl_backend*)userdata;

    if (!l_PluginInit)
        return;

    /* mark the time, for synchronization on the input side */
    sdl_backend->last_cb_time = SDL_GetTicks();

    unsigned int newsamplerate = sdl_backend->output_frequency * 100 / sdl_backend->speed_factor;
    unsigned int oldsamplerate = sdl_backend->input_frequency;
    size_t needed = (len * oldsamplerate) / newsamplerate;
    size_t available;
    size_t consumed;

    const void* src = cbuff_tail(&sdl_backend->primary_buffer, &available);
    if ((available > 0) && (available >= needed))
    {
#if defined(HAS_OSS_SUPPORT)
        if (VolumeControlType== VOLUME_TYPE_OSS)
        {
            consumed = sdl_backend->iresampler->resample(sdl_backend->resampler, src, available, oldsamplerate, stream, len, newsamplerate);
        }
        else
#endif
        {
            consumed = sdl_backend->iresampler->resample(sdl_backend->resampler, src, available, oldsamplerate, sdl_backend->mix_buffer, len, newsamplerate);
            memset(stream, 0, len);
            SDL_MixAudio(stream, sdl_backend->mix_buffer, len, VolSDL);
        }
        consume_cbuff_data(&sdl_backend->primary_buffer, consumed);
    }
    else
    {
        ++sdl_backend->underrun_count;
        memset(stream , 0, len);
    }
}

static size_t new_primary_buffer_size(const struct sdl_backend* sdl_backend)
{
    return N64_SAMPLE_BYTES * ((uint64_t)sdl_backend->primary_buffer_size * sdl_backend->input_frequency * sdl_backend->speed_factor) /
        (sdl_backend->output_frequency * 100);
}

static void resize_primary_buffer(struct circular_buffer* cbuff, size_t new_size)
{
    /* only grows the buffer */
    if (new_size > cbuff->size) {
        SDL_LockAudio();
        cbuff->data = realloc(cbuff->data, new_size);
        memset(cbuff->data + cbuff->size, 0, new_size - cbuff->size);
        SDL_UnlockAudio();
        cbuff->size = new_size;
    }
}

static unsigned int select_output_frequency(unsigned int input_frequency)
{
    if (input_frequency <= 11025) { return 11025; }
    else if (input_frequency <= 22050) { return 22050; }
    else { return 44100; }
}

static void init_audio_device(struct sdl_backend* sdl_backend)
{
    SDL_AudioSpec desired, obtained;

    if (SDL_WasInit(SDL_INIT_AUDIO|SDL_INIT_TIMER) == (SDL_INIT_AUDIO|SDL_INIT_TIMER) )
    {
        DebugMessage(M64MSG_VERBOSE, "init_audio_device(): SDL Audio sub-system already initialized.");

        SDL_PauseAudio(1);
        SDL_CloseAudio();
    }
    else
    {
        if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0)
        {
            DebugMessage(M64MSG_ERROR, "Failed to initialize SDL audio subsystem.");
            critical_failure = 1;
            return;
        }

        critical_failure = 0;
    }

    sdl_backend->paused_for_sync = 1;

    /* reload these because they gets re-assigned from SDL data below, and init_audio_device can be called more than once */
    sdl_backend->primary_buffer_size = ConfigGetParamInt(l_ConfigAudio, "PRIMARY_BUFFER_SIZE");
    sdl_backend->target = ConfigGetParamInt(l_ConfigAudio, "PRIMARY_BUFFER_TARGET");
    sdl_backend->secondary_buffer_size = ConfigGetParamInt(l_ConfigAudio, "SECONDARY_BUFFER_SIZE");

    DebugMessage(M64MSG_INFO,    "Initializing SDL audio subsystem...");
    DebugMessage(M64MSG_VERBOSE, "Primary buffer: %i output samples.", sdl_backend->primary_buffer_size);
    DebugMessage(M64MSG_VERBOSE, "Primary target fullness: %i output samples.", sdl_backend->target);
    DebugMessage(M64MSG_VERBOSE, "Secondary buffer: %i output samples.", sdl_backend->secondary_buffer_size);

    memset(&desired, 0, sizeof(desired));
    desired.freq = select_output_frequency(sdl_backend->input_frequency);
    desired.format = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples = sdl_backend->secondary_buffer_size;
    desired.callback = my_audio_callback;
    desired.userdata = sdl_backend;

    DebugMessage(M64MSG_VERBOSE, "Requesting frequency: %iHz.", desired.freq);
    DebugMessage(M64MSG_VERBOSE, "Requesting format: %i.", desired.format);

    /* Open the audio device */
    if (SDL_OpenAudio(&desired, &obtained) < 0)
    {
        DebugMessage(M64MSG_ERROR, "Couldn't open audio: %s", SDL_GetError());
        critical_failure = 1;
        return;
    }
    if (desired.format != obtained.format)
    {
        DebugMessage(M64MSG_WARNING, "Obtained audio format differs from requested.");
    }
    if (desired.freq != obtained.freq)
    {
        DebugMessage(M64MSG_WARNING, "Obtained frequency differs from requested.");
    }

    /* adjust some variables given the obtained audio spec */
    sdl_backend->output_frequency = obtained.freq;
    sdl_backend->secondary_buffer_size = obtained.samples;

    if (sdl_backend->target < sdl_backend->secondary_buffer_size)
        sdl_backend->target = sdl_backend->secondary_buffer_size;

    if (sdl_backend->primary_buffer_size < sdl_backend->target)
        sdl_backend->primary_buffer_size = sdl_backend->target;
    if (sdl_backend->primary_buffer_size < sdl_backend->secondary_buffer_size * 2)
        sdl_backend->primary_buffer_size = sdl_backend->secondary_buffer_size * 2;

    /* allocate memory for audio buffers */
    resize_primary_buffer(&sdl_backend->primary_buffer, new_primary_buffer_size(sdl_backend));
    sdl_backend->mix_buffer = realloc(sdl_backend->mix_buffer, sdl_backend->secondary_buffer_size * SDL_SAMPLE_BYTES);

    /* preset the last callback time */
    if (sdl_backend->last_cb_time == 0) {
        sdl_backend->last_cb_time = SDL_GetTicks();
    }

    DebugMessage(M64MSG_VERBOSE, "Frequency: %i", obtained.freq);
    DebugMessage(M64MSG_VERBOSE, "Format: %i", obtained.format);
    DebugMessage(M64MSG_VERBOSE, "Channels: %i", obtained.channels);
    DebugMessage(M64MSG_VERBOSE, "Silence: %i", obtained.silence);
    DebugMessage(M64MSG_VERBOSE, "Samples: %i", obtained.samples);
    DebugMessage(M64MSG_VERBOSE, "Size: %i", obtained.size);

    /* set playback volume */
#if defined(HAS_OSS_SUPPORT)
    if (VolumeControlType== VOLUME_TYPE_OSS)
    {
        VolPercent = volGet();
    }
    else
#endif
    {
        VolSDL = SDL_MIX_MAXVOLUME * VolPercent / 100;
    }
}

static void release_audio_device(struct sdl_backend* sdl_backend)
{
    if (SDL_WasInit(SDL_INIT_AUDIO) != 0) {
        SDL_PauseAudio(1);
        SDL_CloseAudio();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }

    if (SDL_WasInit(SDL_INIT_TIMER) != 0) {
        SDL_QuitSubSystem(SDL_INIT_TIMER);
    }
}


static struct sdl_backend* init_sdl_backend(unsigned int default_frequency,
                                            unsigned int swap_channels,
                                            unsigned int audio_sync,
                                            const char* resampler_id)
{
    /* allocate memory for sdl_backend */
    struct sdl_backend* sdl_backend = malloc(sizeof(*sdl_backend));
    if (sdl_backend == NULL) {
        return NULL;
    }

    /* reset sdl_backend */
    memset(sdl_backend, 0, sizeof(*sdl_backend));

    /* instanciate resampler */
    void* resampler = NULL;
    const struct resampler_interface* iresampler = get_iresampler(resampler_id, &resampler);
    if (iresampler == NULL) {
        free(sdl_backend);
        return NULL;
    }

    sdl_backend->input_frequency = default_frequency;
    sdl_backend->swap_channels = swap_channels;
    sdl_backend->audio_sync = audio_sync;
    sdl_backend->paused_for_sync = 1;
    sdl_backend->speed_factor = 100;
    sdl_backend->resampler = resampler;
    sdl_backend->iresampler = iresampler;

    return sdl_backend;
}


static struct sdl_backend* init_sdl_backend_from_config(m64p_handle config)
{
    unsigned int default_frequency = ConfigGetParamInt(config, "DEFAULT_FREQUENCY");
    unsigned int swap_channels = ConfigGetParamBool(config, "SWAP_CHANNELS");
    unsigned int audio_sync = ConfigGetParamBool(config, "AUDIO_SYNC");
    const char* resampler_id = ConfigGetParamString(config, "RESAMPLE");

    return init_sdl_backend(
            default_frequency,
            swap_channels,
            audio_sync,
            resampler_id);
}


static void release_sdl_backend(struct sdl_backend* sdl_backend)
{
    if (sdl_backend == NULL) {
        return;
    }

    /* release primary buffer */
    release_cbuff(&sdl_backend->primary_buffer);

    /* release mix buffer */
    free(sdl_backend->mix_buffer);

    /* release resampler */
    sdl_backend->iresampler->release(sdl_backend->resampler);

    /* release sdl backend */
    free(sdl_backend);
}

static size_t estimate_level_at_next_audio_cb(struct sdl_backend* sdl_backend)
{
    size_t available;
    unsigned int now = SDL_GetTicks();

    cbuff_tail(&sdl_backend->primary_buffer, &available);

    /* Start by calculating the current Primary buffer fullness in terms of output samples */
    size_t expected_level = (size_t)(((int64_t)(available/N64_SAMPLE_BYTES) * sdl_backend->output_frequency * 100) / (sdl_backend->input_frequency * sdl_backend->speed_factor));

    /* Next, extrapolate to the buffer level at the expected time of the next audio callback, assuming that the
       buffer is filled at the same rate as the output frequency */
    unsigned int expected_next_cb_time = sdl_backend->last_cb_time + ((1000 * sdl_backend->secondary_buffer_size) / sdl_backend->output_frequency);

    if (now < expected_next_cb_time) {
        expected_level += (expected_next_cb_time - now) * sdl_backend->output_frequency / 1000;
    }

    return expected_level;
}

static void synchronize_audio(struct sdl_backend* sdl_backend)
{
    enum { TOLERANCE_MS = 10 };

    size_t expected_level = estimate_level_at_next_audio_cb(sdl_backend);

    /* If the expected value of the Primary Buffer Fullness at the time of the next audio callback is more than 10
       milliseconds ahead of our target buffer fullness level, then insert a delay now */
    if (sdl_backend->audio_sync && expected_level >= sdl_backend->target + sdl_backend->output_frequency * TOLERANCE_MS / 1000)
    {
        /* Core is ahead of SDL audio thread,
         * delay emulation to allow the SDL audio thread to catch up */
        unsigned int wait_time = (expected_level - sdl_backend->target) * 1000 / sdl_backend->output_frequency;

        if (sdl_backend->paused_for_sync) { SDL_PauseAudio(0); }
        sdl_backend->paused_for_sync = 0;

        SDL_Delay(wait_time);
    }
    else if (expected_level < sdl_backend->secondary_buffer_size)
    {
        /* Core is behind SDL audio thread (predicting an underflow),
         * pause the audio to let the Core catch up */
        if (!sdl_backend->paused_for_sync) { SDL_PauseAudio(0); }
        sdl_backend->paused_for_sync = 1;
    }
    else
    {
        /* Expected fullness is within tolerance,
         * audio thread is running */
        if (sdl_backend->paused_for_sync) { SDL_PauseAudio(0); }
        sdl_backend->paused_for_sync = 0;
    }
}



#ifdef USE_AUDIORESOURCE
void on_audioresource_acquired(audioresource_t *audioresource, bool acquired, void *user_data)
{
    DebugMessage(M64MSG_VERBOSE, "audioresource acquired: %d", acquired);
    l_audioresource_acquired = acquired;
}
#endif

/* Mupen64Plus plugin functions */
EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle CoreLibHandle, void *Context,
                                   void (*DebugCallback)(void *, int, const char *))
{
    ptr_CoreGetAPIVersions CoreAPIVersionFunc;

    int ConfigAPIVersion, DebugAPIVersion, VidextAPIVersion, bSaveConfig;
    float fConfigParamsVersion = 0.0f;

    if (l_PluginInit)
        return M64ERR_ALREADY_INIT;

    /* first thing is to set the callback function for debug info */
    l_DebugCallback = DebugCallback;
    l_DebugCallContext = Context;

    /* attach and call the CoreGetAPIVersions function, check Config API version for compatibility */
    CoreAPIVersionFunc = (ptr_CoreGetAPIVersions) osal_dynlib_getproc(CoreLibHandle, "CoreGetAPIVersions");
    if (CoreAPIVersionFunc == NULL)
    {
        DebugMessage(M64MSG_ERROR, "Core emulator broken; no CoreAPIVersionFunc() function found.");
        return M64ERR_INCOMPATIBLE;
    }

    (*CoreAPIVersionFunc)(&ConfigAPIVersion, &DebugAPIVersion, &VidextAPIVersion, NULL);
    if ((ConfigAPIVersion & 0xffff0000) != (CONFIG_API_VERSION & 0xffff0000))
    {
        DebugMessage(M64MSG_ERROR, "Emulator core Config API (v%i.%i.%i) incompatible with plugin (v%i.%i.%i)",
                VERSION_PRINTF_SPLIT(ConfigAPIVersion), VERSION_PRINTF_SPLIT(CONFIG_API_VERSION));
        return M64ERR_INCOMPATIBLE;
    }

    /* Get the core config function pointers from the library handle */
    ConfigOpenSection = (ptr_ConfigOpenSection) osal_dynlib_getproc(CoreLibHandle, "ConfigOpenSection");
    ConfigDeleteSection = (ptr_ConfigDeleteSection) osal_dynlib_getproc(CoreLibHandle, "ConfigDeleteSection");
    ConfigSaveSection = (ptr_ConfigSaveSection) osal_dynlib_getproc(CoreLibHandle, "ConfigSaveSection");
    ConfigSetParameter = (ptr_ConfigSetParameter) osal_dynlib_getproc(CoreLibHandle, "ConfigSetParameter");
    ConfigGetParameter = (ptr_ConfigGetParameter) osal_dynlib_getproc(CoreLibHandle, "ConfigGetParameter");
    ConfigSetDefaultInt = (ptr_ConfigSetDefaultInt) osal_dynlib_getproc(CoreLibHandle, "ConfigSetDefaultInt");
    ConfigSetDefaultFloat = (ptr_ConfigSetDefaultFloat) osal_dynlib_getproc(CoreLibHandle, "ConfigSetDefaultFloat");
    ConfigSetDefaultBool = (ptr_ConfigSetDefaultBool) osal_dynlib_getproc(CoreLibHandle, "ConfigSetDefaultBool");
    ConfigSetDefaultString = (ptr_ConfigSetDefaultString) osal_dynlib_getproc(CoreLibHandle, "ConfigSetDefaultString");
    ConfigGetParamInt = (ptr_ConfigGetParamInt) osal_dynlib_getproc(CoreLibHandle, "ConfigGetParamInt");
    ConfigGetParamFloat = (ptr_ConfigGetParamFloat) osal_dynlib_getproc(CoreLibHandle, "ConfigGetParamFloat");
    ConfigGetParamBool = (ptr_ConfigGetParamBool) osal_dynlib_getproc(CoreLibHandle, "ConfigGetParamBool");
    ConfigGetParamString = (ptr_ConfigGetParamString) osal_dynlib_getproc(CoreLibHandle, "ConfigGetParamString");

    if (!ConfigOpenSection || !ConfigDeleteSection || !ConfigSetParameter || !ConfigGetParameter ||
        !ConfigSetDefaultInt || !ConfigSetDefaultFloat || !ConfigSetDefaultBool || !ConfigSetDefaultString ||
        !ConfigGetParamInt   || !ConfigGetParamFloat   || !ConfigGetParamBool   || !ConfigGetParamString)
        return M64ERR_INCOMPATIBLE;

    /* ConfigSaveSection was added in Config API v2.1.0 */
    if (ConfigAPIVersion >= 0x020100 && !ConfigSaveSection)
        return M64ERR_INCOMPATIBLE;

    /* get a configuration section handle */
    if (ConfigOpenSection("Audio-SDL", &l_ConfigAudio) != M64ERR_SUCCESS)
    {
        DebugMessage(M64MSG_ERROR, "Couldn't open config section 'Audio-SDL'");
        return M64ERR_INPUT_NOT_FOUND;
    }

    /* check the section version number */
    bSaveConfig = 0;
    if (ConfigGetParameter(l_ConfigAudio, "Version", M64TYPE_FLOAT, &fConfigParamsVersion, sizeof(float)) != M64ERR_SUCCESS)
    {
        DebugMessage(M64MSG_WARNING, "No version number in 'Audio-SDL' config section. Setting defaults.");
        ConfigDeleteSection("Audio-SDL");
        ConfigOpenSection("Audio-SDL", &l_ConfigAudio);
        bSaveConfig = 1;
    }
    else if (((int) fConfigParamsVersion) != ((int) CONFIG_PARAM_VERSION))
    {
        DebugMessage(M64MSG_WARNING, "Incompatible version %.2f in 'Audio-SDL' config section: current is %.2f. Setting defaults.", fConfigParamsVersion, (float) CONFIG_PARAM_VERSION);
        ConfigDeleteSection("Audio-SDL");
        ConfigOpenSection("Audio-SDL", &l_ConfigAudio);
        bSaveConfig = 1;
    }
    else if ((CONFIG_PARAM_VERSION - fConfigParamsVersion) >= 0.0001f)
    {
        /* handle upgrades */
        float fVersion = CONFIG_PARAM_VERSION;
        ConfigSetParameter(l_ConfigAudio, "Version", M64TYPE_FLOAT, &fVersion);
        DebugMessage(M64MSG_INFO, "Updating parameter set version in 'Audio-SDL' config section to %.2f", fVersion);
        bSaveConfig = 1;
    }

    /* set the default values for this plugin */
    ConfigSetDefaultFloat(l_ConfigAudio, "Version",             CONFIG_PARAM_VERSION,  "Mupen64Plus SDL Audio Plugin config parameter version number");
    ConfigSetDefaultInt(l_ConfigAudio, "DEFAULT_FREQUENCY",     DEFAULT_FREQUENCY,     "Frequency which is used if rom doesn't want to change it");
    ConfigSetDefaultBool(l_ConfigAudio, "SWAP_CHANNELS",        0,                     "Swaps left and right channels");
    ConfigSetDefaultInt(l_ConfigAudio, "PRIMARY_BUFFER_SIZE",   PRIMARY_BUFFER_SIZE,   "Size of primary buffer in output samples. This is where audio is loaded after it's extracted from n64's memory.");
    ConfigSetDefaultInt(l_ConfigAudio, "PRIMARY_BUFFER_TARGET", PRIMARY_BUFFER_TARGET, "Fullness level target for Primary audio buffer, in equivalent output samples. This value must be larger than the SECONDARY_BUFFER_SIZE. Decreasing this value will reduce audio latency but requires a faster PC to avoid choppiness. Increasing this will increase audio latency but reduce the chance of drop-outs.");
    ConfigSetDefaultInt(l_ConfigAudio, "SECONDARY_BUFFER_SIZE", SECONDARY_BUFFER_SIZE, "Size of secondary buffer in output samples. This is SDL's hardware buffer. The SDL documentation states that this should be a power of two between 512 and 8192.");
    ConfigSetDefaultString(l_ConfigAudio, "RESAMPLE",           DEFAULT_RESAMPLER,             "Audio resampling algorithm. src-sinc-best-quality, src-sinc-medium-quality, src-sinc-fastest, src-zero-order-hold, src-linear, speex-fixed-{10-0}, trivial");
    ConfigSetDefaultInt(l_ConfigAudio, "VOLUME_CONTROL_TYPE",   VOLUME_TYPE_SDL,       "Volume control type: 1 = SDL (only affects Mupen64Plus output)  2 = OSS mixer (adjusts master PC volume)");
    ConfigSetDefaultInt(l_ConfigAudio, "VOLUME_ADJUST",         5,                     "Percentage change each time the volume is increased or decreased");
    ConfigSetDefaultInt(l_ConfigAudio, "VOLUME_DEFAULT",        80,                    "Default volume when a game is started.  Only used if VOLUME_CONTROL_TYPE is 1");
    ConfigSetDefaultBool(l_ConfigAudio, "AUDIO_SYNC", 0,                               "Synchronize Video/Audio");

    if (bSaveConfig && ConfigAPIVersion >= 0x020100)
        ConfigSaveSection("Audio-SDL");

#ifdef USE_AUDIORESOURCE
    setenv("PULSE_PROP_media.role", "x-maemo", 1);

    l_audioresource = audioresource_init(AUDIO_RESOURCE_GAME, on_audioresource_acquired, NULL);

    audioresource_acquire(l_audioresource);

    while(!l_audioresource_acquired)
    {
        DebugMessage(M64MSG_INFO, "Waiting for audioresource...");
        g_main_context_iteration(NULL, false);
    }
#endif

    l_PluginInit = 1;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginShutdown(void)
{
    if (!l_PluginInit)
        return M64ERR_NOT_INIT;

    /* reset some local variables */
    l_DebugCallback = NULL;
    l_DebugCallContext = NULL;

#ifdef USE_AUDIORESOURCE
    audioresource_release(l_audioresource);
    audioresource_free(l_audioresource);
#endif

    l_PluginInit = 0;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *PluginType, int *PluginVersion, int *APIVersion, const char **PluginNamePtr, int *Capabilities)
{
    /* set version info */
    if (PluginType != NULL)
        *PluginType = M64PLUGIN_AUDIO;

    if (PluginVersion != NULL)
        *PluginVersion = SDL_AUDIO_PLUGIN_VERSION;

    if (APIVersion != NULL)
        *APIVersion = AUDIO_PLUGIN_API_VERSION;

    if (PluginNamePtr != NULL)
        *PluginNamePtr = "Mupen64Plus SDL Audio Plugin";

    if (Capabilities != NULL)
    {
        *Capabilities = 0;
    }

    return M64ERR_SUCCESS;
}

/* ----------- Audio Functions ------------- */
EXPORT void CALL AiDacrateChanged( int SystemType )
{
    if (!l_PluginInit)
        return;

    /* determine input frequency based on system type */
    switch (SystemType)
    {
    case SYSTEM_NTSC:
        l_sdl_backend->input_frequency = 48681812 / (*AudioInfo.AI_DACRATE_REG + 1);
        break;
    case SYSTEM_PAL:
        l_sdl_backend->input_frequency = 49656530 / (*AudioInfo.AI_DACRATE_REG + 1);
        break;
    case SYSTEM_MPAL:
        l_sdl_backend->input_frequency = 48628316 / (*AudioInfo.AI_DACRATE_REG + 1);
        break;
    }

    init_audio_device(l_sdl_backend);
}

EXPORT void CALL AiLenChanged( void )
{
    size_t available;
    struct sdl_backend* sdl_backend = l_sdl_backend;

    if (critical_failure == 1)
        return;

    if (!l_PluginInit)
        return;

    void* dst = cbuff_head(&sdl_backend->primary_buffer, &available);
    size_t size = *AudioInfo.AI_LEN_REG;

    if (size <= available)
    {
        const unsigned char* src = AudioInfo.RDRAM + (*AudioInfo.AI_DRAM_ADDR_REG & 0xffffff);

        SDL_LockAudio();

        if (sdl_backend->swap_channels) {
            memcpy(dst, src, size);
        }
        else {
            size_t i;
            for (i = 0 ; i < size ; i += 4 )
            {
                memcpy(dst + i, src + i + 2, 2); /* Left */
                memcpy(dst + i + 2, src + i, 2); /* Right */
            }
        }

        SDL_UnlockAudio();

        produce_cbuff_data(&sdl_backend->primary_buffer, (size + 3) & ~0x3);
    }
    else
    {
        DebugMessage(M64MSG_WARNING, "AiLenChanged(): Audio buffer overflow.");
    }

    synchronize_audio(sdl_backend);
}

EXPORT int CALL InitiateAudio( AUDIO_INFO Audio_Info )
{
    if (!l_PluginInit)
        return 0;

    AudioInfo = Audio_Info;

    return 1;
}

EXPORT int CALL RomOpen(void)
{
    if (!l_PluginInit)
        return 0;

    l_sdl_backend = init_sdl_backend_from_config(l_ConfigAudio);

    VolumeControlType = ConfigGetParamInt(l_ConfigAudio, "VOLUME_CONTROL_TYPE");
    VolDelta = ConfigGetParamInt(l_ConfigAudio, "VOLUME_ADJUST");
    VolPercent = ConfigGetParamInt(l_ConfigAudio, "VOLUME_DEFAULT");

    init_audio_device(l_sdl_backend);

    return 1;
}

EXPORT void CALL RomClosed(void)
{
    if (!l_PluginInit) {
        return;
    }
    if (critical_failure == 1) {
        return;
    }

    release_audio_device(l_sdl_backend);
    release_sdl_backend(l_sdl_backend);
    l_sdl_backend = NULL;
}

EXPORT void CALL ProcessAList(void)
{
}

EXPORT void CALL SetSpeedFactor(int percentage)
{
    struct sdl_backend* sdl_backend = l_sdl_backend;

    if (!l_PluginInit)
        return;

    if (percentage >= 10 && percentage <= 300) {
        sdl_backend->speed_factor = percentage;

        /* we need a different size primary buffer to store the N64 samples when the speed changes */
        resize_primary_buffer(&sdl_backend->primary_buffer, new_primary_buffer_size(sdl_backend));
    }
}

// Returns the most recent ummuted volume level.
static int VolumeGetUnmutedLevel(void)
{
#if defined(HAS_OSS_SUPPORT)
    // reload volume if we're using OSS
    if (!VolIsMuted && VolumeControlType == VOLUME_TYPE_OSS)
    {
        return volGet();
    }
#endif

    return VolPercent;
}

// Sets the volume level based on the contents of VolPercent and VolIsMuted
static void VolumeCommit(void)
{
    int levelToCommit = VolIsMuted ? 0 : VolPercent;

#if defined(HAS_OSS_SUPPORT)
    if (VolumeControlType== VOLUME_TYPE_OSS)
    {
        //OSS mixer volume
        volSet(levelToCommit);
    }
    else
#endif
    {
        VolSDL = SDL_MIX_MAXVOLUME * levelToCommit / 100;
    }
}

EXPORT void CALL VolumeMute(void)
{
    if (!l_PluginInit)
        return;

    // Store the volume level in order to restore it later
    if (!VolIsMuted)
        VolPercent = VolumeGetUnmutedLevel();

    // Toogle mute
    VolIsMuted = !VolIsMuted;
    VolumeCommit();
}

EXPORT void CALL VolumeUp(void)
{
    if (!l_PluginInit)
        return;

    VolumeSetLevel(VolumeGetUnmutedLevel() + VolDelta);
}

EXPORT void CALL VolumeDown(void)
{
    if (!l_PluginInit)
        return;

    VolumeSetLevel(VolumeGetUnmutedLevel() - VolDelta);
}

EXPORT int CALL VolumeGetLevel(void)
{
    return VolIsMuted ? 0 : VolumeGetUnmutedLevel();
}

EXPORT void CALL VolumeSetLevel(int level)
{
    if (!l_PluginInit)
        return;

    //if muted, unmute first
    VolIsMuted = 0;

    // adjust volume
    VolPercent = level;
    if (VolPercent < 0)
        VolPercent = 0;
    else if (VolPercent > 100)
        VolPercent = 100;

    VolumeCommit();
}

EXPORT const char * CALL VolumeGetString(void)
{
    static char VolumeString[32];

    if (VolIsMuted)
    {
        strcpy(VolumeString, "Mute");
    }
    else
    {
        sprintf(VolumeString, "%i%%", VolPercent);
    }

    return VolumeString;
}

