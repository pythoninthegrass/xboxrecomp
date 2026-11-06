/**
 * Xbox Memory Layout Implementation
 *
 * Maps the XBE data sections to their expected virtual addresses on Windows.
 * This is critical for the recompiled code which references globals by
 * absolute address (e.g., mov eax, [0x004D532C]).
 *
 * Implementation:
 * 1. VirtualAlloc a contiguous region at XBOX_BASE_ADDRESS
 * 2. Copy .rdata and initialized .data from the XBE
 * 3. Zero-fill the BSS region
 * 4. Set memory protection (read-only for .rdata)
 */

#include "xbox_memory_layout.h"
#include "guest_image.h"
#include "guest_allocation.h"
#include "guest_thread_memory.h"
#include "guest_virtual_memory.h"
#include "guest_memory_services.h"
#include "host_pages.h"
#include "host_memory_stats.h"
#include "host_virtual_memory.h"
#include "pc_guest_memory.h"
#include "kernel.h"
#include "recomp_config.h"
#include "xbox_watchpoint.h"
#include <stdio.h>
/* <stdlib.h> is load-bearing, not tidiness.
 *
 * Without it MSVC applies the implicit-declaration rule and assumes
 * `int getenv()`, so the returned pointer is truncated to 32 bits and sign
 * extended. RECOMP_WATCHDOG_SECS was the visible symptom: `if (!secs ||
 * !*secs)` dereferenced 0xFFFFFFFFA94859D5 and faulted about one run in
 * three, and when the truncated byte happened to read zero it returned
 * early instead -- so the watchdog was either a crash or silently inert,
 * and never once fired. The `getenv(...) != NULL` tests elsewhere in this
 * file survived only because a truncated non-zero value is still non-zero.
 */
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#if !defined(_WIN32)
#include <unistd.h>   /* _exit */
#endif

/* Certificate display metadata is decoded by the project image provider. */
static char g_xbe_title_name[128];

const char *xbox_XbeTitleName(void)
{
    return g_xbe_title_name[0] ? g_xbe_title_name : NULL;
}

static void *g_memory_base = NULL;
static size_t g_memory_size = 0;
static ptrdiff_t g_memory_offset = 0;  /* actual_base - XBOX_BASE_ADDRESS */

/* Actual mapped RAM for this run; see the header. Default retail 64 MB. */
size_t g_xbox_total_ram = XBOX_TOTAL_RAM;
size_t g_xbox_map_size = 0;   /* 0 = same as RAM */

void xbox_SetTotalRam(size_t bytes)
{
    g_xbox_total_ram = bytes;
}


void xbox_SetMapSize(size_t bytes)
{
    g_xbox_map_size = bytes;
}

/* File mapping handle for the Xbox memory region.
 * Using CreateFileMapping + MapViewOfFileEx allows mirror views to alias
 * the same physical pages as the base region, so writes to mirror addresses
 * (which wrap modulo 64 MB on real Xbox hardware) correctly modify the
 * underlying data. */
static HANDLE g_mapping_handle = NULL;

/* Mirror view pointers for cleanup */
static void *g_mirror_views[XBOX_NUM_MIRRORS] = {0};
static void *g_tiled_view = NULL;

/* Contiguous / physical memory window (see MemoryLayoutInit).
 * XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE come from kernel.h - the bridges need
 * the same numbers for MmClaimGpuInstanceMemory. */
static void *g_contig_memory = NULL;

/* NV2A GPU register aperture (see MemoryLayoutInit). Backed as plain RAM so
 * that D3D8 code linked into the title can poke it without faulting. */
#define XBOX_NV2A_BASE 0xFD000000u
#define XBOX_NV2A_SIZE (16u * 1024u * 1024u)
static void *g_nv2a_memory = NULL;

/* MCPX southbridge register span: APU 0xFE800000 through NIC 0xFEF00000. */
#define XBOX_MCPX_BASE 0xFE800000u
#define XBOX_MCPX_SIZE (8u * 1024u * 1024u)
/* The AC97 page, offset from the MCPX base. Same constant as apu.h; the
 * kernel does not link the APU library, so it is repeated rather than
 * shared. */
#define XBOX_MCPX_AC97_PAGE 0x00400000u
/* The CRTC page, offset from the NV2A base. Same constant as apu.h. */
#define XBOX_NV2A_PCRTC_PAGE 0x00600000u
static void *g_mcpx_memory = NULL;

/* Flash ROM. The console's 256 KB flash is mirrored through the top of the
 * address space, and the MCPX span above stops one page short of it -- so a
 * title that touches it faulted on an address that is perfectly ordinary on
 * hardware.
 *
 * The Xbox Dashboard does, from two directions at once: its XIP workers hash
 * 64 KB from 0xFF000000 (it verifies archives against digests), and its render
 * path writes to 0xFF000040. Both are hard faults today, and they kill the
 * process a few dozen lines after its first frame clears.
 *
 * Plain memory, like the other two apertures, and mapped for the same stated
 * reason: a read of zero is survivable, a fault is not. Zeros are not the
 * console's BIOS, so a digest taken over this will not match one taken over
 * real flash -- that is a separate question from whether the access should
 * fault, and this is the half that has an obviously right answer. */
#define XBOX_FLASH_BASE 0xFF000000u
#define XBOX_FLASH_SIZE (1u * 1024u * 1024u)
static void *g_flash_memory = NULL;
/* The contiguous window's backing section. It is a file mapping rather than
 * plain committed memory for one reason: the tiled aperture has to be a
 * second view of the very same bytes, and only a mapping can be mapped
 * twice. See the tiled aperture below for why that matters.
 */
static HANDLE g_contig_mapping = NULL;
/* How much of the tiled aperture can exist.
 *
 * Two ceilings, both below the mapped RAM size once that is large:
 *
 *   - it starts at 0xF0000000 in a 32-bit guest address space, so it can
 *     never reach past 0x100000000; and
 *   - the NV2A register aperture sits at 0xFD000000, which is where the
 *     window really ends on hardware.
 *
 * Asking for the full RAM size overlapped both and MapViewOfFileEx failed
 * with ERROR_INVALID_ADDRESS -- a warning at startup and then a fault on the
 * title's first surface write, with nothing connecting the two. */
/* How much guest address space is mapped.
 *
 * For anything that dereferences an address it read out of guest memory --
 * a descriptor pointer a device model follows, say. Those are attacker-ish
 * input in the only sense that matters here: the title can leave one
 * uninitialised, and 0xCCCCCCCC dereferenced is a crash in the runtime
 * rather than a fault the title would have taken. */
size_t xbox_GetMappedSize(void)
{
    return g_memory_size;
}

static size_t xbox_TiledApertureSize(void)
{
    uint64_t end = XBOX_NV2A_BASE < 0x100000000ULL
                 ? XBOX_NV2A_BASE : 0x100000000ULL;
    size_t max = (size_t)(end - XBOX_TILED_BASE);
    return g_memory_size < max ? g_memory_size : max;
}

static HANDLE g_nv2a_ack_thread = NULL;
static HANDLE g_nv2a_ack_wake = NULL; /* auto-reset: requests may coalesce */
static volatile LONG g_nv2a_ack_notifiers = 0;
static volatile LONG g_nv2a_ack_stop = 0;

/*
 * NV2A busy-bit acknowledgement.
 *
 * D3D8 talks to the GPU through set-a-bit / wait-for-hardware-to-clear-it
 * handshakes. Against plain RAM the bit is set and nothing ever clears it, so
 * the title spins forever. Halo hangs in the push-buffer kick at 0x001EF930:
 *
 *     mov  [eax+0x100410], edx     ; set 0x10000
 *   L: test [eax+0x100410], 0x10000
 *     jne  L                       ; wait for the GPU
 *
 * Clearing those bits from a thread is not a hack around the handshake, it is
 * the handshake: on hardware the GPU clears them asynchronously, which is
 * exactly what this does. Work that would have been submitted is being done by
 * the D3D11 layer instead, so acknowledging immediately is honest.
 *
 * Only registers listed here are touched. Blanket-zeroing the aperture would
 * also wipe registers holding real state.
 *
 * ponytail: table-driven, extend as more handshakes turn up. A spin on a bit
 * that is not listed still hangs -- run the title and the watchdog sample will
 * name the register.
 */
/* The interrupt-status registers, as opposed to the busy bits that share the
 * same table. Only these are the title's to clear. */
static int intr_status_reg(uint32_t offset)
{
    return offset == 0x000100u      /* PMC_INTR_0   */
        || offset == 0x600100u;     /* PCRTC_INTR_0 */
}

/* Set when the vblank tick is delivering interrupts, so the acknowledgement
 * thread knows a real ISR is servicing them. */
/* Written by the timer thread, read by the acknowledgement thread, so it is
 * shared state and gets the same interlocked treatment as g_nv2a_ack_stop. */
static volatile LONG s_vblank_owns_intr = 0;

void xbox_NV2A_VblankOwnsInterrupts(int owns)
{
    InterlockedExchange(&s_vblank_owns_intr, owns ? 1 : 0);
}

static const struct { uint32_t offset; uint32_t busy_mask; } NV2A_ACK[] = {
    { 0x100410, 0x00010000u },  /* PFB flush kick, Halo 0x001EF930 */

    /* Interrupt status registers. These are write-1-to-clear on hardware, so
     * an ISR "clearing" one writes the pending bit back -- against plain RAM
     * that sets it instead, the interrupt stays pending forever, and the
     * service routine re-enters until the stack is gone. Halo dies exactly
     * that way: CMiniport::ServiceGrInterrupt writes 0x1000 to PGRAPH_INTR to
     * acknowledge, reads it back still pending, and recurses into a native
     * stack overflow.
     *
     * Holding them at zero is correct rather than convenient: nothing here
     * ever raises a GPU interrupt, so "none pending" is the truth. */
    { 0x000100, 0xFFFFFFFFu },  /* PMC_INTR_0    */
    { 0x001100, 0xFFFFFFFFu },  /* PBUS_INTR_0   */
    { 0x002100, 0xFFFFFFFFu },  /* PFIFO_INTR_0  */
    { 0x400100, 0xFFFFFFFFu },  /* PGRAPH_INTR   */
    { 0x600100, 0xFFFFFFFFu },  /* PCRTC_INTR_0  */
};

/*
 * Bits that must always read as SET. The mirror image of the table above:
 * where an interrupt-pending bit is false because nothing raises interrupts,
 * a queue-empty bit is true because nothing is queued.
 *
 * Halo's CMiniport::TilingUpdateIdle spins until the PFIFO caches report
 * empty (0x001F5CD1). Zeroed RAM says "not empty" forever, so tile setup
 * during CDevice::InitializeFrameBuffers never completes.
 *
 * Note 0x003220 is deliberately absent -- that one exits on the bit being
 * CLEAR, which zeroed memory already gives.
 */
static const struct { uint32_t offset; uint32_t idle_mask; } NV2A_IDLE[] = {
    { 0x002400, 0x00000010u },  /* PFIFO_RUNOUT_STATUS  LOW_MARK (empty) */
    { 0x003214, 0x00000010u },  /* PFIFO_CACHE1_STATUS  LOW_MARK (empty) */
};

/*
 * PFIFO channel DMA pointers. Software writes DMA_PUT and spins until the GPU
 * advances DMA_GET to match -- "you have consumed everything I submitted".
 * Halo's wait is at 0x001F3948:
 *
 *   L: call BusyLoop
 *      ecx = [[dev+0x2304] + 0x44]   ; DMA_GET
 *      edx = [dev]                   ; DMA_PUT
 *      test (edx ^ ecx), 0xfffffff
 *      jne L
 *
 * [dev+0x2304] is 0xFD800000, so the channel's USER area sits at aperture
 * offset 0x800000 and the two pointers are at +0x40 / +0x44. Copying PUT to
 * GET is the acknowledgement; the commands are not executed from the push
 * buffer here -- the D3D11 layer draws -- so reporting them consumed is the
 * truthful answer.
 *
 * This was written once, removed, and restored. It was removed because
 * [dev+0x2304] read as 0x0080F7FF, i.e. no register to acknowledge -- but that
 * garbage was a downstream symptom of ordinal 47 having no stdcall arg size,
 * which walked esp 8 bytes off and made D3D initialise the DMA channel with
 * `this` = 1. With that fixed the pointer is correct and so is this.
 */
#define NV2A_USER_DMA_PUT 0x800040u
#define NV2A_USER_DMA_GET 0x800044u

/*
 * Free-running counters in the MCPX aperture.
 *
 * Some hardware registers are clocks, not flags: software reads them and waits
 * until the value passes a target. Against zeroed RAM the value never moves and
 * the wait is forever. DirectSound's CMcpxCore::SetupVoiceProcessor spins on
 * the APU sample counter at 0xFE820010 exactly this way, which is where Halo
 * stopped once input initialisation started working.
 *
 * Ticking it is the honest model: on hardware this counter advances on its own
 * whether or not anything is listening.
 *
 * ponytail: the rate is "as fast as this thread loops", not 48 kHz. Nothing
 * paces audio off it yet. Derive it from a real clock if timing starts to
 * matter.
 */
static const uint32_t MCPX_COUNTERS[] = {
    0x020010,   /* APU GP sample counter, DirectSound SetupVoiceProcessor */
};

static void *g_mcpx_regs = NULL;
/* Set when the APU's registers are unmapped so they can be routed to the
 * emulated APU. Once that happens they are no longer plain memory, and the
 * counter ticking below must leave them alone -- writing through the pointer
 * faults, and the emulated APU owns those registers anyway. */
static int g_apu_mmio_trapped = 0;

/*
 * GPU completion fences the title waits on in guest memory rather than in the
 * aperture. See xbox_Nv2aMirrorFence in the header for why this is the same
 * acknowledgement the NV2A_ACK table makes, and why the address has to be
 * followed through the device struct instead of being a constant.
 */
#define XBOX_MAX_FENCE_MIRRORS 4

static struct {
    uint32_t device_ptr_va;
    uint32_t put_off;
    uint32_t get_ptr_off;
} g_fence_mirrors[XBOX_MAX_FENCE_MIRRORS];
static int g_fence_mirror_count = 0;

