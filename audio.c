///
/// @file audio.c   @brief Audio module
///
/// Copyright (c) 2009 - 2014 by Johns.  All Rights Reserved.
///
/// Contributor(s):
///
/// License: AGPLv3
///
/// This program is free software: you can redistribute it and/or modify
/// it under the terms of the GNU Affero General Public License as
/// published by the Free Software Foundation, either version 3 of the
/// License.
///
/// This program is distributed in the hope that it will be useful,
/// but WITHOUT ANY WARRANTY; without even the implied warranty of
/// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
/// GNU Affero General Public License for more details.
///
/// $Id: 77fa65030b179e78c13d0bf69a7cc417dae89e1a $
//////////////////////////////////////////////////////////////////////////////

///
/// @defgroup Audio The audio module.
///
/// This module contains all audio output functions.
///
/// ALSA PCM/Mixer api is supported.
/// @see http://www.alsa-project.org/alsa-doc/alsa-lib
///
/// @note alsa async playback is broken, don't use it!
///
/// OSS PCM/Mixer api is supported.
/// @see http://manuals.opensound.com/developer/
///
///
/// @todo FIXME: there can be problems with little/big endian.
///
#ifdef DEBUG
#undef DEBUG
#endif
//#define USE_ALSA      ///< enable alsa support
//#define USE_OSS       ///< enable OSS support
#define USE_AUDIO_THREAD                ///< use thread for audio playback
#define USE_AUDIO_MIXER                 ///< use audio module mixer

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <math.h>
#include <sys/prctl.h>
#include <sched.h>

#include <libintl.h>
#define _(str) gettext(str)             ///< gettext shortcut
#define _N(str) str                     ///< gettext_noop shortcut

#ifdef USE_ALSA
#include <alsa/asoundlib.h>
#endif
#ifdef USE_OSS
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
// SNDCTL_DSP_HALT_OUTPUT compatibility
#ifndef SNDCTL_DSP_HALT_OUTPUT
#  if defined(SNDCTL_DSP_RESET_OUTPUT)
#    define SNDCTL_DSP_HALT_OUTPUT SNDCTL_DSP_RESET_OUTPUT
#  elif defined(SNDCTL_DSP_RESET)
#    define SNDCTL_DSP_HALT_OUTPUT SNDCTL_DSP_RESET
#  else
#    error "No valid SNDCTL_DSP_HALT_OUTPUT found."
#  endif
#endif
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#endif

#ifdef USE_AUDIO_THREAD
#ifndef __USE_GNU
#define __USE_GNU
#endif
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#ifndef HAVE_PTHREAD_NAME
/// only available with newer glibc
#define pthread_setname_np(thread, name)
#endif
#endif

#include "iatomic.h"                    // portable atomic_t

#include "ringbuffer.h"
#include "misc.h"
#include "audio.h"


//----------------------------------------------------------------------------
//  Declarations
//----------------------------------------------------------------------------
typedef int Bool;
#define bool Bool

/**
**	Audio output module structure and typedef.
*/
typedef struct _audio_module_
{
    const char *Name;                   ///< audio output module name

    int (*const Thread)(void);          ///< module thread handler
    void (*const FlushBuffers)(void);   ///< flush sample buffers
     int64_t(*const GetDelay) (void);   ///< get current audio delay
    void (*const SetVolume)(int);       ///< set output volume
    int (*const Setup)(int *, int *, int);  ///< setup channels, samplerate
    void (*const Play)(void);           ///< play audio
    void (*const Pause)(void);          ///< pause audio
    void (*const Init)(void);           ///< initialize audio output module
    void (*const Exit)(void);           ///< cleanup audio output module
} AudioModule;

static const AudioModule NoopModule;    ///< forward definition of noop module

//----------------------------------------------------------------------------
//  Variables
//----------------------------------------------------------------------------

char AudioAlsaDriverBroken;             ///< disable broken driver message
char AudioAlsaNoCloseOpen=0;              ///< disable alsa close/open fix
char AudioAlsaCloseOpenDelay;           ///< enable alsa close/open delay fix

static const char *AudioModuleName;     ///< which audio module to use

/// Selected audio module.
static const AudioModule *AudioUsedModule = &NoopModule;
static const char *AudioPCMDevice = "hw:0,3";          ///< PCM device name
static const char *AudioPassthroughDevice = "hw:0,0";  ///< Passthrough device name
static char AudioAppendAES;             ///< flag automatic append AES
static const char *AudioMixerDevice;    ///< mixer device name
static const char *AudioMixerChannel;   ///< mixer channel name
static char AudioDoingInit;             ///> flag in init, reduce error
volatile char AudioRunning;             ///< thread running / stopped
static volatile char AudioPaused;       ///< audio paused
char AudioVideoIsReady; ///< video ready start early
static int AudioSkip;                   ///< skip audio to sync to video


static const int AudioBytesProSample = 2;   ///< number of bytes per sample

static int AudioBufferTime = 336;       ///< audio buffer time in ms

#ifdef USE_AUDIO_THREAD
static pthread_t AudioThread;           ///< audio play thread
static pthread_mutex_t AudioMutex;      ///< audio condition mutex
static pthread_cond_t AudioStartCond;   ///< condition variable
static char AudioThreadStop;            ///< stop audio thread
#else
static const int AudioThread;           ///< dummy audio thread
#endif

static char AudioSoftVolume;            ///< flag use soft volume
static char AudioNormalize;             ///< flag use volume normalize
static char AudioCompression;           ///< flag use compress volume
static char AudioMute;                  ///< flag muted
static int AudioAmplifier;              ///< software volume factor
static int AudioNormalizeFactor;        ///< current normalize factor
static const int AudioMinNormalize = 100;   ///< min. normalize factor
static int AudioMaxNormalize;           ///< max. normalize factor
static int AudioCompressionFactor;      ///< current compression factor
static int AudioMaxCompression;         ///< max. compression factor
static int AudioStereoDescent;          ///< volume descent for stereo
static int AudioVolume;                 ///< current volume (0 .. 1000)
static int use_cec = 0;                 /// USe CEC Commands for volup and voldown
static int AudioCECDev=0;               /// Audio CEC Device Number
extern int VideoAudioDelay;             ///< import audio/video delay
extern int ConfigVideoFastSwitch;      ///< config fast channel switch

/// default ring buffer size ~2s 8ch 16bit (3 * 5 * 7 * 8)
static const unsigned AudioRingBufferSize = 3 * 5 * 7 * 8 * 1000;

static int AudioChannelsInHw[9];        ///< table which channels are supported
enum _audio_rates
{                                       ///< sample rates enumeration
    // HW: 32000 44100 48000 88200 96000 176400 192000
    // Audio32000,       ///< 32.0Khz
    Audio44100,                         ///< 44.1Khz
    Audio48000,                         ///< 48.0Khz
    // Audio88200,       ///< 88.2Khz
    // Audio96000,       ///< 96.0Khz
    // Audio176400,      ///< 176.4Khz
    Audio192000,                        ///< 192.0Khz
    AudioRatesMax                       ///< max index
};

/// table which rates are supported
//static int AudioRatesInHw[AudioRatesMax];

/// input to hardware channel matrix
static int AudioChannelMatrix[AudioRatesMax][9];

/// rates tables (must be sorted by frequency)
static const unsigned AudioRatesTable[AudioRatesMax] = {
    44100, 48000, 192000
};

//----------------------------------------------------------------------------
//  filter
//----------------------------------------------------------------------------

static const int AudioNormSamples = 4096;   ///< number of samples

#define AudioNormMaxIndex 128           ///< number of average values
/// average of n last sample blocks
static uint32_t AudioNormAverage[AudioNormMaxIndex];
static int AudioNormIndex;              ///< index into average table
static int AudioNormReady;              ///< index counter
static int AudioNormCounter;            ///< sample counter


/**
**	Audio normalizer.
**
**	@param samples	sample buffer
**	@param count	number of bytes in sample buffer
*/
static void AudioNormalizer(int16_t * samples, int count)
{
    int i;
    int l;
    int n;
    uint32_t avg;
    int factor;
    int16_t *data;

    // average samples
    l = count / AudioBytesProSample;
    data = samples;
    do {
        n = l;
        if (AudioNormCounter + n > AudioNormSamples) {
            n = AudioNormSamples - AudioNormCounter;
        }
        avg = AudioNormAverage[AudioNormIndex];
        for (i = 0; i < n; ++i) {
            int t;

            t = data[i];
            avg += (t * t) / AudioNormSamples;
        }
        AudioNormAverage[AudioNormIndex] = avg;
        AudioNormCounter += n;
        if (AudioNormCounter >= AudioNormSamples) {
            if (AudioNormReady < AudioNormMaxIndex) {
                AudioNormReady++;
            } else {
                avg = 0;
                for (i = 0; i < AudioNormMaxIndex; ++i) {
                    avg += AudioNormAverage[i] / AudioNormMaxIndex;
                }

                // calculate normalize factor
                if (avg > 0) {
                    factor = ((INT16_MAX / 8) * 1000U) / (uint32_t) sqrt(avg);
                    // smooth normalize
                    AudioNormalizeFactor = (AudioNormalizeFactor * 500 + factor * 500) / 1000;
                    if (AudioNormalizeFactor < AudioMinNormalize) {
                        AudioNormalizeFactor = AudioMinNormalize;
                    }
                    if (AudioNormalizeFactor > AudioMaxNormalize) {
                        AudioNormalizeFactor = AudioMaxNormalize;
                    }
                } else {
                    factor = 1000;
                }
                Debug(4, "audio/noramlize: avg %8d, fac=%6.3f, norm=%6.3f\n", avg, factor / 1000.0,
                    AudioNormalizeFactor / 1000.0);
            }

            AudioNormIndex = (AudioNormIndex + 1) % AudioNormMaxIndex;
            AudioNormCounter = 0;
            AudioNormAverage[AudioNormIndex] = 0U;
        }
        data += n;
        l -= n;
    } while (l > 0);

    // apply normalize factor
    for (i = 0; i < count / AudioBytesProSample; ++i) {
        int t;

        t = (samples[i] * AudioNormalizeFactor) / 1000;
        if (t < INT16_MIN) {
            t = INT16_MIN;
        } else if (t > INT16_MAX) {
            t = INT16_MAX;
        }
        samples[i] = t;
    }
}

/**
**	Reset normalizer.
*/
static void AudioResetNormalizer(void)
{
    int i;

    AudioNormCounter = 0;
    AudioNormReady = 0;
    for (i = 0; i < AudioNormMaxIndex; ++i) {
        AudioNormAverage[i] = 0U;
    }
    AudioNormalizeFactor = 1000;
}

