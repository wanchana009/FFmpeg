/*
 * Copyright (c) 2013-2014 Clément Bœsch
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * A simple, relatively efficient and slow DCT image denoiser.
 *
 * @see http://www.ipol.im/pub/art/2011/ys-dct/
 *
 * The DCT factorization used is based on "Fast and numerically stable
 * algorithms for discrete cosine transforms" from Gerlind Plonkaa & Manfred
 * Tasche (DOI: 10.1016/j.laa.2004.07.015).
 */

#include "libavutil/avassert.h"
#include "libavutil/eval.h"
#include "libavutil/opt.h"
#include "internal.h"

static const char *const var_names[] = { "c", NULL };
enum { VAR_C, VAR_VARS_NB };

typedef struct DCTdnoizContext {
    const AVClass *class;

    /* coefficient factor expression */
    char *expr_str;
    AVExpr *expr;
    double var_values[VAR_VARS_NB];

    int pr_width, pr_height;    // width and height to process
    float sigma;                // used when no expression are st
    float th;                   // threshold (3*sigma)
    float *cbuf[2][3];          // two planar rgb color buffers
    float *weights;             // dct coeff are cumulated with overlapping; these values are used for averaging
    int p_linesize;             // line sizes for color and weights
    int overlap;                // number of block overlapping pixels
    int step;                   // block step increment (blocksize - overlap)
    int n;                      // 1<<n is the block size
    int bsize;                  // block size, 1<<n
    void (*filter_freq_func)(struct DCTdnoizContext *s,
                             const float *src, int src_linesize,
                             float *dst, int dst_linesize);
    void (*color_decorrelation)(float **dst, int dst_linesize,
                                const uint8_t *src, int src_linesize,
                                int w, int h);
    void (*color_correlation)(uint8_t *dst, int dst_linesize,
                              float **src, int src_linesize,
                              int w, int h);
} DCTdnoizContext;

#define MIN_NBITS 3 /* blocksize = 1<<3 =  8 */
#define MAX_NBITS 4 /* blocksize = 1<<4 = 16 */
#define DEFAULT_NBITS 3

