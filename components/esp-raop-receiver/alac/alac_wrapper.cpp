/*
 * alac_wrapper.cpp — minimal C wrapper around Apple's reference ALAC decoder.
 *
 * Exposes the philippe44 raop-sink decoder API declared in alac_wrapper.h
 * (alac_create_decoder / alac_to_pcm / alac_delete_decoder), backed by Apple's
 * open-source ALAC decoder (Apache-2.0, vendored under alac/). This replaces the
 * upstream prebuilt `libalac.a` with auditable source so nothing opaque is linked
 * into the firmware.
 *
 * The RAOP layer (rtp.c) builds an ALACSpecificConfig from the RTSP `fmtp` line
 * (big-endian fields) and hands it to us as the "magic cookie"; ALACDecoder::Init
 * byte-swaps it back to host order.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include "ALACDecoder.h"
#include "ALACBitUtilities.h"
#include "alac_wrapper.h"

struct alac_codec_s {
	ALACDecoder *decoder;
	unsigned frames_per_packet;
	unsigned sample_rate;
	unsigned int max_frame_bytes;
	unsigned char sample_size;
	unsigned char channels;
};

extern "C" struct alac_codec_s *alac_create_decoder(int magic_cookie_size, unsigned char *magic_cookie,
	unsigned char *sample_size, unsigned *sample_rate,
	unsigned char *channels, unsigned int *block_size) {
	struct alac_codec_s *codec = (struct alac_codec_s *) calloc(1, sizeof(struct alac_codec_s));
	if (!codec) {
		return NULL;
	}

	codec->decoder = new ALACDecoder();
	if (!codec->decoder) {
		free(codec);
		return NULL;
	}

	if (codec->decoder->Init(magic_cookie, magic_cookie_size) != 0) {
		delete codec->decoder;
		free(codec);
		return NULL;
	}

	const ALACSpecificConfig &cfg = codec->decoder->mConfig;
	codec->frames_per_packet = cfg.frameLength;
	codec->sample_rate = cfg.sampleRate;
	codec->sample_size = cfg.bitDepth;
	codec->channels = cfg.numChannels;
	// Generous upper bound for the per-frame input BitBuffer (the decoder stops at
	// the frame's encoded end; the compressed input is always smaller than this).
	codec->max_frame_bytes = cfg.frameLength * cfg.numChannels * ((cfg.bitDepth + 7) / 8) + 16;

	if (sample_size) {
		*sample_size = codec->sample_size;
	}
	if (sample_rate) {
		*sample_rate = codec->sample_rate;
	}
	if (channels) {
		*channels = codec->channels;
	}
	if (block_size) {
		*block_size = codec->frames_per_packet;
	}

	return codec;
}

extern "C" void alac_delete_decoder(struct alac_codec_s *codec) {
	if (!codec) {
		return;
	}
	if (codec->decoder) {
		delete codec->decoder;
	}
	free(codec);
}

extern "C" bool alac_to_pcm(struct alac_codec_s *codec, unsigned char *input,
	unsigned char *output, char channels, unsigned *out_frames) {
	if (!codec || !codec->decoder) {
		return false;
	}

	BitBuffer bits;
	BitBufferInit(&bits, input, codec->max_frame_bytes);

	uint32_t decoded = 0;
	int32_t status = codec->decoder->Decode(&bits, output, codec->frames_per_packet,
		(uint32_t) channels, &decoded);

	if (out_frames) {
		*out_frames = decoded;
	}
	return status == 0;
}