/**
**	Audio compression.
**
**	@param samples	sample buffer
**	@param count	number of bytes in sample buffer
*/
static void AudioCompressor(int16_t * samples, int count)
{
    int max_sample;
    int i;
    int factor;

    // find loudest sample
    max_sample = 0;
    for (i = 0; i < count / AudioBytesProSample; ++i) {
        int t;

        t = abs(samples[i]);
        if (t > max_sample) {
            max_sample = t;
        }
    }

    // calculate compression factor
    if (max_sample > 0) {
        factor = (INT16_MAX * 1000) / max_sample;
        // smooth compression (FIXME: make configurable?)
        AudioCompressionFactor = (AudioCompressionFactor * 950 + factor * 50) / 1000;
        if (AudioCompressionFactor > factor) {
            AudioCompressionFactor = factor;    // no clipping
        }
        if (AudioCompressionFactor > AudioMaxCompression) {
            AudioCompressionFactor = AudioMaxCompression;
        }
    } else {
        return;                         // silent nothing todo
    }

    Debug(4, "audio/compress: max %5d, fac=%6.3f, com=%6.3f\n", max_sample, factor / 1000.0,
        AudioCompressionFactor / 1000.0);

    // apply compression factor
    for (i = 0; i < count / AudioBytesProSample; ++i) {
        int t;

        t = (samples[i] * AudioCompressionFactor) / 1000;
        if (t < INT16_MIN) {
            t = INT16_MIN;
        } else if (t > INT16_MAX) {
            t = INT16_MAX;
        }
        samples[i] = t;
    }
}

/**
**	Reset compressor.
*/
static void AudioResetCompressor(void)
{
    AudioCompressionFactor = 2000;
    if (AudioCompressionFactor > AudioMaxCompression) {
        AudioCompressionFactor = AudioMaxCompression;
    }
}

/**
**	Audio software amplifier.
**
**	@param samples	sample buffer
**	@param count	number of bytes in sample buffer
**
**	@todo FIXME: this does hard clipping
*/
static void AudioSoftAmplifier(int16_t * samples, int count)
{
    int i;

    // silence
    if (AudioMute || !AudioAmplifier) {
        memset(samples, 0, count);
        return;
    }

    for (i = 0; i < count / AudioBytesProSample; ++i) {
        int t;

        t = (samples[i] * AudioAmplifier) / 1000;
        if (t < INT16_MIN) {
            t = INT16_MIN;
        } else if (t > INT16_MAX) {
            t = INT16_MAX;
        }
        samples[i] = t;
    }
}

#ifdef USE_AUDIO_MIXER

/**
**	Upmix mono to stereo.
**
**	@param in	input sample buffer
**	@param frames	number of frames in sample buffer
**	@param out	output sample buffer
*/
static void AudioMono2Stereo(const int16_t * in, int frames, int16_t * out)
{
    int i;

    for (i = 0; i < frames; ++i) {
        int t;

        t = in[i];
        out[i * 2 + 0] = t;
        out[i * 2 + 1] = t;
    }
}

/**
**	Downmix stereo to mono.
**
**	@param in	input sample buffer
**	@param frames	number of frames in sample buffer
**	@param out	output sample buffer
*/
static void AudioStereo2Mono(const int16_t * in, int frames, int16_t * out)
{
    int i;

    for (i = 0; i < frames; i += 2) {
        out[i / 2] = (in[i + 0] + in[i + 1]) / 2;
    }
}

/**
**	Downmix surround to stereo.
**
**	ffmpeg L  R  C	Ls Rs		-> alsa L R  Ls Rs C
**	ffmpeg L  R  C	LFE Ls Rs	-> alsa L R  Ls Rs C  LFE
**	ffmpeg L  R  C	LFE Ls Rs Rl Rr	-> alsa L R  Ls Rs C  LFE Rl Rr
**
**	@param in	input sample buffer
**	@param in_chan	nr. of input channels
**	@param frames	number of frames in sample buffer
**	@param out	output sample buffer
*/
static void AudioSurround2Stereo(const int16_t * in, int in_chan, int frames, int16_t * out)
{
    while (frames--) {
        int l;
        int r;

        switch (in_chan) {
            case 3:                    // stereo or surround? =>stereo
                l = in[0] * 600;        // L
                r = in[1] * 600;        // R
                l += in[2] * 400;       // C
                r += in[2] * 400;
                break;
            case 4:                    // quad or surround? =>quad
                l = in[0] * 600;        // L
                r = in[1] * 600;        // R
                l += in[2] * 400;       // Ls
                r += in[3] * 400;       // Rs
                break;
            case 5:                    // 5.0
                l = in[0] * 500;        // L
                r = in[1] * 500;        // R
                l += in[2] * 200;       // Ls
                r += in[3] * 200;       // Rs
                l += in[4] * 300;       // C
                r += in[4] * 300;
                break;
            case 6:                    // 5.1
                l = in[0] * 400;        // L
                r = in[1] * 400;        // R
                l += in[2] * 200;       // Ls
                r += in[3] * 200;       // Rs
                l += in[4] * 300;       // C
                r += in[4] * 300;
                l += in[5] * 100;       // LFE
                r += in[5] * 100;
                break;
            case 7:                    // 7.0
                l = in[0] * 400;        // L
                r = in[1] * 400;        // R
                l += in[2] * 200;       // Ls
                r += in[3] * 200;       // Rs
                l += in[4] * 300;       // C
                r += in[4] * 300;
                l += in[5] * 100;       // RL
                r += in[6] * 100;       // RR
                break;
            case 8:                    // 7.1
                l = in[0] * 400;        // L
                r = in[1] * 400;        // R
                l += in[2] * 150;       // Ls
                r += in[3] * 150;       // Rs
                l += in[4] * 250;       // C
                r += in[4] * 250;
                l += in[5] * 100;       // LFE
                r += in[5] * 100;
                l += in[6] * 100;       // RL
                r += in[7] * 100;       // RR
                break;
            default:
                abort();
        }
        in += in_chan;

        out[0] = l / 1000;
        out[1] = r / 1000;
        out += 2;
    }
}

/**
**	Upmix @a in_chan channels to @a out_chan.
**
**	@param in	input sample buffer
**	@param in_chan	nr. of input channels
**	@param frames	number of frames in sample buffer
**	@param out	output sample buffer
**	@param out_chan	nr. of output channels
*/
static void AudioUpmix(const int16_t * in, int in_chan, int frames, int16_t * out, int out_chan)
{
    while (frames--) {
        int i;

        for (i = 0; i < in_chan; ++i) { // copy existing channels
            *out++ = *in++;
        }
        for (; i < out_chan; ++i) {     // silents missing channels
            *out++ = 0;
        }
    }
}

/**
**	Resample ffmpeg sample format to hardware format.
**
**	FIXME: use libswresample for this and move it to codec.
**	FIXME: ffmpeg to alsa conversion is already done in codec.c.
**
**	ffmpeg L  R  C	Ls Rs		-> alsa L R  Ls Rs C
**	ffmpeg L  R  C	LFE Ls Rs	-> alsa L R  Ls Rs C  LFE
**	ffmpeg L  R  C	LFE Ls Rs Rl Rr	-> alsa L R  Ls Rs C  LFE Rl Rr
**
**	@param in	input sample buffer
**	@param in_chan	nr. of input channels
**	@param frames	number of frames in sample buffer
**	@param out	output sample buffer
**	@param out_chan	nr. of output channels
*/
static void AudioResample(const int16_t * in, int in_chan, int frames, int16_t * out, int out_chan)
{
    switch (in_chan * 8 + out_chan) {
        case 1 * 8 + 1:
        case 2 * 8 + 2:
        case 3 * 8 + 3:
        case 4 * 8 + 4:
        case 5 * 8 + 5:
        case 6 * 8 + 6:
        case 7 * 8 + 7:
        case 8 * 8 + 8:                // input = output channels
            memcpy(out, in, frames * in_chan * AudioBytesProSample);
            break;
        case 2 * 8 + 1:
            AudioStereo2Mono(in, frames, out);
            break;
        case 1 * 8 + 2:
            AudioMono2Stereo(in, frames, out);
            break;
        case 3 * 8 + 2:
        case 4 * 8 + 2:
        case 5 * 8 + 2:
        case 6 * 8 + 2:
        case 7 * 8 + 2:
        case 8 * 8 + 2:
            AudioSurround2Stereo(in, in_chan, frames, out);
            break;
        case 5 * 8 + 6:
        case 3 * 8 + 8:
        case 5 * 8 + 8:
        case 6 * 8 + 8:
            AudioUpmix(in, in_chan, frames, out, out_chan);
            break;

        default:
            Error("audio: unsupported %d -> %d channels resample\n", in_chan, out_chan);
            // play silence
            memset(out, 0, frames * out_chan * AudioBytesProSample);
            break;
    }
}

#endif

//----------------------------------------------------------------------------
//  ring buffer
//----------------------------------------------------------------------------

#define AUDIO_RING_MAX 4                ///< number of audio ring buffers

/**
**	Audio ring buffer.
*/
typedef struct _audio_ring_ring_
{
    char FlushBuffers;                  ///< flag: flush buffers
    char Passthrough;                   ///< flag: use pass-through (AC-3, ...)
    int16_t PacketSize;                 ///< packet size
    unsigned HwSampleRate;              ///< hardware sample rate in Hz
    unsigned HwChannels;                ///< hardware number of channels
    unsigned InSampleRate;              ///< input sample rate in Hz
    unsigned InChannels;                ///< input number of channels
    int64_t PTS;                        ///< pts clock
    RingBuffer *RingBuffer;             ///< sample ring buffer
} AudioRingRing;

    /// ring of audio ring buffers
static AudioRingRing AudioRing[AUDIO_RING_MAX];
static int AudioRingWrite;              ///< audio ring write pointer
static int AudioRingRead;               ///< audio ring read pointer
static atomic_t AudioRingFilled;        ///< how many of the ring is used
static unsigned AudioStartThreshold;    ///< start play, if filled
char AudioTemp[200 * 1024];               /// Temp Buffer for playout

/**
**	Add sample-rate, number of channels change to ring.
**
**	@param sample_rate	sample-rate frequency
**	@param channels		number of channels
**	@param passthrough	use /pass-through (AC-3, ...) device
**
**	@retval -1	error
**	@retval 0	okay
**
**	@note this function shouldn't fail.  Checks are done during AudoInit.
*/
static int AudioRingAdd(unsigned sample_rate, int channels, int passthrough)
{
    unsigned u;

    // search supported sample-rates
    for (u = 0; u < AudioRatesMax; ++u) {
        if (AudioRatesTable[u] == sample_rate) {
            goto found;
        }
        if (AudioRatesTable[u] > sample_rate) {
            break;
        }
    }
    Error(_("audio: %dHz sample-rate unsupported\n"), sample_rate);
    return -1;                          // unsupported sample-rate

  found:
    if (!AudioChannelMatrix[u][channels]) {
        Error(_("audio: %d channels unsupported\n"), channels);
        return -1;                      // unsupported nr. of channels
    }

    if (atomic_read(&AudioRingFilled) == AUDIO_RING_MAX) {  // no free slot
        // FIXME: can wait for ring buffer empty
        Error(_("audio: out of ring buffers\n"));
        return -1;
    }
    AudioRingWrite = (AudioRingWrite + 1) % AUDIO_RING_MAX;

    AudioRing[AudioRingWrite].FlushBuffers = 0;
    AudioRing[AudioRingWrite].Passthrough = passthrough;
    AudioRing[AudioRingWrite].PacketSize = 0;
    AudioRing[AudioRingWrite].InSampleRate = sample_rate;
    AudioRing[AudioRingWrite].InChannels = channels;
    AudioRing[AudioRingWrite].HwSampleRate = sample_rate;
    AudioRing[AudioRingWrite].HwChannels = AudioChannelMatrix[u][channels];
    AudioRing[AudioRingWrite].PTS = AV_NOPTS_VALUE;
    RingBufferReset(AudioRing[AudioRingWrite].RingBuffer);

    Debug(3, "audio: %d ring buffer prepared\n", atomic_read(&AudioRingFilled) + 1);

    atomic_inc(&AudioRingFilled);

#ifdef USE_AUDIO_THREAD
    if (AudioThread) {
        // tell thread, that there is something todo
        AudioRunning = 1;
        pthread_cond_signal(&AudioStartCond);
        Debug(3, "audio: Start on AudioRingAdd\n");
    }
#endif

    return 0;
}

