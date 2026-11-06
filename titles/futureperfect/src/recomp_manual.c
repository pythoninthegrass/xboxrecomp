#include <stddef.h>
/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>   /* getenv, exit: the spin verdict below */
#include <string.h>   /* strcmp: RECOMP_ICALL_FATAL */
#ifdef TSFP_PC_CONTROLS
#include "pc_controls.h"
#include "pc_display_menu.h"
#endif
#ifdef TSFP_NATIVE_DISPLAY
#include "pc_display_menu.h"
#endif

/* One diagnostic report, one piece.
 *
 * Each report below is a dozen fprintf calls and the CRT takes its lock per
 * call, so with more than one guest thread running the lines interleave in
 * the middle of a report. Dino Crisis 3 runs six workers once it is past its
 * menus, and its log came out like this:
 *
 *   callers: 0x004DB424 <- 0x002705B4 <- 0x002705B4[ICALL] unresolved jump
 *   target 0x41B0B870 -- 1 time(s) (total calls: 6930990)
 *
 * -- two threads' reports spliced together, with a caller list that stops
 * mid-sentence and an address that belongs to neither line as read. These
 * diagnostics exist to be read during exactly the multi-threaded bring-up
 * that breaks them.
 *
 * The CRT exposes the lock these calls were already taking one at a time, so
 * hold it across the whole report instead. */
#if defined(_MSC_VER)
#  define RECOMP_DIAG_LOCK()   _lock_file(stderr)
#  define RECOMP_DIAG_UNLOCK() _unlock_file(stderr)
#elif defined(__unix__) || defined(__APPLE__)
#  define RECOMP_DIAG_LOCK()   flockfile(stderr)
#  define RECOMP_DIAG_UNLOCK() funlockfile(stderr)
#else
#  define RECOMP_DIAG_LOCK()   ((void)0)
#  define RECOMP_DIAG_UNLOCK() ((void)0)
#endif

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

/* These are defined thread-local in xbox_memory_layout.c. Declaring them
 * without the same storage class here does not fail to link -- it silently
 * resolves to different storage, so every read gets 0. That is why the log
 * below reported no call site: not because the guest esp was stale, but
 * because this file was not reading the guest esp at all. */
#if defined(_MSC_VER)
#  define RECOMP_MANUAL_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define RECOMP_MANUAL_TLS __thread
#else
#  define RECOMP_MANUAL_TLS _Thread_local
#endif

extern RECOMP_MANUAL_TLS uint32_t g_eax;
/* The guest stack pointer. At the moment an indirect call is refused, the
 * caller has already pushed its guest return address, so the top of the
 * guest stack is the call site -- the one thing the old log did not say. */
extern RECOMP_MANUAL_TLS uint32_t g_esp;
extern ptrdiff_t g_xbox_mem_offset;
/* The lifted code sections, so a value on the guest stack can be told
 * apart from data when naming the callers of a refused call. */
extern uint32_t g_xbox_code_lo;
extern uint32_t g_xbox_code_hi;
/* The esp the dispatch macro captured. Not g_esp, which is stale by the time
 * a refused call is reported. Zero when the title's generated header predates
 * this, and the log then says it has no callers rather than inventing them. */
extern RECOMP_MANUAL_TLS uint32_t g_icall_saved_esp;
/* Which dispatch form was refused: 0 unknown, 1 call, 2 jump. Unknown
 * means this title was lifted before the macros published it. */
extern RECOMP_MANUAL_TLS uint32_t g_icall_dispatch_form;


/* PC System Link: public XNet API boundary, US 45410066. */
#include "pc_network_guest.inc"
/* Keep the original network pump and ABI; sample PC movement every guest tick. */
extern void sub_003544B0_gen(void);
void sub_003544B0(void)
{ pc_network_guest_tick(sub_003544B0_gen); }

