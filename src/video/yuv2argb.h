/*
 * yuv2argb.h
 *
 * Fused YV12 -> ARGB8888 conversion + nearest-neighbour scaling for the
 * libretro software render path.  Reads the three planes in place (with
 * their pitches) and writes directly into the destination surface, so the
 * old pack-memcpy + SDL_ConvertPixels + separate scale pass (three full
 * passes over the frame) become a single pass.  SDL2's YUV converter is
 * SSE-only, i.e. plain scalar on ARM; here the common 1:1 row is NEON.
 *
 * Coefficients are BT.601 studio range (matches SDL_ConvertPixels YV12),
 * fixed point scaled by 64:  R = (74*C + 102*E + 32) >> 6, etc.
 *
 * Self-check:  g++ -O2 -DYUV2ARGB_SELFTEST -x c++ src/video/yuv2argb.h -o t && ./t
 */

#ifndef YUV2ARGB_H
#define YUV2ARGB_H

#include <stdint.h>
#include <string.h>
#include <stddef.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

static inline uint32_t yuv2argb_px(uint8_t yv, uint8_t uv, uint8_t vv)
{
    int c = yv - 16;
    if (c < 0) c = 0; /* match the NEON path's saturating subtract */
    int d = uv - 128, e = vv - 128;
    int y74 = c * 74;
    int r = (y74 + 102 * e + 32) >> 6;
    int g = (y74 - 25 * d - 52 * e + 32) >> 6;
    int b = (y74 + 129 * d + 32) >> 6;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Convert one row 1:1 (dst x == src x).  w may be any width; the NEON body
 * handles 16 px per iteration, the scalar tail the rest. */
static inline void yv12row_to_argb(uint32_t *dst, const uint8_t *yrow,
                                   const uint8_t *urow, const uint8_t *vrow,
                                   int w)
{
    int x = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    const uint8x16_t c16  = vdupq_n_u8(16);
    const uint8x8_t  c74  = vdup_n_u8(74);
    const int16x8_t  c128 = vdupq_n_s16(128);
    const int16x8_t  rnd  = vdupq_n_s16(32);
    const uint8x16_t a255 = vdupq_n_u8(255);
    for (; x + 16 <= w; x += 16) {
        /* luma * 74 (u8*u8 -> u16, max 239*74 = 17686, fits s16) */
        uint8x16_t y8  = vqsubq_u8(vld1q_u8(yrow + x), c16);
        int16x8_t  ylo = vreinterpretq_s16_u16(vmull_u8(vget_low_u8(y8),  c74));
        int16x8_t  yhi = vreinterpretq_s16_u16(vmull_u8(vget_high_u8(y8), c74));

        /* chroma contributions, one lane per 2x2 block (8 lanes / 16 px) */
        int16x8_t d = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vld1_u8(urow + x / 2))), c128);
        int16x8_t e = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vld1_u8(vrow + x / 2))), c128);
        int16x8_t rc = vmulq_n_s16(e, 102);
        int16x8_t gc = vaddq_s16(vmulq_n_s16(d, 25), vmulq_n_s16(e, 52));
        int16x8_t bc = vmulq_n_s16(d, 129);

        /* duplicate each chroma lane to its 2 pixels */
        int16x8x2_t rcz = vzipq_s16(rc, rc);
        int16x8x2_t gcz = vzipq_s16(gc, gc);
        int16x8x2_t bcz = vzipq_s16(bc, bc);

        /* saturating adds: y74 + bc can exceed s16 max, vqadd clamps to
         * 32767 and vqshrun then clamps to 255 -- same result as exact */
        uint8x8_t rlo = vqshrun_n_s16(vqaddq_s16(vqaddq_s16(ylo, rcz.val[0]), rnd), 6);
        uint8x8_t rhi = vqshrun_n_s16(vqaddq_s16(vqaddq_s16(yhi, rcz.val[1]), rnd), 6);
        uint8x8_t glo = vqshrun_n_s16(vqaddq_s16(vqsubq_s16(ylo, gcz.val[0]), rnd), 6);
        uint8x8_t ghi = vqshrun_n_s16(vqaddq_s16(vqsubq_s16(yhi, gcz.val[1]), rnd), 6);
        uint8x8_t blo = vqshrun_n_s16(vqaddq_s16(vqaddq_s16(ylo, bcz.val[0]), rnd), 6);
        uint8x8_t bhi = vqshrun_n_s16(vqaddq_s16(vqaddq_s16(yhi, bcz.val[1]), rnd), 6);

        /* ARGB8888 little-endian memory order is B,G,R,A */
        uint8x16x4_t out;
        out.val[0] = vcombine_u8(blo, bhi);
        out.val[1] = vcombine_u8(glo, ghi);
        out.val[2] = vcombine_u8(rlo, rhi);
        out.val[3] = a255;
        vst4q_u8((uint8_t *)(dst + x), out);
    }
