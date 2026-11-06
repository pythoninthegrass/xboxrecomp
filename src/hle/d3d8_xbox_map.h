/*
 * d3d8_xbox_map.h -- Xbox D3D8 enumerations to the host layer's PC ones.
 *
 * Used by shadow mode (hle_d3d8.c). The frame replay tool shared these while
 * captures held Xbox values; captures now hold host values (d3d8_capture.h,
 * version 2), so replay no longer needs them. Header-only and static inline
 * all the same, so a second user can include them without linking xbox_hle.
 *
 * Windows only: the host D3D8 types come from d3d8_xbox.h, which is part of
 * the D3D11 backend. Everything here is pure value mapping -- no device, no
 * state, no allocation.
 */
#ifndef XBOXRECOMP_D3D8_XBOX_MAP_H
#define XBOXRECOMP_D3D8_XBOX_MAP_H

#include <stdint.h>
#include "d3d8_xbox.h"

/* Xbox D3DPRIMITIVETYPE (Cxbx-Reloaded, XbD3D8Types.h). */
enum {
    XPT_POINTLIST = 1, XPT_LINELIST, XPT_LINELOOP, XPT_LINESTRIP,
    XPT_TRIANGLELIST, XPT_TRIANGLESTRIP, XPT_TRIANGLEFAN,
    XPT_QUADLIST, XPT_QUADSTRIP, XPT_POLYGON
};

/* Preserve per-channel clears: FP uses alpha as a temporary text mask.
 * The standard host target flag remains a full RGBA clear; channel flags
 * carry partial clears through both renderers and diagnostic captures. */
static __inline DWORD xbox_clear_flags_to_host(uint32_t xbox_flags)
{
    DWORD channels=xbox_flags & D3DCLEAR_CHANNEL_MASK;
    DWORD host=channels==D3DCLEAR_CHANNEL_MASK ? D3DCLEAR_TARGET : channels;
    if(xbox_flags & 0x01)host |= D3DCLEAR_ZBUFFER;
    if(xbox_flags & 0x02)host |= D3DCLEAR_STENCIL;
    return host;
}

/* An Xbox primitive type and a vertex count, against the host's PC numbering
 * and a primitive count. Line loops, quad strips and polygons have no PC
 * equivalent: a quad strip draws as the triangle strip over the same
 * vertices, a polygon as a fan, and a line loop as a strip that repeats its
 * first vertex. */
static __inline int xbox_primitive_to_host(uint32_t xpt, uint32_t vertices,
                                           D3DPRIMITIVETYPE *pt, UINT *prims)
{
    switch (xpt) {
    case XPT_POINTLIST:     *pt = D3DPT_POINTLIST;     *prims = vertices;       break;
    case XPT_LINELIST:      *pt = D3DPT_LINELIST;      *prims = vertices / 2;   break;
    case XPT_LINELOOP:      *pt = D3DPT_LINESTRIP;     *prims = vertices;       break;
    case XPT_LINESTRIP:     *pt = D3DPT_LINESTRIP;     *prims = vertices - 1;   break;
    case XPT_TRIANGLELIST:  *pt = D3DPT_TRIANGLELIST;  *prims = vertices / 3;   break;
    case XPT_TRIANGLESTRIP: *pt = D3DPT_TRIANGLESTRIP; *prims = vertices - 2;   break;
    case XPT_QUADSTRIP:     /* whole quads only; an odd last vertex is unused */
                            *pt = D3DPT_TRIANGLESTRIP; *prims = (vertices & ~1u) - 2; break;
    case XPT_TRIANGLEFAN:
    case XPT_POLYGON:       *pt = D3DPT_TRIANGLEFAN;   *prims = vertices - 2;   break;
    case XPT_QUADLIST:      *pt = D3DPT_QUADLIST;      *prims = vertices / 4;   break;
    default:
        return 0;
    }
    /* A single point is a valid draw (Cxbx-Reloaded, IsValidXboxVertexCount);
     * too few vertices for anything else leaves prims at 0 or wrapped below. */
    return (int)*prims > 0 && *prims <= vertices;
}

/* The Xbox packs the transform states together -- VIEW 0, PROJECTION 1,
 * TEXTURE0-3 2-5, WORLD-WORLD3 6-9 (Cxbx-Reloaded, XbD3D8Types.h) -- where
 * the host has the PC's 2, 3, 16-19, 256-259. Returns 0 for a state outside
 * that range, which the caller drops. */
static __inline int xbox_transform_state_to_host(uint32_t state, DWORD *host)
{
    static const DWORD table[10] = { 2, 3, 16, 17, 18, 19, 256, 257, 258, 259 };

    if (state >= 10u)
        return 0;
    *host = table[state];
    return 1;
}

#endif /* XBOXRECOMP_D3D8_XBOX_MAP_H */

/* Clear history for host depth replicas of the same guest image.
 * Host targets may have a different size from the Xbox depth attachment.
 * Keep depth and stencil independent; binding a replica is not a new clear.
 * No platform or graphics API dependency: flag values are D3DCLEAR_*.
 */
#ifndef HLE_D3D8_DEPTH_CLEAR_H
#define HLE_D3D8_DEPTH_CLEAR_H
#include <stdint.h>
#include <string.h>
#define HLE_DEPTH_CLEAR_SLOTS 32
typedef struct { uint32_t data, format, size; } HleDepthKey;
typedef struct { uint64_t depth, stencil; } HleDepthApplied;
typedef struct {
    HleDepthKey key;
    HleDepthApplied generation;
    float z;
    uint32_t stencil;
    int valid;
} HleDepthClear;
typedef struct {
    HleDepthClear entries[HLE_DEPTH_CLEAR_SLOTS];
    uint64_t serial;
    unsigned next;
} HleDepthClears;
static int hle_depth_key_equal(HleDepthKey a, HleDepthKey b)
{
    return a.data == b.data && a.format == b.format && a.size == b.size;
}
static HleDepthClear *hle_depth_clear_find(HleDepthClears *history, HleDepthKey key)
{
    unsigned i;
    HleDepthClear *clear;
    for (i = 0; i < HLE_DEPTH_CLEAR_SLOTS; ++i)
        if (history->entries[i].valid && hle_depth_key_equal(history->entries[i].key, key))
            return &history->entries[i];
    clear = &history->entries[history->next++ % HLE_DEPTH_CLEAR_SLOTS];
    memset(clear, 0, sizeof *clear);
    clear->valid = 1;
    clear->key = key;
    /* Newly allocated host storage must not start at near depth. A real
     * guest clear replaces these defaults, even before a replica exists. */
    clear->z = 1.0f;
    clear->generation.depth = clear->generation.stencil = ++history->serial;
    return clear;
}
static void hle_depth_clear_note(HleDepthClears *history, HleDepthClear *clear,
                                 uint32_t flags, float z, uint32_t stencil)
{
    if (flags & 2) { clear->z = z; clear->generation.depth = ++history->serial; }
    if (flags & 4) { clear->stencil = stencil; clear->generation.stencil = ++history->serial; }
}
static uint32_t hle_depth_clear_pending(const HleDepthApplied *applied,
                                        const HleDepthClear *clear)
{
    return (applied->depth != clear->generation.depth ? 2u : 0u) |
           (applied->stencil != clear->generation.stencil ? 4u : 0u);
}
static void hle_depth_clear_applied(HleDepthApplied *applied,
                                    const HleDepthClear *clear, uint32_t flags)
{
    if (flags & 2) applied->depth = clear->generation.depth;
    if (flags & 4) applied->stencil = clear->generation.stencil;
}
#endif