extern void sub_00431BB1_gen(void);
void sub_00431BB1(void) { pc_network_guest(0x00431BB1u,sub_00431BB1_gen); }
extern void sub_00431BC8_gen(void);
void sub_00431BC8(void) { pc_network_guest(0x00431BC8u,sub_00431BC8_gen); }
extern void sub_00431CB6_gen(void);
void sub_00431CB6(void) { pc_network_guest(0x00431CB6u,sub_00431CB6_gen); }
extern void sub_00431CC3_gen(void);
void sub_00431CC3(void) { pc_network_guest(0x00431CC3u,sub_00431CC3_gen); }
extern void sub_00431D53_gen(void);
void sub_00431D53(void) { pc_network_guest(0x00431D53u,sub_00431D53_gen); }
extern void sub_00431700_gen(void);
void sub_00431700(void) { pc_network_guest(0x00431700u,sub_00431700_gen); }
extern void sub_0043170B_gen(void);
void sub_0043170B(void) { pc_network_guest(0x0043170Bu,sub_0043170B_gen); }
extern void sub_00431716_gen(void);
void sub_00431716(void) { pc_network_guest(0x00431716u,sub_00431716_gen); }
extern void sub_00431721_gen(void);
void sub_00431721(void) { pc_network_guest(0x00431721u,sub_00431721_gen); }
extern void sub_0043172C_gen(void);
void sub_0043172C(void) { pc_network_guest(0x0043172Cu,sub_0043172C_gen); }
extern void sub_00431770_gen(void);
void sub_00431770(void) { pc_network_guest(0x00431770u,sub_00431770_gen); }
extern void sub_0043177F_gen(void);
void sub_0043177F(void) { pc_network_guest(0x0043177Fu,sub_0043177F_gen); }
extern void sub_0043179B_gen(void);
void sub_0043179B(void) { pc_network_guest(0x0043179Bu,sub_0043179B_gen); }
extern void sub_004317A6_gen(void);
void sub_004317A6(void) { pc_network_guest(0x004317A6u,sub_004317A6_gen); }
extern void sub_004317B1_gen(void);
void sub_004317B1(void) { pc_network_guest(0x004317B1u,sub_004317B1_gen); }
extern void sub_004317BC_gen(void);
void sub_004317BC(void) { pc_network_guest(0x004317BCu,sub_004317BC_gen); }
extern void sub_004317D2_gen(void);
void sub_004317D2(void) { pc_network_guest(0x004317D2u,sub_004317D2_gen); }
extern void sub_004317DD_gen(void);
void sub_004317DD(void) { pc_network_guest(0x004317DDu,sub_004317DD_gen); }
extern void sub_00431802_gen(void);
void sub_00431802(void) { pc_network_guest(0x00431802u,sub_00431802_gen); }
extern void sub_0043184C_gen(void);
void sub_0043184C(void) { pc_network_guest(0x0043184Cu,sub_0043184C_gen); }
extern void sub_00431866_gen(void);
void sub_00431866(void) { pc_network_guest(0x00431866u,sub_00431866_gen); }
extern void sub_0043188F_gen(void);
void sub_0043188F(void) { pc_network_guest(0x0043188Fu,sub_0043188F_gen); }
extern void sub_00431968_gen(void);
void sub_00431968(void) { pc_network_guest(0x00431968u,sub_00431968_gen); }

/* Shared text sizing and rendering: translate controller prompt tokens
 * before either pass so alignment, wrapping, and displayed text agree. */
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_PROMPTS)
#include "pc_controls.h"
#include "pc_prompt_guest.inc"
#endif
extern void sub_0003FCF0_gen(void);
void sub_0003FCF0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_PROMPTS)
    pc_prompt_guest(sub_0003FCF0_gen,3,4);
#else
    sub_0003FCF0_gen();
#endif
}
extern void sub_00040FA0_gen(void);
void sub_00040FA0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_PROMPTS)
    pc_prompt_guest(sub_00040FA0_gen,4,6);
#else
    sub_00040FA0_gen();
#endif
}

/* -- Future Perfect: the MMX memcpy at 0x000245C0 ----------------------- */

