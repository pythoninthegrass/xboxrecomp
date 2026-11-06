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

/* Optional Arcade feed after the original per-view HUD. */
extern void sub_0014F0B0_gen(void);
void sub_0014F0B0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_kill_feed_hud(sub_0014F0B0_gen);
#else
    sub_0014F0B0_gen();
#endif
}

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
extern RECOMP_MANUAL_TLS uint32_t g_ecx;
extern RECOMP_MANUAL_TLS uint32_t g_ebx, g_edi;
extern RECOMP_MANUAL_TLS uint32_t g_esi;
#include "pc_network_bots_guest.inc"
extern void sub_000DAC30_gen(void);
void sub_000DAC30(void)
{
#ifdef TSFP_PC_CONTROLS
    pc_story_network_npc_constructor(sub_000DAC30_gen);
#else
    sub_000DAC30_gen();
#endif
}

extern void sub_0035BE90_gen(void);
void sub_0035BE90(void)
{
#ifdef TSFP_PC_CONTROLS
    pc_story_network_npc_send(sub_0035BE90_gen);
#else
    sub_0035BE90_gen();
#endif
}

extern void sub_003580D0_gen(void);
void sub_003580D0(void)
{
#ifdef TSFP_PC_CONTROLS
    pc_story_network_npc_receive(sub_003580D0_gen);
#else
    sub_003580D0_gen();
#endif
}

extern void sub_000D73B0_gen(void);
void sub_000D73B0(void)
{
#ifdef TSFP_PC_CONTROLS
    pc_story_network_npc_trace(sub_000D73B0_gen,0x000D73B0u);
#else
    sub_000D73B0_gen();
#endif
}

extern void sub_0015C560_gen(void);
void sub_0015C560(void)
{
#ifdef TSFP_PC_CONTROLS
    pc_story_network_npc_trace(sub_0015C560_gen,0x0015C560u);
#else
    sub_0015C560_gen();
#endif
}

extern void sub_0015F8C0_gen(void);
void sub_0015F8C0(void)
{
#ifdef TSFP_PC_CONTROLS
    pc_story_network_npc_spawn(sub_0015F8C0_gen);
#else
    sub_0015F8C0_gen();
#endif
}

extern void sub_0035CF80_gen(void);
void sub_0035CF80(void)
{
#ifdef TSFP_PC_CONTROLS
    pc_story_network_npc_trace(sub_0035CF80_gen,0x0035CF80u);
#else
    sub_0035CF80_gen();
#endif
}

extern void sub_0006B100_gen(void);
void sub_0006B100(void)
{
#ifdef TSFP_PC_CONTROLS
    pc_story_network_asset(sub_0006B100_gen);
#else
    sub_0006B100_gen();
#endif
}

extern void sub_00357540_gen(void);
void sub_00357540(void)
{
#ifdef TSFP_PC_CONTROLS
    pc_story_network_ready(sub_00357540_gen);
#else
    sub_00357540_gen();
#endif
}

extern void sub_00065C70_gen(void);
void sub_00065C70(void)
{
#ifdef TSFP_PC_CONTROLS
    pc_story_network_advance(sub_00065C70_gen);
#else
    sub_00065C70_gen();
#endif
}

extern void sub_0015BCC0_gen(void);
void sub_0015BCC0(void)
{
#ifdef TSFP_PC_CONTROLS
    pc_story_network_character_remove(sub_0015BCC0_gen);
#else
    sub_0015BCC0_gen();
#endif
}

extern void sub_000D50E0_gen(void);
void sub_000D50E0(void)
{
#ifdef TSFP_PC_CONTROLS
    pc_story_failure_trace(sub_000D50E0_gen,0x000D50E0u);
#else
    sub_000D50E0_gen();
#endif
}

extern void sub_000D4B00_gen(void);
void sub_000D4B00(void)
{
#ifdef TSFP_PC_CONTROLS
    pc_story_network_script_trace(sub_000D4B00_gen,0x000D4B00u);
#else
    sub_000D4B00_gen();
#endif
}

extern void sub_000D4BE0_gen(void);
void sub_000D4BE0(void)
{
#ifdef TSFP_PC_CONTROLS
    pc_story_network_script_trace(sub_000D4BE0_gen,0x000D4BE0u);
#else
    sub_000D4BE0_gen();
#endif
}

