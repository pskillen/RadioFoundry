// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "c2fx.h"

static uint32_t pcm_crc32(uint32_t crc, const int16_t pcm[C2FX_FRAME_LEN])
{
	for (int i = 0; i < C2FX_FRAME_LEN; i++) {
		uint16_t sample = (uint16_t)pcm[i];
		for (int shift = 0; shift < 16; shift += 8) {
			crc ^= (sample >> shift) & 0xffu;
			for (int bit = 0; bit < 8; bit++)
				crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
		}
	}
	return crc;
}

int main(void)
{
	FILE *vector = fopen(C2FX_VECTOR, "rb");
	if (vector == NULL) {
		fprintf(stderr, "c2fxSeamTest: cannot read %s\n", C2FX_VECTOR);
		return 1;
	}
	uint8_t bits[100][C2FX_BITS_BYTES];
	if (fread(bits, 1, sizeof(bits), vector) != sizeof(bits) || fgetc(vector) != EOF) {
		fprintf(stderr, "c2fxSeamTest: %s must be exactly 100 frames\n", C2FX_VECTOR);
		fclose(vector);
		return 1;
	}
	fclose(vector);
	const uint32_t expected = 0x35f6f837u;
	c2fx_dec_t dec;
	c2fx_scratch_t scratch;
	int16_t pcm[C2FX_FRAME_LEN];

	c2fx_dec_init(&dec);
	uint32_t crc = UINT32_MAX;
	for (int frame = 0; frame < 100; frame++) {
		c2fx_decode(&dec, bits[frame], pcm, &scratch);
		crc = pcm_crc32(crc, pcm);
	}
	crc = ~crc;
	c2fx_decode(&dec, NULL, pcm, &scratch);
	if (crc != expected) {
		fprintf(stderr, "c2fxSeamTest: 100-frame PCM CRC %08" PRIx32 ", expected %08" PRIx32 "\n", crc, expected);
		return 1;
	}
	printf("c2fxSeamTest: 100 frames, PCM CRC %08" PRIx32 ", lost frame returned\n", crc);
	return 0;
}