/*
 * sub_000245C0 is the title's MMX block copy: void *(void *dst, const void *src,
 * size_t n), cdecl, returning dst (five callers, among them sub_0011A930).
 *
 * It enters two slides of string moves through computed jumps:
 *
 *   mov ecx, 8; sub ecx, edi; and ecx, 7; ... neg ecx; add ecx, 0x2460C; jmp ecx
 *       -> 0 to 7 movsb (0x00024604..0x0002460B) to align the destination
 *   shr ecx, 2; and ecx, 0xF; neg ecx; add ecx, 0x24784; jmp ecx
 *       -> 0 to 15 movsd (0x00024774..0x00024780) for the tail
 *
 * The lifter turns `jmp <reg>` into gotos only when the register was loaded with
 * `mov reg, imm` (tools/recomp/translator.py, imm_code_refs), and the
 * disassembler ends the function at 0x00024772, before the movsd slide; its
 * epilogue became a function of its own, sub_00024784. Lifted, the first jump
 * resolves to nothing: run 5 reported "[ICALL] unresolved jump target
 * 0x0002460C" from 0x0011ACAC at 182 s, in the 2401 intro cutscene, and the
 * title faulted at once in sub_00383678 on the data the copy never wrote. It is
 * the only `jmp <reg>` in the XBE whose target is computed by `add reg, imm`.
 *
 * A copy has no behaviour beyond the bytes it moves, so this replacement does
 * exactly what the original does; memmove, because overlap was never defined.
 * Delete it once the lifter follows computed jumps into slides and the
 * disassembler keeps the slide inside the function.
 */
#define FP_GUEST(va) ((uint8_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define FP_MEM32(va) (*(volatile uint32_t *)FP_GUEST(va))

/* US 45410066: adapt only the two cinematic matte pairs to the display.
 * The return PCs identify story and scripted in-game bars; all other solid
 * rectangles (including fades, menus and scope borders) remain untouched. */
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
#include "pc_display.h"
#include "pc_cutscene.h"
#endif
extern void sub_0001EFB0_gen(void);
void sub_0001EFB0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    uint32_t frame = g_esp, caller = FP_MEM32(frame);
    int top = caller == 0x0006F322u || caller == 0x0013972Eu;
    int story = caller == 0x0006F322u || caller == 0x0006F34Bu;
    if (story || caller == 0x0013970Eu || caller == 0x0013972Eu) {
        PcDisplayMode mode = pc_display_current();
        int y1 = (int32_t)FP_MEM32(frame + 8u);
        int y2 = (int32_t)FP_MEM32(frame + 16u);
        int height, full, adjusted, side, view_top;
        int x1 = (int32_t)FP_MEM32(frame + 4u);
        int x2 = (int32_t)FP_MEM32(frame + 12u);
        if (story) {
            height = (int32_t)FP_MEM32(0x004E8CF4u);
            view_top = 0;
            full = (int)(height * .145f);
        } else {
            uint32_t viewport = FP_MEM32(0x007E71C8u);
            height = (int32_t)FP_MEM32(viewport + 0x18u) -
                     (int32_t)FP_MEM32(viewport + 0x14u) + 1;
            view_top = (int32_t)FP_MEM32(viewport + 0x14u);
            full = 71; /* Original maximum: trunc(70 * animation) + 1. */
        }
        adjusted = pc_cutscene_bar_height(mode.width, mode.height, height, full, y2-y1);
        side = pc_cutscene_bar_width(mode.width,mode.height,x2-x1,height,full,y2-y1);
        if (!side && adjusted == y2-y1) { sub_0001EFB0_gen(); return; }
        if (side) {
            FP_MEM32(frame + 4u) = (uint32_t)(top ? x1 : x2-side);
            FP_MEM32(frame + 12u) = (uint32_t)(top ? x1+side : x2);
            FP_MEM32(frame + 8u) = (uint32_t)view_top;
            FP_MEM32(frame + 16u) = (uint32_t)(view_top+height);
        }
        else if (top) FP_MEM32(frame + 16u) = (uint32_t)(y1 + adjusted);
        else FP_MEM32(frame + 8u) = (uint32_t)(y2 - adjusted);
        if (adjusted > 0 || side > 0) sub_0001EFB0_gen();
        else g_esp += 4u; /* cdecl: caller retains the five arguments. */
        FP_MEM32(frame + 4u) = (uint32_t)x1;
        FP_MEM32(frame + 12u) = (uint32_t)x2;
        FP_MEM32(frame + 8u) = (uint32_t)y1;
        FP_MEM32(frame + 16u) = (uint32_t)y2;
        return;
    }