#define OFFSET(x) offsetof(DCTdnoizContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
static const AVOption dctdnoiz_options[] = {
    { "sigma",   "set noise sigma constant",               OFFSET(sigma),    AV_OPT_TYPE_FLOAT,  {.dbl=0},            0, 999,          .flags = FLAGS },
    { "s",       "set noise sigma constant",               OFFSET(sigma),    AV_OPT_TYPE_FLOAT,  {.dbl=0},            0, 999,          .flags = FLAGS },
    { "overlap", "set number of block overlapping pixels", OFFSET(overlap),  AV_OPT_TYPE_INT,    {.i64=-1}, -1, (1<<MAX_NBITS)-1, .flags = FLAGS },
    { "expr",    "set coefficient factor expression",      OFFSET(expr_str), AV_OPT_TYPE_STRING, {.str=NULL},                          .flags = FLAGS },
    { "e",       "set coefficient factor expression",      OFFSET(expr_str), AV_OPT_TYPE_STRING, {.str=NULL},                          .flags = FLAGS },
    { "n",       "set the block size, expressed in bits",  OFFSET(n),        AV_OPT_TYPE_INT,    {.i64=DEFAULT_NBITS}, MIN_NBITS, MAX_NBITS, .flags = FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(dctdnoiz);

static void av_always_inline fdct8_1d(float *dst, const float *src,
                                      int dst_stridea, int dst_strideb,
                                      int src_stridea, int src_strideb)
{
    int i;

    for (i = 0; i < 8; i++) {
        const float x00 = src[0*src_stridea] + src[7*src_stridea];
        const float x01 = src[1*src_stridea] + src[6*src_stridea];
        const float x02 = src[2*src_stridea] + src[5*src_stridea];
        const float x03 = src[3*src_stridea] + src[4*src_stridea];
        const float x04 = src[0*src_stridea] - src[7*src_stridea];
        const float x05 = src[1*src_stridea] - src[6*src_stridea];
        const float x06 = src[2*src_stridea] - src[5*src_stridea];
        const float x07 = src[3*src_stridea] - src[4*src_stridea];
        const float x08 = x00 + x03;
        const float x09 = x01 + x02;
        const float x0a = x00 - x03;
        const float x0b = x01 - x02;
        const float x0c = 1.38703984532215*x04 + 0.275899379282943*x07;
        const float x0d = 1.17587560241936*x05 + 0.785694958387102*x06;
        const float x0e = -0.785694958387102*x05 + 1.17587560241936*x06;
        const float x0f = 0.275899379282943*x04 - 1.38703984532215*x07;
        const float x10 = 0.353553390593274 * (x0c - x0d);
        const float x11 = 0.353553390593274 * (x0e - x0f);
        dst[0*dst_stridea] = 0.353553390593274 * (x08 + x09);
        dst[1*dst_stridea] = 0.353553390593274 * (x0c + x0d);
        dst[2*dst_stridea] = 0.461939766255643*x0a + 0.191341716182545*x0b;
        dst[3*dst_stridea] = 0.707106781186547 * (x10 - x11);
        dst[4*dst_stridea] = 0.353553390593274 * (x08 - x09);
        dst[5*dst_stridea] = 0.707106781186547 * (x10 + x11);
        dst[6*dst_stridea] = 0.191341716182545*x0a - 0.461939766255643*x0b;
        dst[7*dst_stridea] = 0.353553390593274 * (x0e + x0f);
        dst += dst_strideb;
        src += src_strideb;
    }
}

static void av_always_inline idct8_1d(float *dst, const float *src,
                                      int dst_stridea, int dst_strideb,
                                      int src_stridea, int src_strideb,
                                      int add)
{
    int i;

    for (i = 0; i < 8; i++) {
        const float x00 = 1.4142135623731*src[0*src_stridea];
        const float x01 = 1.38703984532215*src[1*src_stridea] + 0.275899379282943*src[7*src_stridea];
        const float x02 = 1.30656296487638*src[2*src_stridea] + 0.541196100146197*src[6*src_stridea];
        const float x03 = 1.17587560241936*src[3*src_stridea] + 0.785694958387102*src[5*src_stridea];
        const float x04 = 1.4142135623731*src[4*src_stridea];
        const float x05 = -0.785694958387102*src[3*src_stridea] + 1.17587560241936*src[5*src_stridea];
        const float x06 = 0.541196100146197*src[2*src_stridea] - 1.30656296487638*src[6*src_stridea];
        const float x07 = -0.275899379282943*src[1*src_stridea] + 1.38703984532215*src[7*src_stridea];
        const float x09 = x00 + x04;
        const float x0a = x01 + x03;
        const float x0b = 1.4142135623731*x02;
        const float x0c = x00 - x04;
        const float x0d = x01 - x03;
        const float x0e = 0.353553390593274 * (x09 - x0b);
        const float x0f = 0.353553390593274 * (x0c + x0d);
        const float x10 = 0.353553390593274 * (x0c - x0d);
        const float x11 = 1.4142135623731*x06;
        const float x12 = x05 + x07;
        const float x13 = x05 - x07;
        const float x14 = 0.353553390593274 * (x11 + x12);
        const float x15 = 0.353553390593274 * (x11 - x12);
        const float x16 = 0.5*x13;
        const float x08 = -x15;
        dst[0*dst_stridea] = (add ? dst[ 0*dst_stridea] : 0) + 0.25 * (x09 + x0b) + 0.353553390593274*x0a;
        dst[1*dst_stridea] = (add ? dst[ 1*dst_stridea] : 0) + 0.707106781186547 * (x0f - x08);
        dst[2*dst_stridea] = (add ? dst[ 2*dst_stridea] : 0) + 0.707106781186547 * (x0f + x08);
        dst[3*dst_stridea] = (add ? dst[ 3*dst_stridea] : 0) + 0.707106781186547 * (x0e + x16);
        dst[4*dst_stridea] = (add ? dst[ 4*dst_stridea] : 0) + 0.707106781186547 * (x0e - x16);
        dst[5*dst_stridea] = (add ? dst[ 5*dst_stridea] : 0) + 0.707106781186547 * (x10 - x14);
        dst[6*dst_stridea] = (add ? dst[ 6*dst_stridea] : 0) + 0.707106781186547 * (x10 + x14);
        dst[7*dst_stridea] = (add ? dst[ 7*dst_stridea] : 0) + 0.25 * (x09 + x0b) - 0.353553390593274*x0a;
        dst += dst_strideb;
        src += src_strideb;
    }
}


static void av_always_inline fdct16_1d(float *dst, const float *src,
                                       int dst_stridea, int dst_strideb,
                                       int src_stridea, int src_strideb)
{
    int i;

    for (i = 0; i < 16; i++) {
        const float x00 = src[ 0*src_stridea] + src[15*src_stridea];
        const float x01 = src[ 1*src_stridea] + src[14*src_stridea];
        const float x02 = src[ 2*src_stridea] + src[13*src_stridea];
        const float x03 = src[ 3*src_stridea] + src[12*src_stridea];
        const float x04 = src[ 4*src_stridea] + src[11*src_stridea];
        const float x05 = src[ 5*src_stridea] + src[10*src_stridea];
        const float x06 = src[ 6*src_stridea] + src[ 9*src_stridea];
        const float x07 = src[ 7*src_stridea] + src[ 8*src_stridea];
        const float x08 = src[ 0*src_stridea] - src[15*src_stridea];
        const float x09 = src[ 1*src_stridea] - src[14*src_stridea];
        const float x0a = src[ 2*src_stridea] - src[13*src_stridea];
        const float x0b = src[ 3*src_stridea] - src[12*src_stridea];
        const float x0c = src[ 4*src_stridea] - src[11*src_stridea];
        const float x0d = src[ 5*src_stridea] - src[10*src_stridea];
        const float x0e = src[ 6*src_stridea] - src[ 9*src_stridea];
        const float x0f = src[ 7*src_stridea] - src[ 8*src_stridea];
        const float x10 = x00 + x07;
        const float x11 = x01 + x06;
        const float x12 = x02 + x05;
        const float x13 = x03 + x04;
        const float x14 = x00 - x07;
        const float x15 = x01 - x06;
        const float x16 = x02 - x05;
        const float x17 = x03 - x04;
        const float x18 = x10 + x13;
        const float x19 = x11 + x12;
        const float x1a = x10 - x13;
        const float x1b = x11 - x12;
        const float x1c = 1.38703984532215*x14 + 0.275899379282943*x17;
        const float x1d = 1.17587560241936*x15 + 0.785694958387102*x16;
        const float x1e = -0.785694958387102*x15 + 1.17587560241936*x16;
        const float x1f = 0.275899379282943*x14 - 1.38703984532215*x17;
        const float x20 = 0.25 * (x1c - x1d);
        const float x21 = 0.25 * (x1e - x1f);
        const float x22 = 1.40740373752638*x08 + 0.138617169199091*x0f;
        const float x23 = 1.35331800117435*x09 + 0.410524527522357*x0e;
        const float x24 = 1.24722501298667*x0a + 0.666655658477747*x0d;
        const float x25 = 1.09320186700176*x0b + 0.897167586342636*x0c;
        const float x26 = -0.897167586342636*x0b + 1.09320186700176*x0c;
        const float x27 = 0.666655658477747*x0a - 1.24722501298667*x0d;
        const float x28 = -0.410524527522357*x09 + 1.35331800117435*x0e;
        const float x29 = 0.138617169199091*x08 - 1.40740373752638*x0f;
        const float x2a = x22 + x25;
        const float x2b = x23 + x24;
        const float x2c = x22 - x25;
        const float x2d = x23 - x24;
        const float x2e = 0.25 * (x2a - x2b);
        const float x2f = 0.326640741219094*x2c + 0.135299025036549*x2d;
        const float x30 = 0.135299025036549*x2c - 0.326640741219094*x2d;
        const float x31 = x26 + x29;
        const float x32 = x27 + x28;
        const float x33 = x26 - x29;
        const float x34 = x27 - x28;
        const float x35 = 0.25 * (x31 - x32);
        const float x36 = 0.326640741219094*x33 + 0.135299025036549*x34;
        const float x37 = 0.135299025036549*x33 - 0.326640741219094*x34;
        dst[ 0*dst_stridea] = 0.25 * (x18 + x19);
        dst[ 1*dst_stridea] = 0.25 * (x2a + x2b);
        dst[ 2*dst_stridea] = 0.25 * (x1c + x1d);
        dst[ 3*dst_stridea] = 0.707106781186547 * (x2f - x37);
        dst[ 4*dst_stridea] = 0.326640741219094*x1a + 0.135299025036549*x1b;
        dst[ 5*dst_stridea] = 0.707106781186547 * (x2f + x37);
        dst[ 6*dst_stridea] = 0.707106781186547 * (x20 - x21);
        dst[ 7*dst_stridea] = 0.707106781186547 * (x2e + x35);
        dst[ 8*dst_stridea] = 0.25 * (x18 - x19);
        dst[ 9*dst_stridea] = 0.707106781186547 * (x2e - x35);
        dst[10*dst_stridea] = 0.707106781186547 * (x20 + x21);
        dst[11*dst_stridea] = 0.707106781186547 * (x30 - x36);
        dst[12*dst_stridea] = 0.135299025036549*x1a - 0.326640741219094*x1b;
        dst[13*dst_stridea] = 0.707106781186547 * (x30 + x36);
        dst[14*dst_stridea] = 0.25 * (x1e + x1f);
        dst[15*dst_stridea] = 0.25 * (x31 + x32);
        dst += dst_strideb;
        src += src_strideb;
    }
}

static void av_always_inline idct16_1d(float *dst, const float *src,
                                       int dst_stridea, int dst_strideb,
                                       int src_stridea, int src_strideb,
                                       int add)
{
    int i;

    for (i = 0; i < 16; i++) {
        const float x00 =  1.4142135623731  *src[ 0*src_stridea];
        const float x01 =  1.40740373752638 *src[ 1*src_stridea] + 0.138617169199091*src[15*src_stridea];
        const float x02 =  1.38703984532215 *src[ 2*src_stridea] + 0.275899379282943*src[14*src_stridea];
        const float x03 =  1.35331800117435 *src[ 3*src_stridea] + 0.410524527522357*src[13*src_stridea];
        const float x04 =  1.30656296487638 *src[ 4*src_stridea] + 0.541196100146197*src[12*src_stridea];
        const float x05 =  1.24722501298667 *src[ 5*src_stridea] + 0.666655658477747*src[11*src_stridea];
        const float x06 =  1.17587560241936 *src[ 6*src_stridea] + 0.785694958387102*src[10*src_stridea];
        const float x07 =  1.09320186700176 *src[ 7*src_stridea] + 0.897167586342636*src[ 9*src_stridea];
        const float x08 =  1.4142135623731  *src[ 8*src_stridea];
        const float x09 = -0.897167586342636*src[ 7*src_stridea] + 1.09320186700176*src[ 9*src_stridea];
        const float x0a =  0.785694958387102*src[ 6*src_stridea] - 1.17587560241936*src[10*src_stridea];
        const float x0b = -0.666655658477747*src[ 5*src_stridea] + 1.24722501298667*src[11*src_stridea];
        const float x0c =  0.541196100146197*src[ 4*src_stridea] - 1.30656296487638*src[12*src_stridea];
        const float x0d = -0.410524527522357*src[ 3*src_stridea] + 1.35331800117435*src[13*src_stridea];
        const float x0e =  0.275899379282943*src[ 2*src_stridea] - 1.38703984532215*src[14*src_stridea];
        const float x0f = -0.138617169199091*src[ 1*src_stridea] + 1.40740373752638*src[15*src_stridea];
        const float x12 = x00 + x08;
        const float x13 = x01 + x07;
        const float x14 = x02 + x06;
        const float x15 = x03 + x05;
        const float x16 = 1.4142135623731*x04;
        const float x17 = x00 - x08;
        const float x18 = x01 - x07;
        const float x19 = x02 - x06;
        const float x1a = x03 - x05;
        const float x1d = x12 + x16;
        const float x1e = x13 + x15;
        const float x1f = 1.4142135623731*x14;
        const float x20 = x12 - x16;
        const float x21 = x13 - x15;
        const float x22 = 0.25 * (x1d - x1f);
        const float x23 = 0.25 * (x20 + x21);
        const float x24 = 0.25 * (x20 - x21);
        const float x25 = 1.4142135623731*x17;
        const float x26 = 1.30656296487638*x18 + 0.541196100146197*x1a;
        const float x27 = 1.4142135623731*x19;
        const float x28 = -0.541196100146197*x18 + 1.30656296487638*x1a;
        const float x29 = 0.176776695296637 * (x25 + x27) + 0.25*x26;
        const float x2a = 0.25 * (x25 - x27);
        const float x2b = 0.176776695296637 * (x25 + x27) - 0.25*x26;
        const float x2c = 0.353553390593274*x28;
        const float x1b = 0.707106781186547 * (x2a - x2c);
        const float x1c = 0.707106781186547 * (x2a + x2c);
        const float x2d = 1.4142135623731*x0c;
        const float x2e = x0b + x0d;
        const float x2f = x0a + x0e;
        const float x30 = x09 + x0f;
        const float x31 = x09 - x0f;
        const float x32 = x0a - x0e;
        const float x33 = x0b - x0d;
        const float x37 = 1.4142135623731*x2d;
        const float x38 = 1.30656296487638*x2e + 0.541196100146197*x30;
        const float x39 = 1.4142135623731*x2f;
        const float x3a = -0.541196100146197*x2e + 1.30656296487638*x30;
        const float x3b = 0.176776695296637 * (x37 + x39) + 0.25*x38;
        const float x3c = 0.25 * (x37 - x39);
        const float x3d = 0.176776695296637 * (x37 + x39) - 0.25*x38;
        const float x3e = 0.353553390593274*x3a;
        const float x34 = 0.707106781186547 * (x3c - x3e);
        const float x35 = 0.707106781186547 * (x3c + x3e);
        const float x3f = 1.4142135623731*x32;
        const float x40 = x31 + x33;
        const float x41 = x31 - x33;
        const float x42 = 0.25 * (x3f + x40);
        const float x43 = 0.25 * (x3f - x40);
        const float x44 = 0.353553390593274*x41;
        const float x36 = -x43;
        const float x10 = -x34;
        const float x11 = -x3d;
        dst[ 0*dst_stridea] = (add ? dst[ 0*dst_stridea] : 0) + 0.176776695296637 * (x1d + x1f) + 0.25*x1e;
        dst[ 1*dst_stridea] = (add ? dst[ 1*dst_stridea] : 0) + 0.707106781186547 * (x29 - x11);
        dst[ 2*dst_stridea] = (add ? dst[ 2*dst_stridea] : 0) + 0.707106781186547 * (x29 + x11);
        dst[ 3*dst_stridea] = (add ? dst[ 3*dst_stridea] : 0) + 0.707106781186547 * (x23 + x36);
        dst[ 4*dst_stridea] = (add ? dst[ 4*dst_stridea] : 0) + 0.707106781186547 * (x23 - x36);
        dst[ 5*dst_stridea] = (add ? dst[ 5*dst_stridea] : 0) + 0.707106781186547 * (x1b - x35);
        dst[ 6*dst_stridea] = (add ? dst[ 6*dst_stridea] : 0) + 0.707106781186547 * (x1b + x35);
        dst[ 7*dst_stridea] = (add ? dst[ 7*dst_stridea] : 0) + 0.707106781186547 * (x22 + x44);
        dst[ 8*dst_stridea] = (add ? dst[ 8*dst_stridea] : 0) + 0.707106781186547 * (x22 - x44);
        dst[ 9*dst_stridea] = (add ? dst[ 9*dst_stridea] : 0) + 0.707106781186547 * (x1c - x10);
        dst[10*dst_stridea] = (add ? dst[10*dst_stridea] : 0) + 0.707106781186547 * (x1c + x10);
        dst[11*dst_stridea] = (add ? dst[11*dst_stridea] : 0) + 0.707106781186547 * (x24 + x42);
        dst[12*dst_stridea] = (add ? dst[12*dst_stridea] : 0) + 0.707106781186547 * (x24 - x42);
        dst[13*dst_stridea] = (add ? dst[13*dst_stridea] : 0) + 0.707106781186547 * (x2b - x3b);
        dst[14*dst_stridea] = (add ? dst[14*dst_stridea] : 0) + 0.707106781186547 * (x2b + x3b);
        dst[15*dst_stridea] = (add ? dst[15*dst_stridea] : 0) + 0.176776695296637 * (x1d + x1f) - 0.25*x1e;
        dst += dst_strideb;
        src += src_strideb;
    }
}

#define DEF_FILTER_FREQ_FUNCS(bsize)                                                        \
static av_always_inline void filter_freq_##bsize(const float *src, int src_linesize,        \
                                                 float *dst, int dst_linesize,              \
                                                 AVExpr *expr, double *var_values,          \
                                                 int sigma_th)                              \
{                                                                                           \
    unsigned i;                                                                             \
    DECLARE_ALIGNED(32, float, tmp_block1)[bsize * bsize];                                  \
    DECLARE_ALIGNED(32, float, tmp_block2)[bsize * bsize];                                  \
                                                                                            \
    /* forward DCT */                                                                       \
    fdct##bsize##_1d(tmp_block1, src, 1, bsize, 1, src_linesize);                           \
    fdct##bsize##_1d(tmp_block2, tmp_block1, bsize, 1, bsize, 1);                           \
                                                                                            \
    for (i = 0; i < bsize*bsize; i++) {                                                     \
        float *b = &tmp_block2[i];                                                          \
        /* frequency filtering */                                                           \
        if (expr) {                                                                         \
            var_values[VAR_C] = FFABS(*b);                                                  \
            *b *= av_expr_eval(expr, var_values, NULL);                                     \
        } else {                                                                            \
            if (FFABS(*b) < sigma_th)                                                       \
                *b = 0;                                                                     \
        }                                                                                   \
    }                                                                                       \
                                                                                            \
    /* inverse DCT */                                                                       \
    idct##bsize##_1d(tmp_block1, tmp_block2, 1, bsize, 1, bsize, 0);                        \
    idct##bsize##_1d(dst, tmp_block1, dst_linesize, 1, bsize, 1, 1);                        \
}                                                                                           \
                                                                                            \
static void filter_freq_sigma_##bsize(DCTdnoizContext *s,                                   \
                                      const float *src, int src_linesize,                   \
                                      float *dst, int dst_linesize)                         \
{                                                                                           \
    filter_freq_##bsize(src, src_linesize, dst, dst_linesize, NULL, NULL, s->th);           \
}                                                                                           \
                                                                                            \