int xbox_Nv2aMirrorFence(uint32_t device_ptr_va,
                         uint32_t put_off, uint32_t get_ptr_off)
{
    if (g_fence_mirror_count >= XBOX_MAX_FENCE_MIRRORS)
        return -1;
    g_fence_mirrors[g_fence_mirror_count].device_ptr_va = device_ptr_va;
    g_fence_mirrors[g_fence_mirror_count].put_off = put_off;
    g_fence_mirrors[g_fence_mirror_count].get_ptr_off = get_ptr_off;
    g_fence_mirror_count++;
    fprintf(stderr, "  NV2A fence mirror: device at 0x%08X,"
            " PUT +0x%X -> *(GET +0x%X)\n",
            device_ptr_va, put_off, get_ptr_off);
    return 0;
}
/* A guest address is usable only once the window is mapped and it lands
 * inside it; the chain is followed fresh every poll because the title may not
 * have built it yet. */
static int fence_readable(uint32_t va, uint32_t bytes)
{
    /* Page zero is unmapped, so the bound is the first mapped page rather than
     * just "not null": the device pointer is zero until the title creates the
     * device, and this thread polls from before that. Rejecting only 0 let
     * dev + get_ptr_off through as 0x34 and faulted on the very first tick. */
    if (g_memory_base == NULL || va < XBOX_FS_BASE)
        return 0;
    /* The contiguous window is mapped separately and sits far above the main
     * range, so a size check against g_memory_size rejects it. The fence a
     * title waits on is exactly the kind of block that lives there --
     * MmAllocateContiguousMemory is where a GPU-written semaphore comes
     * from -- so a chain ending in that window has to be followed, not
     * discarded. */
    if (va >= XBOX_CONTIG_BASE
            && (uint64_t)va + bytes <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)
        return g_contig_memory != NULL;
    return (size_t)va + bytes <= g_memory_size;
}

/*
 * Two counters inside the device, one of which the GPU owns.
 *
 * D3D's swap throttle is a pair: the title bumps "frames submitted" itself and
 * waits for "frames completed", which on hardware only the GPU moves. The Xbox
 * dashboard's is exactly that, at guest 0x000AF121 --
 *
 *     eax = [esi+0x2518]        ; completed
 *     ecx = [esi+0x2B60]        ; submitted
 *     ecx = ecx - eax
 *     if (ecx < 2) proceed      ; else spin on a 400-iteration delay loop
 *
 * -- and with nothing moving completed it spins there forever once two frames
 * are outstanding. That delay loop was 99.8 million of the dashboard's calls,
 * against 35 thousand for the next function down.
 *
 * This differs from xbox_Nv2aFrameCounter, which advances a counter on a 60 Hz
 * clock, in the way that matters for this pair: a free-running counter can
 * pass submitted, and then submitted - completed underflows to about four
 * billion, which is >= 2, and the spin never ends again. Mirroring cannot do
 * that, because completed is only ever whatever submitted already is.
 *
 * It is also simply true here. The pushbuffer is executed at submit, so by the
 * time the title asks whether the frame is finished, it is.
 */
#define XBOX_MAX_COUNTER_MIRRORS 4

static struct {
    uint32_t device_ptr_va;
    uint32_t src_off, dst_off;
} g_counter_mirrors[XBOX_MAX_COUNTER_MIRRORS];
static int g_counter_mirror_count = 0;

int xbox_Nv2aMirrorCounter(uint32_t device_ptr_va,
                           uint32_t src_off, uint32_t dst_off)
{
    if (g_counter_mirror_count >= XBOX_MAX_COUNTER_MIRRORS)
        return -1;
    g_counter_mirrors[g_counter_mirror_count].device_ptr_va = device_ptr_va;
    g_counter_mirrors[g_counter_mirror_count].src_off = src_off;
    g_counter_mirrors[g_counter_mirror_count].dst_off = dst_off;
    g_counter_mirror_count++;
    fprintf(stderr, "  NV2A counter mirror: device at 0x%08X,"
            " +0x%X -> +0x%X\n", device_ptr_va, src_off, dst_off);
    return 0;
}

/* Bumped by the ack thread's pass whenever it changed a guest-visible word,
 * so the thread can tell a quiet spell from a busy one (nv2a_ack_wait). */
static LONG g_ack_activity;