#endif
    sub_0001EFB0_gen();
}

void sub_000245C0(void)
{
    uint32_t dst = FP_MEM32(g_esp + 4u);
    uint32_t src = FP_MEM32(g_esp + 8u);
    uint32_t n   = FP_MEM32(g_esp + 12u);

    if (n && dst != src)
        memmove(FP_GUEST(dst), FP_GUEST(src), n);
    g_eax = dst;
    g_esp += 4u;            /* ret: the caller pops the arguments (cdecl) */
}

/* ── Manual function overrides ─────────────────────────────── */
/* 0x000D2B80 builds player-relative movement vectors from camera yaw.
 * Apply raw mouse counts just before those vectors, after the game's own
 * controller look step. Keep the generated original body and its ABI. */
extern void sub_000D2B80_gen(void);
void sub_000D2B80(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_PROMPTS)
    pc_controls_apply_mouse(FP_MEM32(0x007B0C7Cu), FP_MEM32(0x007B0CC0u), g_xbox_mem_offset);
#endif
    sub_000D2B80_gen();
}

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
extern void sub_00151CF0_gen(void);
void sub_00151CF0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_perspective_projection(sub_00151CF0_gen);
#else
    sub_00151CF0_gen();
#endif
}


extern void sub_000C77C0_gen(void);
void sub_000C77C0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_camera(sub_000C77C0_gen);
#else
    sub_000C77C0_gen();
#endif
}

extern void sub_00079D60_gen(void);
void sub_00079D60(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_menu_button(sub_00079D60_gen);
#else
    sub_00079D60_gen();
#endif
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    recomp_func_t display_callback=pc_display_menu_lookup(xbox_va);
    if(display_callback) return display_callback;
#endif
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* Positional gunfire range only: both call sites belong to the original
 * weapon sound dispatcher, for one-shot and repeating fire respectively. */
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
#include "gameplay/pc_spatial_audio.h"
#include "interface/pc_audio_settings.h"
#endif
extern void sub_00021FB0_gen(void);
void sub_00021FB0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    uint32_t caller=0,argument=g_esp+8u,original=0;memcpy(&caller,(const void *)((uintptr_t)g_esp+g_xbox_mem_offset),4);memcpy(&original,(const void *)((uintptr_t)argument+g_xbox_mem_offset),4);
    if(pc_audio_enhanced_enabled() && (caller==0x001B7216u || caller==0x001B71D6u)) {
        float range;memcpy(&range,&original,4);range=recomp_weapon_sound_range(range);memcpy((void *)((uintptr_t)argument+g_xbox_mem_offset),&range,4);
        sub_00021FB0_gen();
        memcpy((void *)((uintptr_t)argument+g_xbox_mem_offset),&original,4); /* preserve cdecl arguments */
        return;
    }
#endif
    sub_00021FB0_gen();
}