extern void sub_000CF060_gen(void);
void sub_000CF060(void)
{
#ifdef TSFP_PC_CONTROLS
    pc_story_network_spawn_pad(sub_000CF060_gen);
#else
    sub_000CF060_gen();
#endif
}

extern void sub_001986A0_gen(void);
void sub_001986A0(void)
{
#ifdef TSFP_PC_CONTROLS
    pc_story_lobby_details(sub_001986A0_gen);
#else
    sub_001986A0_gen();
#endif
}

extern void sub_00067450_gen(void);
void sub_00067450(void)
{
#ifdef TSFP_PC_CONTROLS
    pc_story_failure_trace(sub_00067450_gen,0x00067450u);
#else
    sub_00067450_gen();
#endif
}
extern void sub_00066FC0_gen(void);
void sub_00066FC0(void)
{
#ifdef TSFP_PC_CONTROLS
    pc_story_failure_trace(sub_00066FC0_gen,0x00066FC0u);
#else
    sub_00066FC0_gen();
#endif
}
extern void sub_000681C0_gen(void);
void sub_000681C0(void)
{
#ifdef TSFP_PC_CONTROLS
    pc_story_network_frame(sub_000681C0_gen);
#else
    sub_000681C0_gen();
#endif
}
extern void sub_000D2FD0_gen(void);
void sub_000D2FD0(void)
{
#ifdef TSFP_PC_CONTROLS
    pc_story_failure_trace(sub_000D2FD0_gen,0x000D2FD0u);
#else
    sub_000D2FD0_gen();
#endif
}
extern void sub_000D3C50_gen(void);
void sub_000D3C50(void)
{
#ifdef TSFP_PC_CONTROLS
    pc_story_failure_trace(sub_000D3C50_gen,0x000D3C50u);
#else
    sub_000D3C50_gen();
#endif
}
extern void sub_000D2CE0_gen(void);
void sub_000D2CE0(void)
{
#ifdef TSFP_PC_CONTROLS
    pc_story_failure_trace(sub_000D2CE0_gen,0x000D2CE0u);
#else
    sub_000D2CE0_gen();
#endif
}
extern void sub_003533F0_gen(void);
void sub_003533F0(void)
{ pc_network_bots_guest_score_network(sub_003533F0_gen); }
extern void sub_001BB5C0_gen(void);
void sub_001BB5C0(void)
{ pc_network_bots_guest_shot(sub_001BB5C0_gen); }
extern void sub_0009EAB0_gen(void);
void sub_0009EAB0(void)
{ pc_network_bots_guest_bounds(sub_0009EAB0_gen); }
extern void sub_0035CC10_gen(void);
void sub_0035CC10(void)
{ pc_network_bots_guest_death_gate(sub_0035CC10_gen); }
extern void sub_0005FDC0_gen(void);
void sub_0005FDC0(void)
{ pc_network_bots_guest_pose(sub_0005FDC0_gen); }
extern void sub_00354240_gen(void);
void sub_00354240(void)
{ pc_network_bots_guest_lobby_send(sub_00354240_gen); }
extern void sub_0035E630_gen(void);
void sub_0035E630(void)
{ pc_network_bots_guest_capacity(sub_0035E630_gen); }
extern void sub_002D38C0_gen(void);
void sub_002D38C0(void)
{ pc_network_bots_guest_start(sub_002D38C0_gen); }
extern void sub_00197890_gen(void);
void sub_00197890(void)
{ pc_network_bots_guest_lobby_draw(sub_00197890_gen); }
extern void sub_0003D7F0_gen(void);
void sub_0003D7F0(void)
{ pc_network_bots_guest_lobby_text(sub_0003D7F0_gen); }
extern void sub_000BB6C0_gen(void);
void sub_000BB6C0(void)
{ pc_network_bots_guest_damage(sub_000BB6C0_gen); }
extern void sub_00356490_gen(void);
void sub_00356490(void)
{ pc_network_bots_guest_config(sub_00356490_gen); }
extern void sub_000658D0_gen(void);
void sub_000658D0(void)
{ pc_network_bots_guest_config(sub_000658D0_gen); }
extern void sub_002C78F0_gen(void);
void sub_002C78F0(void)
{ pc_network_bots_menu(sub_002C78F0_gen); }
extern void sub_002D5E70_gen(void);
void sub_002D5E70(void)
{ pc_network_bots_menu(sub_002D5E70_gen); }
extern void sub_002C8370_gen(void);
void sub_002C8370(void)
{ pc_network_bots_menu(sub_002C8370_gen); }
extern void sub_003568C0_gen(void);
void sub_003568C0(void)
{ pc_network_bots_guest_active(sub_003568C0_gen); }
extern void sub_003593D0_gen(void);
void sub_003593D0(void)
{ pc_network_bots_guest_serializable(sub_003593D0_gen); }
extern void sub_00359400_gen(void);
void sub_00359400(void)
{ pc_network_bots_guest_encode(sub_00359400_gen); }
extern void sub_003594B0_gen(void);
void sub_003594B0(void)
{ pc_network_bots_guest_decode(sub_003594B0_gen); }
extern void sub_003559A0_gen(void);
void sub_003559A0(void)
{ pc_network_bots_guest_frame(sub_003559A0_gen); }
/* Keep the original network pump and ABI; sample PC movement every guest tick. */
extern void sub_003544B0_gen(void);
void sub_003544B0(void)
{ pc_network_guest_tick(sub_003544B0_gen); }