static void counter_mirrors_tick(void)
{
    for (int i = 0; i < g_counter_mirror_count; i++) {
        uint32_t dev;

        if (!fence_readable(g_counter_mirrors[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_counter_mirrors[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_counter_mirrors[i].src_off, 4)
                || !fence_readable(dev + g_counter_mirrors[i].dst_off, 4))
            continue;
        {
            volatile uint32_t *dst =
                (volatile uint32_t *)((uintptr_t)(dev + g_counter_mirrors[i].dst_off)
                                      + g_memory_offset);
            uint32_t src =
                *(volatile uint32_t *)((uintptr_t)(dev + g_counter_mirrors[i].src_off)
                                       + g_memory_offset);
            if (*dst != src) {
                *dst = src;
                g_ack_activity++;
            }
        }
    }
}

/*
 * Frame counters the title polls to pace itself.
 *
 * D3D keeps a swap count inside the device and bumps it once per presented
 * frame; a title that wants to wait a frame reads it and spins until it moves.
 * Wreckless does exactly that at guest 0x000DC5E0 -- "loop while the counter
 * has advanced by less than 2" -- so a counter that never moves is not a
 * dropped frame, it is a hang with a full asset load behind it.
 *
 * Nothing here presents, so nothing would ever move it. Advancing it on a
 * clock is what makes the wait terminate, and 60 Hz is the rate the title
 * expects the display to run at. Followed through the device pointer for the
 * same reason the fence is: the device is allocated at runtime.
 */
#define XBOX_MAX_FRAME_COUNTERS 4
#define XBOX_FRAME_PERIOD_MS    16      /* ~60 Hz */

static struct {
    uint32_t device_ptr_va;
    uint32_t counter_off;
} g_frame_counters[XBOX_MAX_FRAME_COUNTERS];
static int   g_frame_counter_count = 0;
static DWORD g_frame_counter_last_ms = 0;

int xbox_Nv2aFrameCounter(uint32_t device_ptr_va, uint32_t counter_off)
{
    if (g_frame_counter_count >= XBOX_MAX_FRAME_COUNTERS)
        return -1;
    g_frame_counters[g_frame_counter_count].device_ptr_va = device_ptr_va;
    g_frame_counters[g_frame_counter_count].counter_off   = counter_off;
    g_frame_counter_count++;
    fprintf(stderr, "  Frame counter: device at 0x%08X, count +0x%X @ %d Hz\n",
            device_ptr_va, counter_off, 1000 / XBOX_FRAME_PERIOD_MS);
    return 0;
}

/* A real swap happened: advance every registered counter, and remember when.
 *
 * The timer below exists for a title nothing presents for. Once the
 * pushbuffer executor is actually running flips, the timer is the wrong
 * clock and an actively harmful one: Half-Life 2's loader paces its intro on
 * this count, so a 62 Hz timer against an executor managing a fraction of a
 * frame per second ran the video forward in virtual time far faster than it
 * could be drawn. Only every few hundredth frame was ever presented, each one
 * sampled part way through its own decode -- which looks exactly like a
 * stalling, blocky video rather than a clock running away.
 */
static DWORD g_frame_counter_flip_ms;

void xbox_Nv2aFrameCounterFlip(void)
{
    int i;

    g_frame_counter_flip_ms = GetTickCount();
    if (!g_frame_counter_flip_ms)
        g_frame_counter_flip_ms = 1;          /* 0 means "never" */
    for (i = 0; i < g_frame_counter_count; i++) {
        uint32_t dev;

        if (!fence_readable(g_frame_counters[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_frame_counters[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_frame_counters[i].counter_off, 4))
            continue;
        *(volatile uint32_t *)((uintptr_t)(dev + g_frame_counters[i].counter_off)
                               + g_memory_offset) += 1;
    }
}

static void frame_counters_tick(void)
{
    DWORD now = GetTickCount();
    int i;

    if (!g_frame_counter_count)
        return;
    if (g_frame_counter_last_ms
            && (now - g_frame_counter_last_ms) < XBOX_FRAME_PERIOD_MS)
        return;
    /* Something is presenting: let it drive the count instead. Two seconds,
     * because the executor's flips are not evenly spaced and a title that
     * genuinely stops presenting still has to be got moving again. */
    if (g_frame_counter_flip_ms && (now - g_frame_counter_flip_ms) < 2000) {
        g_frame_counter_last_ms = now;
        return;
    }
    g_frame_counter_last_ms = now;

    for (i = 0; i < g_frame_counter_count; i++) {
        uint32_t dev;

        if (!fence_readable(g_frame_counters[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_frame_counters[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_frame_counters[i].counter_off, 4))
            continue;
        *(volatile uint32_t *)((uintptr_t)(dev + g_frame_counters[i].counter_off)
                               + g_memory_offset) += 1;
    }
}

/* The flip gate (xbox_memory_layout.h).
 *
 * The HLE Swap calls xbox_Nv2aFlipGateArm() before it runs the title's own
 * Swap, and that call sleeps until the kernel's vblank tick releases it. So
 * there is one Swap per vblank, the guest thread sleeps for the rest of the
 * frame instead of spinning, and the title's own fence wait is not involved.
 *
 * It is not involved on purpose. The first version held the fence mirror
 * instead, so that the title's wait inside Swap would block; but a title
 * waits for the *previous* frame's fence there (its flips are double
 * buffered), which the mirror had already completed at the last vblank, and
 * two Swaps got through per vblank: TimeSplitters 2's menus measured 120-140
 * fps with a 60 Hz vblank.
 *
 * The gate only holds once a vblank has ever been delivered -- without
 * RECOMP_VBLANK nothing would release it -- and never for more than a quarter
 * of a second, so a vblank thread that stops cannot hang the title; the first
 * timeout is logged, because it means pacing is not happening. A vblank that
 * arrived while the title was still drawing does not count: the wait is for
 * the next one, or the frame after it could present again in the same period.
 *
 * Adaptive by default: a Swap that arrives after the vblank it should have
 * waited for presents at once, and only a frame that finished inside the
 * period is held for the next vblank. RECOMP_FPS_CAP=<fps> is the strict
 * console cadence instead -- every Swap waits for the next release, and the
 * release comes every (vblank rate / fps)th vblank; 30 with a 60 Hz vblank
 * is what a title sees on hardware when it misses every other frame, fine
 * for a fixed-30 title and half speed for one that steps its logic per
 * presented frame. RECOMP_FPS_CAP=0 switches the gate off, for measuring.
 *
 * Why adaptive won (TimeSplitters 2's Siberia, 19 Sep 2026, quiet machine,
 * three scripted runs of each, docs/technical/resolution-and-framerate.md):
 * uncapped the level ran 79-89 fps with a 12.4 ms frame, so the title was
 * not paced at all; adaptive held 59.9-60.2 in every five-second window with
 * 3.7 ms of gate wait per frame; strict 60 held 60 most of the time but
 * dipped to 55-58 and once to 45 at the same points in all three runs,
 * because a frame that just misses its vblank waits out a whole extra one.
 * Burnout 2's front end, 1 ms of work a frame, holds 60.0 under either. */
static HANDLE        g_flip_gate_event;
static volatile LONG g_flip_gate_vblanks;
static int           g_flip_gate_divisor = -1;      /* -1: not configured */
static int           g_flip_gate_strict;            /* RECOMP_FPS_CAP given */

/* What the on-screen toggle cycles through: the settings a player picks
 * between, in that order. A RECOMP_FPS_CAP outside this list still works --
 * it is simply not one of the stops, and the first press moves to the
 * first one. */
static const struct { int divisor, strict; const char *name; } g_gate_modes[] = {
    { 1, 0, "adaptive" },
    { 1, 1, "60" },
    { 2, 1, "30" },
    { 0, 0, "off" },
};

/* A switch that is on unless it is turned off, or off unless turned on.
 *
 * The things a title needs in order to run at all -- the vblank, the audio
 * codec's ready bit, the replacement renderer -- began as experiments, and an
 * experiment is off until asked for. They are not experiments any more: a
 * player double-clicking the executable should get the game, not a black
 * window, so they default on and the variable turns them off. "0", "off",
 * "no" and "false" mean off; anything else, including an empty value, means
 * on. */
/* ── The guest lock ─────────────────────────────────────────────────────
 * See xbox_memory_layout.h for what it is for. A CRITICAL_SECTION because it
 * is recursive and uncontended acquisition is cheap, which matters: this is
 * taken and dropped once per kernel call, and Dino Crisis 3 makes over a
 * million of those a minute. */
static CRITICAL_SECTION g_guest_cs;
static int g_guest_cs_ready;
static int g_guest_lock_on = -1;
static RECOMP_TLS int g_guest_depth;

/* How often two guest threads are inside lifted code at the same time.
 *
 * The threading hypothesis rests on that happening at all, and waiting for
 * the resulting corruption to show is a poor way to find out: Dino Crisis 3
 * faults on roughly one run in twenty, so an A/B of the guest lock over eight
 * runs a side produced zero events on both sides and settled nothing.
 *
 * This measures the hazard instead of the damage, and it is deterministic.
 * A guest thread is "in lifted code" whenever it is not inside a kernel
 * bridge, which the existing drop/restore around bridge() already brackets.
 * If the peak is 1, guest threads never overlap and threading cannot be the
 * cause whatever else is wrong; if it is above 1, the uniprocessor
 * assumption is being violated continuously and that is worth fixing on its
 * own terms, crash or no crash.
 *
 * RECOMP_GUEST_CONCURRENCY=1. Independent of the lock, so the hazard can be
 * measured with the lock off, which is the configuration that ships.
 */
static volatile LONG g_lifted_now;
static volatile LONG g_lifted_peak;
static volatile LONG g_lifted_overlaps;
static int g_concurrency_on = -1;

int xbox_GuestConcurrencyOn(void)
{
    if (g_concurrency_on < 0)
        g_concurrency_on = xbox_EnvSwitch("RECOMP_GUEST_CONCURRENCY", 0);
    return g_concurrency_on;
}

/* One guest thread at DISPATCH_LEVEL stops the others.
 *
 * On the Xbox's single CPU a thread at DISPATCH_LEVEL is the only thing
 * running until it lowers, and titles build on that: Marvel vs Capcom 2's
 * ADX sound thread runs its server at DISPATCH_LEVEL, guarded only by a busy
 * flag, and its main thread edits the same stream buffers at PASSIVE_LEVEL.
 * Here the two run on two host cores, and a watchpoint saw both write one
 * ring buffer's positions. No failure has been traced to that yet.
 *
 * The parking points are the bridge boundaries every guest thread already
 * passes (the Lifted enter/leave pair): g_park_running counts guest threads
 * in lifted code. A thread raising to DISPATCH_LEVEL (xbox_DispatchParkOthers,
 * from kernel_hal.c) becomes the owner and waits for that count to drain;
 * any other thread coming back from a bridge waits while an owner exists.
 *
 * Both waits are bounded. A thread spinning in lifted code never reaches a
 * boundary, and one raised thread blocking on another would be a deadlock;
 * past the bound each side proceeds and the first time says so. Off by
 * default; RECOMP_DISPATCH_LOCK=1, the same switch as the DPC hold-off. */
static volatile LONG  g_park_running;
static volatile DWORD g_park_owner;
static HANDLE         g_park_free;     /* manual reset, set when no owner */
static int            g_park_on = -1;

static int park_on(void)
{
    if (g_park_on < 0) {
        HANDLE ev = CreateEventW(NULL, TRUE, TRUE, NULL);
        if (InterlockedCompareExchangePointer((PVOID *)&g_park_free, ev, NULL))
            CloseHandle(ev);
        g_park_on = xbox_EnvSwitch("RECOMP_DISPATCH_LOCK", 0);
    }
    return g_park_on;
}

void xbox_DispatchParkOthers(void)
{
    ULONGLONG deadline;
    DWORD me = GetCurrentThreadId();

    if (!park_on())
        return;
    g_park_owner = me;
    ResetEvent(g_park_free);
    /* This thread is inside a bridge, so it is not counted itself. */
    if (g_park_running <= 0)
        return;
    deadline = GetTickCount64() + 50;
    while (g_park_running > 0) {
        if (GetTickCount64() >= deadline) {
            static volatile LONG said;
            if (InterlockedIncrement(&said) <= 3) {
                fprintf(stderr, "  [KERNEL] DISPATCH_LEVEL: %ld other guest "
                                "thread(s) still in lifted code after 50 ms; "
                                "going on without them (RECOMP_DISPATCH_LOCK)\n",
                        (long)g_park_running);
                fflush(stderr);
            }
            return;
        }
        SwitchToThread();
    }
}

void xbox_DispatchReleaseOthers(void)
{
    if (!park_on())
        return;
    g_park_owner = 0;
    SetEvent(g_park_free);
}

/* A guest thread leaving lifted code for good (its thread function returned). */
void xbox_GuestLiftedExit(void)
{
    if (park_on())
        InterlockedDecrement(&g_park_running);
}

static void park_enter(void)
{
    DWORD me;
    ULONGLONG deadline = 0;

    if (!park_on())
        return;
    me = GetCurrentThreadId();
    for (;;) {
        DWORD owner;
        InterlockedIncrement(&g_park_running);
        owner = g_park_owner;
        if (!owner || owner == me)
            return;
        InterlockedDecrement(&g_park_running);
        if (!deadline)
            deadline = GetTickCount64() + 100;
        else if (GetTickCount64() >= deadline) {
            static volatile LONG said;
            if (InterlockedIncrement(&said) <= 3) {
                fprintf(stderr, "  [KERNEL] DISPATCH_LEVEL held 100 ms by "
                                "thread %lu; this thread goes on "
                                "(RECOMP_DISPATCH_LOCK)\n", (unsigned long)owner);
                fflush(stderr);
            }
            InterlockedIncrement(&g_park_running);
            return;
        }
        WaitForSingleObject(g_park_free, 10);
    }
}

/* RECOMP_GUEST_ONE_CPU: every guest thread on one host core, as on the Xbox.
 *
 * Guest code is written for a single CPU. It finishes a job and publishes
 * the semaphore for it in two steps, or checks a count and then sleeps on
 * it, trusting that nothing runs in between unless it blocks or its quantum
 * ends. Run on several host cores at once, other guest threads land in those
 * gaps all the time: Outrun 2 hung on its AM2 logo in 4 of 4 runs, a task
 * finishing between "is it still running?" and "here is your semaphore".
 * Host threads (the renderer, audio, the window) are left free.
 *
 * 1 picks a core for the guest: the highest-numbered core of the fastest
 * class the process may run on. A number above 1 is taken as a core index
 * + 1 (2 = core 1, ...). Off when unset or 0.
 *
 * "The fastest class" matters on a hybrid CPU. Windows numbers the
 * efficiency cores last, so "the highest core" on an i7-12700H was CPU 19,
 * an E-core, and the whole title -- lifted code, HLE, renderer submission --
 * ran on it: Outrun 2's race measured 12-15 fps there and 35-40 fps on a
 * P-core, the same build, the same drive (1 Oct 2026). CPU sets report each
 * logical processor's EfficiencyClass (higher is faster); the pick is the
 * highest-numbered processor of the highest class, which also keeps the
 * guest off CPU 0. */
#ifdef _WIN32
typedef BOOL (WINAPI *GetSystemCpuSetInformation_t)(PVOID, ULONG, PULONG, HANDLE, ULONG);

static DWORD_PTR fastest_core_mask(DWORD_PTR proc)
{
    GetSystemCpuSetInformation_t get_sets =
        (GetSystemCpuSetInformation_t)(void *)GetProcAddress(
            GetModuleHandleW(L"kernel32.dll"), "GetSystemCpuSetInformation");
    ULONG len = 0;
    BYTE *buf, *p;
    DWORD_PTR best = 0;
    int best_class = -1;

    if (!get_sets)
        return 0;
    get_sets(NULL, 0, &len, GetCurrentProcess(), 0);
    if (!len || !(buf = (BYTE *)malloc(len)))
        return 0;
    if (!get_sets(buf, len, &len, GetCurrentProcess(), 0)) {
        free(buf);
        return 0;
    }
    /* SYSTEM_CPU_SET_INFORMATION: Size at +0, Type at +4 (0 = CpuSet), then
     * the CpuSet record at +8: Id (ULONG), Group (USHORT, +12),
     * LogicalProcessorIndex (BYTE, +14), CoreIndex (+15),
     * LastLevelCacheIndex (+16), NumaNodeIndex (+17), EfficiencyClass (+18).
     * Spelt out so this builds against older SDK headers. */
    for (p = buf; p + 24 <= buf + len; p += *(ULONG *)p) {
        ULONG size = *(ULONG *)p;
        BYTE lp = p[14], cls = p[18];

        if (size < 24)
            break;
        if (*(ULONG *)(p + 4) != 0 || lp >= sizeof(DWORD_PTR) * 8)
            continue;
        if (!(proc & ((DWORD_PTR)1 << lp)))
            continue;
        if ((int)cls > best_class || ((int)cls == best_class && lp > 0)) {
            if ((int)cls > best_class)
                best = 0;
            best_class = cls;
            best = (DWORD_PTR)1 << lp;      /* highest index wins: ascending order */
        }
    }
    free(buf);
    return best;
}

static DWORD_PTR guest_cpu_mask(void)
{
    static DWORD_PTR mask = (DWORD_PTR)-1;
    if (mask == (DWORD_PTR)-1) {
        const char *v = getenv("RECOMP_GUEST_ONE_CPU");
        long n = v ? strtol(v, NULL, 0) : 0;
        DWORD_PTR proc = 0, sys = 0;
        mask = 0;
        if (n > 0 && GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) && proc) {
            if (n == 1) {
                mask = fastest_core_mask(proc);
                if (!mask) {
                    /* No CPU-set information: the highest core allowed. */
                    for (mask = (DWORD_PTR)1 << (sizeof mask * 8 - 1); !(mask & proc); mask >>= 1)
                        ;
                }
            } else {
                mask = ((DWORD_PTR)1 << (n - 2)) & proc;
            }
            fprintf(stderr, "[THREAD] guest threads on one host core (mask 0x%llX, "
                    "RECOMP_GUEST_ONE_CPU)\n", (unsigned long long)mask);
        }
    }
    return mask;
}

#endif /* _WIN32: CPU affinity is optional, not a macOS correctness primitive. */

void xbox_GuestLiftedEnter(void)
{
    LONG n;
#ifdef _WIN32
    {
        static RECOMP_TLS int pinned;
        if (!pinned) {
            DWORD_PTR m = guest_cpu_mask();
            pinned = 1;
            if (m)
                SetThreadAffinityMask(GetCurrentThread(), m);
        }
    }
#endif
    park_enter();
    if (!xbox_GuestConcurrencyOn())
        return;
    n = InterlockedIncrement(&g_lifted_now);
    if (n > 1)
        InterlockedIncrement(&g_lifted_overlaps);
    for (;;) {
        LONG peak = g_lifted_peak;
        if (n <= peak ||
            InterlockedCompareExchange(&g_lifted_peak, n, peak) == peak)
            break;
    }
}

void xbox_GuestLiftedLeave(void)
{
    if (park_on())
        InterlockedDecrement(&g_park_running);
    if (!xbox_GuestConcurrencyOn())
        return;
    InterlockedDecrement(&g_lifted_now);
}

void xbox_GuestConcurrencyReport(void)
{
    if (!xbox_GuestConcurrencyOn())
        return;
    fprintf(stderr, "  [CONCURRENCY] peak guest threads in lifted code: %ld; "
                    "entries that found another already there: %ld\n",
            (long)g_lifted_peak, (long)g_lifted_overlaps);
    fflush(stderr);
}

int xbox_GuestLockOn(void)
{
    if (g_guest_lock_on < 0)
        g_guest_lock_on = xbox_EnvSwitch("RECOMP_GUEST_LOCK", 0);
    return g_guest_lock_on;
}

void xbox_GuestLockInit(void)
{
    if (g_guest_cs_ready)
        return;
    InitializeCriticalSection(&g_guest_cs);
    g_guest_cs_ready = 1;
    if (xbox_GuestLockOn()) {
        fprintf(stderr, "  Guest lock: on -- one guest thread runs lifted "
                        "code at a time (RECOMP_GUEST_LOCK=0 to switch off)\n");
        fflush(stderr);
    }
}

void xbox_GuestLockEnter(void)
{
    if (!xbox_GuestLockOn() || !g_guest_cs_ready)
        return;
    EnterCriticalSection(&g_guest_cs);
    g_guest_depth++;
}

void xbox_GuestLockLeave(void)
{
    if (!xbox_GuestLockOn() || !g_guest_cs_ready || g_guest_depth <= 0)
        return;
    g_guest_depth--;
    LeaveCriticalSection(&g_guest_cs);
}

/* Release every level this thread holds, so it cannot block while holding
 * the lock, and report how many to take back afterwards. */
int xbox_GuestLockDrop(void)
{
    int held = 0;
    if (!xbox_GuestLockOn() || !g_guest_cs_ready)
        return 0;
    while (g_guest_depth > 0) {
        g_guest_depth--;
        LeaveCriticalSection(&g_guest_cs);
        held++;
    }
    return held;
}

void xbox_GuestLockRestore(int held)
{
    if (!xbox_GuestLockOn() || !g_guest_cs_ready)
        return;
    while (held-- > 0) {
        EnterCriticalSection(&g_guest_cs);
        g_guest_depth++;
    }
}

int xbox_EnvSwitch(const char *name, int default_on)
{
    const char *v = name ? getenv(name) : NULL;

    if (!v)
        return default_on;
    if (!*v)
        return 1;
    return !(strcmp(v, "0") == 0 || _stricmp(v, "off") == 0 ||
             _stricmp(v, "no") == 0 || _stricmp(v, "false") == 0);
}

static int flip_gate_divisor(void)
{
    if (g_flip_gate_divisor < 0) {
        const char *cap = recomp_config_lookup("RECOMP_FPS_CAP", "frame_cap");
        const char *hz = getenv("RECOMP_VBLANK_HZ");
        int vblank = hz && atoi(hz) > 0 ? atoi(hz) : 60;
        int d = 1;                                      /* adaptive unless asked */

        if (cap && *cap) {
            int fps = atoi(cap);
            if (strcmp(cap, "adaptive") == 0)
                d = 1;
            else if (strcmp(cap, "0") == 0 || strcmp(cap, "off") == 0)
                d = 0;
            else if (fps > 0) {
                d = (vblank + fps / 2) / fps;
                if (d < 1) d = 1;
                g_flip_gate_strict = 1;
            }
        }
        g_flip_gate_divisor = d;
        fprintf(stderr, "  [NV2A] flip gate: %s\n",
                d == 0 ? "off, Swap never waits (RECOMP_FPS_CAP=adaptive or 60 to pace)"
                       : !g_flip_gate_strict ? "adaptive, at most one Swap per vblank (RECOMP_FPS_CAP=0 to switch off)"
                       : d == 1 ? "strict, one Swap per vblank" : "strict, one Swap per N vblanks");
        if (d > 1)
            fprintf(stderr, "  [NV2A] flip gate divisor %d (RECOMP_FPS_CAP=%s at %d Hz)\n",
                    d, cap, vblank);
        fflush(stderr);
    }
    return g_flip_gate_divisor;
}

/* Where the current setting sits in that list, or -1 for one that is not in
 * it. */
static int flip_gate_mode_index(void)
{
    int d = flip_gate_divisor(), i;

    for (i = 0; i < (int)(sizeof g_gate_modes / sizeof g_gate_modes[0]); i++)
        if (g_gate_modes[i].divisor == d &&
            (d == 0 || g_gate_modes[i].strict == g_flip_gate_strict))
            return i;
    return -1;
}

const char *xbox_Nv2aFlipGateModeName(void)
{
    int i = flip_gate_mode_index();
    return i < 0 ? "custom" : g_gate_modes[i].name;
}

void xbox_Nv2aFlipGateCycle(void)
{
    /* Timing is fixed at 60; the old hotkey must not enable uncapped mode. */
}

/* The gate's wait, lent out (xbox_Nv2aFlipGateSetIdle). The time of the
 * last vblank and the period are what the hook is told the release will be:
 * the next vblank is the last one plus a period. */
static xbox_FlipGateIdleFn g_flip_gate_idle;
static volatile LONGLONG   g_flip_gate_last_vblank;
static LONGLONG            g_flip_gate_previous;
static LONGLONG            g_flip_gate_period, g_flip_gate_qpf;

void xbox_Nv2aFlipGateSetIdle(xbox_FlipGateIdleFn fn)
{
    g_flip_gate_idle = fn;
#ifdef _WIN32
    /* The timer can query pacing before the title/config is selected.
     * Re-read it when the renderer installs interpolation, otherwise a
     * saved strict 60 cap stays adaptive and leaves no replay interval. */
    g_flip_gate_divisor = -1;
    g_flip_gate_strict = 0;
    if (fn && flip_gate_divisor() == 1) g_flip_gate_strict = 1;
#endif
}

/* Lend the wait to the hook until it wants nothing more or the vblank comes.
 * TRUE if the vblank came (the gate is released). */
static BOOL flip_gate_lend(void)
{
    static HANDLE timer;
    HANDLE both[2];

    if (!g_flip_gate_period)
        return FALSE;
    if (!timer) {
        /* High resolution: a slot a few milliseconds away has to be met to
         * well under a millisecond, which the default timer cannot do. */
        timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */,
                                       TIMER_ALL_ACCESS);
        if (!timer)
            timer = CreateWaitableTimerW(NULL, FALSE, NULL);
        if (!timer)
            return FALSE;
    }
    both[0] = g_flip_gate_event;
    both[1] = timer;
    for (;;) {
        LARGE_INTEGER now, due;
        LONGLONG want;
        DWORD r;

        if (WaitForSingleObject(g_flip_gate_event, 0) == WAIT_OBJECT_0)
            return TRUE;
        QueryPerformanceCounter(&now);
        want = g_flip_gate_idle(now.QuadPart, g_flip_gate_last_vblank + g_flip_gate_period);
        if (want <= 0)
            return FALSE;
        QueryPerformanceCounter(&now);
        if (want <= now.QuadPart)
            continue;
        /* Relative, in 100 ns units. */
        due.QuadPart = -(LONGLONG)((double)(want - now.QuadPart) * 1e7 /
                                   (double)g_flip_gate_qpf);
        if (!SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
            return FALSE;
        r = WaitForMultipleObjects(2, both, FALSE, 250);
        if (r == WAIT_OBJECT_0) {
            CancelWaitableTimer(timer);
            return TRUE;
        }
        if (r != WAIT_OBJECT_0 + 1)
            return FALSE;                               /* the plain wait reports it */
    }
}

/* Native frames bypass the gate but retain the last real vblank. When the
 * original clock resumes and waits for its next ISR, Arm can consume that
 * signal without adding a second tick. Do not consume or synthesize events. */
void xbox_Nv2aFlipGateNativeFrame(void)
{
    g_flip_gate_previous = g_flip_gate_last_vblank;
}

void xbox_Nv2aFlipGateArm(void)
{
    static int said;

    if (flip_gate_divisor() == 0)
        return;
    if (!InterlockedCompareExchange(&g_flip_gate_vblanks, 0, 0))
        return;                                         /* no vblank has ever come */
    if (!g_flip_gate_event)
        return;
#if defined(__APPLE__) || defined(_WIN32)
    /* A cinematic may already have waited for this vblank inside the title.
     * Interpolation must not add another full tick and halve its simulation.
     * A cheap frame still waits normally, lending time for its midpoint. */
    LARGE_INTEGER native_now;
    QueryPerformanceCounter(&native_now);
    if (g_flip_gate_previous && g_flip_gate_period &&
        native_now.QuadPart - g_flip_gate_previous >= g_flip_gate_period * 9 / 10 &&
        WaitForSingleObject(g_flip_gate_event, 0) == WAIT_OBJECT_0) {
        g_flip_gate_previous = native_now.QuadPart;
#ifdef _WIN32
        /* Finish matching this guest frame even when its own wait already
         * consumed the tick. Its next host slot may follow the boundary. */
        if (g_flip_gate_idle)
            g_flip_gate_idle(native_now.QuadPart, g_flip_gate_last_vblank);
#endif
        return;
    }
    g_flip_gate_previous = g_flip_gate_last_vblank + g_flip_gate_period;
#endif
    if (g_flip_gate_strict)
        ResetEvent(g_flip_gate_event);                  /* the next vblank, not a past one */
    /* Only while one Swap a vblank is the cadence: the release is then
     * always the next vblank, which is what the hook is told. */
    if (g_flip_gate_idle && flip_gate_divisor() == 1 && flip_gate_lend())
        return;
    if (WaitForSingleObject(g_flip_gate_event, 250) == WAIT_TIMEOUT && !said++) {
        fprintf(stderr, "  [NV2A] flip gate timed out: no vblank for 250 ms, "
                        "the title is not being paced\n");
        fflush(stderr);
    }
}

void xbox_Nv2aFlipGateRelease(void)
{
    LONG n = InterlockedIncrement(&g_flip_gate_vblanks);
    int d = flip_gate_divisor();
    LARGE_INTEGER now;

    QueryPerformanceCounter(&now);
    if (!g_flip_gate_qpf) {
        LARGE_INTEGER f;
        const char *hz = getenv("RECOMP_VBLANK_HZ");
        double rate = hz && atof(hz) >= 1.0 ? atof(hz) : 60.0;

        QueryPerformanceFrequency(&f);
        g_flip_gate_period = (LONGLONG)((double)f.QuadPart / rate);
        g_flip_gate_qpf = f.QuadPart;
    }
    g_flip_gate_last_vblank = now.QuadPart;
    if (!g_flip_gate_event)
        g_flip_gate_event = CreateEventW(NULL, FALSE, FALSE, NULL);   /* auto-reset */
    if (g_flip_gate_event && (d <= 1 || n % d == 0))
        SetEvent(g_flip_gate_event);
}

static void fence_mirrors_tick(void)
{
    for (int i = 0; i < g_fence_mirror_count; i++) {
        uint32_t dev, get_ptr;

        if (!fence_readable(g_fence_mirrors[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_fence_mirrors[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_fence_mirrors[i].get_ptr_off, 4)
                || !fence_readable(dev + g_fence_mirrors[i].put_off, 4))
            continue;
        get_ptr = *(volatile uint32_t *)((uintptr_t)(dev + g_fence_mirrors[i].get_ptr_off)
                                         + g_memory_offset);
        if (!fence_readable(get_ptr, 4))
            continue;
        {
            volatile uint32_t *fence =
                (volatile uint32_t *)((uintptr_t)get_ptr + g_memory_offset);
            uint32_t put =
                *(volatile uint32_t *)((uintptr_t)(dev + g_fence_mirrors[i].put_off)
                                       + g_memory_offset);
            if (*fence != put) {
                *fence = put;
                g_ack_activity++;
            }
        }
    }
}

static int s_nv2a_trace = 0;

/* The display framebuffer, as reported by AvSetDisplayMode. Checksummed once a
 * second so a run can answer the only question that matters before building a
 * presenter: is the guest putting pixels anywhere at all, and do they change
 * from frame to frame. */
static uint32_t s_fb_va, s_fb_pitch, s_fb_height = 480;

void xbox_SetDisplayFramebuffer(uint32_t fb_va, uint32_t pitch)
{
    s_fb_va = fb_va;
    s_fb_pitch = pitch;
}

/* Where the title last said its framebuffer is, for anything that wants to read
 * guest pixels directly. There was only a setter, so every such reader carried
 * its own hardcoded address instead -- and a title that moves its framebuffer
 * (which is most of them, once it owns one) left that constant pointing at
 * uninitialised memory. A dump taken there is not empty, it is noise, which
 * reads as "the title drew garbage" rather than "you read the wrong page".
 *
 * This is the resolved address, not the physical one AvSetDisplayMode states:
 * the caller resolves before storing, because a physical framebuffer address
 * read directly lands in the loaded image. Getting that wrong is the same bug
 * twice over -- once in the probe below, once in a caller of this. */
uint32_t xbox_GetDisplayFramebuffer(uint32_t *pitch)
{
    if (pitch)
        *pitch = s_fb_pitch;
    return s_fb_va;
}

static void framebuffer_probe_tick(void)
{
    static DWORD last_ms;
    static uint32_t last_sum;
    DWORD now = GetTickCount();
    uint32_t sum = 0, nonzero = 0, i, n;
    const uint32_t *p;

    if (!s_nv2a_trace || !s_fb_va || !s_fb_pitch)
        return;
    if (last_ms && (now - last_ms) < 1000)
        return;
    last_ms = now;
    if ((size_t)s_fb_va + s_fb_pitch * s_fb_height > g_memory_size)
        return;
    p = (const uint32_t *)((uintptr_t)s_fb_va + g_memory_offset);
    n = (s_fb_pitch * s_fb_height) / 4;
    for (i = 0; i < n; i++) {
        sum = sum * 33u + p[i];
        if (p[i]) nonzero++;
    }
    fprintf(stderr, "  [FB] 0x%08X sum=%08X nonzero=%u/%u %s\n",
            s_fb_va, sum, nonzero, n,
            sum != last_sum ? "CHANGED" : "same");
    last_sum = sum;
    fflush(stderr);
}

/* Between passes of the ack thread.
 *
 * It used to be Sleep(0) and nothing else, which returns at once unless a
 * thread of the same priority is waiting, so the thread spun a whole host
 * core for the life of the process (OutRun 2: 172 s of CPU in a 180 s run)
 * -- on a laptop that is power and thermal headroom the guest's own core
 * needs. A guest waiting on one of these words is spinning on another core,
 * so while words keep changing the thread still goes round at full speed;
 * once nothing has changed for a millisecond it waits on a high-resolution
 * timer between passes instead (RECOMP_NV2A_ACK_IDLE_US, default 500; 0 for
 * the old spin). The cost is that the first wait after a quiet spell -- the
 * first kickoff after a vblank wait, say -- can be answered up to that much
 * later. */
static void nv2a_ack_wait(void)
{
    static int      idle_us = -1;
    static HANDLE   timer;
    static LONG     seen;
    static LONGLONG quiet_since, qpf;
    LARGE_INTEGER   now, due;

    if (idle_us < 0) {
        const char *v = getenv("RECOMP_NV2A_ACK_IDLE_US");
        LARGE_INTEGER f;

        idle_us = v ? atoi(v) : 500;
        if (idle_us < 0)
            idle_us = 0;
        if (idle_us) {
            timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */,
                                           TIMER_ALL_ACCESS);
            if (!timer)
                timer = CreateWaitableTimerW(NULL, FALSE, NULL);
            if (!timer)
                idle_us = 0;
        }
        QueryPerformanceFrequency(&f);
        qpf = f.QuadPart;
        fprintf(stderr, "  NV2A busy-bit ack: %s\n",
                idle_us ? "waits between passes once idle (RECOMP_NV2A_ACK_IDLE_US)"
                        : "spins (RECOMP_NV2A_ACK_IDLE_US=0)");
    }
    if (!idle_us) {
        Sleep(0);
        return;
    }
    QueryPerformanceCounter(&now);
    if (g_ack_activity != seen) {
        seen = g_ack_activity;
        quiet_since = now.QuadPart;
    }
    if (now.QuadPart - quiet_since < qpf / 1000) {
        Sleep(0);   /* busy: a waiter is spinning on another core */
        return;
    }
#ifndef _WIN32
    if (g_nv2a_ack_wake) {
        w32_WaitForEventMicroseconds(g_nv2a_ack_wake,
            (uint32_t)(idle_us > 50000 ? 50000 : idle_us));
        return;
    }
#endif
    due.QuadPart = -(LONGLONG)idle_us * 10;   /* relative, 100 ns units */
    if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
        if (g_nv2a_ack_wake) {
            HANDLE waits[] = {g_nv2a_ack_wake, timer};
            WaitForMultipleObjects(2, waits, FALSE, 50);
        } else {
            WaitForSingleObject(timer, 50);
        }
    }
    else
        Sleep(1);
}

static DWORD WINAPI nv2a_ack_thread(LPVOID param)
{
    volatile uint32_t *regs = (volatile uint32_t *)param;
    xbox_NameCurrentThread(L"nv2a ack");
    while (!InterlockedCompareExchange(&g_nv2a_ack_stop, 0, 0)) {
        for (size_t i = 0; i < sizeof(NV2A_ACK) / sizeof(NV2A_ACK[0]); i++) {
            volatile uint32_t *r =
                (volatile uint32_t *)((char *)regs + NV2A_ACK[i].offset);

            /* Leave the interrupt status alone once something actually
             * raises interrupts.
             *
             * Holding these at zero was correct while nothing here ever
             * raised a GPU interrupt -- "none pending" was the truth, and a
             * title's ISR re-entering on a bit that never cleared is how
             * Halo reached a native stack overflow.
             *
             * With RECOMP_VBLANK the vblank tick raises one and the title's
             * own ISR services it, so the premise is gone. Burnout 2's
             * deferred routine reads PMC_INTR_0 through the context the
             * kernel handed it (0x00226EB8 holds 0xFD000000, so the flags it
             * tests are at 0xFD000100) and decides from those bits what work
             * to do. Clearing them from here meant the routine ran 4,818
             * times and found nothing pending every time.
             *
             * The ISR clears them itself, which is what write-1-to-clear is
             * for; this thread stops competing with it. */
            if (InterlockedCompareExchange(&s_vblank_owns_intr, 0, 0)
                && intr_status_reg(NV2A_ACK[i].offset))
                continue;

            if (*r & NV2A_ACK[i].busy_mask) {
                *r &= ~NV2A_ACK[i].busy_mask;
                g_ack_activity++;
            }
        }
        for (size_t i = 0; i < sizeof(NV2A_IDLE) / sizeof(NV2A_IDLE[0]); i++) {
            volatile uint32_t *r =
                (volatile uint32_t *)((char *)regs + NV2A_IDLE[i].offset);
            if ((*r & NV2A_IDLE[i].idle_mask) != NV2A_IDLE[i].idle_mask) {
                *r |= NV2A_IDLE[i].idle_mask;
                g_ack_activity++;
            }
        }
        {
            volatile uint32_t *put =
                (volatile uint32_t *)((char *)regs + NV2A_USER_DMA_PUT);
            volatile uint32_t *get =
                (volatile uint32_t *)((char *)regs + NV2A_USER_DMA_GET);
            if (*get != *put) {
                *get = *put;
                g_ack_activity++;
            }
        }
        fence_mirrors_tick();
        counter_mirrors_tick();
        frame_counters_tick();
        framebuffer_probe_tick();

        /* Which framebuffer the display would be scanning out.
         *
         * PCRTC_START holds the address the CRTC reads pixels from, so
         * whatever the title last set there is the frame it believes is on
         * screen. Nothing here scans out, so this is the one place that says
         * whether the guest is producing an image at all -- and where it is.
         * Gated, because it is a bring-up question, not a runtime one. */
        /* Not gated on the trace flag: nv2a_pb_scan is what drives the
         * executor, and it already returns unless RECOMP_PB_SCAN or
         * RECOMP_PB_EXEC asked for it. Gating the call as well meant
         * RECOMP_PB_EXEC on its own did nothing at all, and the executor
         * only ran when someone happened to also be tracing. */
        {
            /* Is the title submitting GPU work at all? PUT is where the
             * title's pushbuffer writer has got to; if it never moves, nothing
             * is being drawn and the missing piece is upstream of the GPU. */
            static DWORD  last_put_ms;
            static uint32_t last_put;
            DWORD now_ms = GetTickCount();
            uint32_t put = *(volatile uint32_t *)((char *)regs + NV2A_USER_DMA_PUT);
            if (put != last_put || (now_ms - last_put_ms) > 2000) {
                /* Survey the segment the title just submitted, once. */
                {
                    extern void nv2a_pb_scan(uint32_t, uint32_t);
                    extern void nv2a_pb_scan_report(void);
                    static DWORD last_report;

                    /* DMA_PUT holds a PHYSICAL address -- Xbox D3D writes
                     * `VA & 0x0FFFFFFF` and reads the GPU's position back as
                     * `GET | 0x80000000`. nv2a_pb_scan reads guest VAs, so
                     * handing it the raw register value pointed it at low
                     * memory: for the Xbox Dashboard, whose pushbuffer is at
                     * 0x80001000, PUT reads 0x1000 and the survey walked the
                     * fake TIB. It reported a plausible-looking inventory of
                     * nothing, which is worse than reporting none -- the
                     * conclusion drawn was "the title submits no methods"
                     * while it was submitting them the whole time.
                     *
                     * The contiguous window IS the physical-address view, so
                     * OR-ing its base is the documented round trip, not a
                     * guess. */
                    if (last_put && put > last_put)
                        nv2a_pb_scan(XBOX_CONTIG_BASE | (last_put & 0x0FFFFFFFu),
                                     XBOX_CONTIG_BASE | (put      & 0x0FFFFFFFu));
                    /* Periodic, because what the title submits at init is not
                     * what it submits once it is drawing a menu, and the
                     * question the survey answers is about the latter. */
                    if (s_nv2a_trace && now_ms - last_report > 10000) {
                        last_report = now_ms;
                        nv2a_pb_scan_report();
                    }
                }
                last_put = put; last_put_ms = now_ms;
                /* GET as well as PUT. A title that stops submitting has either
                 * finished or is spinning on the GPU catching up, and only GET
                 * tells those apart -- D3D waits for GET to reach PUT before it
                 * reuses the buffer, so GET stuck behind PUT is the shape of a
                 * pushbuffer-full hang. Also show the same pair as the Xbox
                 * Dashboard reads them: its D3D holds a register-block pointer
                 * in its device struct rather than assuming 0xFD800000, and
                 * mirroring the wrong block leaves it spinning on a GET that
                 * never moves. */
                if (s_nv2a_trace) {
                    uint32_t g = *(volatile uint32_t *)
                                 ((char *)regs + NV2A_USER_DMA_GET);
                    fprintf(stderr, "  [NV2A] DMA_PUT = 0x%08X  DMA_GET = "
                            "0x%08X%s\n", put, g,
                            g == put ? "" : "  (GPU behind)");
                }
                fflush(stderr);
            }
        }
        if (s_nv2a_trace) {
            static uint32_t last_start = 0xFFFFFFFFu;
            uint32_t start = *(volatile uint32_t *)((char *)regs + 0x600800);
            if (start != last_start) {
                last_start = start;
                fprintf(stderr, "  [NV2A] PCRTC_START = 0x%08X\n", start);
                fflush(stderr);
            }
        }

        if (g_mcpx_regs && !g_apu_mmio_trapped) {
            for (size_t i = 0; i < sizeof(MCPX_COUNTERS) / sizeof(MCPX_COUNTERS[0]); i++) {
                volatile uint32_t *c =
                    (volatile uint32_t *)((char *)g_mcpx_regs + MCPX_COUNTERS[i]);
                *c += 1;
            }
        }

        /* Advance KeTickCount. It was written once at init and left frozen,
         * which silently breaks every timeout that polls it: Halo's DHCP setup
         * waits on a tick deadline that never arrives and spins forever bringing
         * up XNet. A live clock is also just the truth -- KeTickCount ticks on
         * hardware whether or not anyone is asleep. GetTickCount() shares the
         * millisecond unit, so the rate matches. */
        *(volatile uint32_t *)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT)
                               + g_memory_offset) = GetTickCount();

        nv2a_ack_wait();
    }
    return 0;
}

static void xbox_Nv2aAckStart(void)
{
    g_nv2a_ack_stop = 0;
    g_nv2a_ack_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_nv2a_ack_thread = CreateThread(NULL, 0, nv2a_ack_thread,
                                     g_nv2a_memory, 0, NULL);
    if (g_nv2a_ack_thread) {
        fprintf(stderr, "  NV2A busy-bit ack: %zu register(s) acknowledged\n",
                sizeof(NV2A_ACK) / sizeof(NV2A_ACK[0]));
    } else if (g_nv2a_ack_wake) {
        CloseHandle(g_nv2a_ack_wake);
        g_nv2a_ack_wake = NULL;
    }
}

/* Do not acknowledge one register on the guest thread. The worker must
 * service its original busy-bit, DMA and fence sequence. An auto-reset event
 * retains a request arriving before/during the timed wait; no reset can lose it.
 * Failure to create/signal the event retains the normal polling fallback. */
void xbox_Nv2aNotifyPfbFlush(uint32_t guest_address)
{
    static volatile LONG policy; /* 0 unknown, 1 enabled, 2 diagnostic fallback */
    LONG enabled;
    if (guest_address != XBOX_NV2A_BASE + 0x100410u)
        return;
    enabled = InterlockedCompareExchange(&policy, 0, 0);
    if (!enabled) {
        enabled = xbox_EnvSwitch("RECOMP_PFB_WAKE_ACK", 1) ? 1 : 2;
        InterlockedCompareExchange(&policy, enabled, 0);
    }
    if (enabled == 2)
        return;
    /* Enter before checking stop. Shutdown publishes stop, drains callers,
     * wakes/joins the worker, then releases the event. Later callers see stop. */
    InterlockedIncrement(&g_nv2a_ack_notifiers);
    if (!InterlockedCompareExchange(&g_nv2a_ack_stop, 0, 0) &&
        g_nv2a_memory && g_nv2a_ack_thread && g_nv2a_ack_wake)
        SetEvent(g_nv2a_ack_wake);
    InterlockedDecrement(&g_nv2a_ack_notifiers);
}

/* Separate allocation for Xbox kernel address space (0x80010000+).
 * Some RenderWare code reads the kernel PE header to detect features. */
static void *g_kernel_memory = NULL;

/* Global offset accessible by recompiled code (via recomp_types.h) */
ptrdiff_t g_xbox_mem_offset = 0;

/* Bounds of the title's executable sections, from its own XBE section table.
 *
 * RECOMP_ICALL uses these to decide whether an indirect-call target is code
 * before dispatching it. This used to be a hardcoded "0x00400000..0xFE000000 is
 * not code" test, which is true for Burnout 3 -- its .text ends at 0x002CC200,
 * so everything above 0x400000 really is data -- and false for any title with
 * more code than that. Half-Life 2's .text runs to 0x005F4A6C, so the constant
 * silently discarded every indirect call into the top two thirds of the game,
 * including the one that enters its main. No log, no crash: eax = 0 and carry
 * on, which looks exactly like a function that returned early.
 *
 * Zero until the layout is initialised, which the macro treats as "allow" so
 * nothing breaks before the title is loaded. */
uint32_t g_xbox_image_lo = 0;
uint32_t g_xbox_image_hi = 0;
uint32_t g_xbox_code_lo = 0;
uint32_t g_xbox_code_hi = 0;

/* How far the runtime's low memory moved to clear the image; see XBOX_LOW_VA. */
uint32_t g_xbox_low_shift = 0;

/* Thread storage and identity construction use the project provider. */

/* Architectural storage and ABI reports live in project runtime providers.
 * Only the loader/watchdog's required public cells are declared here. */
extern RECOMP_TLS uint32_t g_eax, g_ebx, g_ecx, g_edx;
extern RECOMP_TLS uint32_t g_esi, g_edi, g_esp, g_ebp;
extern int g_force_return;

/* Native continuations are provided by project guest_nonlocal_jump.c. */

/* Watchdog: dump the guest call stack if the title stops making progress.
 *
 * A hang gives nothing to work from -- no crash, no last log line, no native
 * stack that means anything, because the guest frames live in guest memory and
 * the native one only shows whichever translated function is spinning. Sampling
 * the guest stack from a second thread is the one view that says where the
 * title actually is. Same GS format the crash handler uses, so tools/
 * stackwalk.py reads either.
 *
 * Off unless RECOMP_WATCHDOG_SECS is set, so it costs a getenv in normal runs.
 */
/* Defined below, after the watchdog. */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

static uint32_t *s_watchdog_esp;
/* The other guest registers are thread-local too, so the watchdog has to be
 * handed the guest thread's copies rather than reading its own -- which are
 * always zero, and read as "every register is null" at exactly the moment the
 * registers are the thing being asked about. */
static uint32_t *s_watchdog_regs[6];
static unsigned  s_watchdog_secs;

static DWORD WINAPI xbox_watchdog_thread(LPVOID unused)
{
    const uint8_t *mem;
    uint32_t esp, i;

    (void)unused;
    Sleep(s_watchdog_secs * 1000u);

    mem = (const uint8_t *)g_memory_offset;
    esp = s_watchdog_esp ? *s_watchdog_esp : 0;
    fprintf(stderr, "[WATCHDOG] no exit after %us; guest esp=0x%08X\n"
            "  regs: eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X\n",
            s_watchdog_secs, esp,
            s_watchdog_regs[0] ? *s_watchdog_regs[0] : 0,
            s_watchdog_regs[1] ? *s_watchdog_regs[1] : 0,
            s_watchdog_regs[2] ? *s_watchdog_regs[2] : 0,
            s_watchdog_regs[3] ? *s_watchdog_regs[3] : 0,
            s_watchdog_regs[4] ? *s_watchdog_regs[4] : 0,
            s_watchdog_regs[5] ? *s_watchdog_regs[5] : 0);
    /* The recent indirect-call targets name whatever is spinning: a stuck loop
     * inside a function reached through a pointer leaves no clue on the stack
     * beyond the return address of the call that entered it. */
    {
        uint32_t k;
        /* The running indirect-call total separates a hang from mere
         * slowness. Kernel calls cannot: a pure CPU loop makes none, so
         * "same count at 20s and 60s" proves nothing about it. */
        fprintf(stderr, "  icalls so far: %llu\n",
                (unsigned long long)g_icall_count);
        fprintf(stderr, "  recent ICALL targets:");
        for (k = 0; k < 16; k++)
            fprintf(stderr, " %08X",
                    g_icall_trace[(g_icall_trace_idx + k) & 15]);
        fprintf(stderr, "\n");
    }
    /* Guest globals worth seeing at the moment of the hang.
     *
     * RECOMP_PEEK is otherwise only sampled by the pushbuffer reporter, which
     * a title that hangs before rendering never reaches -- and a spin that
     * makes no kernel calls is invisible to RECOMP_KERNEL_WATCH too. A pure
     * CPU loop polling a global is exactly the case neither of those covers.
     */
    {
        const char *spec = getenv("RECOMP_PEEK");
        char buf[256], *q, *end;
        if (spec && *spec) {
            strncpy(buf, spec, sizeof buf - 1);
            buf[sizeof buf - 1] = 0;
            fprintf(stderr, "  peek:");
            for (q = buf; *q; ) {
                unsigned long va = strtoul(q, &end, 0);
                if (end == q)
                    break;
                if (va >= XBOX_BASE_ADDRESS && va < XBOX_TOTAL_RAM)
                    fprintf(stderr, " [%08lX]=%08X", va,
                            *(const uint32_t *)(mem + va));
                q = (*end == ',') ? end + 1 : end;
            }
            fprintf(stderr, "\n");
        }
    }
    /* RECOMP_FIND_VALUE: who holds this value. A hang has no crash handler
     * to run the scan, and "which object still points at the freed block"
     * is as much a hang question as a crash one (Max Payne's heap spin). */
    xbox_watch_scan_on_crash();

    for (i = 0; i < 400 && esp; i++) {
        uint32_t a = esp + i * 4;
        if (a < XBOX_STACK_BASE || a >= XBOX_STACK_TOP) break;
        fprintf(stderr, "    GS %08X %08X\n", a,
                *(const uint32_t *)(mem + a));
    }
    fflush(stderr);
    _exit(3);
    return 0;
}

void xbox_WatchdogStart(void)
{
    const char *secs = getenv("RECOMP_WATCHDOG_SECS");
    HANDLE h;

    if (!secs || !*secs)
        return;
    s_watchdog_secs = (unsigned)atoi(secs);
    if (!s_watchdog_secs)
        return;

    /* Taken on the guest thread: g_esp is thread-local, so the watchdog has to
     * be handed the address of the one that matters rather than reading its
     * own, which is always zero. */
    s_watchdog_esp = &g_esp;
    s_watchdog_regs[0] = &g_eax; s_watchdog_regs[1] = &g_ecx;
    s_watchdog_regs[2] = &g_edx; s_watchdog_regs[3] = &g_ebx;
    s_watchdog_regs[4] = &g_esi; s_watchdog_regs[5] = &g_edi;
    h = CreateThread(NULL, 0, xbox_watchdog_thread, NULL, 0, NULL);
    if (h)
        CloseHandle(h);
}

static tsfp_thread_memory g_guest_threads;

/* Project allocation provider storage. Initialization precedes device workers. */
static tsfp_allocations g_guest_allocations;
static tsfp_allocation_record g_guest_heap_records[65536];
/* One additional record protects the synthetic kernel page. */
static tsfp_allocation_record g_guest_physical_records[4097];

static int xbox_AllocationInit(void)
{
    tsfp_allocation_config config = {0};
    if (g_memory_size > UINT32_MAX || g_xbox_total_ram > UINT32_MAX)
        return 0;
    config.ram = g_memory_base;
    config.ram_size = (uint32_t)g_memory_size;
    config.heap_begin = XBOX_HEAP_BASE;
    config.reserve_begin = (uint32_t)(g_xbox_total_ram < g_memory_size
        ? g_xbox_total_ram : g_memory_size);
    if (g_contig_memory) {
        config.physical = g_contig_memory;
        config.physical_base = XBOX_CONTIG_BASE;
        config.physical_size = XBOX_CONTIG_SIZE;
        config.physical_begin = XBOX_CONTIG_BASE + XBOX_CONTIG_RESERVED_LOW;
        config.physical_end = XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE - XBOX_GPU_INSTANCE_DEFAULT;
        config.protected_begin = 0x80010000u;
        config.protected_end = 0x80011000u;
    }
    return tsfp_allocations_init(&g_guest_allocations, &config,
        g_guest_heap_records, sizeof(g_guest_heap_records) / sizeof(g_guest_heap_records[0]),
        g_guest_physical_records, sizeof(g_guest_physical_records) / sizeof(g_guest_physical_records[0]));
}
/* End project allocation initialization. */

BOOL xbox_MemoryLayoutInit(const void *xbe_data, size_t xbe_size)
{
    DWORD old_protect;
    tsfp_image_info image;
    tsfp_image_result image_status;
    uint32_t image_low_shift = 0, image_tls_total = 0;
    size_t image_limit = g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram;

    if (g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: already initialized\n");
        return FALSE;
    }
    /* Reject malformed input and insufficient bootstrap space before mapping
     * or changing runtime state. The caller owns immutable XBE bytes. */
    image_status = tsfp_image_inspect(xbe_data, xbe_size, image_limit, &image);
    if (image_status == TSFP_IMAGE_OK)
        image_status = tsfp_image_bootstrap(&image, image_limit,
                                           &image_low_shift, &image_tls_total);
    if (image_status != TSFP_IMAGE_OK) {
        fprintf(stderr, "xbox_MemoryLayoutInit: XBE rejected (%s)\n",
                tsfp_image_result_name(image_status));
        return FALSE;
    }
    g_force_return = getenv("RECOMP_FORCE_RETURN") != NULL;

    /*
     * Calculate the full range we need to map.
     * From XBOX_MAP_START (0x0) to the end of the furthest section.
     * This includes low memory (KPCR at 0x0-0xFF) which game code reads
     * from, the XBE sections, and the simulated stack.
     */
    /* Map the full Xbox address space (covers all sections + stack + heap).
     * Size is runtime-configurable: retail 64 MB, devkit debug builds 128 MB. */
    /* The mapped range, which is not necessarily RAM. Mirrors are placed
     * at multiples of this, so growing it is what stops a title's
     * above-RAM allocations from aliasing low memory. */
    g_memory_size = g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram;

    /*
     * Create a file mapping backed by the page file.
     *
     * Using file mapping instead of VirtualAlloc allows us to map the same
     * physical pages at multiple virtual addresses via MapViewOfFileEx.
     * This is critical for the Xbox RAM mirror: the Xbox memory controller
     * uses a 26-bit address bus, so ALL addresses wrap modulo 64 MB.
     * Code that writes to address 0x20000448 is really writing to 0x00000448.
     * With file mapping views, we create aliased mappings at 64 MB intervals
     * that all point to the same physical memory.
     */
    g_mapping_handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,   /* page file backed */
        NULL,                   /* default security */
        PAGE_READWRITE,         /* read-write access */
        0,                      /* high DWORD of size */
        (DWORD)g_memory_size,   /* low DWORD of size (64 MB) */
        NULL                    /* unnamed mapping */
    );
    if (!g_mapping_handle) {
        fprintf(stderr, "xbox_MemoryLayoutInit: CreateFileMapping failed (error %lu)\n",
                GetLastError());
        return FALSE;
    }

    /*
     * Map the base view at the desired virtual address.
     * Try the original Xbox base address first. If that fails (common on
     * Windows 11 where low addresses are often reserved), try page-aligned
     * addresses upward until we find a free region.
     */
    {
#ifdef __APPLE__
        void *arena = w32_guest_arena_reserve((SIZE_T)0x100000000ull);
        if (arena)
            g_memory_base = MapViewOfFileEx(g_mapping_handle, FILE_MAP_ALL_ACCESS,
                                            0, 0, g_memory_size, arena);
        if (!g_memory_base) w32_guest_arena_release();
#else
        static const uintptr_t try_bases[] = {
            XBOX_BASE_ADDRESS,      /* 0x00010000 - original Xbox address */
            0x00800000,             /* 8 MB - above typical PEB/TEB region */
            0x01000000,             /* 16 MB */
            0x02000000,             /* 32 MB */
            0x10000000,             /* 256 MB */
            0,                      /* sentinel - let OS choose */
        };

        for (size_t i = 0; i < sizeof(try_bases) / sizeof(try_bases[0]); i++) {
            LPVOID hint = try_bases[i] ? (LPVOID)try_bases[i] : NULL;
            g_memory_base = MapViewOfFileEx(
                g_mapping_handle,
                FILE_MAP_ALL_ACCESS,
                0, 0,           /* offset into mapping */
                g_memory_size,  /* size */
                hint            /* desired base address */
            );
            if (g_memory_base) {
                if (try_bases[i] != 0 && (uintptr_t)g_memory_base != try_bases[i]) {
                    /* OS gave us a different address, retry */
                    UnmapViewOfFile(g_memory_base);
                    g_memory_base = NULL;
                    continue;
                }
                break;
            }
        }
    #endif
    }

    if (!g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: failed to map base view (%zu KB)\n",
                g_memory_size / 1024);
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
        return FALSE;
    }

    g_memory_offset = (uintptr_t)g_memory_base - XBOX_MAP_START;

    /* Guest page zero: no access.
     *
     * Nothing legitimate lives there -- every XBE's image base is 0x00010000
     * and the TIB now sits at XBOX_FS_BASE -- so any access is a null pointer
     * the title dereferenced. Left readable it did quiet damage: a null check
     * of the form `cmp byte [ecx], 0` read whatever happened to be at 0 and
     * decided the pointer was fine, and a store through a null pointer landed
     * on real memory and surfaced as corruption somewhere unrelated. Faulting
     * here turns both into one access violation at the instruction that made
     * the mistake, which the crash handler can name.
     *
     * Opt-in through RECOMP_TRAP_NULL, because it converts a class of bug the
     * title currently survives into a hard stop: a guest that dereferences null
     * and ignores the result keeps running while page zero reads as zero, and
     * stops dead once it faults. That is the right default for hunting one of
     * these and the wrong one for making progress past the rest, so it is a
     * switch rather than a policy.
     *
     * Note this is separate from moving the TIB off page zero, which is not
     * optional: with the TIB gone, address 0 reads as plain zero, so a null
     * check written as a load through the pointer now gets the answer it
     * expects whether or not the page is trapped.
     *
     * Best-effort: failing to protect it costs only the diagnostic. */
    if (XBOX_MAP_START == 0 && getenv("RECOMP_TRAP_NULL")) {
        DWORD old_protect;
        if (VirtualProtect(g_memory_base, 0x1000, PAGE_NOACCESS, &old_protect))
            fprintf(stderr, "  guest page 0 is PAGE_NOACCESS"
                            " (null dereferences fault)\n");
    }

    if (g_memory_offset == 0) {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%08X (original Xbox address)\n",
                g_memory_size / 1024, XBOX_MAP_START);
    } else {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%p (offset %+td from Xbox base)\n",
                g_memory_size / 1024, g_memory_base, g_memory_offset);
    }

    /*
     * Helper macro: convert Xbox VA to actual mapped address.
     * When g_memory_offset == 0 (ideal case), this is identity.
     */
    #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))

    /* Materialize the independently validated image. Host mappings and their
     * shutdown remain owned by this transitional memory service. */
    image_status = tsfp_image_load(xbe_data, xbe_size, g_memory_base, g_memory_size, &image);
    if (image_status != TSFP_IMAGE_OK) {
        fprintf(stderr, "xbox_MemoryLayoutInit: image load failed (%s)\n",
                tsfp_image_result_name(image_status));
        xbox_MemoryLayoutShutdown();
        return FALSE;
    }
    g_xbox_low_shift = image_low_shift;
    g_xbox_image_lo = image.image_lo; g_xbox_image_hi = image.image_hi;
    g_xbox_code_lo = image.code_lo; g_xbox_code_hi = image.code_hi;
    xbox_kernel_set_thunk_address(image.thunk, image.thunk_count);
    xbox_kernel_set_xbe_game_region(image.region);
    recomp_config_set_title(image.title_id);
    memcpy(g_xbe_title_name, image.title, sizeof image.title);
    fprintf(stderr, "  Project XBE: %u sections, image 0x%08X..0x%08X, code 0x%08X..0x%08X\n",
            image.section_count, image.image_lo, image.image_hi, image.code_lo, image.code_hi);
    fprintf(stderr, "  Kernel thunks: %u entries at Xbox VA 0x%08X; low shift 0x%08X\n",
            image.thunk_count, image.thunk, image_low_shift);
    fprintf(stderr, "  XBE certificate: title \"%s\", region 0x%08X\n",
            g_xbe_title_name, image.region);

    /*
     * NOTE: .rdata is NOT set read-only.
     * VirtualProtect rounds to page boundaries, and the .rdata end (0x003B2454)
     * and .data start (0x003B2360) share the same 4KB page (0x003B2000-0x003B2FFF).
     * Making .rdata read-only also makes the first ~0xCA0 bytes of .data read-only,
     * which causes game initialization code to fault when writing to .data globals
     * in that overlap range.
     */
    (void)old_protect;

    #undef XBOX_VA

    /* Set the global offset for recompiled code MEM macros */
    g_xbox_mem_offset = g_memory_offset;

    /*
     * Contiguous / physical memory window at 0x80000000.
     *
     * MmAllocateContiguousMemory hands back addresses in this window: physical
     * page P is visible at 0x80000000 + P. Titles that pin buffers at fixed
     * physical addresses then use the whole range, so it has to be backed for
     * its full length - Halo pins 3.4 MB at 0x61000 and 22 MB at 0x3A6000, and
     * with only the fake kernel page mapped here a write walked off the end of
     * it a few pages in.
     *
     * Deliberately NOT a view of the 64 MB RAM mapping. On hardware this window
     * aliases physical RAM, but we load the XBE image into the low addresses of
     * that same region, so aliasing would put a title's pinned pools on top of
     * its own code. Separate storage costs an extra mapping and behaves
     * correctly; nothing here depends on the aliasing.
     *
     * Reserved before the kernel page below, which lives inside it.
     */
    {
        uintptr_t contig_native = XBOX_CONTIG_BASE + g_memory_offset;
        g_contig_mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
            0, (DWORD)XBOX_CONTIG_SIZE, NULL);
        g_contig_memory = g_contig_mapping
            ? MapViewOfFileEx(g_contig_mapping, FILE_MAP_ALL_ACCESS,
                              0, 0, XBOX_CONTIG_SIZE, (LPVOID)contig_native)
            : NULL;
        if (!g_contig_memory)
            g_contig_memory = VirtualAlloc(
                (LPVOID)contig_native,
                XBOX_CONTIG_SIZE,
                MEM_RESERVE | MEM_COMMIT,
                PAGE_READWRITE
            );
        if (g_contig_memory) {
            fprintf(stderr, "  Contiguous window: %u MB at Xbox VA 0x%08X\n",
                    XBOX_CONTIG_SIZE / (1024 * 1024), XBOX_CONTIG_BASE);
        } else {
            fprintf(stderr, "  WARNING: contiguous window at 0x%08X failed "
                    "(error %lu); pinned physical allocations will fault\n",
                    XBOX_CONTIG_BASE, GetLastError());
        }
    }

    /*
     * NV2A hardware register aperture at 0xFD000000 (16 MB).
     *
     * The GPU's registers are memory-mapped here on real hardware. A title
     * that only calls D3D never notices, but the D3D8 library is linked into
     * the XBE rather than provided by the kernel, so once execution is inside
     * it the register pokes are just loads and stores in recompiled code.
     * Halo faults reading 0xFD001804 during rasterizer_preinitialize, a few
     * instructions after Direct3DCreate8 returns.
     *
     * Backed as ordinary zeroed RAM. That is enough to get through
     * initialisation, and reads returning zero are the benign answer for the
     * status and capability registers touched here.
     *
     * ponytail: plain memory, no register semantics. A spin loop waiting for
     * a bit to *set* would hang rather than fault -- if that shows up, the fix
     * is to bridge the D3D8 entry point that owns the loop, not to start
     * emulating NV2A. Nothing has needed that yet.
     */
    {
        uintptr_t nv2a_native = XBOX_NV2A_BASE + g_memory_offset;
#ifdef __APPLE__
        g_nv2a_memory = w32_guest_mmio_map((LPVOID)nv2a_native, XBOX_NV2A_SIZE);
#else
        g_nv2a_memory = VirtualAlloc(
            (LPVOID)nv2a_native,
            XBOX_NV2A_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
#endif
        /* The pushbuffer survey rides on the same poll, so either
         * variable arms it. */
        s_nv2a_trace = getenv("RECOMP_NV2A_TRACE") != NULL
                    || getenv("RECOMP_PB_SCAN") != NULL
                    || getenv("RECOMP_PB_EXEC") != NULL;
        /* Trap writes to the CRTC interrupt page when the vblank chain runs.
         *
         * PCRTC_INTR_0 is write-1-to-clear and PMC_INTR_0 bit 24 is a summary
         * of it. A guest ISR acknowledges by writing the one and spinning on
         * the other, and against plain memory that spin never ends: Burnout
         * 2's D3D8 does it on the DPC, which this runtime dispatches from the
         * timer thread -- the same thread that raises the next vblank. One
         * frame was delivered and the clock stopped.
         *
         * PAGE_READONLY, not PAGE_NOACCESS: the status registers are read far
         * more than written, and only the write needs different semantics.
         * Only when RECOMP_VBLANK is set, because that is the only thing that
         * raises an interrupt for anyone to acknowledge.
         */
        if (g_nv2a_memory && xbox_EnvSwitch("RECOMP_VBLANK", 1)) {
            DWORD old_nv;
            if (VirtualProtect((char *)g_nv2a_memory + XBOX_NV2A_PCRTC_PAGE,
                               4096, PAGE_READONLY, &old_nv))
                fprintf(stderr, "  NV2A: 0x%08X..0x%08X write-trapped "
                        "(PCRTC interrupt status)\n",
                        XBOX_NV2A_BASE + XBOX_NV2A_PCRTC_PAGE,
                        XBOX_NV2A_BASE + XBOX_NV2A_PCRTC_PAGE + 4096);
            else
                fprintf(stderr, "  WARNING: NV2A PCRTC page protect failed "
                        "(error %lu); the vblank ack will spin\n",
                        GetLastError());
        }

        if (g_nv2a_memory) {
            fprintf(stderr, "  NV2A register aperture: %u MB at Xbox VA "
                    "0x%08X (zeroed, no register semantics)\n",
                    XBOX_NV2A_SIZE / (1024 * 1024), XBOX_NV2A_BASE);
        } else {
            fprintf(stderr, "  WARNING: NV2A aperture at 0x%08X failed "
                    "(error %lu); D3D register access will fault\n",
                    XBOX_NV2A_BASE, GetLastError());
        }
    }

    /*
     * MCPX device apertures.
     *
     * The NV2A block above is not the only hardware the title touches
     * directly. The southbridge devices live higher up:
     *
     *   0xFE800000  APU (audio processing unit)
     *   0xFEC00000  AC97
     *   0xFED00000  USB0 / USB1
     *   0xFEF00000  NIC
     *
     * Halo faults reading 0xFED00000 during input initialisation -- the XDK's
     * USB code talks to the host controller's registers rather than going
     * through a driver. Back the whole span as plain RAM for the same reason
     * the NV2A aperture is backed: a read of zero is survivable, a fault is
     * not.
     *
     * ponytail: no register semantics anywhere in here. If something spins
     * waiting for a bit to set, extend the NV2A ack thread's table rather than
     * emulating the device.
     */
    {
        uintptr_t mcpx_native = XBOX_MCPX_BASE + g_memory_offset;
#ifdef __APPLE__
        g_mcpx_memory = w32_guest_mmio_map((LPVOID)mcpx_native, XBOX_MCPX_SIZE);
#else
        g_mcpx_memory = VirtualAlloc(
            (LPVOID)mcpx_native,
            XBOX_MCPX_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
#endif
        g_mcpx_regs = g_mcpx_memory;
        if (g_mcpx_memory) {
            /* AC'97 codec ready.
             *
             * DirectSound resets the codec by setting a bit in 0xFEC0012C and
             * then polls 0xFEC00130 for bit 8 a thousand times waiting for the
             * codec to come up. On zeroed registers that bit never appears, so
             * the wait times out and DirectSoundCreate returns DSERR_NODRIVER
             * (0x88780078).
             *
             * That failure is not confined to audio. Wreckless initialises its
             * whole engine object behind `if (DirectSoundCreate() >= 0)`, so a
             * failed create skips the initialisation, leaves the object's table
             * pointer null, and the null propagates: a null-derived divisor
             * produces a NaN transform matrix, which produces a garbage index,
             * which crashes. Reporting the codec as present is what lets the
             * engine initialise at all.
             *
             * The aperture is plain memory, so setting the bit once is enough:
             * nothing clears it, and the poll reads it on the first pass. */
            #define MCPX_AC97_CODEC_STATUS 0x00400130u   /* 0xFEC00130 */
            #define MCPX_AC97_CODEC_READY  0x00000100u
            /* Opt-in, and not because it is wrong.
             *
             * Reporting the codec is the correct answer -- DSERR_NODRIVER is
             * not what hardware returns -- but it is only correct as far as it
             * goes. DirectSound then hands the audio DSP a command block in
             * RAM and spins until the DSP clears it, and there is no DSP here,
             * so the title trades a late crash for an early hang: 44 assets
             * loaded and then a fault, versus one asset and a stall in audio
             * init. Until the DSP handshake is answered, the honest default is
             * the failure that gets further, with the correct behaviour one
             * variable away. */
            if (xbox_EnvSwitch("RECOMP_AC97_READY", 1)) {
                /* The APU's registers have to fault so they can be routed to
                 * the emulated APU, which is the half that answers the DSP
                 * handshake. Backed as plain memory the guest's writes go
                 * nowhere the APU can see, so it initialises and then waits
                 * forever. Only the APU's own 512K is unmapped: AC'97 above it
                 * stays plain memory, which is what the codec-ready bit needs.
                 *
                 * Enabled by the same variable, because neither half is any
                 * use without the other. */
                /* Registers only, not the DSP's memory.
                 *
                 * The aperture's first 512 KB holds two different kinds of
                 * thing. Up to 0x30000 are registers the emulated APU models:
                 * the PAPU block below 0x20000 and the voice processor from
                 * 0x20000. From 0x30000 up are the GP and EP DSPs' own program
                 * and data memory, which apu_core.c ignores -- so trapping
                 * them buys nothing and costs correctness.
                 *
                 * It cost a crash, not just cycles. A title loads DSP firmware
                 * by copying it into that window, and MSVC compiles the lifted
                 * copy into AVX: the fault at offset 0x30314 was C5 FE 6F 02,
                 * `vmovdqu ymm0, [rdx]`, which the MMIO decoder does not
                 * handle and never should -- a 32-byte vector load is not a
                 * register access. Left as plain memory the copy just lands,
                 * which is what the hardware does and what a stubbed DSP
                 * needs.
                 */
                DWORD old_protect;
                #define XBOX_APU_REG_BYTES 0x00030000u
                if (VirtualProtect((char *)g_mcpx_memory, XBOX_APU_REG_BYTES,
                                   PAGE_NOACCESS, &old_protect))
                    g_apu_mmio_trapped = 1;
                if (g_apu_mmio_trapped)
                    fprintf(stderr, "  APU: 0x%08X..0x%08X trapped for MMIO"
                            " (registers; DSP memory above stays plain)\n",
                            XBOX_MCPX_BASE,
                            XBOX_MCPX_BASE + XBOX_APU_REG_BYTES);
                *(volatile uint32_t *)((char *)g_mcpx_memory
                                       + MCPX_AC97_CODEC_STATUS)
                    |= MCPX_AC97_CODEC_READY;
                fprintf(stderr, "  AC97: codec reported ready at 0x%08X"
                                " (DirectSound will initialise)\n",
                        XBOX_MCPX_BASE + MCPX_AC97_CODEC_STATUS);

                /* Writes to this page trap; reads do not.
                 *
                 * PAGE_READONLY rather than PAGE_NOACCESS, because the two
                 * things on it want opposite treatment. The codec status is
                 * read and has to stay plain memory -- it is polled a
                 * thousand times in a row. The DSP command bytes have to be
                 * caught at the instant they are written, because the wait
                 * that follows reads the byte once and then spins on the
                 * register copy, so anything that changes memory afterwards
                 * arrives too late to be seen.
                 *
                 * Set the codec bit before protecting: afterwards this is not
                 * writable from here either.
                 */
                if (VirtualProtect((char *)g_mcpx_memory + XBOX_MCPX_AC97_PAGE,
                                   4096, PAGE_READONLY, &old_protect)) {
                    fprintf(stderr, "  AC97: 0x%08X..0x%08X write-trapped\n",
                            XBOX_MCPX_BASE + XBOX_MCPX_AC97_PAGE,
                            XBOX_MCPX_BASE + XBOX_MCPX_AC97_PAGE + 4096);
                } else {
                    fprintf(stderr, "  WARNING: AC97 page protect failed "
                            "(error %lu); the DSP wait will not clear\n",
                            GetLastError());
                }
            }
            fprintf(stderr, "  MCPX device aperture: %u MB at Xbox VA "
                    "0x%08X (APU/AC97/USB/NIC, zeroed)\n",
                    XBOX_MCPX_SIZE / (1024 * 1024), XBOX_MCPX_BASE);
        } else {
            fprintf(stderr, "  WARNING: MCPX aperture at 0x%08X failed "
                    "(error %lu); USB/audio register access will fault\n",
                    XBOX_MCPX_BASE, GetLastError());
        }
    }

    /* Flash ROM aperture -- see XBOX_FLASH_BASE for why. */
    {
        uintptr_t flash_native = XBOX_FLASH_BASE + g_memory_offset;

        g_flash_memory = VirtualAlloc(
            (LPVOID)flash_native,
            XBOX_FLASH_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        if (g_flash_memory) {
            fprintf(stderr, "  Flash ROM aperture: %u MB at Xbox VA "
                    "0x%08X (zeroed, not a real BIOS image)\n",
                    XBOX_FLASH_SIZE / (1024 * 1024), XBOX_FLASH_BASE);
        } else {
            fprintf(stderr, "  WARNING: flash aperture at 0x%08X failed "
                    "(error %lu); a title reading flash will fault\n",
                    XBOX_FLASH_BASE, GetLastError());
        }
    }

    if (!xbox_AllocationInit()) {
        fprintf(stderr, "Project guest allocator rejected the mapped memory layout\n");
        xbox_MemoryLayoutShutdown();
        return FALSE;
    }
    /* Project main-thread storage is ready before any device worker starts. */
    if (!tsfp_thread_memory_init(&g_guest_threads, &g_guest_allocations, &image) ||
        g_guest_threads.low_shift != image_low_shift ||
        g_guest_threads.tls_total != image_tls_total) {
        fprintf(stderr, "Project thread storage rejected the image layout\n");
        xbox_MemoryLayoutShutdown();
        return FALSE;
    }
    g_fs_base = XBOX_TIB_MAIN;
    g_esp = XBOX_STACK_TOP;
    fprintf(stderr, "  Project thread storage: TIB 0x%08X, TLS %u bytes, ESP 0x%08X\n",
            (unsigned)g_fs_base, (unsigned)g_guest_threads.tls_total, (unsigned)g_esp);
    fprintf(stderr, "  Project allocations: heap 0x%08X..0x%08X, physical %s\n",
            (unsigned)XBOX_HEAP_BASE, (unsigned)g_memory_size,
            g_contig_memory ? "available" : "unavailable");
    if (g_nv2a_memory) {
        xbox_Nv2aAckStart();
    }

    /*
     * Allocate a page at Xbox kernel address space (0x80010000).
     *
     * RenderWare's Xbox driver code (xbcache.c) reads MEM32(0x8001003C)
     * to parse the Xbox kernel's PE header and find the INIT section for
     * CPU cache line sizing. On PC, we provide a minimal fake PE header
     * with 0 sections so the function gracefully skips the cache init.
     *
     * The actual native address is 0x80010000 + g_memory_offset.
     */
    {
        #define XBOX_KERNEL_BASE 0x80010000u
        #define KERNEL_PAGE_SIZE 4096
        uintptr_t kernel_native = XBOX_KERNEL_BASE + g_memory_offset;
        /* Already committed if the contiguous window above succeeded -
         * 0x80010000 sits inside it - so just use that storage. */
        g_kernel_memory = g_contig_memory
            ? (void *)kernel_native
            : VirtualAlloc((LPVOID)kernel_native, KERNEL_PAGE_SIZE,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (g_kernel_memory) {
            /* Zero-fill then set e_lfanew = 0x80 (offset to PE header).
             * With the rest zeroed, NumberOfSections = 0 and the INIT
             * section search finds nothing, which is the safe path. */
            memset(g_kernel_memory, 0, KERNEL_PAGE_SIZE);
            *(uint32_t *)((uint8_t *)g_kernel_memory + 0x3C) = 0x80;  /* e_lfanew */
            fprintf(stderr, "  Kernel: fake PE header at Xbox VA 0x%08X (native %p)\n",
                    XBOX_KERNEL_BASE, g_kernel_memory);
        } else {
            fprintf(stderr, "  WARNING: could not map Xbox kernel VA 0x%08X\n",
                    XBOX_KERNEL_BASE);
        }
        #undef XBOX_KERNEL_BASE
        #undef KERNEL_PAGE_SIZE
    }

    /* Initialize the dynamic heap. */
    fprintf(stderr, "  Heap: %u MB at Xbox VA 0x%08X-0x%08X\n",
            (unsigned)((XBOX_HEAP_TOP - XBOX_HEAP_BASE) / (1024 * 1024)),
            XBOX_HEAP_BASE, XBOX_HEAP_TOP);

    /*
     * Map mirror views of the 64 MB region.
     *
     * On retail Xbox, physical RAM wraps at 64 MB due to the 26-bit
     * address bus. Address 0x04070000 reads the same data as 0x00070000.
     * The RenderWare engine's memory walker crosses 64 MB and accesses
     * mirrored data for an extended walk covering 256+ MB of virtual
     * addresses. Game init code also writes large data structures past
     * 64 MB that on real hardware wrap into physical RAM.
     *
     * We map additional views of the SAME file mapping section at 64 MB
     * intervals. All views alias the same physical pages, so reads and
     * writes at any mirror address correctly access the base data.
     */
    {
        int mirrors_ok = 0;
        /* The tiled aperture is a specific architectural alias -- physical RAM
         * a second time at 0xF0000000, which is where titles render -- while
         * these mirrors are a generic emulation of the address wrap. When the
         * mapped size is large enough that a mirror would cover 0xF0000000,
         * the mirror wins the address and the tiled mapping fails with
         * ERROR_INVALID_ADDRESS; Half-Life 2 then faults on its first surface
         * write. The specific alias is worth more than one wrap mirror, so
         * skip any that would overlap it.
         *
         * Guest addresses, not host: mirror m covers guest
         * (m + 1) * g_memory_size. */
        uint64_t tiled_lo = XBOX_TILED_BASE;
        uint64_t tiled_hi = tiled_lo + xbox_TiledApertureSize();

        for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
            uintptr_t mirror_base = (uintptr_t)g_memory_base +
                                    (uintptr_t)(m + 1) * g_memory_size;
            uint64_t guest_lo = (uint64_t)(m + 1) * g_memory_size;
            uint64_t guest_hi = guest_lo + g_memory_size;

            if (guest_lo < tiled_hi && tiled_lo < guest_hi) {
                fprintf(stderr, "  Mirror %d: skipped, overlaps the tiled"
                                " aperture at 0x%08X\n",
                        m + 1, (unsigned)XBOX_TILED_BASE);
                continue;
            }
            g_mirror_views[m] = MapViewOfFileEx(
                g_mapping_handle,
                FILE_MAP_ALL_ACCESS,
                0, 0,
                g_memory_size,
                (LPVOID)mirror_base
            );
            if (g_mirror_views[m]) {
                mirrors_ok++;
            } else {
                fprintf(stderr, "  Mirror %d: FAILED at %p (error %lu)\n",
                        m + 1, (void *)mirror_base, GetLastError());
            }
        }
        fprintf(stderr, "  RAM mirror: %d/%d views mapped (covers %d MB)\n",
                mirrors_ok, XBOX_NUM_MIRRORS,
                (int)((mirrors_ok + 1) * g_memory_size / (1024 * 1024)));
    }

    /*
     * Tiled / write-combined aperture at 0xF0000000.
     *
     * The NV2A exposes physical RAM a second time here and titles render
     * through it. Wreckless's first surface write goes to guest 0xF1954000 --
     * the tiled alias of physical 0x01954000, already inside our RAM -- and
     * faulted because nothing was mapped there.
     *
     * A view of the same section rather than fresh storage: the title writes a
     * surface through the tiled address and reads it back through the normal
     * one, so the two have to be the same bytes. That is the whole reason the
     * RAM lives in a file mapping.
     */
    {
        uintptr_t tiled_native = XBOX_TILED_BASE + g_memory_offset;
        size_t tiled_size = xbox_TiledApertureSize();
        /* A view of the CONTIGUOUS window, not of RAM.
         *
         * On hardware all three -- physical P, 0x80000000+P and 0xF0000000+P
         * -- are one and the same memory. Here they cannot be: the XBE image
         * is loaded at its own VA in the RAM mapping, so aliasing the
         * contiguous window onto RAM would drop a title's pinned physical
         * pools on top of its own code (Halo pins 3.4 MB at 0x61000, which is
         * inside its image). The contiguous window therefore has separate
         * storage, and the question becomes which of the two the tiled
         * aperture should be a view of.
         *
         * It is the contiguous one. A tiled address is a GPU surface address
         * by construction, and GPU surfaces come from
         * MmAllocateContiguousMemory -- so the pairing that has to hold is
         * tiled to contiguous. Against RAM instead, Half-Life 2's loader wrote
         * every decoded video frame through 0xF1C63000 while D3D sampled the
         * texture at 0x81C63000, and the sampler read zeros: 1.8 billion black
         * pixels rasterised, perfectly, from an empty texture.
         */
        if (tiled_size > XBOX_CONTIG_SIZE)
            tiled_size = XBOX_CONTIG_SIZE;
        g_tiled_view = g_contig_mapping
            ? MapViewOfFileEx(
                g_contig_mapping,
                FILE_MAP_ALL_ACCESS,
                0, 0,
                tiled_size,
                (LPVOID)tiled_native)
            : NULL;
        if (g_tiled_view) {
            /* Prove the alias rather than assert it. Everything the title
             * renders goes through this window and is read back through the
             * physical address, so if the two are not the same bytes the GPU
             * sees empty buffers and the screen stays black -- with nothing
             * anywhere to say why. One write and one read turns that into a
             * startup line. */
            {
                volatile uint32_t *via_tiled =
                    (volatile uint32_t *)((uintptr_t)(XBOX_TILED_BASE + 0x1000)
                                          + g_memory_offset);
                volatile uint32_t *via_contig =
                    (volatile uint32_t *)((uintptr_t)(XBOX_CONTIG_BASE + 0x1000)
                                          + g_memory_offset);
                uint32_t saved = *via_contig;

                *via_tiled = 0xA5C30F17u;
                if (*via_contig != 0xA5C30F17u)
                    fprintf(stderr, "  WARNING: tiled aperture does NOT alias"
                            " the contiguous window (wrote A5C30F17, read"
                            " %08X) -- the GPU will sample empty textures\n",
                            *via_contig);
                else
                    fprintf(stderr, "  Tiled aperture alias verified"
                            " (tiled 0x%08X == contiguous 0x%08X)\n",
                            XBOX_TILED_BASE, XBOX_CONTIG_BASE);
                *via_contig = saved;
            }
            /* RECOMP_ALIAS_HIGH=1: a second alias of the same window at
             * 0xF8000000, for finding out whether a title that dereferences
             * an address there means physical memory or is simply holding a
             * bad pointer. Outrun 2 faults on a this-pointer of 0xF8604020,
             * which is a valid cached pointer 0x80604020 biased by
             * 0x78000000 -- suggestive, but suggestive is not evidence, and
             * mapping it settles the question in one run: real memory and
             * the title carries on, garbage and it faults again at once.
             *
             * Off by default. Nothing is known to need it, and silently
             * backing an address no console ever had would hide the next
             * title's bad pointer instead of reporting it. */
            if (xbox_EnvSwitch("RECOMP_ALIAS_HIGH", 0) && g_contig_mapping) {
                void *high = MapViewOfFileEx(
                    g_contig_mapping, FILE_MAP_ALL_ACCESS, 0, 0, tiled_size,
                    (LPVOID)(uintptr_t)(0xF8000000u + g_memory_offset));
                fprintf(stderr, "  RECOMP_ALIAS_HIGH: 0x%08X %s\n", 0xF8000000u,
                        high ? "aliased to the contiguous window (an experiment)"
                             : "could not be mapped");
            }
            fprintf(stderr, "  Tiled aperture: %u MB at Xbox VA 0x%08X"
                    " (aliases the contiguous window)\n",
                    (unsigned)(g_memory_size / (1024 * 1024)),
                    XBOX_TILED_BASE);
        } else {
            fprintf(stderr, "  WARNING: tiled aperture at 0x%08X failed"
                    " (error %lu); rendering writes will fault\n",
                    XBOX_TILED_BASE, GetLastError());
        }
    }

    fprintf(stderr, "xbox_MemoryLayoutInit: complete\n");
    return TRUE;
}

/*
 * Make every RAM mirror read-only, for finding writes that reach low memory
 * through an alias.
 *
 * Xbox RAM is visible at 28 virtual addresses that alias the same pages, so a
 * store to 0x04000004 changes Xbox VA 4 without ever touching VA 4. Both a
 * page-protection watchpoint and a DR0 hardware watchpoint on VA 4 therefore
 * report nothing while the memory demonstrably changes -- which is exactly
 * what happened chasing Halo's fs:[4] corruption.
 *
 * Debug aid, not part of normal startup: a title that legitimately writes
 * through a mirror will fault here too, and the fault address names the alias
 * and the code.
 */
void xbox_ProtectMirrorsForDebug(void)
{
    int n = 0;
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        DWORD old;
        if (g_mirror_views[m] &&
            VirtualProtect(g_mirror_views[m], g_memory_size,
                           PAGE_READONLY, &old)) {
            n++;
        }
    }
    fprintf(stderr, "  Mirrors: %d/%d made read-only (debug)\n",
            n, XBOX_NUM_MIRRORS);
}

void xbox_MemoryLayoutShutdown(void)
{
    /* Reject subsequent ownership requests before their mappings disappear. */
    tsfp_thread_memory_stop(&g_guest_threads);
    tsfp_allocations_stop(&g_guest_allocations);
    if (g_kernel_memory) {
        VirtualFree(g_kernel_memory, 0, MEM_RELEASE);
        g_kernel_memory = NULL;
    }
    if (g_nv2a_ack_thread) {
        InterlockedExchange(&g_nv2a_ack_stop, 1);
        while (InterlockedCompareExchange(&g_nv2a_ack_notifiers, 0, 0))
            Sleep(0);
        if (g_nv2a_ack_wake)
            SetEvent(g_nv2a_ack_wake);
        WaitForSingleObject(g_nv2a_ack_thread, 1000);
        CloseHandle(g_nv2a_ack_thread);
        g_nv2a_ack_thread = NULL;
    }
    if (g_nv2a_ack_wake) {
        CloseHandle(g_nv2a_ack_wake);
        g_nv2a_ack_wake = NULL;
    }
    if (g_nv2a_memory) {
        VirtualFree(g_nv2a_memory, 0, MEM_RELEASE);
        g_nv2a_memory = NULL;
    }
    /* Unmap mirror views first */
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        if (g_mirror_views[m]) {
            UnmapViewOfFile(g_mirror_views[m]);
            g_mirror_views[m] = NULL;
        }
    }
    /* Unmap base view */
    if (g_memory_base) {
        UnmapViewOfFile(g_memory_base);
        g_memory_base = NULL;
        g_memory_size = 0;
    }
    /* Close file mapping handle */
    if (g_mapping_handle) {
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
    }
#ifdef __APPLE__
    w32_guest_arena_release();
#endif
    fprintf(stderr, "xbox_MemoryLayoutShutdown: released\n");
}