/**
**	Setup audio ring.
*/
static void AudioRingInit(void)
{
    int i;

    for (i = 0; i < AUDIO_RING_MAX; ++i) {
        // ~2s 8ch 16bit
        AudioRing[i].RingBuffer = RingBufferNew(AudioRingBufferSize);
    }
    atomic_set(&AudioRingFilled, 0);
}

/**
**	Cleanup audio ring.
*/
static void AudioRingExit(void)
{
    int i;

    for (i = 0; i < AUDIO_RING_MAX; ++i) {
        if (AudioRing[i].RingBuffer) {
            RingBufferDel(AudioRing[i].RingBuffer);
            AudioRing[i].RingBuffer = NULL;
        }
        AudioRing[i].HwSampleRate = 0;  // checked for valid setup
        AudioRing[i].InSampleRate = 0;
    }
    AudioRingRead = 0;
    AudioRingWrite = 0;
}



//============================================================================
//  A L S A
//============================================================================

//----------------------------------------------------------------------------
//  Alsa variables
//----------------------------------------------------------------------------

static snd_pcm_t *AlsaPCMHandle;        ///< alsa pcm handle
static char AlsaCanPause;               ///< hw supports pause
static int AlsaUseMmap;                 ///< use mmap

static snd_mixer_t *AlsaMixer;          ///< alsa mixer handle
static snd_mixer_elem_t *AlsaMixerElem; ///< alsa pcm mixer element
static int AlsaRatio;                   ///< internal -> mixer ratio * 1000

//----------------------------------------------------------------------------
//  alsa pcm
//----------------------------------------------------------------------------

/**
**	Play samples from ringbuffer.
**
**	Fill the kernel buffer, as much as possible.
**
**	@retval	0	ok
**	@retval 1	ring buffer empty
**	@retval -1	underrun error
*/
static int AlsaPlayRingbuffer(void)
{
    int first;

#ifdef PERFTEST
    static uint64_t mytime=0;
#endif

    first = 1;
    for (;;) {                          // loop for ring buffer wrap
        int avail;
        int n,m,wrap;
        int err;
        int frames;
        const void *p;

        // how many bytes can be written?
        n = snd_pcm_avail_update(AlsaPCMHandle);
        if (n < 0) {
            if (n == -EAGAIN) {
                continue;
            }
            Warning(_("audio: avail underrun error? '%s'\n"), snd_strerror(n));
            err = snd_pcm_recover(AlsaPCMHandle, n, 0);
            if (err >= 0) {
                continue;
            }
            Error(_("audio: snd_pcm_avail_update(): %s\n"), snd_strerror(n));
            return -1;
        }
        avail = snd_pcm_frames_to_bytes(AlsaPCMHandle, n);
        if (avail < 256) {              // too much overhead
            if (first) {
                // happens with broken alsa drivers
                if (AudioThread) {
                    if (!AudioAlsaDriverBroken) {
                        Error(_("audio: broken driver %d state '%s'\n"), avail,
                            snd_pcm_state_name(snd_pcm_state(AlsaPCMHandle)));
                    }
                    // try to recover
                    if (snd_pcm_state(AlsaPCMHandle)
                        == SND_PCM_STATE_PREPARED) {
                        if ((err = snd_pcm_start(AlsaPCMHandle)) < 0) {
                            Error(_("audio: snd_pcm_start(): %s\n"), snd_strerror(err));
                        }
                    }
                    usleep(5 * 1000);
                }
            }
            Debug(4, "audio: break state '%s'\n", snd_pcm_state_name(snd_pcm_state(AlsaPCMHandle)));
            break;
        }
        m = RingBufferUsedBytes(AudioRing[AudioRingRead].RingBuffer);
        n = RingBufferGetReadPointer(AudioRing[AudioRingRead].RingBuffer, &p);
        if (m != n) {
            wrap = 1;
            n = m;
        } else {   
            wrap = 0;
        }
        if (!n) {                       // ring buffer empty
            if (first) {                // only error on first loop
                Debug(4, "audio: empty buffers %d\n", avail);
                // ring buffer empty
                // AlsaLowWaterMark = 1;
                return 1;
            }
            return 0;
        }
        if (n < avail) {                // not enough bytes in ring buffer
            avail = n;
        }
        if (!avail) {                   // full or buffer empty
            break;
        }
        if (wrap) {
            avail = RingBufferRead(AudioRing[AudioRingRead].RingBuffer,&AudioTemp[0],avail);
            p = &AudioTemp[0];
        }
        // muting pass-through AC-3, can produce disturbance
        if (AudioMute || (AudioSoftVolume && !AudioRing[AudioRingRead].Passthrough)) {
            // FIXME: quick&dirty cast
            AudioSoftAmplifier((int16_t *) p, avail);
            // FIXME: if not all are written, we double amplify them
        }
        frames = snd_pcm_bytes_to_frames(AlsaPCMHandle, avail);
        
#ifdef DEBUG
        if (avail != snd_pcm_frames_to_bytes(AlsaPCMHandle, frames)) {
            Error(_("audio: bytes lost -> out of sync\n"));
        }
#endif

        for (;;) {
#ifdef PERFTEST1
    
   // if (((GetusTicks()-mytime) / 1000) > 20) {
        printf("Ring %d avail %d n %d frames %d Audio diff %ldms \n ",AudioRingRead,avail,n,frames,(GetusTicks()-mytime) / 1000);
   // }
    mytime = GetusTicks();
#endif
            if (AlsaUseMmap) {
                err = snd_pcm_mmap_writei(AlsaPCMHandle, p, frames);
            } else {
                err = snd_pcm_writei(AlsaPCMHandle, p, frames);
            }
            //Debug(3, "audio: wrote %d/%d frames\n", err, frames);
            if (err != frames) {
                if (err < 0) {
                    if (err == -EAGAIN) {
                        continue;
                    }
                    /*
                       if (err == -EBADFD) {
                       goto again;
                       }
                     */
                    Warning(_("audio: writei underrun error? '%s'\n"), snd_strerror(err));
                    err = snd_pcm_recover(AlsaPCMHandle, err, 0);
                    if (err >= 0) {
                        continue;
                    }
                    Error(_("audio: snd_pcm_writei failed: %s\n"), snd_strerror(err));
                    return -1;
                }
                // this could happen, if underrun happened
                Warning(_("audio: not all frames written\n"));
                avail = snd_pcm_frames_to_bytes(AlsaPCMHandle, err);
            }
            break;
        }
        if (!wrap) 
            RingBufferReadAdvance(AudioRing[AudioRingRead].RingBuffer, avail);
        first = 0;

    }
    return 0;
}

/**
**	Flush alsa buffers.
*/
static void AlsaFlushBuffers(void)
{
    if (AlsaPCMHandle) {
        int err;
        snd_pcm_state_t state;

        state = snd_pcm_state(AlsaPCMHandle);
        Debug(3, "audio: flush state %s\n", snd_pcm_state_name(state));
        if (state != SND_PCM_STATE_OPEN) {
            if ((err = snd_pcm_drop(AlsaPCMHandle)) < 0) {
                Error(_("audio: snd_pcm_drop(): %s\n"), snd_strerror(err));
            }
            // ****ing alsa crash, when in open state here
            if ((err = snd_pcm_prepare(AlsaPCMHandle)) < 0) {
                Error(_("audio: snd_pcm_prepare(): %s\n"), snd_strerror(err));
            }
        }
    }
}

#ifdef USE_AUDIO_THREAD

//----------------------------------------------------------------------------
//  thread playback
//----------------------------------------------------------------------------

/**
**	Alsa thread
**
**	Play some samples and return.
**
**	@retval	-1	error
**	@retval 0	underrun
**	@retval	1	running
*/
static int AlsaThread(void)
{
    int err;

    if (!AlsaPCMHandle) {
        usleep(24 * 1000);
        return -1;
    }
    for (;;) {
        if (AudioPaused) {
            return 1;
        }
        // wait for space in kernel buffers
        if ((err = snd_pcm_wait(AlsaPCMHandle, 24)) < 0) {
            Warning(_("audio: wait underrun error? '%s'\n"), snd_strerror(err));
            err = snd_pcm_recover(AlsaPCMHandle, err, 0);
            if (err >= 0) {
                continue;
            }
            Error(_("audio: snd_pcm_wait(): %s\n"), snd_strerror(err));
            usleep(24 * 1000);
            return -1;
        }
        break;
    }
    if (!err || AudioPaused) {          // timeout or some commands
        return 1;
    }

    if ((err = AlsaPlayRingbuffer())) { // empty or error
        snd_pcm_state_t state;

        if (err < 0) {                  // underrun error
            return -1;
        }

        state = snd_pcm_state(AlsaPCMHandle);
        if (state != SND_PCM_STATE_RUNNING) {
            Debug(3, "audio: stopping play '%s'\n", snd_pcm_state_name(state));
            return 0;
        }

        usleep(24 * 1000);              // let fill/empty the buffers
    }
    return 1;
}

#endif

//----------------------------------------------------------------------------

/**
**	Open alsa pcm device.
**
**	@param passthrough	use pass-through (AC-3, ...) device
*/
static snd_pcm_t *AlsaOpenPCM(int passthrough)
{
    const char *device;
    snd_pcm_t *handle;
    int err;

    // &&|| hell
    if (!(passthrough && ((device = AudioPassthroughDevice)
                || (device = getenv("ALSA_PASSTHROUGH_DEVICE"))))
        && !(device = AudioPCMDevice) && !(device = getenv("ALSA_DEVICE"))) {
        device = "default";
    }
    if (!AudioDoingInit) {              // reduce blabla during init
        Info(_("audio: using %sdevice '%s'\n"), passthrough ? "pass-through " : "", device);
    }
    //
    // for AC3 pass-through try to set the non-audio bit, use AES0=6
    //
    if (passthrough && AudioAppendAES) {
#if 0
        // FIXME: not yet finished
        char *buf;
        const char *s;
        int n;

        n = strlen(device);
        buf = alloca(n + sizeof(":AES0=6") + 1);
        strcpy(buf, device);
        if (!(s = strchr(buf, ':'))) {
            // no alsa parameters
            strcpy(buf + n, ":AES=6");
        }
        Debug(3, "audio: try '%s'\n", buf);
#endif
    }
    // open none blocking; if device is already used, we don't want wait
    if ((err = snd_pcm_open(&handle, device, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK)) < 0) {
        Error(_("audio: playback open '%s' error: %s\n"), device, snd_strerror(err));
        return NULL;
    }

    if ((err = snd_pcm_nonblock(handle, 0)) < 0) {
        Error(_("audio: can't set block mode: %s\n"), snd_strerror(err));
    }
    return handle;
}