extern void sub_003588D0_gen(void);
void sub_003588D0(void)
{ pc_network_guest_sample(sub_003588D0_gen); }

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

/* US outlined label: four outline passes and the foreground draw use the
 * same text/stack mappings synchronously. Discard read facts at return. */
extern void sub_00041E30_gen(void);
void sub_00041E30(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_PROMPTS)
    pc_prompts_layout_begin();
    sub_00041E30_gen();
    pc_prompts_layout_end();
#else
    sub_00041E30_gen();
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

/* US 45410066: the Back/Tab quick status menu sets 0x100, which the
 * original menu renderer uses to omit health/shield. Let Story draw its
 * existing indicators, including their animations and split-screen layout.
 * Mask the flag only during rendering: it also controls menu input and pause
 * behavior elsewhere. Preserve 0x80 too, since the renderer normally clears
 * that flag only on the full pause menu path. */
extern void sub_002E5750_gen(void);
void sub_002E5750(void)
{
    uint32_t player = FP_MEM32(0x007B0C7Cu);
    uint32_t menu = 0, saved = 0;
    if (player && ((FP_MEM32(0x007DE454u) >> 8) & 0xFFu) == 10u) {
        uint32_t index = FP_MEM32(player + 4u);
        if (index < 4u) {
            uint32_t candidate = FP_MEM32(0x00761C48u + index * 4u);
            if (candidate && (FP_MEM32(candidate) & 0x101u) == 0x101u) {
                menu = candidate;
                saved = FP_MEM32(menu) & 0x180u;
                FP_MEM32(menu) &= ~0x100u;
            }
        }
    }
    sub_002E5750_gen();
    if (menu) FP_MEM32(menu) = (FP_MEM32(menu) & ~0x180u) | saved;
}

#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
#include "pc_objectives.h"
#endif
/* Build the Story Objectives panel in the same fitted horizontal space as
 * its gauges. These are the US builder's percentage widths, used both by
 * container creation and text measurement. Keep the original font size and
 * let the existing layout wrap/scroll long objectives. Restore the shared
 * constants immediately; other menus must retain their own widths. */
extern void sub_002F5E60_gen(void);
void sub_002F5E60(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    uint32_t player = FP_MEM32(0x007B0C7Cu);
    uint32_t view = player ? FP_MEM32(player + 8u) : 0;
    /* player+8 is the camera; its HUD viewport starts at +0x224. */
    if (view) view += 0x224u;
    if (view && ((FP_MEM32(0x007DE454u) >> 8) & 0xFFu) == 10u) {
        uint32_t saved[2] = {FP_MEM32(0x0047D1F4u),FP_MEM32(0x00478628u)};
        float left = (float)(int32_t)FP_MEM32(view);
        float top = (float)(int32_t)FP_MEM32(view + 0x14u);
        float right = (float)(int32_t)FP_MEM32(view + 4u) + 1.f;
        float bottom = (float)(int32_t)FP_MEM32(view + 0x18u) + 1.f;
        float ratio = pc_objectives_scale(1.2f,left,top,right,bottom,
                      (left+right)*.5f,(top+bottom)*.5f,
                      FP_MEM32(0x00790950u),FP_MEM32(player + 4u))/1.2f;
        float width;
        memcpy(&width, &saved[0], sizeof width); width *= ratio;
        memcpy(FP_GUEST(0x0047D1F4u), &width, sizeof width);
        memcpy(&width, &saved[1], sizeof width); width *= ratio;
        memcpy(FP_GUEST(0x00478628u), &width, sizeof width);
        sub_002F5E60_gen();
        FP_MEM32(0x0047D1F4u) = saved[0];
        FP_MEM32(0x00478628u) = saved[1];
        return;
    }
#endif
    sub_002F5E60_gen();
}