/* Project guest VM adapter: only known guest RAM views are admitted. */
static int guest_vm_window(uintptr_t address, uint32_t bytes, const void *base, size_t size)
{
    uintptr_t begin = (uintptr_t)base;
    return base && address >= begin && address - begin <= size &&
        bytes <= size - (address - begin);
}
static void *guest_vm_resolve(void *host, uint32_t address, uint32_t bytes, int access)
{
    uintptr_t native, cursor, end;
    int mapped = 0;
    (void)host;
    if (!address || !bytes || (uint64_t)address + bytes > UINT64_C(0x100000000) ||
        (uintptr_t)g_memory_offset > UINTPTR_MAX - address) return NULL;
    native = (uintptr_t)g_memory_offset + address;
    if (native > UINTPTR_MAX - bytes) return NULL;
    mapped = guest_vm_window(native, bytes, g_memory_base, g_memory_size) ||
        guest_vm_window(native, bytes, g_contig_memory, XBOX_CONTIG_SIZE) ||
        guest_vm_window(native, bytes, g_tiled_view, xbox_TiledApertureSize()) ||
        guest_vm_window(native, bytes, g_kernel_memory, 4096);
    for (int i = 0; !mapped && i < XBOX_NUM_MIRRORS; ++i)
        mapped = guest_vm_window(native, bytes, g_mirror_views[i], g_memory_size);
    if (!mapped) return NULL;
    /* Mapped-only admission permits restoring access. The host mapping owner
     * keeps these windows alive; no guest reads occur in that case. */
    if (access < 0) return (void *)native;
    cursor = native; end = native + bytes;
    while (cursor < end) {
        size_t available = pc_guest_region(cursor, access != 0);
        size_t required = (size_t)(end - cursor);
        if (!available) return NULL;
        cursor += available < required ? available : required;
    }
    return (void *)native;
}
static int guest_vm_protect(void *host, uint32_t address, uint32_t bytes,
                            uint32_t protection, uint32_t *previous)
{
    DWORD old = 0;
    void *p = guest_vm_resolve(host, address, bytes, -1);
    if (!p || !VirtualProtect(p, bytes, (DWORD)protection, &old)) return 0;
    *previous = (uint32_t)old;
    return 1;
}
void xbox_GetGuestVirtualMemory(tsfp_guest_vm *context)
{
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    uint32_t page_size = (uint32_t)info.dwPageSize;
#else
    long native_page = sysconf(_SC_PAGESIZE);
    uint32_t page_size = native_page > 0 && (uint64_t)native_page <= UINT32_MAX
        ? (uint32_t)native_page : 0;
#endif
    context->allocations = &g_guest_allocations;
    context->host = NULL;
    context->resolve = guest_vm_resolve;
    context->protect = guest_vm_protect;
    context->code_begin = g_xbox_code_lo;
    context->ram_size = XBOX_TOTAL_RAM;
    context->host_page_size = page_size;
}
/* End project guest VM adapter. */