static void filter_freq_expr_##bsize(DCTdnoizContext *s,                                    \
                                     const float *src, int src_linesize,                    \
                                     float *dst, int dst_linesize)                          \
{                                                                                           \
    filter_freq_##bsize(src, src_linesize, dst, dst_linesize, s->expr, s->var_values, 0);   \
}

DEF_FILTER_FREQ_FUNCS(8)
DEF_FILTER_FREQ_FUNCS(16)

// TODO: remove
static void color_decorrelation_rgb(float **dst, int dst_linesize,
                                    const uint8_t *src, int src_linesize,
                                    int w, int h);
static void color_correlation_rgb(uint8_t *dst, int dst_linesize,
                                  float **src, int src_linesize,
                                  int w, int h);
static void color_decorrelation_bgr(float **dst, int dst_linesize,
                                    const uint8_t *src, int src_linesize,
                                    int w, int h);
static void color_correlation_bgr(uint8_t *dst, int dst_linesize,
                                  float **src, int src_linesize,
                                  int w, int h);

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    DCTdnoizContext *s = ctx->priv;
    int i, x, y, bx, by, linesize, *iweights;
    const int bsize = 1 << s->n;

    switch (inlink->format) {
    case AV_PIX_FMT_BGR24:
        s->color_decorrelation = color_decorrelation_bgr;
        s->color_correlation   = color_correlation_bgr;
        break;
    case AV_PIX_FMT_RGB24:
        s->color_decorrelation = color_decorrelation_rgb;
        s->color_correlation   = color_correlation_rgb;
        break;
    default:
        av_assert0(0);
    }

    s->pr_width  = inlink->w - (inlink->w - bsize) % s->step;
    s->pr_height = inlink->h - (inlink->h - bsize) % s->step;
    if (s->pr_width != inlink->w)
        av_log(ctx, AV_LOG_WARNING, "The last %d horizontal pixels won't be denoised\n",
               inlink->w - s->pr_width);
    if (s->pr_height != inlink->h)
        av_log(ctx, AV_LOG_WARNING, "The last %d vertical pixels won't be denoised\n",
               inlink->h - s->pr_height);

    s->p_linesize = linesize = FFALIGN(s->pr_width, 32);
    for (i = 0; i < 2; i++) {
        s->cbuf[i][0] = av_malloc(linesize * s->pr_height * sizeof(*s->cbuf[i][0]));
        s->cbuf[i][1] = av_malloc(linesize * s->pr_height * sizeof(*s->cbuf[i][1]));
        s->cbuf[i][2] = av_malloc(linesize * s->pr_height * sizeof(*s->cbuf[i][2]));
        if (!s->cbuf[i][0] || !s->cbuf[i][1] || !s->cbuf[i][2])
            return AVERROR(ENOMEM);
    }

    s->weights = av_malloc(s->pr_height * linesize * sizeof(*s->weights));
    if (!s->weights)
        return AVERROR(ENOMEM);
    iweights = av_calloc(s->pr_height, linesize * sizeof(*iweights));
    if (!iweights)
        return AVERROR(ENOMEM);
    for (y = 0; y < s->pr_height - bsize + 1; y += s->step)
        for (x = 0; x < s->pr_width - bsize + 1; x += s->step)
            for (by = 0; by < bsize; by++)
                for (bx = 0; bx < bsize; bx++)
                    iweights[(y + by)*linesize + x + bx]++;
    for (y = 0; y < s->pr_height; y++)
        for (x = 0; x < s->pr_width; x++)
            s->weights[y*linesize + x] = 1. / iweights[y*linesize + x];
    av_free(iweights);

    return 0;
}

