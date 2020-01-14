/*

Copyright 2009, 2011 Sebastian Gesemann. All rights reserved.

Copyright 2020 Max Kellermann <max.kellermann@gmail.com>

Redistribution and use in source and binary forms, with or without modification, are
permitted provided that the following conditions are met:

   1. Redistributions of source code must retain the above copyright notice, this list of
      conditions and the following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright notice, this list
      of conditions and the following disclaimer in the documentation and/or other materials
      provided with the distribution.

THIS SOFTWARE IS PROVIDED BY SEBASTIAN GESEMANN ''AS IS'' AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SEBASTIAN GESEMANN OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The views and conclusions contained in the software and documentation are those of the
authors and should not be interpreted as representing official policies, either expressed
or implied, of Sebastian Gesemann.

 */

#include "Dsd2Pcm.hxx"
#include "util/bit_reverse.h"

#include <stdlib.h>
#include <string.h>

/** number of FIR constants */
static constexpr size_t HTAPS = 48;

/** number of "8 MACs" lookup tables */
static constexpr int CTABLES = (HTAPS + 7) / 8;

/* must be a power of two */
static constexpr int FIFOSIZE = 16;

/** bit mask for FIFO offsets */
static constexpr size_t FIFOMASK = FIFOSIZE - 1;

static_assert(FIFOSIZE*8 >= HTAPS*2, "FIFOSIZE too small");

/*
 * Properties of this 96-tap lowpass filter when applied on a signal
 * with sampling rate of 44100*64 Hz:
 *
 * () has a delay of 17 microseconds.
 *
 * () flat response up to 48 kHz
 *
 * () if you downsample afterwards by a factor of 8, the
 *    spectrum below 70 kHz is practically alias-free.
 *
 * () stopband rejection is about 160 dB
 *
 * The coefficient tables ("ctables") take only 6 Kibi Bytes and
 * should fit into a modern processor's fast cache.
 */

/*
 * The 2nd half (48 coeffs) of a 96-tap symmetric lowpass filter
 */
static constexpr double htaps[HTAPS] = {
  0.09950731974056658,
  0.09562845727714668,
  0.08819647126516944,
  0.07782552527068175,
  0.06534876523171299,
  0.05172629311427257,
  0.0379429484910187,
  0.02490921351762261,
  0.0133774746265897,
  0.003883043418804416,
 -0.003284703416210726,
 -0.008080250212687497,
 -0.01067241812471033,
 -0.01139427235000863,
 -0.0106813877974587,
 -0.009007905078766049,
 -0.006828859761015335,
 -0.004535184322001496,
 -0.002425035959059578,
 -0.0006922187080790708,
  0.0005700762133516592,
  0.001353838005269448,
  0.001713709169690937,
  0.001742046839472948,
  0.001545601648013235,
  0.001226696225277855,
  0.0008704322683580222,
  0.0005381636200535649,
  0.000266446345425276,
  7.002968738383528e-05,
 -5.279407053811266e-05,
 -0.0001140625650874684,
 -0.0001304796361231895,
 -0.0001189970287491285,
 -9.396247155265073e-05,
 -6.577634378272832e-05,
 -4.07492895872535e-05,
 -2.17407957554587e-05,
 -9.163058931391722e-06,
 -2.017460145032201e-06,
  1.249721855219005e-06,
  2.166655190537392e-06,
  1.930520892991082e-06,
  1.319400334374195e-06,
  7.410039764949091e-07,
  3.423230509967409e-07,
  1.244182214744588e-07,
  3.130441005359396e-08
};

static float ctables[CTABLES][256];
static int precalculated = 0;

static constexpr float
CalculateCtableValue(int t, int k, int e) noexcept
{
	double acc = 0;
	for (int m = 0; m < k; ++m) {
		acc += (((e >> (7 - m)) & 1) * 2 - 1) * htaps[t * 8 + m];
	}

	return acc;
}

static void
precalc() noexcept
{
	int t, e, k;
	if (precalculated) return;
	for (t=0; t<CTABLES; ++t) {
		k = HTAPS - t*8;
		if (k>8) k=8;
		for (e=0; e<256; ++e) {
			ctables[CTABLES-1-t][e] = CalculateCtableValue(t, k, e);
		}
	}
	precalculated = 1;
}

struct dsd2pcm_ctx_s
{
	unsigned char fifo[FIFOSIZE];
	unsigned fifopos;
};

dsd2pcm_ctx *
dsd2pcm_init() noexcept
{
	dsd2pcm_ctx* ptr;
	if (!precalculated) precalc();
	ptr = (dsd2pcm_ctx*) malloc(sizeof(dsd2pcm_ctx));
	if (ptr) dsd2pcm_reset(ptr);
	return ptr;
}

void
dsd2pcm_destroy(dsd2pcm_ctx *ptr) noexcept
{
	free(ptr);
}

dsd2pcm_ctx *
dsd2pcm_clone(dsd2pcm_ctx *ptr) noexcept
{
	dsd2pcm_ctx* p2;
	p2 = (dsd2pcm_ctx*) malloc(sizeof(dsd2pcm_ctx));
	if (p2) {
		memcpy(p2,ptr,sizeof(dsd2pcm_ctx));
	}
	return p2;
}

void
dsd2pcm_reset(dsd2pcm_ctx *ptr) noexcept
{
	int i;
	for (i=0; i<FIFOSIZE; ++i)
		ptr->fifo[i] = 0x69; /* my favorite silence pattern */
	ptr->fifopos = 0;
	/* 0x69 = 01101001
	 * This pattern "on repeat" makes a low energy 352.8 kHz tone
	 * and a high energy 1.0584 MHz tone which should be filtered
	 * out completely by any playback system --> silence
	 */
}

void
dsd2pcm_translate(dsd2pcm_ctx *ptr,
		  size_t samples,
		  const unsigned char *src, ptrdiff_t src_stride,
		  bool lsbf,
		  float *dst, ptrdiff_t dst_stride) noexcept
{
	unsigned ffp;
	unsigned i;
	unsigned bite1, bite2;
	unsigned char* p;
	double acc;
	ffp = ptr->fifopos;
	while (samples-- > 0) {
		bite1 = *src & 0xFFu;
		if (lsbf) bite1 = bit_reverse(bite1);
		ptr->fifo[ffp] = bite1; src += src_stride;
		p = ptr->fifo + ((ffp-CTABLES) & FIFOMASK);
		*p = bit_reverse(*p);
		acc = 0;
		for (i=0; i<CTABLES; ++i) {
			bite1 = ptr->fifo[(ffp              -i) & FIFOMASK] & 0xFF;
			bite2 = ptr->fifo[(ffp-(CTABLES*2-1)+i) & FIFOMASK] & 0xFF;
			acc += ctables[i][bite1] + ctables[i][bite2];
		}
		*dst = (float)acc; dst += dst_stride;
		ffp = (ffp + 1) & FIFOMASK;
	}
	ptr->fifopos = ffp;
}