/* Project guest Mm boundary: common policy, registered spans, host operations. */
static uint32_t guest_mm_query_access(void *host,const void *address)
{
    tsfp_host_page_region region;
    (void)host;
    return tsfp_host_pages_query(address,&region)?region.protection:1;
}
static int guest_mm_set_access(void *host,void *address,uint32_t bytes,uint32_t protection)
{
    (void)host;
    return tsfp_host_pages_protect(address,bytes,tsfp_host_vm_protection(protection));
}
static int guest_mm_available(void *host,uint64_t *bytes)
{
    (void)host;return tsfp_host_available_memory(bytes);
}
void xbox_GetGuestMemoryServices(tsfp_guest_mm *memory)
{
    xbox_GetGuestVirtualMemory(&memory->vm);
    memory->physical_base=XBOX_CONTIG_BASE;
    memory->physical_size=XBOX_CONTIG_SIZE;
    memory->gpu_default=XBOX_GPU_INSTANCE_DEFAULT;
    memory->query_access=guest_mm_query_access;
    memory->set_access=guest_mm_set_access;
    memory->available=guest_mm_available;
}
/* End project guest Mm boundary. */


BOOL xbox_IsXboxAddress(uintptr_t address)
{
    return (address >= XBOX_BASE_ADDRESS &&
            address < XBOX_BASE_ADDRESS + g_memory_size);
}

