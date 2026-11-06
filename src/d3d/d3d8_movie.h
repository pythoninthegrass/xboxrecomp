/*
 * d3d8_movie.h -- a decoded movie frame drawn over the finished frame.
 *
 * The Xbox shows some movies on its video overlay, a plane the scan-out puts
 * in front of the frame buffer (D3DDevice_UpdateOverlay). The host has no
 * such plane, so the frame is drawn over the back buffer just before it is
 * presented, with proportional fit or title-selected width fill.
 *
 * Same rules as d3d8_overlay.h, which it is modelled on: the render target
 * and viewport are put back, nothing binds a sampler or a vertex buffer, and
 * everything else it sets is set again by the next draw. It uses
 * shader-resource slot 9 and constant slot 7.
 */
#ifndef XBOXRECOMP_D3D8_MOVIE_H
#define XBOXRECOMP_D3D8_MOVIE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hands over a new frame: BGRA, top row first, width*height*4 bytes. Copied
 * before it returns. */
void d3d8_movie_set_frame(const uint8_t *bgra, uint32_t width, uint32_t height,
                            int fill_width, unsigned crop_left, unsigned crop_right);

/* Rect in clip space. Cinematic fill preserves the source's display aspect;
 * excess height is clipped by the output viewport. Other movies fit intact. */
static inline void d3d8_movie_rectangle(unsigned width, unsigned height,
                                        int fill_width, float *w, float *h)
{
    float aspect = height ? (float)width / height : 4.f / 3.f;
    const float source = 4.f / 3.f; /* Xbox non-square-pixel display aspect. */
    *w = !fill_width && aspect > source ? 2.f * source / aspect : 2.f;
    *h = fill_width || aspect < source ? 2.f * aspect / source : 2.f;
}

/* Crop authored side padding by extending the source quad proportionally.
 * UVs and source pixels remain unchanged. Narrow/default modes fit intact. */
static inline void d3d8_movie_bounds(unsigned width, unsigned height,
    int fill_width, unsigned frame_width, unsigned left, unsigned right,
    float *x, float *y, float *w, float *h)
{
    d3d8_movie_rectangle(width, height, fill_width, w, h);
    *x = -*w / 2.f;
    if (fill_width && height && (double)width * 3 > (double)height * 4 &&
        left < frame_width && right < frame_width - left) {
        float scale = (float)frame_width / (frame_width - left - right);
        *w *= scale;
        *h *= scale;
        *x = -1.f - *w * left / frame_width;
    }
    *y = *h / 2.f;
}

/* Draws the last frame handed over across the back buffer. Does nothing if
 * there is none. */
void d3d8_movie_draw(void);

/* Forgets the frame, so d3d8_movie_draw does nothing until the next one. */
void d3d8_movie_clear(void);

void d3d8_movie_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