static av_cold int init(AVFilterContext *ctx)
{
    DCTdnoizContext *s = ctx->priv;

    s->bsize = 1 << s->n;
    if (s->overlap == -1)
        s->overlap = s->bsize - 1;

    if (s->overlap > s->bsize - 1) {
        av_log(s, AV_LOG_ERROR, "Overlap value can not except %d "
               "with a block size of %dx%d\n",
               s->bsize - 1, s->bsize, s->bsize);
        return AVERROR(EINVAL);
    }

    if (s->expr_str) {
        int ret = av_expr_parse(&s->expr, s->expr_str, var_names,
                                NULL, NULL, NULL, NULL, 0, ctx);
        if (ret < 0)
            return ret;
        switch (s->n) {
        case 3: s->filter_freq_func = filter_freq_expr_8;  break;
        case 4: s->filter_freq_func = filter_freq_expr_16; break;
        default: av_assert0(0);
        }
    } else {
        switch (s->n) {
        case 3: s->filter_freq_func = filter_freq_sigma_8;  break;
        case 4: s->filter_freq_func = filter_freq_sigma_16; break;
        default: av_assert0(0);
        }
    }

    s->th   = s->sigma * 3.;
    s->step = s->bsize - s->overlap;
    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_BGR24, AV_PIX_FMT_RGB24,
        AV_PIX_FMT_NONE
    };
    ff_set_common_formats(ctx, ff_make_format_list(pix_fmts));
    return 0;
}