/**
**	Initialize alsa pcm device.
**
**	@see AudioPCMDevice
*/
static void AlsaInitPCM(void)
{
    snd_pcm_t *handle;
    snd_pcm_hw_params_t *hw_params;
    int err;

    if (!(handle = AlsaOpenPCM(0))) {
        return;
    }
    // FIXME: pass-through and pcm out can support different features
    snd_pcm_hw_params_alloca(&hw_params);
    // choose all parameters
    if ((err = snd_pcm_hw_params_any(handle, hw_params)) < 0) {
        Error(_("audio: snd_pcm_hw_params_any: no configurations available: %s\n"), snd_strerror(err));
    }
    AlsaCanPause = snd_pcm_hw_params_can_pause(hw_params);
    Info(_("audio: supports pause: %s\n"), AlsaCanPause ? "yes" : "no");

    AlsaPCMHandle = handle;
}

//----------------------------------------------------------------------------
//  Alsa Mixer
//----------------------------------------------------------------------------

/**
**	Set alsa mixer volume (0-1000)
**
**	@param volume	volume (0 .. 1000)
*/
static void AlsaSetVolume(int volume)
{
#ifdef USE_CEC
    static int vol = -1;

    //char command[20];
    //uint64_t moses=GetusTicks();



    if (use_cec) {
        if (vol == -1 && volume) {
            vol = volume;
        }
        if (volume) {
            if (vol > volume) {
                cec_send_command(AudioCECDev,"down");

                //sprintf(command,"1%1d:44:42",AudioCECDev);
                //Debug(3,"CEC Command %s\n",command);
                //ProcessCommandTX(command);
            }
            if(vol < volume) {
                //sprintf(command,"1%1d:44:41",AudioCECDev);
                //Debug(3,"CEC Command %s\n",command);
                //ProcessCommandTX(command);

            }
            if(vol < volume) {

                cec_send_command(AudioCECDev,"up");
            }
            vol = volume;
            //printf("Vol %d Used CEC time %ld\n",vol,GetusTicks() - moses);
        }
    } else 
#endif
    if (AlsaMixer && AlsaMixerElem) {
        int v = (volume * AlsaRatio) / (1000 * 1000);
        snd_mixer_selem_set_playback_volume(AlsaMixerElem, 0, v);
        snd_mixer_selem_set_playback_volume(AlsaMixerElem, 1, v);
    }

}

/**
**	Initialize alsa mixer.
*/
static void AlsaInitMixer(void)
{
    const char *device;
    const char *channel;
    snd_mixer_t *alsa_mixer;
    snd_mixer_elem_t *alsa_mixer_elem;
    long alsa_mixer_elem_min;
    long alsa_mixer_elem_max;

    if (!(device = AudioMixerDevice)) {
        if (!(device = getenv("ALSA_MIXER"))) {
            device = "default";
        }
    }
    if (!(channel = AudioMixerChannel)) {
        if (!(channel = getenv("ALSA_MIXER_CHANNEL"))) {
            channel = "PCM";
        }
    }
    Debug(3, "audio: mixer %s - %s open\n", device, channel);
    snd_mixer_open(&alsa_mixer, 0);
    if (alsa_mixer && snd_mixer_attach(alsa_mixer, device) >= 0
        && snd_mixer_selem_register(alsa_mixer, NULL, NULL) >= 0 && snd_mixer_load(alsa_mixer) >= 0) {

        const char *const alsa_mixer_elem_name = channel;

        alsa_mixer_elem = snd_mixer_first_elem(alsa_mixer);
        while (alsa_mixer_elem) {
            const char *name;

            name = snd_mixer_selem_get_name(alsa_mixer_elem);
            if (!strcasecmp(name, alsa_mixer_elem_name)) {
                snd_mixer_selem_get_playback_volume_range(alsa_mixer_elem, &alsa_mixer_elem_min, &alsa_mixer_elem_max);
                AlsaRatio = 1000 * (alsa_mixer_elem_max - alsa_mixer_elem_min);
                Debug(3, "audio: PCM mixer found %ld - %ld ratio %d\n", alsa_mixer_elem_min, alsa_mixer_elem_max,
                    AlsaRatio);
                break;
            }

            alsa_mixer_elem = snd_mixer_elem_next(alsa_mixer_elem);
        }

        AlsaMixer = alsa_mixer;
        AlsaMixerElem = alsa_mixer_elem;
    } else {
        Error(_("audio: can't open mixer '%s'\n"), device);
    }
}

//----------------------------------------------------------------------------
//  Alsa API
//----------------------------------------------------------------------------

/**
**	Get alsa audio delay in time-stamps.
**
**	@returns audio delay in time-stamps.
**
**	@todo FIXME: handle the case no audio running
*/
static int64_t AlsaGetDelay(void)
{
    int err;
    snd_pcm_sframes_t delay;
    int64_t pts;

    // setup error
    if (!AlsaPCMHandle || !AudioRing[AudioRingRead].HwSampleRate) {
        return 0L;
    }
    // delay in frames in alsa + kernel buffers
    if ((err = snd_pcm_delay(AlsaPCMHandle, &delay)) < 0) {
        // Debug(3, "audio: no hw delay\n");
        delay = 0L;
#ifdef DEBUG
    } else if (snd_pcm_state(AlsaPCMHandle) != SND_PCM_STATE_RUNNING) {
        //Debug(3, "audio: %ld frames delay ok, but not running\n", delay);
#endif
    }
    Debug(4, "audio: %ld frames hw delay\n", delay);

    // delay can be negative, when underrun occur
    if (delay < 0) {
        delay = 0L;
    }

    pts = ((int64_t) delay * 90 * 1000) / AudioRing[AudioRingRead].HwSampleRate;

    return pts;
}

/**
**	Setup alsa audio for requested format.
**
**	@param freq		sample frequency
**	@param channels		number of channels
**	@param passthrough	use pass-through (AC-3, ...) device
**
**	@retval 0	everything ok
**	@retval 1	didn't support frequency/channels combination
**	@retval -1	something gone wrong
**
**	@todo FIXME: remove pointer for freq + channels
*/
static int AlsaSetup(int *freq, int *channels, int passthrough)
{
    snd_pcm_uframes_t buffer_size;
    snd_pcm_uframes_t period_size;
    int err;
    int delay;

    if (!AlsaPCMHandle) {               // alsa not running yet
        // FIXME: if open fails for fe. pass-through, we never recover
        return -1;
    }
    if (!AudioAlsaNoCloseOpen) {        // close+open to fix HDMI no sound bug
        snd_pcm_t *handle;

        handle = AlsaPCMHandle;
        // no lock needed, thread exit in main loop only
        //Debug(3, "audio: %s [\n", __FUNCTION__);
        AlsaPCMHandle = NULL;           // other threads should check handle
        snd_pcm_close(handle);
        if (AudioAlsaCloseOpenDelay) {
            usleep(50 * 1000);          // 50ms delay for alsa recovery
        }
        // FIXME: can use multiple retries
        if (!(handle = AlsaOpenPCM(passthrough))) {
            return -1;
        }
        AlsaPCMHandle = handle;
        //Debug(3, "audio: %s ]\n", __FUNCTION__);
    }

    for (;;) {
        if ((err =
                snd_pcm_set_params(AlsaPCMHandle, SND_PCM_FORMAT_S16,
                    AlsaUseMmap ? SND_PCM_ACCESS_MMAP_INTERLEAVED : SND_PCM_ACCESS_RW_INTERLEAVED, *channels, *freq, 1,
                    96 * 1000))) {
            // try reduced buffer size (needed for sunxi)
            // FIXME: alternativ make this configurable
            if ((err =
                    snd_pcm_set_params(AlsaPCMHandle, SND_PCM_FORMAT_S16,
                        AlsaUseMmap ? SND_PCM_ACCESS_MMAP_INTERLEAVED : SND_PCM_ACCESS_RW_INTERLEAVED, *channels,
                        *freq, 1, 72 * 1000))) {

                /*
                   if ( err == -EBADFD ) {
                   snd_pcm_close(AlsaPCMHandle);
                   AlsaPCMHandle = NULL;
                   continue;
                   }
                 */

                if (!AudioDoingInit) {
                    Error(_("audio: set params error: %s\n"), snd_strerror(err));
                }
                // FIXME: must stop sound, AudioChannels ... invalid
                return -1;
            }
        }
        break;
    }

    // this is disabled, no advantages!
    if (0) {                            // no underruns allowed, play silence
        snd_pcm_sw_params_t *sw_params;
        snd_pcm_uframes_t boundary;

        snd_pcm_sw_params_alloca(&sw_params);
        err = snd_pcm_sw_params_current(AlsaPCMHandle, sw_params);
        if (err < 0) {
            Error(_("audio: snd_pcm_sw_params_current failed: %s\n"), snd_strerror(err));
        }
        if ((err = snd_pcm_sw_params_get_boundary(sw_params, &boundary)) < 0) {
            Error(_("audio: snd_pcm_sw_params_get_boundary failed: %s\n"), snd_strerror(err));
        }
        Debug(4, "audio: boundary %lu frames\n", boundary);
        if ((err = snd_pcm_sw_params_set_stop_threshold(AlsaPCMHandle, sw_params, boundary)) < 0) {
            Error(_("audio: snd_pcm_sw_params_set_silence_size failed: %s\n"), snd_strerror(err));
        }
        if ((err = snd_pcm_sw_params_set_silence_size(AlsaPCMHandle, sw_params, boundary)) < 0) {
            Error(_("audio: snd_pcm_sw_params_set_silence_size failed: %s\n"), snd_strerror(err));
        }
        if ((err = snd_pcm_sw_params(AlsaPCMHandle, sw_params)) < 0) {
            Error(_("audio: snd_pcm_sw_params failed: %s\n"), snd_strerror(err));
        }
    }
    // update buffer

    snd_pcm_get_params(AlsaPCMHandle, &buffer_size, &period_size);
    Debug(3, "audio: buffer size %lu %zdms, period size %lu %zdms\n", buffer_size,
        snd_pcm_frames_to_bytes(AlsaPCMHandle, buffer_size) * 1000 / (*freq * *channels * AudioBytesProSample),
        period_size, snd_pcm_frames_to_bytes(AlsaPCMHandle,
            period_size) * 1000 / (*freq * *channels * AudioBytesProSample));
    Debug(3, "audio: state %s\n", snd_pcm_state_name(snd_pcm_state(AlsaPCMHandle)));

    AudioStartThreshold = snd_pcm_frames_to_bytes(AlsaPCMHandle, period_size);
    // buffer time/delay in ms
    delay = AudioBufferTime;
    if (VideoAudioDelay > 0) {
        delay += VideoAudioDelay / 90;
    }
    if (AudioStartThreshold < (*freq * *channels * AudioBytesProSample * delay) / 1000U) {
        AudioStartThreshold = (*freq * *channels * AudioBytesProSample * delay) / 1000U;
    }
    // no bigger, than 1/3 the buffer
    if (AudioStartThreshold > AudioRingBufferSize / 3) {
        AudioStartThreshold = AudioRingBufferSize / 3;
    }
    if (!AudioDoingInit) {
        Info(_("audio: start delay %ums\n"), (AudioStartThreshold * 1000)
            / (*freq * *channels * AudioBytesProSample));
    }
    return 0;
}