void *xbox_GetMemoryBase(void)
{
    return g_memory_base;
}

ptrdiff_t xbox_GetMemoryOffset(void)
{
    return g_memory_offset;
}

/* Project thread-memory ABI adapters. */
uint32_t xbox_CurrentThreadObject(void)
{
    return tsfp_thread_current_object(&g_guest_threads, g_fs_base);
}

uint32_t xbox_AllocThreadTib(void)
{
    return tsfp_thread_tib_create(&g_guest_threads);
}

uint32_t xbox_AllocThreadStack(void)
{
    return tsfp_thread_stack_acquire(&g_guest_threads);
}

void xbox_FreeThreadStack(uint32_t top)
{
    (void)tsfp_thread_stack_release(&g_guest_threads, top);
}

int xbox_worker_stack_alloc(void)
{
    return tsfp_worker_stack_acquire(&g_guest_threads);
}

void xbox_worker_stack_free(int slot)
{
    (void)tsfp_worker_stack_release(&g_guest_threads, slot);
}
/* End project thread-memory ABI adapters. */

/* Project allocation ABI adapters; ownership and zeroing live in one provider. */
uint32_t xbox_ReserveAlloc(uint32_t size, uint32_t alignment)
{
    return tsfp_reserve_allocate(&g_guest_allocations, size, alignment);
}
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment)
{
    return tsfp_heap_allocate(&g_guest_allocations, size, alignment);
}
void xbox_HeapFree(uint32_t address)
{
    (void)tsfp_heap_release(&g_guest_allocations, address);
}
uint32_t xbox_HeapBlockSize(uint32_t address)
{
    return tsfp_allocation_remaining(&g_guest_allocations, address);
}
uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment)
{
    return tsfp_physical_allocate(&g_guest_allocations, size, alignment);
}
uint32_t xbox_ContiguousAllocAt(uint32_t address, uint32_t size)
{
    return tsfp_physical_allocate_at(&g_guest_allocations, address, size);
}
uint32_t xbox_ContiguousAllocRange(uint32_t size, uint32_t alignment, uint32_t low, uint32_t high)
{
    return tsfp_physical_allocate_range(&g_guest_allocations, size, alignment, low, high);
}
int xbox_ContiguousFree(uint32_t address)
{
    return tsfp_physical_release(&g_guest_allocations, address);
}
int xbox_ContiguousBlock(int index, uint32_t *address, uint32_t *size)
{
    return index >= 0 && tsfp_physical_block(&g_guest_allocations, (size_t)index, address, size);
}
uint32_t xbox_ContiguousAllocatedBytes(void)
{
    return tsfp_physical_extent(&g_guest_allocations);
}
/* End project allocation ABI adapters. */

HANDLE xbox_GetMappingHandle(void)
{
    return g_mapping_handle;
}