extern void sub_0013AB60_gen(void);
void sub_0013AB60(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    uint32_t frame = g_esp;
    uint32_t view = FP_MEM32(0x007E71C8u), player = FP_MEM32(0x007B0C7Cu);
    if (FP_MEM32(frame) == 0x002E58C9u && view && player &&
        ((FP_MEM32(0x007DE454u) >> 8) & 0xFFu) == 10u) {
        uint32_t saved = FP_MEM32(frame + 16u);
        float scale, x, y, adjusted;
        float left = (float)(int32_t)FP_MEM32(view);
        float top = (float)(int32_t)FP_MEM32(view + 0x14u);
        float right = (float)(int32_t)FP_MEM32(view + 4u) + 1.f;
        float bottom = (float)(int32_t)FP_MEM32(view + 0x18u) + 1.f;
        memcpy(&scale, &saved, sizeof scale);
        memcpy(&x, FP_GUEST(frame + 8u), sizeof x);
        memcpy(&y, FP_GUEST(frame + 12u), sizeof y);
        x += (float)(int32_t)FP_MEM32(view + 8u) - left;
        y += (float)(int32_t)FP_MEM32(view + 0x1Cu) - top;
        adjusted = pc_objectives_scale(scale,left,top,right,bottom,x,y,
                        FP_MEM32(0x00790950u),FP_MEM32(player + 4u));
        memcpy(FP_GUEST(frame + 16u), &adjusted, sizeof adjusted);
        pc_display_health(sub_0013AB60_gen);
        FP_MEM32(frame + 16u) = saved;
        return;
    }
    pc_display_health(sub_0013AB60_gen);
#else
    sub_0013AB60_gen();
#endif
}

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
    if (pc_display_transition_overlay(sub_0001EFB0_gen)) return;
    if (pc_display_scope_mask(sub_0001EFB0_gen)) return;
    if (caller == 0x002D90EBu || caller == 0x002F3AEDu || caller == 0x002F3B0Fu) {
        pc_display_footer_background(sub_0001EFB0_gen);
        return;
    }
    int top = caller == 0x0006F322u || caller == 0x0013972Eu;
    int story = caller == 0x0006F322u || caller == 0x0006F34Bu;
    if (story || caller == 0x0013970Eu || caller == 0x0013972Eu) {
        PcDisplayMode mode = pc_display_effective();
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

/* Shared frontend footer gradient helper; preserve all other callers. */
extern void sub_002D8100_gen(void);
void sub_002D8100(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_footer_background(sub_002D8100_gen);
#else
    sub_002D8100_gen();
#endif
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
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_PROMPTS)
#include "pc_auto_look_guest.inc"
#endif
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_PROMPTS)
#include "pc_tank_guest.inc"
#endif
extern void sub_001730E0_gen(void);
void sub_001730E0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_PROMPTS)
    pc_tank_driver(sub_001730E0_gen);
#else
    sub_001730E0_gen();
#endif
}

extern void sub_0025D520_gen(void);
void sub_0025D520(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_PROMPTS)
    pc_auto_look_camera(sub_0025D520_gen);
#else
    sub_0025D520_gen();
#endif
}
extern void sub_0007B7B0_gen(void);
void sub_0007B7B0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_PROMPTS)
    pc_auto_look_choice(sub_0007B7B0_gen);
#else
    sub_0007B7B0_gen();
#endif
}
extern void sub_002E9BC0_gen(void);
void sub_002E9BC0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_PROMPTS)
    pc_auto_look_settings(sub_002E9BC0_gen);
#else
    sub_002E9BC0_gen();
#endif
}

/* 0x000D2B80 builds player-relative movement vectors from camera yaw.
 * Apply raw mouse counts just before those vectors, after the game's own
 * controller look step. Keep the generated original body and its ABI. */
extern void sub_001476F0_gen(void);
void sub_001476F0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_tutorial(sub_001476F0_gen);
#else
    sub_001476F0_gen();