/**
**	Play audio.
*/
static void AlsaPlay(void)
{
    int err;

    if (AlsaCanPause) {
        if ((err = snd_pcm_pause(AlsaPCMHandle, 0))) {
            Error(_("audio: snd_pcm_pause(): %s\n"), snd_strerror(err));
        }
    } else {
        if ((err = snd_pcm_prepare(AlsaPCMHandle)) < 0) {
            Error(_("audio: snd_pcm_prepare(): %s\n"), snd_strerror(err));
        }
    }
#ifdef DEBUG
    if (snd_pcm_state(AlsaPCMHandle) == SND_PCM_STATE_PAUSED) {
        Error(_("audio: still paused\n"));
    }
#endif
}

/**
**	Pause audio.
*/
static void AlsaPause(void)
{
    int err;

    if (AlsaCanPause) {
        if ((err = snd_pcm_pause(AlsaPCMHandle, 1))) {
            Error(_("snd_pcm_pause(): %s\n"), snd_strerror(err));
        }
    } else {
        if ((err = snd_pcm_drop(AlsaPCMHandle)) < 0) {
            Error(_("snd_pcm_drop(): %s\n"), snd_strerror(err));
        }
    }
}

/**
**	Empty log callback
*/
static void AlsaNoopCallback( __attribute__((unused))
    const char *file, __attribute__((unused))
    int line, __attribute__((unused))
    const char *function, __attribute__((unused))
    int err, __attribute__((unused))
    const char *fmt, ...)
{
}

/**
**	Initialize alsa audio output module.
*/
static void AlsaInit(void)
{
#ifdef DEBUG
    (void)AlsaNoopCallback;
#else
    // disable display of alsa error messages
    snd_lib_error_set_handler(AlsaNoopCallback);
#endif

    AlsaInitPCM();
    AlsaInitMixer();
}

/**
**	Cleanup alsa audio output module.
*/
static void AlsaExit(void)
{
    if (AlsaPCMHandle) {
        snd_pcm_close(AlsaPCMHandle);
        AlsaPCMHandle = NULL;
    }
    if (AlsaMixer) {
        snd_mixer_close(AlsaMixer);
        AlsaMixer = NULL;
        AlsaMixerElem = NULL;
    }
}

/**
**	Alsa module.
*/
static const AudioModule AlsaModule = {
    .Name = "alsa",
#ifdef USE_AUDIO_THREAD
    .Thread = AlsaThread,
#endif
    .FlushBuffers = AlsaFlushBuffers,
    .GetDelay = AlsaGetDelay,
    .SetVolume = AlsaSetVolume,
    .Setup = AlsaSetup,
    .Play = AlsaPlay,
    .Pause = AlsaPause,
    .Init = AlsaInit,
    .Exit = AlsaExit,
};


//============================================================================
//  Noop
//============================================================================

/**
**	Get audio delay in time stamps.
**
**	@returns audio delay in time stamps.
*/
static int64_t NoopGetDelay(void)
{
    return 0L;
}

/**
**	Set mixer volume (0-1000)
**
**	@param volume	volume (0 .. 1000)
*/
static void NoopSetVolume( __attribute__((unused))
    int volume)
{
}

/**
**	Noop setup.
**
**	@param freq		sample frequency
**	@param channels		number of channels
**	@param passthrough	use pass-through (AC-3, ...) device
*/
static int NoopSetup( __attribute__((unused))
    int *channels, __attribute__((unused))
    int *freq, __attribute__((unused))
    int passthrough)
{
    return -1;
}

/**
**	Noop void
*/
static void NoopVoid(void)
{
}

/**
**	Noop module.
*/
static const AudioModule NoopModule = {
    .Name = "noop",
    .FlushBuffers = NoopVoid,
    .GetDelay = NoopGetDelay,
    .SetVolume = NoopSetVolume,
    .Setup = NoopSetup,
    .Play = NoopVoid,
    .Pause = NoopVoid,
    .Init = NoopVoid,
    .Exit = NoopVoid,
};

//----------------------------------------------------------------------------
//  thread playback
//----------------------------------------------------------------------------

#ifdef USE_AUDIO_THREAD

/**
**	Prepare next ring buffer.
*/
static int AudioNextRing(void)
{
    int passthrough;
    int sample_rate;
    int channels;
    size_t used;

    // update audio format
    // not always needed, but check if needed is too complex
    passthrough = AudioRing[AudioRingRead].Passthrough;
    sample_rate = AudioRing[AudioRingRead].HwSampleRate;
    channels = AudioRing[AudioRingRead].HwChannels;
    if (channels == 0) channels = 2;
    if (sample_rate == 0) sample_rate = 44100;
    if (AudioUsedModule->Setup(&sample_rate, &channels, passthrough)) {
        Error(_("audio: can't set channels %d sample-rate %dHz\n"), channels, sample_rate);
        // FIXME: handle error
        AudioRing[AudioRingRead].HwSampleRate = 0;
        AudioRing[AudioRingRead].InSampleRate = 0;
        return -1;
    }

    AudioSetVolume(AudioVolume);        // update channel delta
    AudioResetCompressor();
    AudioResetNormalizer();

    Debug(3, "audio: a/v next buf(%d,%4zdms)\n", atomic_read(&AudioRingFilled),
        (RingBufferUsedBytes(AudioRing[AudioRingRead].RingBuffer) * 1000)
        / (AudioRing[AudioRingWrite].HwSampleRate * AudioRing[AudioRingWrite].HwChannels * AudioBytesProSample));

    // stop, if not enough in next buffer
    used = RingBufferUsedBytes(AudioRing[AudioRingRead].RingBuffer);
    if (AudioStartThreshold * 4 < used || (AudioVideoIsReady && AudioStartThreshold < used)) {
        return 0;
    }
    return 1;
}

extern bool isFirstVideoPacket;
/**
**	Audio play thread.
**
**	@param dummy	unused thread argument
*/
static void *AudioPlayHandlerThread(void *dummy)
{
    Debug(3, "audio: play thread started\n");
    prctl(PR_SET_NAME, "cuvid audio", 0, 0, 0);

    for (;;) {
        // check if we should stop the thread
        if (AudioThreadStop) {
            Debug(3, "audio: play thread stopped\n");
            return PTHREAD_CANCELED;
        }

        Debug(3, "audio: wait on start condition\n");
        pthread_mutex_lock(&AudioMutex);
        AudioRunning = 0;

        do {
            pthread_cond_wait(&AudioStartCond, &AudioMutex);
            // cond_wait can return, without signal!
        } while (!AudioRunning);
        pthread_mutex_unlock(&AudioMutex);

        Debug(3, "audio: ----> %dms %d start\n", (AudioUsedBytes() * 1000)
            / (!AudioRing[AudioRingWrite].HwSampleRate + !AudioRing[AudioRingWrite].HwChannels +
                AudioRing[AudioRingWrite].HwSampleRate * AudioRing[AudioRingWrite].HwChannels * AudioBytesProSample),
            AudioUsedBytes());

        do {
            int filled;
            int read;
            int flush;
            int err;
            int i;

            // check if we should stop the thread
            if (AudioThreadStop) {
                Debug(3, "audio: play thread stopped\n");
                return PTHREAD_CANCELED;
            }
            // look if there is a flush command in the queue
            flush = 0;
            filled = atomic_read(&AudioRingFilled);
            read = AudioRingRead;
            i = filled;
            while (i--) {
                read = (read + 1) % AUDIO_RING_MAX;
                if (AudioRing[read].FlushBuffers) {
                    AudioRing[read].FlushBuffers = 0;
                    AudioRingRead = read;
                    // handle all flush in queue
                    flush = filled - i;
                }
            }

            if (flush) {
                Debug(3, "audio: flush %d ring buffer(s)\n", flush);
                AudioUsedModule->FlushBuffers();
                atomic_sub(flush, &AudioRingFilled);
                if (AudioNextRing()) {
                    break;
                }
            }
            // try to play some samples
            err = 0;
            if (RingBufferUsedBytes(AudioRing[AudioRingRead].RingBuffer)) {
                err = AudioUsedModule->Thread();
            }
            // underrun, check if new ring buffer is available
            if (!err) {
                int passthrough;
                int sample_rate;
                int channels;
                int old_passthrough;
                int old_sample_rate;
                int old_channels;

                // underrun, and no new ring buffer, goto sleep.
                if (!atomic_read(&AudioRingFilled)) {
                    Debug(3, "audio: HandlerThread Underrun with no new data\n");
                     if (!ConfigVideoFastSwitch)
                        isFirstVideoPacket = 1;
                    break;
                }

                Debug(3, "audio: next ring buffer\n");
                old_passthrough = AudioRing[AudioRingRead].Passthrough;
                old_sample_rate = AudioRing[AudioRingRead].HwSampleRate;
                old_channels = AudioRing[AudioRingRead].HwChannels;

                atomic_dec(&AudioRingFilled);
                AudioRingRead = (AudioRingRead + 1) % AUDIO_RING_MAX;

                passthrough = AudioRing[AudioRingRead].Passthrough;
                sample_rate = AudioRing[AudioRingRead].HwSampleRate;
                channels = AudioRing[AudioRingRead].HwChannels;
                Debug(3, "audio: thread channels %d frequency %dHz %s\n", channels, sample_rate,
                    passthrough ? "pass-through" : "");
                // audio config changed?
                if (old_passthrough != passthrough || old_sample_rate != sample_rate || old_channels != channels) {
                    // FIXME: wait for buffer drain
                    if (AudioNextRing()) {
                        Debug(3, "audio: HandlerThread break on nextring");
                        break;
                    }
                } else {
                    AudioResetCompressor();
                    AudioResetNormalizer();
                }
            }
            // FIXME: check AudioPaused ...Thread()
            if (AudioPaused) {
                Debug(3, "audio: HandlerThread break on paused");
                break;
            }
        } while (AudioRing[AudioRingRead].HwSampleRate);
    }
    return dummy;
}