#define DCT3X3_0_0  0.5773502691896258f /*  1/sqrt(3) */
#define DCT3X3_0_1  0.5773502691896258f /*  1/sqrt(3) */
#define DCT3X3_0_2  0.5773502691896258f /*  1/sqrt(3) */
#define DCT3X3_1_0  0.7071067811865475f /*  1/sqrt(2) */
#define DCT3X3_1_2 -0.7071067811865475f /* -1/sqrt(2) */
#define DCT3X3_2_0  0.4082482904638631f /*  1/sqrt(6) */
#define DCT3X3_2_1 -0.8164965809277261f /* -2/sqrt(6) */
#define DCT3X3_2_2  0.4082482904638631f /*  1/sqrt(6) */

static av_always_inline void color_decorrelation(float **dst, int dst_linesize,
                                                 const uint8_t *src, int src_linesize,
                                                 int w, int h,
                                                 int r, int g, int b)
{
    int x, y;
    float *dstp_r = dst[0];
    float *dstp_g = dst[1];
    float *dstp_b = dst[2];

    for (y = 0; y < h; y++) {
        const uint8_t *srcp = src;

        for (x = 0; x < w; x++) {
            dstp_r[x] = srcp[r] * DCT3X3_0_0 + srcp[g] * DCT3X3_0_1 + srcp[b] * DCT3X3_0_2;
            dstp_g[x] = srcp[r] * DCT3X3_1_0 +                        srcp[b] * DCT3X3_1_2;
            dstp_b[x] = srcp[r] * DCT3X3_2_0 + srcp[g] * DCT3X3_2_1 + srcp[b] * DCT3X3_2_2;
            srcp += 3;
        }
        src += src_linesize;
        dstp_r += dst_linesize;
        dstp_g += dst_linesize;
        dstp_b += dst_linesize;
    }
}