/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    /*
     * Rate-limited per target, reporting at 1, 10, 100 ...
     *
     * An unresolved target inside a loop is the normal case, not the rare
     * one: Mortal Kombat: Deadly Alliance produced 46843 of these for a
     * single address in one 30-second run, seventeen lines each. That buries
     * every other diagnostic in the log and says nothing the first report did
     * not. The progression is the useful part -- one line says a target was
     * never identified, a run of them says the title is stuck on it.
     */
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }

    RECOMP_DIAG_LOCK();
    fprintf(stderr, "[ICALL] unresolved %starget 0x%08X -- %llu time(s) "
                    "(total calls: %llu)\n",
            g_icall_dispatch_form == 1 ? "call " :
            g_icall_dispatch_form == 2 ? "jump " : "",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    if (g_icall_dispatch_form == 2 && hits[i] == 1)
        fprintf(stderr, "  a jump, not a call: if this address is inside a "
                        "function rather than at its start, the guest is doing "
                        "its own control flow (a coroutine, a longjmp, or a "
                        "switch arm) and seeding it as a function will not "
                        "help\n");

    /*
     * Who called it. The lifted caller pushed its return address on the guest
     * stack, so code-range values above esp name the chain -- the same scan
     * the memory watchpoints use. It is a heuristic: stale return addresses
     * from earlier frames show up too, and the first entry is the reliable
     * one. It is still the difference between an address with no context and
     * a function to open, which is what an unresolved target needs.
     */
    {
        const uint8_t *stack = (const uint8_t *)g_xbox_mem_offset
                             + g_icall_saved_esp;
        int shown = 0;
        fprintf(stderr, "  callers:");
        for (i = 0; g_xbox_mem_offset && g_icall_saved_esp
                    && i < 160 && shown < 6; i++) {
            uint32_t v;
            memcpy(&v, stack + (size_t)i * 4, sizeof v);
            if (v >= g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "%s 0x%08X", shown ? " <-" : "", v);
                shown++;
            }
        }
        if (!shown)
            fprintf(stderr, " (none on the stack)");
        fprintf(stderr, "\n");
    }

    /* The recent-target ring buffer, once per address. What ran just before
     * an unresolved call is context for the first report and noise after. */
    if (hits[i] == 1) {
        fprintf(stderr, "  Recent ICALL targets:\n");
        for (i = 0; i < 16; i++) {
            int idx = (g_icall_trace_idx - 16 + i) & 15;
            if (g_icall_trace[idx])
                fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
        }
    }
    RECOMP_DIAG_UNLOCK();
    fflush(stderr);
}
/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va, uint32_t saved_esp)
{
    /* A power of ten, because the rate limiter above only reaches this
     * function body at 1, 10, 100 ... and the verdict has to land on one of
     * those. A hundred thousand skips of one target is unambiguous: no title
     * makes progress through that. */
    enum { SLOTS = 16, SPIN_VERDICT = 100000 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    static int said_it;   /* the verdict below is said once, not once per target */
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    {
        /* The call site, read off the guest stack. Without it the log
         * says a wild pointer was skipped but not by whom, and the
         * caller is the only thing that leads anywhere.
         *
         * It comes from the esp the dispatch macro captured, not from
         * g_esp. The lifted call site pushes its return address onto the
         * *local* esp and only syncs g_esp at certain points, so g_esp is
         * stale here -- it read 0 exactly when a null target most needed
         * explaining, which sent three rounds of Jet Set Radio Future
         * chasing inferences instead of a call site. saved_esp is the value
         * before that push, so the return address is the dword below it.
         *
         * Only for the call form. A tail jump pushes no return address, so
         * that dword is whatever the frame happened to leave there: Tony
         * Hawk's Pro Skater 2X reported "from 0x00000008" and then "from
         * 0x00000004" for the same million skipped calls, and both are
         * plainly not code. Printing a number that cannot be an address as
         * though it were the caller is worse than printing nothing, because
         * it is the one field a reader goes to next.
         *
         * So: name the form, and where the top of the stack cannot answer,
         * scan for code-range values the way the unresolved-target logger
         * above does. Stale return addresses from earlier frames come back
         * too and the first is the reliable one, but a list of real code
         * addresses is a place to start and 0x00000008 is not. */
        uint32_t caller = 0;
        if (g_icall_dispatch_form != 2 && saved_esp >= 4 && g_xbox_mem_offset)
            caller = *(const uint32_t *)((const uint8_t *)g_xbox_mem_offset
                                         + (saved_esp - 4));
        if (caller < g_xbox_code_lo || caller >= g_xbox_code_hi)
            caller = 0;
        RECOMP_DIAG_LOCK();
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped "
                        "%llu time(s) via a %s",
                va, (unsigned long long)hits[i],
                g_icall_dispatch_form == 2 ? "jump" : "call");
        if (caller)
            fprintf(stderr, " from 0x%08X", caller);
        fprintf(stderr, " (null or wild function pointer, at call #%llu)\n",
                (unsigned long long)g_icall_count);

        /* The stack scan, once per address: the same heuristic the
         * unresolved-target logger uses, and the only thing that names a
         * caller when the return address is not where it should be. */
        if (hits[i] == 1) {
            const uint8_t *stack = (const uint8_t *)g_xbox_mem_offset
                                 + saved_esp;
            int shown = 0, k;
            fprintf(stderr, "  callers:");
            for (k = 0; g_xbox_mem_offset && saved_esp
                        && k < 160 && shown < 6; k++) {
                uint32_t v;
                memcpy(&v, stack + (size_t)k * 4, sizeof v);
                if (v >= g_xbox_code_lo && v < g_xbox_code_hi) {
                    fprintf(stderr, "%s 0x%08X", shown ? " <-" : "", v);
                    shown++;
                }
            }
            if (!shown)
                fprintf(stderr, " (none on the stack)");
            fprintf(stderr, "\n");
        }

        /* RECOMP_ICALL_FATAL=1: fault here instead of skipping, so the crash
         * handler prints the guest call stack that reached this call.
         *
         * The caller printed above is read from the top of the guest stack,
         * which is right only when the lifted call site pushed a return
         * address there and nothing has since moved esp. When it reads 0 --
         * exactly the case where a null target most needs explaining -- the
         * line says a wild pointer was skipped and nothing about by whom. A
         * deliberate fault costs the run and buys the backtrace. */
        {
            static int fatal = -1;
            if (fatal < 0) {
                const char *v = getenv("RECOMP_ICALL_FATAL");
                fatal = v && *v && strcmp(v, "0") != 0;
            }
            if (fatal) {
                fprintf(stderr, "[ICALL] RECOMP_ICALL_FATAL: faulting here for "
                                "a backtrace\n");
                fflush(stderr);
                *(volatile int *)0 = 1;
            }
        }
    }

    /* Past a certain count this stops being a warning and becomes a verdict.
     *
     * A skipped call sets eax to 0 and returns. Zero is a fine answer for a
     * null function pointer in most code -- it reads as NULL, false, or
     * nothing -- but it is also S_OK, so in COM-shaped code a loop that
     * repeats while the result is non-negative can never leave. Panzer
     * Dragoon Orta hung exactly there, three billion skips in forty seconds,
     * with no frame ever presented.
     *
     * The value is deliberately left alone: there is no return value that is
     * right for both conventions, and guessing failure would break the
     * callers for which zero is correct. What can be fixed is the silence.
     * Say plainly that the title is hung, once, and say what to do about it.
     */
    if (hits[i] == SPIN_VERDICT && !said_it) {
        said_it = 1;
        fprintf(stderr,
            "[ICALL] ^^ this is a hang, not slow progress. One target has been\n"
            "        skipped %d times. The skip returns eax = 0, which is also\n"
            "        S_OK, so a loop testing for a non-negative result will\n"
            "        never exit. Try `py -3 -m tools.seed_from_log <log>` to\n"
            "        recover the target as a function, and read\n"
            "        docs/technical/memory-watchpoints.md for who wrote the\n"
            "        pointer. Set RECOMP_ICALL_SPIN_FATAL=1 to stop here\n"
            "        instead of spinning.\n", SPIN_VERDICT);
        fflush(stderr);
        if (getenv("RECOMP_ICALL_SPIN_FATAL")) {
            fprintf(stderr, "[ICALL] RECOMP_ICALL_SPIN_FATAL is set; exiting.\n");
            fflush(stderr);
            exit(3);
        }
    }
    RECOMP_DIAG_UNLOCK();
    fflush(stderr);
}