/**
**	Initialize audio thread.
*/
static void AudioInitThread(void)
{
    AudioThreadStop = 0;
    pthread_mutex_init(&AudioMutex, NULL);
    pthread_cond_init(&AudioStartCond, NULL);
    pthread_create(&AudioThread, NULL, AudioPlayHandlerThread, NULL);
    pthread_setname_np(AudioThread, "softhddev audio");
}

/**
**	Cleanup audio thread.
*/
static void AudioExitThread(void)
{
    void *retval;

    Debug(3, "audio: %s\n", __FUNCTION__);

    if (AudioThread) {
        AudioThreadStop = 1;
        AudioRunning = 1;               // wakeup thread, if needed
        pthread_cond_signal(&AudioStartCond);
        if (pthread_join(AudioThread, &retval) || retval != PTHREAD_CANCELED) {
            Error(_("audio: can't cancel play thread\n"));
        }
        pthread_cond_destroy(&AudioStartCond);
        pthread_mutex_destroy(&AudioMutex);
        AudioThread = 0;
    }
}

#endif

//----------------------------------------------------------------------------
//----------------------------------------------------------------------------

    /**
    **	Table of all audio modules.
    */
static const AudioModule *AudioModules[] = {
#ifdef USE_ALSA
    &AlsaModule,
#endif
#ifdef USE_OSS
    &OssModule,
#endif
    &NoopModule,
};

void AudioDelayms(int delayms)
{
    int count;
    unsigned char *p;

#ifdef DEBUG
    printf("Try Delay Audio for %d ms  Samplerate %d Channels %d bps %d\n", delayms,
        AudioRing[AudioRingWrite].HwSampleRate, AudioRing[AudioRingWrite].HwChannels, AudioBytesProSample);
#endif

    count =
        delayms * AudioRing[AudioRingWrite].HwSampleRate * AudioRing[AudioRingWrite].HwChannels * AudioBytesProSample /
        1000;

    if (delayms < 5000 && delayms > 0) {    // not more than 5seconds
        p = calloc(1, count);
        RingBufferWrite(AudioRing[AudioRingWrite].RingBuffer, p, count);
        free(p);
    }
}

extern uint64_t FirstVPTS;
#ifdef PERFTEST
extern uint64_t firstapts;
#endif
extern int SetCurrentPCR(int , uint64_t );
extern int hasVideo;
extern uint64_t last_time;

/**
**	Place samples in audio output queue.
**
**	@param samples	sample buffer
**	@param count	number of bytes in sample buffer
*/
void AudioEnqueue(const void *samples, int count)
{
    size_t n;
    int16_t *buffer;
    
    uint64_t vpts;
   
    
#ifdef PERFTEST1
    static uint64_t mytime;
    if (((GetusTicks()-mytime) / 1000) > 120 || count < 4608) {
        printf("Count %d Enqueue diff %ldms\n",count,(GetusTicks()-mytime) / 1000);
    }
    mytime = GetusTicks();
#endif

#ifdef noDEBUG
    static uint32_t last_tick;
    uint32_t tick;

    tick = GetMsTicks();
    if (tick - last_tick > 101) {
        Debug(3, "audio: enqueue %4d %dms\n", count, tick - last_tick);
    }
    last_tick = tick;
#endif

    if (!AudioRing[AudioRingWrite].HwSampleRate) {
        Debug(3, "audio: enqueue not ready\n");
        return;                         // no setup yet
    }
    // save packet size
    if (!AudioRing[AudioRingWrite].PacketSize) {
        AudioRing[AudioRingWrite].PacketSize = count;
        Debug(3, "audio: a/v packet size %d bytes\n", count);
    }
    // audio sample modification allowed and needed?
    buffer = (void *)samples;
    if (!AudioRing[AudioRingWrite].Passthrough && (AudioCompression || AudioNormalize
            || AudioRing[AudioRingWrite].InChannels != AudioRing[AudioRingWrite].HwChannels)) {
        int frames;

        // resample into ring-buffer is too complex in the case of a roundabout
        // just use a temporary buffer
        frames = count / (AudioRing[AudioRingWrite].InChannels * AudioBytesProSample);
        buffer = alloca(frames * AudioRing[AudioRingWrite].HwChannels * AudioBytesProSample);
#ifdef USE_AUDIO_MIXER
        // Convert / resample input to hardware format
        AudioResample(samples, AudioRing[AudioRingWrite].InChannels, frames, buffer,
            AudioRing[AudioRingWrite].HwChannels);
#else
#ifdef DEBUG
        if (AudioRing[AudioRingWrite].InChannels != AudioRing[AudioRingWrite].HwChannels) {
            Debug(3, "audio: internal failure channels mismatch\n");
            return;
        }
#endif
        memcpy(buffer, samples, count);
#endif
        count = frames * AudioRing[AudioRingWrite].HwChannels * AudioBytesProSample;

        if (AudioCompression) {         // in place operation
            AudioCompressor(buffer, count);
        }
        if (AudioNormalize) {           // in place operation
            AudioNormalizer(buffer, count);
        }
    }

    n = RingBufferWrite(AudioRing[AudioRingWrite].RingBuffer, buffer, count);
    if (n != (size_t)count) {
        Error(_("audio: can't place %d samples in ring buffer\n"), count);
        // too many bytes are lost
        // FIXME: caller checks buffer full.
        // FIXME: should skip more, longer skip, but less often?
        // FIXME: round to channel + sample border
    }

    if (!AudioRunning) {                // check, if we can start the thread
        int skip = 0;

        n = RingBufferUsedBytes(AudioRing[AudioRingWrite].RingBuffer);

        if (!ConfigVideoFastSwitch && hasVideo) {
            vpts = FirstVPTS;

            if (vpts == AV_NOPTS_VALUE || AudioRing[AudioRingWrite].PTS == AV_NOPTS_VALUE || !vpts) {
                //usleep(1000);
                skip = n;   // Clear all audio until video is avail
                //printf("%d No PTS in %ld ms \n",n,(GetusTicks() - last_time) / 1000);
            }
            else if ((uint64_t)AudioRing[AudioRingWrite].PTS  < vpts) {
                skip = n;    // Clear Audio until Video PTS
          
#ifdef PERFTEST
                static int sw=0;
                if (!sw) {
                   //printf("%ld too small PTS apts  %#012" PRIx64 " vpts  %#012" PRIx64 " in %ld ms \n",n,AudioRing[AudioRingWrite].PTS,vpts ,(GetusTicks() - last_time) / 1000);
                   printf("Audio vorlauf ist %ldms \n",(vpts - AudioRing[AudioRingWrite].PTS) / 90);
                   sw = 1;
                }
#endif
            }
            //else {
                //printf("store %ld of %d Audio in %ld ms \n",n,AudioStartThreshold,(GetusTicks() - last_time) / 1000);
            //}
#if 0
            else {
                //printf("AudioEnque: SetCurrentPCR %#012" PRIx64 "\n", AudioRing[AudioRingWrite].PTS - AudioBufferTime * 90 + VideoAudioDelay);
                int i = 10;
                while (SetCurrentPCR(0, (uint64_t)(AudioRing[AudioRingWrite].PTS - AudioBufferTime * 90 + VideoAudioDelay )) == 2 && i--) {
                    usleep(3000);
                }
#ifdef PERFTEST
                
                firstapts =  (uint64_t)(AudioRing[AudioRingWrite].PTS - AudioBufferTime * 90 + VideoAudioDelay );
                printf("AVR %d new firstapts  %#012" PRIx64 " \n",AudioVideoIsReady,firstapts);
#endif
            }
#endif
        }
        
        if (ConfigVideoFastSwitch) {
            skip = AudioSkip;
        }
        
        if (skip) {
            if (n < (unsigned)skip) {
                skip = n;
            }
            AudioSkip -= skip;
            RingBufferReadAdvance(AudioRing[AudioRingWrite].RingBuffer, skip);
            n = RingBufferUsedBytes(AudioRing[AudioRingWrite].RingBuffer);
        }
        // forced start or enough video + audio buffered
        // for some exotic channels * 4 too small

        if (AudioStartThreshold * (ConfigVideoFastSwitch ? 1.8 : 4)  < n
            ||  (AudioVideoIsReady && AudioStartThreshold < n)) {
            // restart play-back
            // no lock needed, can wakeup next time
            AudioRunning = 1;
            FirstVPTS = 0;
            if (!ConfigVideoFastSwitch) {
                //printf("AudioEnque: SetCurrentPCR %#012" PRIx64 "\n", AudioRing[AudioRingWrite].PTS - AudioBufferTime * 90 + VideoAudioDelay);
                int i = 10;
                while (SetCurrentPCR(0, (uint64_t)(AudioRing[AudioRingWrite].PTS - AudioBufferTime * 90 + VideoAudioDelay )) == 2 && i--) {
                    usleep(3000);
                }
            }
#ifdef PERFTEST
                firstapts =  (uint64_t)(AudioRing[AudioRingWrite].PTS - AudioBufferTime * 90 + VideoAudioDelay );
                //printf("AVR %d new firstapts  %#012" PRIx64 " \n",AudioVideoIsReady,firstapts);
                printf("Set PCR PTS in %ld ms \n",(GetusTicks() - last_time) / 1000);
                sw = 0;
#endif
            pthread_cond_signal(&AudioStartCond);
            Debug(3, "audio: Start on AudioEnque Threshold %d n %ld IsReady %d\n", AudioStartThreshold, n, AudioVideoIsReady);
        }

    }
    // Update audio clock (stupid gcc developers thinks INT64_C is unsigned)
    if (AudioRing[AudioRingWrite].PTS != (int64_t) AV_NOPTS_VALUE) {
        AudioRing[AudioRingWrite].PTS += ((int64_t) count * 90 * 1000)
            / (AudioRing[AudioRingWrite].HwSampleRate * AudioRing[AudioRingWrite].HwChannels * AudioBytesProSample);
    }
}