#endif
}
extern void sub_00040F00_gen(void);
void sub_00040F00(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_tutorial_wrap(sub_00040F00_gen);
#else
    sub_00040F00_gen();
#endif
}
extern void sub_00041DC0_gen(void);
void sub_00041DC0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_tutorial_text(sub_00041DC0_gen);
#else
    sub_00041DC0_gen();
#endif
}
/* US HUD groups: preserve proportions and anchor whole widgets. */
extern void sub_0013FC50_gen(void);
void sub_0013FC50(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_ui(sub_0013FC50_gen,0x0013FC50u);
#else
    sub_0013FC50_gen();
#endif
}

/* Keep the complete Arcade RANK widget at its authored left inset. */
extern void sub_0012E3F0_gen(void);
/* The shared panel measures/fits labels before their outlined draws. Reuse
 * read mappings only inside that synchronous original operation, excluding
 * the surrounding placement flushes. Scratch writes still query live. */
static void pc_panel_text_original(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_PROMPTS)
    pc_prompts_layout_begin();
#endif
    sub_0012E3F0_gen();
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_PROMPTS)
    pc_prompts_layout_end();
#endif
}
void sub_0012E3F0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_ui(pc_panel_text_original,0x0012E3F0u);
#else
    pc_panel_text_original();
#endif
}

extern void sub_001334D0_gen(void);
void sub_001334D0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_ui(sub_001334D0_gen,0x001334D0u);
#else
    sub_001334D0_gen();
#endif
}
extern void sub_00133AB0_gen(void);
void sub_00133AB0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_ui(sub_00133AB0_gen,0x00133AB0u);
#else
    sub_00133AB0_gen();
#endif
}
extern void sub_001358A0_gen(void);
void sub_001358A0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_ui(sub_001358A0_gen,0x001358A0u);
#else
    sub_001358A0_gen();
#endif
}
extern void sub_0012F670_gen(void);
void sub_0012F670(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_ui(sub_0012F670_gen,0x0012F670u);
#else
    sub_0012F670_gen();
#endif
}
extern void sub_0014B930_gen(void);
void sub_0014B930(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_ui(sub_0014B930_gen,0x0014B930u);
#else
    sub_0014B930_gen();
#endif
}
extern void sub_001409C0_gen(void);
void sub_001409C0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_ui(sub_001409C0_gen,0x001409C0u);
#else
    sub_001409C0_gen();
#endif
}
extern void sub_00018EA0_gen(void);
void sub_00018EA0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    if(FP_MEM32(g_esp)==0x0013BA9Cu)pc_display_health_side();
#endif
    sub_00018EA0_gen();
}
extern void sub_00080F20_gen(void);
void sub_00080F20(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    if(FP_MEM32(g_esp)==0x001341B7u)pc_display_radar_begin();
#endif
    sub_00080F20_gen();
}

#include "pc_weapon_slots.h"
extern void sub_001BF140_gen(void);
void sub_001BF140(void)
{
    sub_001BF140_gen();
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_PROMPTS)
    pc_display_weapon_slots();
#endif
}

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



/* Frontend hover/click: after final menu layout, before action dispatch. */
extern void sub_002C4D20_gen(void);
void sub_002C4D20(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_menu_character_draw(sub_002C4D20_gen);
#else
    sub_002C4D20_gen();
#endif
}

extern void sub_002E4E90_gen(void);
void sub_002E4E90(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_menu_team_draw(sub_002E4E90_gen);
#else
    sub_002E4E90_gen();
#endif
}

extern void sub_002EDDC0_gen(void);
void sub_002EDDC0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_menu_team_tick(sub_002EDDC0_gen);
#else
    sub_002EDDC0_gen();
#endif
}

extern void sub_0019E970_gen(void);
void sub_0019E970(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_menu_choice_draw(sub_0019E970_gen);
#else
    sub_0019E970_gen();
#endif
}

extern void sub_00360710_gen(void);
void sub_00360710(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_menu_choice_draw(sub_00360710_gen);
#else
    sub_00360710_gen();
#endif
}

extern void sub_00081AD0_gen(void);
void sub_00081AD0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_menu_choice_arrow(sub_00081AD0_gen,8u);
#else
    sub_00081AD0_gen();
#endif
}