#endif
    for (; x < w; ++x)
        dst[x] = yuv2argb_px(yrow[x], urow[x >> 1], vrow[x >> 1]);
}

/*
 * Convert the bw x bh YV12 image into dst at rect (dx0,dy0,dw,dh) with
 * nearest-neighbour scaling, clipped to the vw x vh destination surface.
 * dst_stride is in pixels.  Duplicated rows (upscale) are memcpy'd, and
 * the unscaled-width case goes through the vectorised row converter.
 */
static inline void yv12_to_argb_scaled(const uint8_t *Y, const uint8_t *U,
                                       const uint8_t *V, int ypitch,
                                       int upitch, int vpitch, int bw, int bh,
                                       uint32_t *dst, int dst_stride, int vw,
                                       int vh, int dx0, int dy0, int dw,
                                       int dh)
{
    if (bw <= 0 || bh <= 0 || dw <= 0 || dh <= 0) return;
    int dx_start = dx0 < 0 ? -dx0 : 0;
    int dx_end   = dw < vw - dx0 ? dw : vw - dx0;
    int dy_start = dy0 < 0 ? -dy0 : 0;
    int dy_end   = dh < vh - dy0 ? dh : vh - dy0;
    if (dx_start >= dx_end || dy_start >= dy_end) return;

    /* fast path: no horizontal scaling and the full source row is visible */
    const int fast = (dw == bw && dx_start == 0 && dx_end == dw);

    int       prev_sy  = -1;
    uint32_t *prev_row = 0;
    size_t    row_bytes = (size_t)(dx_end - dx_start) * 4;

    for (int dy = dy_start; dy < dy_end; ++dy) {
        int       sy      = (dy * bh) / dh;
        uint32_t *row_dst = dst + (size_t)(dy0 + dy) * dst_stride + dx0 + dx_start;

        if (sy == prev_sy && prev_row) { /* duplicated row on upscale */
            memcpy(row_dst, prev_row, row_bytes);
            prev_row = row_dst;
            continue;
        }
        const uint8_t *yrow = Y + (size_t)sy * ypitch;
        const uint8_t *urow = U + (size_t)(sy >> 1) * upitch;
        const uint8_t *vrow = V + (size_t)(sy >> 1) * vpitch;

        if (fast) {
            yv12row_to_argb(row_dst, yrow, urow, vrow, dw);
        } else {
            int sx  = (dx_start * bw) / dw;
            int err = (dx_start * bw) % dw;
            for (int dx = dx_start; dx < dx_end; ++dx) {
                row_dst[dx - dx_start] =
                    yuv2argb_px(yrow[sx], urow[sx >> 1], vrow[sx >> 1]);
                err += bw;
                while (err >= dw) { ++sx; err -= dw; }
            }
        }
        prev_sy  = sy;
        prev_row = row_dst;
    }
}