/**
**	Video is ready.
**
**	@param pts	video presentation timestamp
*/
void AudioVideoReady(uint64_t pts)
{
    //int64_t audio_pts;
    //size_t used;

    if (AudioVideoIsReady) {
        return;
    }

    if (!pts || pts == (int64_t) AV_NOPTS_VALUE) {
        Debug(3, "audio: a/v start, no valid video\n");
        return;
    }
    // no valid audio known
    if (!AudioRing[AudioRingWrite].HwSampleRate || !AudioRing[AudioRingWrite].HwChannels
        || AudioRing[AudioRingWrite].PTS == (int64_t) AV_NOPTS_VALUE) {
        Debug(3, "audio: a/v start, no valid audio\n");
        //AudioVideoIsReady = 1;
        return;
    }
    // Audio.PTS = next written sample time stamp
#if 0
    used = RingBufferUsedBytes(AudioRing[AudioRingWrite].RingBuffer);
    audio_pts =
        AudioRing[AudioRingWrite].PTS -
        (used * 90 * 1000) / (AudioRing[AudioRingWrite].HwSampleRate * AudioRing[AudioRingWrite].HwChannels *
        AudioBytesProSample);

    Debug(3, "audio: a/v sync buf(%d,%4zdms) %s | %s = %dms %s\n", atomic_read(&AudioRingFilled),
        (used * 1000) / (AudioRing[AudioRingWrite].HwSampleRate * AudioRing[AudioRingWrite].HwChannels *
            AudioBytesProSample), Timestamp2String(pts), Timestamp2String(audio_pts), (int)(pts - audio_pts) / 90,
        AudioRunning ? "running" : "ready");

    if (!AudioRunning) {
        int skip;

        // buffer ~15 video frames
        // FIXME: HDTV can use smaller video buffer
        skip = pts - 0 * 20 * 90 - AudioBufferTime * 90 - audio_pts + VideoAudioDelay;
        printf("AVR skip %d used %ld\n",skip,used);
#ifdef DEBUG
        //      fprintf(stderr, "a/v-diff %dms a/v-delay %dms skip %dms  Audiobuffer %d\n", (int)(pts - audio_pts) / 90, VideoAudioDelay / 90, skip / 90,AudioBufferTime);
#endif
        // guard against old PTS
        if (skip > 0 && skip < 4000 * 90) {
            skip = (((int64_t) skip * AudioRing[AudioRingWrite].HwSampleRate) / (1000 * 90))
                * AudioRing[AudioRingWrite].HwChannels * AudioBytesProSample;
            // FIXME: round to packet size
            printf("AVR1 skip %d uses %ld\n",skip,used);
            if ((unsigned)skip > used) {
                AudioSkip = skip - used;
                skip = used;
            }
            Debug(3, "audio: sync advance %dms %d/%zd  Rest %d\n",
                (skip * 1000) / (AudioRing[AudioRingWrite].HwSampleRate * AudioRing[AudioRingWrite].HwChannels *
                    AudioBytesProSample), skip, used, AudioSkip);
            RingBufferReadAdvance(AudioRing[AudioRingWrite].RingBuffer, skip);

            used = RingBufferUsedBytes(AudioRing[AudioRingWrite].RingBuffer);
        } else {
            Debug(3, "No audio skip -> should skip %d\n", skip / 90);
        }
#if 0
        // FIXME: skip<0 we need bigger audio buffer
        // enough video + audio buffered
        if (AudioStartThreshold < used) {
            AudioRunning = 1;
            pthread_cond_signal(&AudioStartCond);
            Debug(3, "Start on AudioVideoReady\n");
        }
#endif
    } else {
#endif
        AudioRunning = 0;
    //}
    Debug(3,"audio: AudioVideoIsReady");
    AudioVideoIsReady = 1;

}

/**
**	Flush audio buffers.
*/
void AudioFlushBuffers(void)
{
    int old;
    int i;

    if (atomic_read(&AudioRingFilled) >= AUDIO_RING_MAX) {
        // wait for space in ring buffer, should never happen
        for (i = 0; i < 24 * 2; ++i) {
            if (atomic_read(&AudioRingFilled) < AUDIO_RING_MAX) {
                break;
            }
            Debug(3, "audio: flush out of ring buffers\n");
            usleep(1 * 1000);           // avoid hot polling
        }
        if (atomic_read(&AudioRingFilled) >= AUDIO_RING_MAX) {
            // FIXME: We can set the flush flag in the last wrote ring buffer
            Error(_("audio: flush out of ring buffers\n"));
            return;
        }
    }

    old = AudioRingWrite;
    AudioRingWrite = (AudioRingWrite + 1) % AUDIO_RING_MAX;
    AudioRing[AudioRingWrite].FlushBuffers = 1;
    AudioRing[AudioRingWrite].Passthrough = AudioRing[old].Passthrough;
    AudioRing[AudioRingWrite].HwSampleRate = AudioRing[old].HwSampleRate;
    AudioRing[AudioRingWrite].HwChannels = AudioRing[old].HwChannels;
    AudioRing[AudioRingWrite].InSampleRate = AudioRing[old].InSampleRate;
    AudioRing[AudioRingWrite].InChannels = AudioRing[old].InChannels;
    AudioRing[AudioRingWrite].PTS = AV_NOPTS_VALUE;
    RingBufferReadAdvance(AudioRing[AudioRingWrite].RingBuffer,
        RingBufferUsedBytes(AudioRing[AudioRingWrite].RingBuffer));
    Debug(3, "audio: reset video ready\n");
    AudioVideoIsReady = 0;
    AudioSkip = 0;

    atomic_inc(&AudioRingFilled);

    // FIXME: wait for flush complete needed?
    for (i = 0; i < 24 * 2; ++i) {
        if (!AudioRunning) {            // wakeup thread to flush buffers
            AudioRunning = 1;
            pthread_cond_signal(&AudioStartCond);
            Debug(3, "audio: Start on Flush\n");
        }
        // FIXME: waiting on zero isn't correct, but currently works
        if (!atomic_read(&AudioRingFilled)) {
            break;
        }
        usleep(1 * 1000);               // avoid hot polling
    }
    Debug(3, "audio: audio flush %dms\n", i);
}

/**
**	Call back to play audio polled.
*/
void AudioPoller(void)
{
    // FIXME: write poller
}

/**
**	Get free bytes in audio output.
*/
int AudioFreeBytes(void)
{
    return AudioRing[AudioRingWrite].RingBuffer ? RingBufferFreeBytes(AudioRing[AudioRingWrite].RingBuffer)
        : INT32_MAX;
}

/**
**	Get used bytes in audio output.
*/
int AudioUsedBytes(void)
{
    // FIXME: not correct, if multiple buffer are in use
    return AudioRing[AudioRingWrite].RingBuffer ? RingBufferUsedBytes(AudioRing[AudioRingWrite].RingBuffer) : 0;
}

/**
**	Get audio delay in time stamps.
**
**	@returns audio delay in time stamps.
*/
int64_t AudioGetDelay(void)
{
    int64_t pts;

    if (!AudioRunning) {
        return 0L;                      // audio not running
    }
    if (!AudioRing[AudioRingRead].HwSampleRate) {
        return 0L;                      // audio not setup
    }
    if (atomic_read(&AudioRingFilled)) {
        return 0L;                      // multiple buffers, invalid delay
    }
    pts = AudioUsedModule->GetDelay();
    pts += ((int64_t) RingBufferUsedBytes(AudioRing[AudioRingRead].RingBuffer)
        * 90 * 1000) / (AudioRing[AudioRingRead].HwSampleRate * AudioRing[AudioRingRead].HwChannels *
        AudioBytesProSample);
    Debug(4, "audio: hw+sw delay %zd %" PRId64 "ms\n", RingBufferUsedBytes(AudioRing[AudioRingRead].RingBuffer),
        pts / 90);

    return pts;
}

int AudioGetBufferUsedbytes(void) {
    if (AudioRing[AudioRingWrite].RingBuffer)
        return (int) RingBufferUsedBytes(AudioRing[AudioRingWrite].RingBuffer);
    else
        return 0;
}


/**
**	Set audio clock base.
**
**	@param pts	audio presentation timestamp
*/
void AudioSetClock(int64_t pts)
{
    if (AudioRing[AudioRingWrite].PTS != pts) {
        Debug(4, "audio: set clock %s -> %s pts\n", Timestamp2String(AudioRing[AudioRingWrite].PTS),
            Timestamp2String(pts));
    }
  //printf("Audiosetclock                  pts %#012" PRIx64 " %ld\n",pts,RingBufferUsedBytes(AudioRing[AudioRingWrite].RingBuffer));
    AudioRing[AudioRingWrite].PTS = pts;
    //printf("apts  %#012" PRIx64 " \n",pts);
}

/**
**	Get current audio clock.
**
**	@returns the audio clock in time stamps.
*/
uint64_t AudioGetClock(void)
{
    // (cast) needed for the evil gcc
    if (AudioRing[AudioRingRead].PTS != (int64_t) AV_NOPTS_VALUE) {
        int64_t delay;

        // delay zero, if no valid time stamp
        if ((delay = AudioGetDelay())) {
            if (delay > AudioRing[AudioRingRead].PTS) {
                //delay = 0;
            }
            if (AudioRing[AudioRingRead].Passthrough) {
                return AudioRing[AudioRingRead].PTS + 0 * 90 - delay;
            }
            return AudioRing[AudioRingRead].PTS + 0 * 90 - delay;
        }
    }
    return 0;
}
uint64_t AudioGetwClock(void)
{
    // (cast) needed for the evil gcc
    if (AudioRing[AudioRingWrite].PTS != (int64_t) AV_NOPTS_VALUE) {
        int64_t delay;

        // delay zero, if no valid time stamp
        if ((delay = AudioGetDelay())) {
            if (AudioRing[AudioRingRead].Passthrough) {
                return AudioRing[AudioRingWrite].PTS + 0 * 90 - delay;
            }
            return AudioRing[AudioRingWrite].PTS + 0 * 90 - delay;
        }
    }
    return AV_NOPTS_VALUE;
}

/**
**	Set mixer volume (0-1000)
**
**	@param volume	volume (0 .. 1000)
*/
void AudioSetVolume(int volume)
{

    AudioVolume = volume;
    AudioMute = !volume;
    // reduce loudness for stereo output
    if (AudioStereoDescent && AudioRing[AudioRingRead].InChannels == 2 && !AudioRing[AudioRingRead].Passthrough) {
        volume -= AudioStereoDescent;
        if (volume < 0) {
            volume = 0;
        } else if (volume > 1000) {
            volume = 1000;
        }
    }
    AudioAmplifier = volume;
    if (!AudioSoftVolume) {
        AudioUsedModule->SetVolume(volume);
    }
}

/**
**	Setup audio for requested format.
**
**	@param freq		sample frequency
**	@param channels		number of channels
**	@param passthrough	use pass-through (AC-3, ...) device
**
**	@retval 0	everything ok
**	@retval 1	didn't support frequency/channels combination
**	@retval -1	something gone wrong
**
**	@todo add support to report best fitting format.
*/
int AudioSetup(int *freq, int *channels, int passthrough)
{
    Debug(3, "audio: setup channels %d frequency %dHz %s\n", *channels, *freq, passthrough ? "pass-through" : "");

    // invalid parameter
    if (!freq || !channels || !*freq || !*channels) {
        Debug(3, "audio: bad channels or frequency parameters\n");
        // FIXME: set flag invalid setup
        return -1;
    }
    return AudioRingAdd(*freq, *channels, passthrough);
}

/**
**	Play audio.
*/
void AudioPlay(void)
{
    if (!AudioPaused) {
        Debug(3, "audio: not paused, check the code\n");
        //return;
    }
    Debug(3, "audio: resumed\n");
    AudioPaused = 0;
    AudioEnqueue(NULL, 0);              // wakeup thread
}

/**
**	Pause audio.
*/
void AudioPause(void)
{
    if (AudioPaused) {
        Debug(3, "audio: already paused, check the code\n");
        return;
    }
    Debug(3, "audio: paused\n");
    AudioPaused = 1;
}

/**
**	Set audio buffer time.
**
**	PES audio packets have a max distance of 300 ms.
**	TS audio packet have a max distance of 100 ms.
**	The period size of the audio buffer is 24 ms.
**	With streamdev sometimes extra +100ms are needed.
*/
void AudioSetBufferTime(int delay)
{
    if (!delay) {
        delay = 336;
    }
    AudioBufferTime = delay;
}

/**
**	Enable/disable software volume.
**
**	@param onoff	-1 toggle, true turn on, false turn off
*/
void AudioSetSoftvol(int onoff)
{
    if (onoff < 0) {
        AudioSoftVolume ^= 1;
    } else {
        AudioSoftVolume = onoff;
    }
}

/**
**
**	Set Audio CEC Device
**
**	@param Device Number
*/
void AudioSetCECDevice(int device)
{
    AudioCECDev = device;
}

/**
**	Set normalize volume parameters.
**
**	@param onoff	-1 toggle, true turn on, false turn off
**	@param maxfac	max. factor of normalize /1000
*/
void AudioSetNormalize(int onoff, int maxfac)
{
    if (onoff < 0) {
        AudioNormalize ^= 1;
    } else {
        AudioNormalize = onoff;
    }
    AudioMaxNormalize = maxfac;
}

/**
**	Set volume compression parameters.
**
**	@param onoff	-1 toggle, true turn on, false turn off
**	@param maxfac	max. factor of compression /1000
*/
void AudioSetCompression(int onoff, int maxfac)
{
    if (onoff < 0) {
        AudioCompression ^= 1;
    } else {
        AudioCompression = onoff;
    }
    AudioMaxCompression = maxfac;
    if (!AudioCompressionFactor) {
        AudioCompressionFactor = 1000;
    }
    if (AudioCompressionFactor > AudioMaxCompression) {
        AudioCompressionFactor = AudioMaxCompression;
    }
}

/**
**	Set stereo loudness descent.
**
**	@param delta	value (/1000) to reduce stereo volume
*/
void AudioSetStereoDescent(int delta)
{
    AudioStereoDescent = delta;
    AudioSetVolume(AudioVolume);        // update channel delta
}

/**
**	Set pcm audio device.
**
**	@param device	name of pcm device (fe. "hw:0,9" or "/dev/dsp")
**
**	@note this is currently used to select alsa/OSS output module.
*/
void AudioSetDevice(const char *device)
{
    if (!AudioModuleName) {
        AudioModuleName = "alsa";       // detect alsa/OSS
        if (!device[0]) {
            AudioModuleName = "noop";
        } else if (device[0] == '/') {
            AudioModuleName = "oss";
        }
    }
    AudioPCMDevice = device;
}

/**
**	Set pass-through audio device.
**
**	@param device	name of pass-through device (fe. "hw:0,1")
**
**	@note this is currently usable with alsa only.
*/
void AudioSetPassthroughDevice(const char *device)
{
    if (!AudioModuleName) {
        AudioModuleName = "alsa";       // detect alsa/OSS
        if (!device[0]) {
            AudioModuleName = "noop";
        } else if (device[0] == '/') {
            AudioModuleName = "oss";
        }
    }
    AudioPassthroughDevice = device;
}

/**
**	Set pcm audio mixer channel.
**
**	@param channel	name of the mixer channel (fe. PCM or Master)
**
**	@note this is currently used to select alsa/OSS output module.
*/
void AudioSetChannel(const char *channel)
{
    AudioMixerChannel = channel;
}

/**
**	Set automatic AES flag handling.
**
**	@param onoff	turn setting AES flag on or off
*/
void AudioSetAutoAES(int onoff)
{
    if (onoff < 0) {
        AudioAppendAES ^= 1;
    } else {
        AudioAppendAES = onoff;
    }
}

/**
**	Initialize audio output module.
**
**	@todo FIXME: make audio output module selectable.
*/
void AudioInit(void)
{
    unsigned u;
    const char *name;
    //int freq,chan;

    name = "noop";
#ifdef USE_OSS
    name = "oss";
#endif
#ifdef USE_ALSA
    name = "alsa";
#endif
    if (AudioModuleName) {
        name = AudioModuleName;
    }
    //
    //  search selected audio module.
    //
    for (u = 0; u < sizeof(AudioModules) / sizeof(*AudioModules); ++u) {
        if (!strcasecmp(name, AudioModules[u]->Name)) {
            AudioUsedModule = AudioModules[u];
            Info(_("audio: '%s' output module used\n"), AudioUsedModule->Name);
            goto found;
        }
    }
    Error(_("audio: '%s' output module isn't supported\n"), name);
    AudioUsedModule = &NoopModule;
    return;

  found:

    AudioDoingInit = 1;
    AudioRingInit();
    AudioUsedModule->Init();


    for (u = 0; u < AudioRatesMax; ++u) {
        Info(_("audio: %6dHz supports %d %d %d %d %d %d %d %d channels\n"), AudioRatesTable[u],
            AudioChannelMatrix[u][1], AudioChannelMatrix[u][2], AudioChannelMatrix[u][3], AudioChannelMatrix[u][4],
            AudioChannelMatrix[u][5], AudioChannelMatrix[u][6], AudioChannelMatrix[u][7], AudioChannelMatrix[u][8]);

        AudioChannelMatrix[u][1]=AudioChannelMatrix[u][2]=2;
        AudioChannelMatrix[u][3]=AudioChannelMatrix[u][4]=4;
        AudioChannelMatrix[u][5]=AudioChannelMatrix[u][6]=6;
        AudioChannelMatrix[u][7]=AudioChannelMatrix[u][8]=8;
        //printf("audio: %6dHz supports %d %d %d %d %d %d %d %d channels\n", AudioRatesTable[u],
        //    AudioChannelMatrix[u][1], AudioChannelMatrix[u][2], AudioChannelMatrix[u][3], AudioChannelMatrix[u][4],
        //    AudioChannelMatrix[u][5], AudioChannelMatrix[u][6], AudioChannelMatrix[u][7], AudioChannelMatrix[u][8]);

    }

    AudioChannelsInHw[1]=AudioChannelsInHw[3]=AudioChannelsInHw[4]=AudioChannelsInHw[5]=AudioChannelsInHw[6]=AudioChannelsInHw[7]=AudioChannelsInHw[8]=0;
    AudioChannelsInHw[2]=2;
    //printf("ChannelsinHW %d %d %d %d %d %d %d %d\n",AudioChannelsInHw[1],AudioChannelsInHw[2],AudioChannelsInHw[3],
    //                                                    AudioChannelsInHw[4],AudioChannelsInHw[5],AudioChannelsInHw[6],
    //                                                    AudioChannelsInHw[7],AudioChannelsInHw[8]);

#ifdef USE_AUDIO_THREAD
    if (AudioUsedModule->Thread) {      // supports threads
        AudioInitThread();
    }
#endif

#ifdef USE_CEC
    if (!AudioSoftVolume) {
        use_cec = cec_init();
    }
    else {
        use_cec = 0;
    }
#else
    use_cec = 0;
#endif
    AudioDoingInit = 0;
}

/**
**	Cleanup audio output module.
*/
void AudioExit(void)
{
    const AudioModule *module;

    Debug(3, "audio: %s\n", __FUNCTION__);

#ifdef USE_AUDIO_THREAD
    if (AudioUsedModule->Thread) {      // supports threads
        AudioExitThread();
    }
#endif
    module = AudioUsedModule;
    AudioUsedModule = &NoopModule;
    module->Exit();
    AudioRingExit();
    AudioRunning = 0;
    AudioPaused = 0;

#ifdef USE_CEC
    if (use_cec) {
        cec_exit();
        use_cec = 0;
    }
#endif
}

#ifdef AUDIO_TEST

//----------------------------------------------------------------------------
//  Test
//----------------------------------------------------------------------------

void AudioTest(void)
{
    for (;;) {
        unsigned u;
        uint8_t buffer[16 * 1024];      // some random data
        int i;

        for (u = 0; u < sizeof(buffer); u++) {
            buffer[u] = random() & 0xffff;
        }

        Debug(3, "audio/test: loop\n");
        for (i = 0; i < 100; ++i) {
            while (RingBufferFreeBytes(AlsaRingBuffer) > sizeof(buffer)) {
                AlsaEnqueue(buffer, sizeof(buffer));
            }
            usleep(20 * 1000);
        }
        break;
    }
}

#include <getopt.h>

int SysLogLevel;                        ///< show additional debug informations

/**
**	Print version.
*/
static void PrintVersion(void)
{
    printf("audio_test: audio tester Version " VERSION
#ifdef GIT_REV
        "(GIT-" GIT_REV ")"
#endif
        ",\n\t(c) 2009 - 2013 by Johns\n" "\tLicense AGPLv3: GNU Affero General Public License version 3\n");
}

/**
**	Print usage.
*/
static void PrintUsage(void)
{
    printf("Usage: audio_test [-?dhv]\n" "\t-d\tenable debug, more -d increase the verbosity\n"
        "\t-? -h\tdisplay this message\n" "\t-v\tdisplay version information\n"
        "Only idiots print usage on stderr!\n");
}

/**
**	Main entry point.
**
**	@param argc	number of arguments
**	@param argv	arguments vector
**
**	@returns -1 on failures, 0 clean exit.
*/
int main(int argc, char *const argv[])
{
    SysLogLevel = 0;

    //
    //  Parse command line arguments
    //
    for (;;) {
        switch (getopt(argc, argv, "hv?-c:d")) {
            case 'd':                  // enabled debug
                ++SysLogLevel;
                continue;

            case EOF:
                break;
            case 'v':                  // print version
                PrintVersion();
                return 0;
            case '?':
            case 'h':                  // help usage
                PrintVersion();
                PrintUsage();
                return 0;
            case '-':
                PrintVersion();
                PrintUsage();
                fprintf(stderr, "\nWe need no long options\n");
                return -1;
            case ':':
                PrintVersion();
                fprintf(stderr, "Missing argument for option '%c'\n", optopt);
                return -1;
            default:
                PrintVersion();
                fprintf(stderr, "Unknown option '%c'\n", optopt);
                return -1;
        }
        break;
    }
    if (optind < argc) {
        PrintVersion();
        while (optind < argc) {
            fprintf(stderr, "Unhandled argument '%s'\n", argv[optind++]);
        }
        return -1;
    }
    //
    //    main loop
    //
    AudioInit();
    for (;;) {
        unsigned u;
        uint8_t buffer[16 * 1024];      // some random data

        for (u = 0; u < sizeof(buffer); u++) {
            buffer[u] = random() & 0xffff;
        }

        Debug(3, "audio/test: loop\n");
        for (;;) {
            while (RingBufferFreeBytes(AlsaRingBuffer) > sizeof(buffer)) {
                AlsaEnqueue(buffer, sizeof(buffer));
            }
        }
    }
    AudioExit();

    return 0;
}

#endif