extern void sub_00081B20_gen(void);
void sub_00081B20(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_menu_choice_arrow(sub_00081B20_gen,4u);
#else
    sub_00081B20_gen();
#endif
}

extern void sub_002EA250_gen(void);
void sub_002EA250(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_menu_sensitivity_draw(sub_002EA250_gen);
#else
    sub_002EA250_gen();
#endif
}

extern void sub_000814B0_gen(void);
void sub_000814B0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_menu_slider_bar(sub_000814B0_gen);
#else
    sub_000814B0_gen();
#endif
}

extern void sub_002C3500_gen(void);
void sub_002C3500(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_menu_audio_draw(sub_002C3500_gen,0);
#else
    sub_002C3500_gen();
#endif
}

extern void sub_002C3610_gen(void);
void sub_002C3610(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_menu_audio_draw(sub_002C3610_gen,1);
#else
    sub_002C3610_gen();
#endif
}

extern void sub_00073750_gen(void);
void sub_00073750(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_menu_mouse(sub_00073750_gen);
#else
    sub_00073750_gen();
#endif
}

/* Keep the mouse Confirm source alive through original menu callbacks. */
extern void sub_0007A7B0_gen(void);
void sub_0007A7B0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_menu_mouse_dispatch(sub_0007A7B0_gen);
#else
    sub_0007A7B0_gen();
#endif
}

extern void sub_00073870_gen(void);
void sub_00073870(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_menu_mouse_input(sub_00073870_gen);
#else
    sub_00073870_gen();
#endif
}

extern void sub_000765A0_gen(void);
void sub_000765A0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_menu_mouse_footer(sub_000765A0_gen);
#else
    sub_000765A0_gen();
#endif
}

extern void sub_002D91F0_gen(void);
void sub_002D91F0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_menu_mouse_footer(sub_002D91F0_gen);
#else
    sub_002D91F0_gen();
#endif
}

extern void sub_002F3CA0_gen(void);
void sub_002F3CA0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_menu_mouse_footer(sub_002F3CA0_gen);
#else
    sub_002F3CA0_gen();
#endif
}

extern void sub_000771D0_gen(void);
void sub_000771D0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_menu_finish(sub_000771D0_gen);
#else
    sub_000771D0_gen();
#endif
}

extern void sub_00077140_gen(void);
void sub_00077140(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_menu_finish(sub_00077140_gen);
#else
    sub_00077140_gen();
#endif
}

/* Gameplay white wash follows the output rather than proportional HUD bounds. */
extern void sub_000F90D0_gen(void);
void sub_000F90D0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_flash_overlay(sub_000F90D0_gen);
#else
    sub_000F90D0_gen();
#endif
}

/* Keep the title reveal's outer black panels outside a widened camera. */
extern void sub_002BD810_gen(void);
void sub_002BD810(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_title_matte(sub_002BD810_gen);
#else
    sub_002BD810_gen();
#endif
}

/* US animated frontend background sheet. */
extern void sub_002DB010_gen(void);
void sub_002DB010(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_menu_backdrop(sub_002DB010_gen);
#else
    sub_002DB010_gen();
#endif
}

extern void sub_002D0BE0_gen(void);
void sub_002D0BE0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_main_menu_font(sub_002D0BE0_gen,1);
#else
    sub_002D0BE0_gen();
#endif
}

extern void sub_00076150_gen(void);
void sub_00076150(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_main_menu_font(sub_00076150_gen,0);
#else
    sub_00076150_gen();
#endif
}

extern void sub_00135F30_gen(void);
void sub_00135F30(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_scope(sub_00135F30_gen,0x00135F30u);
#else
    sub_00135F30_gen();
#endif
}

extern void sub_001364A0_gen(void);
void sub_001364A0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_scope(sub_001364A0_gen,0x001364A0u);
#else
    sub_001364A0_gen();
#endif
}

extern void sub_00136B40_gen(void);
void sub_00136B40(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_scope(sub_00136B40_gen,0x00136B40u);
#else
    sub_00136B40_gen();
#endif
}

extern void sub_0001B7B0_gen(void);
void sub_0001B7B0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_aim_primitive(sub_0001B7B0_gen,0);
#else
    sub_0001B7B0_gen();
#endif
}

extern void sub_0001C140_gen(void);
void sub_0001C140(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_aim_primitive(sub_0001C140_gen,1);
#else
    sub_0001C140_gen();
#endif
}