static av_always_inline void color_correlation(uint8_t *dst, int dst_linesize,
                                               float **src, int src_linesize,
                                               int w, int h,
                                               int r, int g, int b)
{
    int x, y;
    const float *src_r = src[0];
    const float *src_g = src[1];
    const float *src_b = src[2];

    for (y = 0; y < h; y++) {
        uint8_t *dstp = dst;

        for (x = 0; x < w; x++) {
            dstp[r] = av_clip_uint8(src_r[x] * DCT3X3_0_0 + src_g[x] * DCT3X3_1_0 + src_b[x] * DCT3X3_2_0);
            dstp[g] = av_clip_uint8(src_r[x] * DCT3X3_0_1 +                         src_b[x] * DCT3X3_2_1);
            dstp[b] = av_clip_uint8(src_r[x] * DCT3X3_0_2 + src_g[x] * DCT3X3_1_2 + src_b[x] * DCT3X3_2_2);
            dstp += 3;
        }
        dst += dst_linesize;
        src_r += src_linesize;
        src_g += src_linesize;
        src_b += src_linesize;
    }
}

#define DECLARE_COLOR_FUNCS(name, r, g, b)                                          \
static void color_decorrelation_##name(float **dst, int dst_linesize,               \
                                       const uint8_t *src, int src_linesize,        \
                                       int w, int h)                                \
{                                                                                   \
    color_decorrelation(dst, dst_linesize, src, src_linesize, w, h, r, g, b);       \
}                                                                                   \
                                                                                    \
