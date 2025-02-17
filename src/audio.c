// Commander X16 Emulator
// Copyright (c) 2020 Frank van den Hoef
// All rights reserved. License: 2-clause BSD

#include "audio.h"
#include "vera_psg.h"
#include "vera_pcm.h"
#include "ym2151.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#define SAMPLERATE (25000000 / 512)

#ifdef __EMSCRIPTEN__
	#define SAMPLES_PER_BUFFER (1024)
#else
	#define SAMPLES_PER_BUFFER (256)
#endif

static SDL_AudioDeviceID audio_dev;
static int               vera_clks = 0;
static int               cpu_clks  = 0;
static int16_t **        buffers;
static int               rdidx    = 0;
static int               wridx    = 0;
static int               buf_cnt  = 0;
static int               num_bufs = 0;

static void
audio_callback(void *userdata, Uint8 *stream, int len)
{
	if (audio_dev == 0) {
		return;
	}

	int expected = 2 * SAMPLES_PER_BUFFER * sizeof(int16_t);
	if (len != expected) {
		fprintf(stderr, "Audio buffer size mismatch! (expected: %d, got: %d)\n", expected, len);
		return;
	}

	if (buf_cnt == 0) {
		memset(stream, 0, len);
		return;
	}

	memcpy(stream, buffers[rdidx++], len);
	if (rdidx == num_bufs) {
		rdidx = 0;
	}
	buf_cnt--;
}

void
audio_init(const char *dev_name, int num_audio_buffers)
{
	if (audio_dev > 0) {
		audio_close();
	}

	if (dev_name) {
		if (!strcmp("none", dev_name)) {
			return;
		}
	}

	// Set number of buffers
	num_bufs = num_audio_buffers;
	if (num_bufs < 3) {
		num_bufs = 3;
	}
	if (num_bufs > 1024) {
		num_bufs = 1024;
	}

	// Allocate audio buffers
	buffers = malloc(num_bufs * sizeof(*buffers));
	if (!buffers) {
		fprintf(stderr, "Cannot allocate audio buffers: %s\n", strerror(errno));
		exit(1);
	}

	for (int i = 0; i < num_bufs; i++) {
		buffers[i] = malloc(2 * SAMPLES_PER_BUFFER * sizeof(buffers[0][0]));
		if (!buffers[i]) {
			fprintf(stderr, "Cannot allocate audio buffers: %s\n", strerror(errno));
			exit(1);
		}
	}

	SDL_AudioSpec desired;
	SDL_AudioSpec obtained;

	// Setup SDL audio
	memset(&desired, 0, sizeof(desired));
	desired.freq     = SAMPLERATE;
	desired.format   = AUDIO_S16SYS;
	desired.samples  = SAMPLES_PER_BUFFER;
	desired.channels = 2;
	desired.callback = audio_callback;

	audio_dev = SDL_OpenAudioDevice(dev_name, 0, &desired, &obtained, 0);
	if (audio_dev <= 0) {
		fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
		if (dev_name != NULL) {
			audio_usage();
		}
		exit(-1);
	}

	// Init YM2151 emulation. 3.579545 MHz clock
	YM_Create(3579545);
	YM_init(obtained.freq, 60);

	// Start playback
	SDL_PauseAudioDevice(audio_dev, 0);
}

void
audio_close(void)
{
	if (audio_dev == 0) {
		return;
	}

	SDL_CloseAudioDevice(audio_dev);
	audio_dev = 0;

	// Free audio buffers
	if (buffers != NULL) {
		for (int i = 0; i < num_bufs; i++) {
			if (buffers[i] != NULL) {
				free(buffers[i]);
				buffers[i] = NULL;
			}
		}
		free(buffers);
		buffers = NULL;
	}
}

void
audio_render(int cpu_clocks)
{
	if (audio_dev == 0) {
		return;
	}

	cpu_clks += cpu_clocks;
	if (cpu_clks > 8) {
		int c = cpu_clks / 8;
		cpu_clks -= c * 8;
		vera_clks += c * 25;
	}

	while (vera_clks >= 512 * SAMPLES_PER_BUFFER) {
		vera_clks -= 512 * SAMPLES_PER_BUFFER;

		int16_t psg_buf[2 * SAMPLES_PER_BUFFER];
		psg_render(psg_buf, SAMPLES_PER_BUFFER);

		int16_t pcm_buf[2 * SAMPLES_PER_BUFFER];
		pcm_render(pcm_buf, SAMPLES_PER_BUFFER);

		int16_t ym_buf[2 * SAMPLES_PER_BUFFER];
		YM_stream_update((uint16_t *)ym_buf, SAMPLES_PER_BUFFER);

		bool buf_available;
		SDL_LockAudioDevice(audio_dev);
		buf_available = buf_cnt < num_bufs;
		SDL_UnlockAudioDevice(audio_dev);

		if (buf_available) {
			// Mix PSG, PCM and YM output
			int16_t *buf = buffers[wridx];
			for (int i = 0; i < 2 * SAMPLES_PER_BUFFER; i++) {
				buf[i] = ((int)psg_buf[i] + (int)pcm_buf[i] + (int)ym_buf[i]) / 3;
			}

			SDL_LockAudioDevice(audio_dev);
			wridx++;
			if (wridx == num_bufs) {
				wridx = 0;
			}
			buf_cnt++;
			SDL_UnlockAudioDevice(audio_dev);
		}
	}
}

void
audio_usage(void)
{
	// SDL_GetAudioDeviceName doesn't work if audio isn't initialized.
	// Since argument parsing happens before initializing SDL, ensure the
	// audio subsystem is initialized before printing audio device names.
	SDL_InitSubSystem(SDL_INIT_AUDIO);

	// List all available sound devices
	printf("The following sound output devices are available:\n");
	const int sounds = SDL_GetNumAudioDevices(0);
	for (int i = 0; i < sounds; ++i) {
		printf("\t%s\n", SDL_GetAudioDeviceName(i, 0));
	}

	SDL_Quit();
	exit(1);
}