extern void sub_0001BA70_gen(void);
void sub_0001BA70(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_aim_primitive(sub_0001BA70_gen,2);
#else
    sub_0001BA70_gen();
#endif
}

/* Crane/remote-gun vignette: four mirrored texture quadrants form one
 * screen effect. Scope only the art, not the caller's control labels. */
extern void sub_00137010_gen(void);
void sub_00137010(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_aim_panel(sub_00137010_gen,0x00137010u);
#else
    sub_00137010_gen();
#endif
}

extern void sub_00138F70_gen(void);
void sub_00138F70(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_aim_panel(sub_00138F70_gen,0x00138F70u);
#else
    sub_00138F70_gen();
#endif
}

extern void sub_0037A9D0_gen(void);
void sub_0037A9D0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_aim_panel(sub_0037A9D0_gen,0x0037A9D0u);
#else
    sub_0037A9D0_gen();
#endif
}

extern void sub_00137F00_gen(void);
void sub_00137F00(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_effect_scope(sub_00137F00_gen);
#else
    sub_00137F00_gen();
#endif
}

extern void sub_0001F090_gen(void);
void sub_0001F090(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_weapon_effect(sub_0001F090_gen,0);
#else
    sub_0001F090_gen();
#endif
}

extern void sub_000167B0_gen(void);
void sub_000167B0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_weapon_effect(sub_000167B0_gen,1);
#else
    sub_000167B0_gen();
#endif
}

extern void sub_0001A6F0_gen(void);
void sub_0001A6F0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_weapon_effect(sub_0001A6F0_gen,2);
#else
    sub_0001A6F0_gen();
#endif
}

extern void sub_000C77C0_gen(void);
void sub_000C77C0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_menu_mouse_mode();
#endif
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

/* US 45410066: scope the spirit compatibility rules to the game's effect
 * renderer and its model nodes / billboard batches, never shader hashes. */
#include "pc_ghost_render_guest.inc"
extern void sub_0024A1A0_gen(void);
void sub_0024A1A0(void) { pc_ghost_render(sub_0024A1A0_gen); }
extern void sub_0018EA90_gen(void);
void sub_0018EA90(void) { pc_ghost_model_node(sub_0018EA90_gen); }
extern void sub_0001D3E0_gen(void);
void sub_0001D3E0(void) { pc_ghost_billboard_begin(sub_0001D3E0_gen); }
extern void sub_0001D380_gen(void);
void sub_0001D380(void) { pc_ghost_billboard_end(sub_0001D380_gen); }

/* Profile-scoped early access; original progress and save ABI are retained. */
extern void sub_001CD3E0_gen(void);
void sub_001CD3E0(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_unlock_requirement(sub_001CD3E0_gen);
#else
    sub_001CD3E0_gen();
#endif
}

extern void sub_001CDA70_gen(void);
void sub_001CDA70(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_unlock_event(sub_001CDA70_gen);
#else
    sub_001CDA70_gen();
#endif
}

extern void sub_001CD560_gen(void);
void sub_001CD560(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_unlock_movie(sub_001CD560_gen);
#else
    sub_001CD560_gen();
#endif
}

extern void sub_00069D90_gen(void);
void sub_00069D90(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_unlock_profile_init(sub_00069D90_gen);
#else
    sub_00069D90_gen();
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
/* Texture cameras share their guest-pixel layout with their copied mask. */
extern void sub_000C8120_gen(void);
void sub_000C8120(void)
{
#if defined(TSFP_PC_CONTROLS) || defined(TSFP_NATIVE_DISPLAY)
    pc_display_camera_feed(sub_000C8120_gen);
#else
    sub_000C8120_gen();
#endif
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

#include "pc_model_tree_trace_guest.inc"
#include "pc_breadcrumbs.h"
static RECOMP_MANUAL_TLS unsigned pc_model_tree_nesting;
extern void sub_000583E0_gen(void);
void sub_000583E0(void)
{
    if(!pc_model_tree_nesting)pc_model_tree_trace();
    pc_capture_model(*(const uint32_t *)((uintptr_t)g_esp+4+g_xbox_mem_offset),
        *(const uint32_t *)((uintptr_t)g_esp+8+g_xbox_mem_offset),pc_model_tree_nesting+1);
    pc_model_tree_nesting++;sub_000583E0_gen();pc_model_tree_nesting--;
}

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