#ifdef YUV2ARGB_SELFTEST
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main()
{
    /* 1. scalar pixel vs float BT.601 reference, whole YUV cube (step 3) */
    for (int y = 0; y < 256; y += 3)
        for (int u = 0; u < 256; u += 3)
            for (int v = 0; v < 256; v += 3) {
                uint32_t p = yuv2argb_px((uint8_t)y, (uint8_t)u, (uint8_t)v);
                double c = y - 16 < 0 ? 0 : y - 16, d = u - 128, e = v - 128;
                double rf = (298 * c + 409 * e) / 256.0;
                double gf = (298 * c - 100 * d - 208 * e) / 256.0;
                double bf = (298 * c + 516 * d) / 256.0;
                double cl[3] = {rf < 0 ? 0 : rf > 255 ? 255 : rf,
                                gf < 0 ? 0 : gf > 255 ? 255 : gf,
                                bf < 0 ? 0 : bf > 255 ? 255 : bf};
                int got[3] = {(int)(p >> 16 & 255), (int)(p >> 8 & 255),
                              (int)(p & 255)};
                for (int i = 0; i < 3; ++i)
                    assert(fabs(got[i] - cl[i]) <= 3.0);
                assert((p >> 24) == 255);
            }

    /* 2. fused/vectorised converter vs naive per-pixel loop */
    srand(42);
    const int bw = 322, bh = 242; /* even-odd mix around chroma edges */
    static uint8_t Yp[400 * 300], Up[200 * 150], Vp[200 * 150];
    int ypitch = 340, upitch = 170, vpitch = 170;
    for (size_t i = 0; i < sizeof(Yp); ++i) Yp[i] = (uint8_t)rand();
    for (size_t i = 0; i < sizeof(Up); ++i) Up[i] = (uint8_t)rand();
    for (size_t i = 0; i < sizeof(Vp); ++i) Vp[i] = (uint8_t)rand();

    struct { int vw, vh, dx0, dy0, dw, dh; } cases[] = {
        {322, 242, 0, 0, 322, 242},   /* 1:1 (fast path) */
        {700, 500, 10, 20, 644, 484}, /* 2x upscale, offset */
        {200, 200, 0, 0, 161, 121},   /* downscale */
        {322, 242, -5, -3, 322, 242}, /* negative offset clip */
        {322, 242, 300, 230, 322, 242}, /* clip right/bottom */
    };
    for (size_t t = 0; t < sizeof(cases) / sizeof(cases[0]); ++t) {
        int vw = cases[t].vw, vh = cases[t].vh;
        int dx0 = cases[t].dx0, dy0 = cases[t].dy0;
        int dw = cases[t].dw, dh = cases[t].dh;
        int stride = vw + 7;
        uint32_t *got = (uint32_t *)calloc((size_t)stride * vh, 4);
        uint32_t *ref = (uint32_t *)calloc((size_t)stride * vh, 4);
        yv12_to_argb_scaled(Yp, Up, Vp, ypitch, upitch, vpitch, bw, bh, got,
                            stride, vw, vh, dx0, dy0, dw, dh);
        for (int dy = 0; dy < dh; ++dy) {
            int ty = dy0 + dy;
            if (ty < 0 || ty >= vh) continue;
            int sy = (dy * bh) / dh;
            for (int dx = 0; dx < dw; ++dx) {
                int tx = dx0 + dx;
                if (tx < 0 || tx >= vw) continue;
                int sx = (dx * bw) / dw;
                ref[(size_t)ty * stride + tx] =
                    yuv2argb_px(Yp[sy * ypitch + sx],
                                Up[(sy / 2) * upitch + sx / 2],
                                Vp[(sy / 2) * vpitch + sx / 2]);
            }
        }
        assert(memcmp(got, ref, (size_t)stride * vh * 4) == 0);
        free(got);
        free(ref);
    }
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    printf("yuv2argb self-test OK (NEON path)\n");
#else
    printf("yuv2argb self-test OK (scalar path)\n");
#endif
    return 0;
}
#endif /* YUV2ARGB_SELFTEST */

#endif /* YUV2ARGB_H */