static void color_correlation_##name(uint8_t *dst, int dst_linesize,                \
                                     float **src, int src_linesize,                 \
                                     int w, int h)                                  \
{                                                                                   \
    color_correlation(dst, dst_linesize, src, src_linesize, w, h, r, g, b);         \
}

DECLARE_COLOR_FUNCS(rgb, 0, 1, 2)
DECLARE_COLOR_FUNCS(bgr, 2, 1, 0)

static void filter_plane(AVFilterContext *ctx,
                         float *dst, int dst_linesize,
                         const float *src, int src_linesize,
                         int w, int h)
{
    int x, y;
    DCTdnoizContext *s = ctx->priv;
    float *dst0 = dst;
    const float *weights = s->weights;

    // reset block sums
    memset(dst, 0, h * dst_linesize * sizeof(*dst));

    // block dct sums
    for (y = 0; y < h - s->bsize + 1; y += s->step) {
        for (x = 0; x < w - s->bsize + 1; x += s->step)
            s->filter_freq_func(s, src + x, src_linesize,
                                   dst + x, dst_linesize);
        src += s->step * src_linesize;
        dst += s->step * dst_linesize;
    }

    // average blocks
    dst = dst0;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++)
            dst[x] *= weights[x];
        dst += dst_linesize;
        weights += dst_linesize;
    }
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    DCTdnoizContext *s = ctx->priv;
    AVFilterLink *outlink = inlink->dst->outputs[0];
    int direct, plane;
    AVFrame *out;

    if (av_frame_is_writable(in)) {
        direct = 1;
        out = in;
    } else {
        direct = 0;
        out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
        if (!out) {
            av_frame_free(&in);
            return AVERROR(ENOMEM);
        }
        av_frame_copy_props(out, in);
    }

    s->color_decorrelation(s->cbuf[0], s->p_linesize,
                           in->data[0], in->linesize[0],
                           s->pr_width, s->pr_height);
    for (plane = 0; plane < 3; plane++)
        filter_plane(ctx, s->cbuf[1][plane], s->p_linesize,
                          s->cbuf[0][plane], s->p_linesize,
                          s->pr_width, s->pr_height);
    s->color_correlation(out->data[0], out->linesize[0],
                         s->cbuf[1], s->p_linesize,
                         s->pr_width, s->pr_height);

    if (!direct) {
        int y;
        uint8_t *dst = out->data[0];
        const uint8_t *src = in->data[0];
        const int dst_linesize = out->linesize[0];
        const int src_linesize = in->linesize[0];
        const int hpad = (inlink->w - s->pr_width) * 3;
        const int vpad = (inlink->h - s->pr_height);

        if (hpad) {
            uint8_t       *dstp = dst + s->pr_width * 3;
            const uint8_t *srcp = src + s->pr_width * 3;

            for (y = 0; y < s->pr_height; y++) {
                memcpy(dstp, srcp, hpad);
                dstp += dst_linesize;
                srcp += src_linesize;
            }
        }
        if (vpad) {
            uint8_t       *dstp = dst + s->pr_height * dst_linesize;
            const uint8_t *srcp = src + s->pr_height * src_linesize;

            for (y = 0; y < vpad; y++) {
                memcpy(dstp, srcp, inlink->w * 3);
                dstp += dst_linesize;
                srcp += src_linesize;
            }
        }

        av_frame_free(&in);
    }

    return ff_filter_frame(outlink, out);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    int i;
    DCTdnoizContext *s = ctx->priv;

    av_free(s->weights);
    for (i = 0; i < 2; i++) {
        av_free(s->cbuf[i][0]);
        av_free(s->cbuf[i][1]);
        av_free(s->cbuf[i][2]);
    }
    av_expr_free(s->expr);
}

static const AVFilterPad dctdnoiz_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
        .config_props = config_input,
    },
    { NULL }
};

static const AVFilterPad dctdnoiz_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_dctdnoiz = {
    .name          = "dctdnoiz",
    .description   = NULL_IF_CONFIG_SMALL("Denoise frames using 2D DCT."),
    .priv_size     = sizeof(DCTdnoizContext),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs        = dctdnoiz_inputs,
    .outputs       = dctdnoiz_outputs,
    .priv_class    = &dctdnoiz_class,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};