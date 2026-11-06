/**
 * Xbox Static Recompilation - Runtime Type Definitions
 *
 * Type definitions and helper macros used by mechanically translated
 * x86 -> C code. Each original x86 function is translated to a C
 * function that uses these types and macros.
 *
 * This is a reusable template for ANY Xbox game. Game-specific
 * customization should go in separate headers.
 *
 * Memory model:
 *   Xbox data sections are mapped to their original VAs via
 *   CreateFileMapping + MapViewOfFileEx (see xbox_memory.h).
 *   Recompiled code accesses globals via pointer casts, e.g.:
 *     *(uint32_t*)0x003B2360
 *
 * Register model:
 *   Volatile registers (eax, ecx, edx, esp) are global variables,
 *   matching real x86 behavior where these registers are shared
 *   across all code. This enables correct argument passing via the
 *   simulated stack and return value communication via eax.
 *
 *   Callee-saved registers (ebx, esi, edi) are also global because
 *   callers pass implicit parameters through them (e.g. 'this' via
 *   esi in thiscall). The callee-save contract is enforced by
 *   PUSH32/POP32 instructions in the generated code, not by C local
 *   variable scoping.
 *
 *   ebp is NOT global - it stays local in each function because many
 *   FPO (Frame Pointer Omission) functions use it as scratch without
 *   save/restore. For SEH functions, g_seh_ebp bridges the gap.
 *
 * Calling convention:
 *   All translated functions are void(void). Arguments are passed
 *   on the simulated Xbox stack (via push instructions before call).
 *   Return values are communicated through g_eax.
 *   The call instruction pushes the real guest return address (the VA of
 *   the instruction after the call); ret discards it with esp += 4.
 *   The value is never used to transfer control -- control flow is C
 *   call/return -- but it must be correct because guest code reads it
 *   (__SEH_prolog's scope table, _alloca probes, "mov eax, [esp]").
 */

#ifndef RECOMP_TYPES_H
#define RECOMP_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Project fatal divide-error boundary; result registers have not been written.
 * Guest exception dispatch is not implemented by this diagnostic adapter. */
#if defined(_MSC_VER)
__declspec(noreturn)
#else
__attribute__((noreturn))
#endif
void tsfp_guest_divide_error(uint32_t site, unsigned reason);

/* Optional title numeric policy. The generic runtime retains x86 conversion
 * semantics; a title can supply a separate header without baking its addresses
 * into the instruction lifter. Memory operands are evaluated only once. */
#ifdef RECOMP_NUMERIC_POLICY_HEADER
#include RECOMP_NUMERIC_POLICY_HEADER
#else
#define RECOMP_I32_TO_F32(address, value) ((float)(int32_t)(value))
#define RECOMP_I32_TO_F64(address, value) ((double)(int32_t)(value))
#endif
#ifndef RECOMP_I32_TO_F32_AT
#define RECOMP_I32_TO_F32_AT(site, address, value) RECOMP_I32_TO_F32(address, value)
#endif
#ifndef RECOMP_STORE_GLOBAL_U32
#define RECOMP_STORE_GLOBAL_U32(site, address, value) (value)
#endif
#ifndef RECOMP_LOAD_GLOBAL_U32
#define RECOMP_LOAD_GLOBAL_U32(site, address, value) (value)
#endif
#ifndef RECOMP_LOAD_U8_AT
#define RECOMP_LOAD_U8_AT(site, value) (value)
#endif
#ifndef RECOMP_PUSH_IMM_U32_AT
#define RECOMP_PUSH_IMM_U32_AT(site, value) (value)
#endif
/* A title may complete an audited leaf after its entry callback. Returning
 * zero must leave guest state untouched so the generated body can run. */
#ifndef RECOMP_FAST_ENTRY
#define RECOMP_FAST_ENTRY(site) 0
#endif
#ifndef RECOMP_DIRECT_CALLEE_AT
#define RECOMP_DIRECT_CALLEE_AT(site, function) (function)
#endif
#ifndef RECOMP_FLOAT_BRANCH_AT
#define RECOMP_FLOAT_BRANCH_AT(site, taken) (taken)
#endif
#ifndef RECOMP_F32_ADD_AT
#define RECOMP_F32_ADD_AT(site, left, right) ((left) + (right))
#endif
#ifndef RECOMP_F32_SUB_AT
#define RECOMP_F32_SUB_AT(site, left, right) ((left) - (right))
#endif
#ifndef RECOMP_F32_MUL_AT
#define RECOMP_F32_MUL_AT(site, left, right) ((left) * (right))
#endif
#ifndef RECOMP_F32_DIV_AT
#define RECOMP_F32_DIV_AT(site, left, right) ((left) / (right))
#endif

/* math.h is load-bearing, and its absence was invisible.
 *
 * The lifter emits sqrt() and fabs() for fsqrt/fabs -- 146 of them in one of
 * Halo's nine chunks alone -- and now sin/cos/tan/atan2/log2/exp2/fmod for the
 * x87 transcendentals. With no declaration in scope, C89 implicit declaration
 * makes every one of them return `int`: the caller reads EAX instead of XMM0 and
 * gets garbage, then converts that garbage to double. Vector normalisation is
 * 1/sqrt(x), so this corrupts every matrix the title builds.
 *
 * Nothing reported it because generated code is compiled with /w (see the game
 * CMakeLists) -- MSVC's C4013 was emitted and discarded. Same failure as the
 * missing stdlib.h in kernel_bridge.c, in a hotter path. */
#include <math.h>
#ifndef RECOMP_AFTER_STORE32
#define RECOMP_AFTER_STORE32(site, address) ((void)0)
#endif
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

/* MSVC's __forceinline -> gcc/clang equivalent on POSIX. */
#if !defined(_MSC_VER) && !defined(__forceinline)
#define __forceinline inline __attribute__((always_inline))
#endif

/* Guest debug output: INT 2D, the Xbox kernel debug trap.
 *
 * eax selects the service, ecx carries its argument. Service 1 prints the
 * ANSI_STRING ecx points at -- what OutputDebugStringA compiles to. The int3
 * that always follows is the slide byte the kernel steps over, so the lifter
 * emits nothing for it; lifting it to __debugbreak() terminated the process
 * with STATUS_BREAKPOINT the first time a title tried to print. */
void recomp_debug_service(uint32_t service, uint32_t arg_va);

/* An int3 that lifted code actually executed: almost always an un-unwound
 * C++ throw. Silent before; see docs/technical/cpp-exceptions.md. */
void recomp_int3_reached(uint32_t va);

/* A C++ throw, reported at the throw with the type that was thrown. This
 * runtime cannot unwind, so it returns; see docs/technical/cpp-exceptions.md. */
void recomp_cxx_throw(uint32_t object_va, uint32_t throwinfo_va);

/* MSVC's __debugbreak() intrinsic -> gcc/clang equivalent.
 * The auto-generated code emits __debugbreak for x86 INT 3 instructions. */
#if !defined(_MSC_VER) && !defined(__debugbreak)
#define __debugbreak() __builtin_trap()
#endif

/* ================================================================
 * Memory offset
 * ================================================================ */

/**
 * Memory offset from Xbox VA to actual mapped address.
 * When Xbox memory is mapped at the original address (0x00010000),
 * this is 0 and the MEM macros are simple identity casts.
 * When mapped elsewhere, this adjusts all memory accesses.
 *
 * Set once during memory initialization, then read-only.
 */
extern ptrdiff_t g_xbox_mem_offset;

/* Bounds of the title's executable sections, filled in by
 * xbox_MemoryLayoutInit from the XBE's own section table. Zero means "not
 * loaded yet", which RECOMP_ICALL_IS_CODE treats as allow.
 *
 * These replace a hardcoded 0x00400000 cutoff that was only ever right for one
 * title -- see the comment where they are defined in xbox_memory_layout.c. */
/* --force-return: read once at startup, so the check at each forced ret is a
 * load rather than a getenv. Zero unless RECOMP_FORCE_RETURN is set, which is
 * what lets a build carrying forced functions behave normally by default. */
extern int g_force_return;

extern uint32_t g_xbox_code_lo;
extern uint32_t g_xbox_code_hi;

/* Is this indirect-call target plausibly code?
 *
 * Synthetic kernel thunks live at 0xFE0000xx and are dispatched by
 * recomp_lookup_kernel, so they are always allowed. Everything else has to lie
 * inside an executable section of the loaded title. A target that fails this is
 * a garbage pointer that reached a call site, and calling it would be worse
 * than dropping it. */
#define RECOMP_ICALL_IS_CODE(_va) \
    ((_va) >= 0xFE000000u || g_xbox_code_hi == 0u || \
     ((_va) >= g_xbox_code_lo && (_va) < g_xbox_code_hi))

/* TSFP guest register interface begin.
 * Architectural values belong to the current host thread. Generated routines
 * share these cells with callbacks; EBP remains function-local, with separate
 * frame/SEH bridges. The double-backed x87 stack and independent MMX storage
 * retain the current execution model, not the physical x87/MMX alias model.
 */
#ifndef RECOMP_TLS
#if defined(_MSC_VER)
#define RECOMP_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#define RECOMP_TLS __thread
#else
#define RECOMP_TLS _Thread_local
#endif
#endif

#ifndef RECOMP_XMM_DEFINED
#define RECOMP_XMM_DEFINED
typedef union RecompXmm {
    uint64_t q[2];
    uint32_t u[4];
    int32_t i[4];
    float f[4];
    double d[2];
} RecompXmm;
#endif
#ifndef RECOMP_MMX_DEFINED
#define RECOMP_MMX_DEFINED
typedef union RecompMmx {
    uint64_t q;
    uint32_t ud[2];
    int32_t d[2];
    uint16_t uw[4];
    int16_t w[4];
    uint8_t ub[8];
    int8_t b[8];
} RecompMmx;
#endif

extern RECOMP_TLS uint32_t g_eax, g_ebx, g_ecx, g_edx;
extern RECOMP_TLS uint32_t g_esi, g_edi, g_esp;
extern RECOMP_TLS uint32_t g_ebp, g_seh_ebp, g_fs_base;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3;
extern RECOMP_TLS RecompMmx g_mm4, g_mm5, g_mm6, g_mm7;
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS int g_fp_cmp, g_df;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS uint16_t g_fp_cc;
#define XBOX_FS_BASE g_fs_base
/* TSFP guest register interface end. */

/* ---- non-local jumps (setjmp / longjmp) --------------------------------
 *
 * A recompiled function is a real C function, so restoring the guest esp is
 * only half of a longjmp: the abandoned frames are still on the native stack,
 * and control returns into them once the resume point finishes. Pairing each
 * guest jmp_buf with a native one -- taken at the setjmp call site, the only
 * place a native setjmp is valid -- makes the guest jump a native jump, so the
 * frames actually unwind.
 *
 * recomp_setjmp_slot() returns the native buffer for a guest jmp_buf address,
 * allocating one on first use. recomp_guest_longjmp() restores the guest
 * registers the CRT's longjmp would and jumps; it returns 0, without jumping,
 * for a buffer that has no native counterpart, leaving the caller to fall back
 * to the guest's own longjmp.
 */
/* TSFP own atomics begin: sequentially consistent operations on mapped RAM.
 * Naturally aligned operands follow the host intrinsic contract. Guest MMIO,
 * alignment/page-crossing faults and guest exception delivery are separate.
 * Unlocked XADD/CMPXCHG retain the existing stronger atomic-memory policy. */
#ifndef TSFP_GUEST_ATOMIC_PRIMITIVES
#define TSFP_GUEST_ATOMIC_PRIMITIVES
#include <stdint.h>
#if defined(_MSC_VER)
#include <intrin.h>
static inline uint8_t tsfp_atomic_exchange8(void *address, uint32_t value) {
    return (uint8_t)_InterlockedExchange8((volatile char *)address, (char)value);
}
static inline uint8_t tsfp_atomic_add8(void *address, uint32_t value) {
    return (uint8_t)_InterlockedExchangeAdd8((volatile char *)address, (char)value);
}
static inline uint8_t tsfp_atomic_cas8(void *address, uint32_t compare, uint32_t value) {
    return (uint8_t)_InterlockedCompareExchange8((volatile char *)address, (char)value, (char)compare);
}
static inline uint16_t tsfp_atomic_exchange16(void *address, uint32_t value) {
    return (uint16_t)_InterlockedExchange16((volatile short *)address, (short)value);
}
static inline uint16_t tsfp_atomic_add16(void *address, uint32_t value) {
    return (uint16_t)_InterlockedExchangeAdd16((volatile short *)address, (short)value);
}
static inline uint16_t tsfp_atomic_cas16(void *address, uint32_t compare, uint32_t value) {
    return (uint16_t)_InterlockedCompareExchange16((volatile short *)address, (short)value, (short)compare);
}
static inline uint32_t tsfp_atomic_exchange32(void *address, uint32_t value) {
    return (uint32_t)_InterlockedExchange((volatile long *)address, (long)value);
}
static inline uint32_t tsfp_atomic_add32(void *address, uint32_t value) {
    return (uint32_t)_InterlockedExchangeAdd((volatile long *)address, (long)value);
}
static inline uint32_t tsfp_atomic_cas32(void *address, uint32_t compare, uint32_t value) {
    return (uint32_t)_InterlockedCompareExchange((volatile long *)address, (long)value, (long)compare);
}
#else
static inline uint8_t tsfp_atomic_exchange8(void *address, uint32_t value) {
    return __atomic_exchange_n((uint8_t *)address, (uint8_t)value, __ATOMIC_SEQ_CST);
}
static inline uint8_t tsfp_atomic_add8(void *address, uint32_t value) {
    return __atomic_fetch_add((uint8_t *)address, (uint8_t)value, __ATOMIC_SEQ_CST);
}
static inline uint8_t tsfp_atomic_cas8(void *address, uint32_t compare, uint32_t value) {
    uint8_t expected = (uint8_t)compare;
    __atomic_compare_exchange_n((uint8_t *)address, &expected, (uint8_t)value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}
static inline uint16_t tsfp_atomic_exchange16(void *address, uint32_t value) {
    return __atomic_exchange_n((uint16_t *)address, (uint16_t)value, __ATOMIC_SEQ_CST);
}
static inline uint16_t tsfp_atomic_add16(void *address, uint32_t value) {
    return __atomic_fetch_add((uint16_t *)address, (uint16_t)value, __ATOMIC_SEQ_CST);
}
static inline uint16_t tsfp_atomic_cas16(void *address, uint32_t compare, uint32_t value) {
    uint16_t expected = (uint16_t)compare;
    __atomic_compare_exchange_n((uint16_t *)address, &expected, (uint16_t)value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}
static inline uint32_t tsfp_atomic_exchange32(void *address, uint32_t value) {
    return __atomic_exchange_n((uint32_t *)address, (uint32_t)value, __ATOMIC_SEQ_CST);
}
static inline uint32_t tsfp_atomic_add32(void *address, uint32_t value) {
    return __atomic_fetch_add((uint32_t *)address, (uint32_t)value, __ATOMIC_SEQ_CST);
}
static inline uint32_t tsfp_atomic_cas32(void *address, uint32_t compare, uint32_t value) {
    uint32_t expected = (uint32_t)compare;
    __atomic_compare_exchange_n((uint32_t *)address, &expected, (uint32_t)value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}
#endif
/* Existing consumers can keep the old interface while the runtime is replaced. */
#define RECOMP_ATOMIC_ADD32(p, v) tsfp_atomic_add32((void *)(p), (v))
#define RECOMP_ATOMIC_CAS32(p, c, v) tsfp_atomic_cas32((void *)(p), (c), (v))
#endif /* TSFP_GUEST_ATOMIC_PRIMITIVES */
/* TSFP own atomics end */

#include <setjmp.h>
jmp_buf *recomp_setjmp_slot(uint32_t buf_va);
int recomp_guest_longjmp(uint32_t buf_va, uint32_t value);
/* A guest longjmp to a buffer armed on another native stack (host fibers):
 * the handler switches to that stack and jumps there; it does not return. */
typedef void (*recomp_foreign_longjmp_fn)(jmp_buf *target, int value,
                                          uintptr_t armed_stack);
void recomp_set_foreign_longjmp(recomp_foreign_longjmp_fn fn);
#define RECOMP_DF_STEP(n) (g_df ? -(int32_t)(n) : (int32_t)(n))

#define RECOMP_FCMP_CC(c) ((uint16_t)((c)==2 ? 0x4500u : (c)<0 ? 0x0100u : (c)>0 ? 0u : 0x4000u))
/* Values in the existing double-backed stack are all representable as normal
 * x87 extended values, including binary64 subnormals. Empty stack tags and
 * unsupported extended encodings are not represented by this stack model. */
static inline uint16_t recomp_fxam(double value) {
    return (uint16_t)((signbit(value) ? 0x0200u : 0u) |
        (isnan(value) ? 0x0100u : isinf(value) ? 0x0500u :
         value == 0.0 ? 0x4000u : 0x0400u));
}

/* Result of an x87 compare, in the shape the status word wants:
 *   -1 less, 0 equal, 1 greater, 2 unordered (either operand is NaN).
 * The unordered case is not a curiosity: `fucompp` of a value with itself
 * followed by `test ah, 0x44; jp` is how this era's CRT asks "is this a NaN",
 * and collapsing it to "equal" answers no every time. */
#define RECOMP_FCMP(a, b)     (((a) != (a) || (b) != (b)) ? 2 : (a) < (b) ? -1 : (a) > (b) ? 1 : 0)
/* x87 rounding to an integral value under the guest RC bits (control word
 * bits 10-11: 0 nearest-even, 1 down, 2 up, 3 toward zero). FRNDINT rounds
 * this way, and so does FIST before its store; neither may follow the HOST's
 * rounding mode, which the guest's fldcw never reaches. MSVC's CRT floor() and
 * ceil() are `fldcw RC=down/up` (via _ctrlfp) around _frnd, a bare frndint,
 * so a frndint lifted as rint() made both of them round to nearest.
 * NaN, the infinities and values already integral come back unchanged, and a
 * zero result keeps the operand's sign, as FRNDINT's does. */
static inline double recomp_frndint(double value, uint16_t control) {
    double rounded;
    if (!isfinite(value)) return value;
    switch((control>>10)&3) {
    case 1: rounded=floor(value); break;
    case 2: rounded=ceil(value); break;
    case 3: rounded=trunc(value); break;
    default: {
        double lo=floor(value), fraction=value-lo;
        rounded=lo;
        if(fraction>0.5 || (fraction==0.5 && fmod(lo,2.0)!=0.0)) rounded=lo+1.0;
        break;
    }
    }
    return rounded == 0.0 ? copysign(0.0, value) : rounded;
}
/* x87 integer stores use the guest RC bits, independently of host rounding.
 * Masked invalid conversions store the signed integer-indefinite value. */
static inline int64_t recomp_fist(double value, uint16_t control, unsigned bits) {
    double rounded = recomp_frndint(value, control);
    double limit=ldexp(1.0,(int)bits-1);
    if(!isfinite(rounded) || rounded < -limit || rounded >= limit)
        return bits==64?INT64_MIN:-(INT64_C(1)<<(bits-1));
    return (int64_t)rounded;
}

/* ================================================================
 * ICALL trace ring buffer (for debugging indirect calls)
 * ================================================================ */

/** Size of the ring buffer (must be power of 2). */
#define ICALL_TRACE_SIZE 16

/** Ring buffer of recent indirect call target VAs. */
extern volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];

/** Current write index into the ring buffer. */
extern volatile uint32_t g_icall_trace_idx;

/** Total count of indirect calls executed. */
extern volatile uint64_t g_icall_count;

/**
 * Called when an indirect call target cannot be resolved.
 * Implement this in your game-specific code to log diagnostics.
 * The va parameter is the Xbox VA that failed to resolve.
 */
/* The guest esp at the moment an indirect call was refused, so the log can
 * name the call site. The top of the guest stack is a return address in
 * every form: the one the call site just pushed for RECOMP_ICALL and
 * RECOMP_ICALL_SAFE, and the current frame's own for RECOMP_ITAIL, which
 * pushes nothing. Set here rather than passed, so a title whose generated
 * header predates this still compiles and simply reports no callers. */
extern RECOMP_TLS uint32_t g_icall_saved_esp;
/* Which dispatch form was refused: 0 unknown, 1 call, 2 jump.
 *
 * The two mean different things and want different next steps. A call to
 * an address nothing identified is a discovery gap, answered by seeding;
 * a jump to one is usually the guest doing its own control flow, which no
 * amount of seeding fixes. Saying "Failed to resolve" for both sent a
 * Mortal Kombat coroutine three rounds through the seeding tool.
 *
 * Zero rather than a boolean because a title whose generated header
 * predates this never assigns it, and a two-valued flag would then read
 * as "call" for every refusal including the jumps -- the exact wrong
 * answer this exists to stop giving. Unknown is reported as unknown. */
extern RECOMP_TLS uint32_t g_icall_dispatch_form;

void recomp_icall_fail_log(uint32_t va);

/* Report an indirect call whose target is not code (a null or wild
 * function pointer). Rate-limited per address by the implementation,
 * because these usually arrive inside a loop -- which is exactly why
 * they must be reported: silently skipping one turns a diagnosable null
 * vtable call into an unexplained hang. */
void recomp_icall_not_code_log(uint32_t va, uint32_t saved_esp);

/* Report a direct call or jump into an address that was never recompiled --
 * the generated stub in recomp_stubs_unresolved.c. The stub keeps esp
 * balanced and does nothing else, so whatever the guest code there would have
 * done is skipped. Once per address, from the runtime, so every title gets it
 * without touching its recomp_manual.c.
 *
 * Burnout 2's sound manager constructor was one of these: reached only by a
 * static initialiser's tail jump, never discovered, and so never run. A field
 * it should have set stayed null, and the game crashed a minute later in an
 * unrelated-looking Pause. */
void recomp_stub_missing(uint32_t va);

/* Indirect-branch target feedback. The ring buffer above is crash forensics --
 * 16 entries, overwritten constantly. This is a durable, deduplicated record of
 * every target the title ever reached, for feeding back into the next codegen
 * run (tools/recomp/icall_feedback.py).
 *
 * The header is pulled in only when the feature is on, so a default build needs
 * neither the file nor src/kernel on its include path. Disabled,
 * RECOMP_ICALL_OBSERVE discards its arguments without expanding them, so the
 * RECOMP_ICALL_SEEN_* constants need not exist either. */
#ifdef RECOMP_ICALL_FEEDBACK
#include "recomp_icall_feedback.h"
#else
#define RECOMP_ICALL_OBSERVE(va, flags) ((void)0)
#define RECOMP_ICALL_OBSERVE_SITE(site, va) ((void)0)
#endif

/**
 * Function entry trace, emitted only for addresses passed to
 * tools.recomp --trace-functions. Bring-up is largely "which of these
 * init calls does it not come back from", and answering that by
 * overriding a function loses the body you were trying to observe.
 */
/* The guest's time source.
 *
 * Xbox's QueryPerformanceCounter is a bare rdtsc and its
 * QueryPerformanceFrequency returns the CPU clock as a constant, so the guest
 * divides this by 733,333,333 to get seconds. Returning the host TSC would
 * make that division wrong by the ratio of the two clocks; the runtime scales
 * to the console's rate instead. */
uint64_t xbox_ReadTimeStampCounter(void);

void recomp_trace_enter(const char *name, uint32_t va);
#define RECOMP_TRACE_ENTER(name, va) recomp_trace_enter((name), (va))
void recomp_trace_exit(const char *name, uint32_t va);
#define RECOMP_TRACE_EXIT(name, va) recomp_trace_exit((name), (va))
void recomp_trace_esp(const char *name, const char *tag);
#define RECOMP_TRACE_ESP(name, tag) recomp_trace_esp((name), (tag))


/* TSFP guest primitive support begin.
 * The supported hosts are little-endian. Guest RAM accesses may be unaligned
 * and may alias any scalar representation. Volatile preserves the requested
 * access order; it is not a substitute for atomic operations or MMIO routing.
 */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "Guest scalar lvalues require a little-endian host"
#endif
#if defined(__GNUC__) || defined(__clang__)
#define TSFP_GUEST_SCALAR(name, type) typedef type name __attribute__((aligned(1), may_alias))
#define TSFP_GUEST_UNALIGNED
#elif defined(_MSC_VER)
#define TSFP_GUEST_SCALAR(name, type) typedef type name
#define TSFP_GUEST_UNALIGNED __unaligned
#else
#error "Define guest unaligned scalar access for this compiler"
#endif
TSFP_GUEST_SCALAR(TsfpGuestU8, uint8_t);
TSFP_GUEST_SCALAR(TsfpGuestU16, uint16_t);
TSFP_GUEST_SCALAR(TsfpGuestU32, uint32_t);
TSFP_GUEST_SCALAR(TsfpGuestU64, uint64_t);
TSFP_GUEST_SCALAR(TsfpGuestI8, int8_t);
TSFP_GUEST_SCALAR(TsfpGuestI16, int16_t);
TSFP_GUEST_SCALAR(TsfpGuestI32, int32_t);
TSFP_GUEST_SCALAR(TsfpGuestI64, int64_t);
TSFP_GUEST_SCALAR(TsfpGuestF32, float);
TSFP_GUEST_SCALAR(TsfpGuestF64, double);
#undef TSFP_GUEST_SCALAR
#define TSFP_GUEST_CELL(type, address) (*(volatile type TSFP_GUEST_UNALIGNED *)XBOX_PTR(address))

/* Return the complete updated register, as an assignment expression does.
 * Both the cell expression and incoming value are sampled once. */
static inline uint32_t tsfp_guest_insert_bits(volatile uint32_t *cell,
                                             uint32_t value, unsigned bits,
                                             unsigned shift)
{
    const uint32_t low = (1u << bits) - 1u;
    const uint32_t prior = *cell;
    const uint32_t result = (prior & ~(low << shift)) | ((value & low) << shift);
    *cell = result;
    return result;
}
/* TSFP guest primitive support end. */

/* Wrap the guest address before adding the host relocation. Cast the offset
 * to uintptr_t to retain modular pointer arithmetic for negative relocation. */
#define XBOX_PTR(address) ((uintptr_t)(uint32_t)(address) + (uintptr_t)g_xbox_mem_offset)
#define MEM8(address) TSFP_GUEST_CELL(TsfpGuestU8, address)
#define MEM16(address) TSFP_GUEST_CELL(TsfpGuestU16, address)
#define MEM32(address) TSFP_GUEST_CELL(TsfpGuestU32, address)
#define MEM64(address) TSFP_GUEST_CELL(TsfpGuestU64, address)
#define SMEM8(address) TSFP_GUEST_CELL(TsfpGuestI8, address)
#define SMEM16(address) TSFP_GUEST_CELL(TsfpGuestI16, address)
#define SMEM32(address) TSFP_GUEST_CELL(TsfpGuestI32, address)
#define SMEM64(addr) TSFP_GUEST_CELL(TsfpGuestI64, addr)
#define MEMF(address) TSFP_GUEST_CELL(TsfpGuestF32, address)
#define MEMD(address) TSFP_GUEST_CELL(TsfpGuestF64, address)

/* TSFP partial-register and stack primitives begin. */
#define LO8(value) ((uint8_t)(value))
#define HI8(value) ((uint8_t)((uint32_t)(value) >> 8u))
#define LO16(value) ((uint16_t)(value))
#define SET_LO8(cell, value) tsfp_guest_insert_bits(&(cell), (uint32_t)(value), 8u, 0u)
#define SET_HI8(cell, value) tsfp_guest_insert_bits(&(cell), (uint32_t)(value), 8u, 8u)
#define SET_LO16(cell, value) tsfp_guest_insert_bits(&(cell), (uint32_t)(value), 16u, 0u)

/* Read a PUSH source against the old stack. POP increments before evaluating
 * its destination, including ESP itself and memory addressed through ESP. */
#define PUSH32(stack, source) do { \
    const uint32_t tsfp_pushed_ = (uint32_t)(source); \
    uint32_t *const tsfp_stack_cell_ = &(stack); \
    const uint32_t tsfp_next_stack_ = *tsfp_stack_cell_ - 4u; \
    *tsfp_stack_cell_ = tsfp_next_stack_; \
    MEM32(tsfp_next_stack_) = tsfp_pushed_; \
} while (0)
#define POP32(stack, destination) do { \
    uint32_t *const tsfp_stack_cell_ = &(stack); \
    const uint32_t tsfp_old_stack_ = *tsfp_stack_cell_; \
    const uint32_t tsfp_popped_ = MEM32(tsfp_old_stack_); \
    *tsfp_stack_cell_ = tsfp_old_stack_ + 4u; \
    (destination) = tsfp_popped_; \
} while (0)
/* TSFP partial-register and stack primitives end. */

/* TSFP scalar vector construction begin.
 * Model transforms emulate a memory MOVSS: preserve the float representation
 * in lane zero and clear the other three lanes. SIMD instructions themselves
 * are emitted directly by the project SSE/MMX translators. */
static inline RecompXmm XMM_SCALAR(float value)
{
    RecompXmm lanes;
    memcpy(&lanes.u[0], &value, sizeof(value));
    lanes.u[1] = 0u;
    lanes.u[2] = 0u;
    lanes.u[3] = 0u;
    return lanes;
}
/* TSFP scalar vector construction end. */

/* ================================================================
 * Flag computation helpers
 *
 * These macros compute x86 flags for conditional branches.
 * Used by the lifter's pattern-matching output:
 *   cmp a, b; jcc target  ->  if (COND(a, b)) goto target;
 * ================================================================ */

/* Unsigned comparison conditions (from CMP a, b -> a - b) */
#define CMP_EQ(a, b)  ((uint32_t)(a) == (uint32_t)(b))
#define CMP_NE(a, b)  ((uint32_t)(a) != (uint32_t)(b))
#define CMP_B(a, b)   ((uint32_t)(a) <  (uint32_t)(b))   /* below (CF=1) */
#define CMP_AE(a, b)  ((uint32_t)(a) >= (uint32_t)(b))   /* above or equal */
#define CMP_BE(a, b)  ((uint32_t)(a) <= (uint32_t)(b))   /* below or equal */
#define CMP_A(a, b)   ((uint32_t)(a) >  (uint32_t)(b))   /* above */

/* Signed comparison conditions */
/* x86 evaluates the signed conditions at the operand width, not at 32 bits.
   The generated code passes LO8/HI8/LO16 sub-register reads straight in, and
   those zero-extend, so recover the width and sign-extend before comparing. */
#define RECOMP_FLAG_WIDTH(a, b) (sizeof(a) < sizeof(b) ? sizeof(a) : sizeof(b))
#define RECOMP_SIGNED(value, width) \
    ((width) == 1u ? (int32_t)(int8_t)(uint8_t)(uint32_t)(value) \
     : (width) == 2u ? (int32_t)(int16_t)(uint16_t)(uint32_t)(value) \
     : (int32_t)(uint32_t)(value))
#define CMP_L(a, b)   (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) <  \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))  /* less */
#define CMP_GE(a, b)  (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) >= \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))  /* >= */
#define CMP_LE(a, b)  (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) <= \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))  /* <= */
#define CMP_G(a, b)   (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) >  \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))  /* > */

/* TEST-based conditions (AND without storing result) */
#define TEST_Z(a, b)  (((uint32_t)(a) & (uint32_t)(b)) == 0)  /* ZF=1 */
#define TEST_NZ(a, b) (((uint32_t)(a) & (uint32_t)(b)) != 0)  /* ZF=0 */
#define TEST_S(a, b)  (RECOMP_SIGNED((uint32_t)(a) & (uint32_t)(b), \
                                     RECOMP_FLAG_WIDTH(a, b)) < 0)  /* SF=1 */

/* ================================================================
 * Arithmetic with carry/overflow detection
 * ================================================================ */

/** Add with carry flag. Returns result, sets *cf. */
static inline uint32_t ADD32_CF(uint32_t a, uint32_t b, int *cf) {
    uint32_t r = a + b;
    *cf = (r < a);
    return r;
}

/** Sub with carry (borrow) flag. Returns result, sets *cf. */
static inline uint32_t SUB32_CF(uint32_t a, uint32_t b, int *cf) {
    *cf = (a < b);
    return a - b;
}

/* ================================================================
 * Rotation / shift helpers
 * ================================================================ */

/* x86 masks the rotate count to 5 bits, and THEN the rotate is modulo the
 * operand's own width -- so `rol al, 16` is a rotate by zero and `rol ax, 31`
 * is a rotate by 15. A narrow rotate performed at 32 bits is not a rotate at
 * all: the bits that should wrap around at bit 7 or 15 land above the operand
 * and are discarded by the store, which turns `ror al, 2` on 0x01 into 0x00
 * where x86 gives 0x40.
 *
 * The zero case is separated out because `val >> (32 - 0)` is a shift of a
 * uint32_t by 32, which is undefined behaviour -- it happened to survive
 * because x86 masks shift counts to 5 bits and gives back `val`, but the
 * compiler is under no obligation to agree, least of all at -O2. */
static inline uint32_t ROL32(uint32_t val, int n) {
    n &= 31;
    return n ? ((val << n) | (val >> (32 - n))) : val;
}

static inline uint32_t ROR32(uint32_t val, int n) {
    n &= 31;
    return n ? ((val >> n) | (val << (32 - n))) : val;
}

/* rcl/rcr: rotate through the carry flag.
 *
 * The carry is a bit sitting one place above the operand's top bit, so the
 * rotation is over width+1 bits. That is also why a count is reduced modulo
 * width+1 for the 8- and 16-bit forms rather than modulo the width: a byte
 * rotates through nine positions, not eight. The 32-bit form takes the count
 * masked to five bits and no further, which is already inside 33.
 *
 * `cf` carries in and out.
 */
static inline uint32_t RC_ROT(uint32_t val, unsigned n, int *cf,
                              unsigned width, int left) {
    unsigned mod = width + 1u;
    uint64_t mask = (width >= 32u) ? 0xFFFFFFFFull
                                   : ((((uint64_t)1 << width) - 1u));
    uint64_t x = (((uint64_t)(*cf & 1)) << width) | ((uint64_t)val & mask);

    n &= 31u;
    if (width < 32u)
        n %= mod;
    if (n) {
        uint64_t full = (((uint64_t)1 << mod) - 1u);
        x = left ? ((x << n) | (x >> (mod - n)))
                 : ((x >> n) | (x << (mod - n)));
        x &= full;
    }
    *cf = (int)((x >> width) & 1);
    return (uint32_t)(x & mask);
}

static inline uint8_t ROL8(uint8_t val, int n) {
    n = (n & 31) % 8;
    return n ? (uint8_t)((val << n) | (val >> (8 - n))) : val;
}

static inline uint8_t ROR8(uint8_t val, int n) {
    n = (n & 31) % 8;
    return n ? (uint8_t)((val >> n) | (val << (8 - n))) : val;
}

static inline uint16_t ROL16(uint16_t val, int n) {
    n = (n & 31) % 16;
    return n ? (uint16_t)((val << n) | (val >> (16 - n))) : val;
}

static inline uint16_t ROR16(uint16_t val, int n) {
    n = (n & 31) % 16;
    return n ? (uint16_t)((val >> n) | (val << (16 - n))) : val;
}

/* ================================================================
 * Sign/zero extension
 * ================================================================ */

#define ZX8(v)   ((uint32_t)(uint8_t)(v))
#define ZX16(v)  ((uint32_t)(uint16_t)(v))
#define SX8(v)   ((uint32_t)(int32_t)(int8_t)(v))
#define SX16(v)  ((uint32_t)(int32_t)(int16_t)(v))

/**
 * x86 parity flag: 1 when the low byte of the result has an EVEN number of set
 * bits (that is what PF means). Used by the x87 float-compare idiom
 * `fnstsw ax; test ah, mask; jp/jnp`, which is how all pre-SSE code branches on
 * a float comparison. Without a real parity here the branch was hardcoded and
 * every such comparison went one fixed direction.
 */
static inline int recomp_parity8(uint32_t x) {
    x &= 0xFFu; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;
    return (int)(~x & 1u);   /* 1 = even parity (PF set) */
}
#define RECOMP_PARITY8(x) recomp_parity8((uint32_t)(x))

/* ================================================================
 * Byte swap (for endian conversion if needed)
 *
 * Xbox is little-endian like x86, so these are rarely needed,
 * but some games use bswap for network byte order or data parsing.
 * ================================================================ */

static inline uint32_t BSWAP32(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000u);
}

static inline uint16_t BSWAP16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

/* ================================================================
 * Indirect call dispatch
 *
 * The dispatch system resolves Xbox virtual addresses to native
 * function pointers at runtime. Three lookup sources are checked:
 *   1. Manual overrides (hand-written reimplementations)
 *   2. Generated dispatch table (auto-recompiled functions)
 *   3. Kernel thunk bridge (Xbox kernel function replacements)
 * ================================================================ */

/**
 * Generic function pointer type for all recompiled functions.
 * All translated functions are void(void) - arguments and return
 * values are passed through global registers and the simulated stack.
 */
#ifndef RECOMP_DISPATCH_H  /* avoid conflict with recomp_dispatch.h */
typedef void (*recomp_func_t)(void);

/**
 * Look up a recompiled function by its Xbox VA.
 * Returns NULL if the VA is not in the generated dispatch table.
 */
recomp_func_t recomp_lookup(uint32_t xbox_va);

/**
 * Build the flat, directly-indexed dispatch table.
 *
 * Turns recomp_lookup from a binary search over every translated function into
 * a bounds check and one indexed load -- the C form of Microsoft's
 * `jmp [table + guest_eip*8]`. Call it once at startup, before any recompiled
 * code runs.
 *
 * Entirely optional: if it is never called, or returns 0 because the allocation
 * failed, recomp_lookup keeps using the binary search and everything still
 * works. Costs 8 bytes per byte of guest code span (see
 * recomp_dispatch_flat_bytes), allocated with calloc so the untouched middle
 * stays uncommitted.
 *
 * Returns 1 if the flat table is in use, 0 if the search is.
 */
int recomp_dispatch_init(void);

/** Bytes held by the flat table, or 0 if it was never built. */
size_t recomp_dispatch_flat_bytes(void);

/**
 * Look up a kernel thunk function by its synthetic VA.
 * Kernel thunks live at 0xFE000000+ (synthetic addresses assigned
 * during kernel bridge initialization).
 * Returns NULL if the VA is not a kernel thunk.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

/**
 * Look up a manually overridden function by its Xbox VA.
 * Manual overrides take priority over generated code.
 * Returns NULL if no manual override exists for this VA.
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
#endif

/* TSFP call checks begin.
 * Diagnostic builds inspect the preserved registers and guest return stack.
 * The address and cleanup expressions stay lazy, including the historical
 * short-circuit behavior of a known-pop call. Production builds call directly.
 */
#ifdef RECOMP_ABI_CHECK
typedef struct {
    uint32_t base, source, destination, stack;
} tsfp_call_snapshot;

static inline tsfp_call_snapshot tsfp_capture_call(void) {
    tsfp_call_snapshot state;
    state.base = g_ebx;
    state.source = g_esi;
    state.destination = g_edi;
    state.stack = g_esp;
    return state;
}

static inline int tsfp_call_registers_match(const tsfp_call_snapshot *state) {
    return g_ebx == state->base && g_esi == state->source &&
           g_edi == state->destination;
}

void recomp_abi_violation_log(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
void recomp_abi_pop_violation_log(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
#define RECOMP_ABI_CALL(address, target) do { \
    const tsfp_call_snapshot tsfp_before_call_ = tsfp_capture_call(); \
    (target)(); \
    if (!tsfp_call_registers_match(&tsfp_before_call_) || \
        g_esp < tsfp_before_call_.stack + 4u) \
        recomp_abi_violation_log((address), tsfp_before_call_.base, \
            tsfp_before_call_.source, tsfp_before_call_.destination, tsfp_before_call_.stack); \
} while (0)
#define RECOMP_ABI_CALL_POP(address, target, cleanup) do { \
    const tsfp_call_snapshot tsfp_before_call_ = tsfp_capture_call(); \
    (target)(); \
    if (!tsfp_call_registers_match(&tsfp_before_call_) || \
        g_esp != tsfp_before_call_.stack + 4u + (cleanup)) \
        recomp_abi_pop_violation_log((address), tsfp_before_call_.base, \
            tsfp_before_call_.source, tsfp_before_call_.destination, \
            tsfp_before_call_.stack, (cleanup)); \
} while (0)
#else
#define RECOMP_ABI_CALL(address, target) (target)()
#define RECOMP_ABI_CALL_POP(address, target, cleanup) (target)()
#endif
/* TSFP call checks end. */


/* A guarded arm of an indirect call. When a recorded run saw a call site
 * reach only a few translated functions, the lifter compares the target
 * against each and calls the match directly (RECOMP_ABI_CALL), falling back
 * to RECOMP_ICALL_SAFE_AT for anything else -- so an unseen target is slow,
 * never wrong. Under RECOMP_ICALL_FEEDBACK the arms are counted, so a run
 * can say how often the guards held; otherwise they cost nothing. */
#ifdef RECOMP_ICALL_FEEDBACK
extern volatile uint64_t g_icall_guard_hits;
extern volatile uint64_t g_icall_guard_misses;
#define RECOMP_ICALL_GUARD_HIT()  ((void)g_icall_guard_hits++)
#define RECOMP_ICALL_GUARD_MISS() ((void)g_icall_guard_misses++)
#else
#define RECOMP_ICALL_GUARD_HIT()  ((void)0)
#define RECOMP_ICALL_GUARD_MISS() ((void)0)
#endif

/* TSFP indirect dispatch support begin.
 * Call sites supply a guest return address before entering this interface.
 * Keep lookup/notification order in one place; recovery stays at the call
 * site so saved-stack and feedback expressions retain their lazy evaluation.
 */
/* The generated table defines RECOMP_DISPATCH_H before including its types
 * header, then declares the public pointer type later. These compatible private
 * declarations keep inline helpers valid in either include order. */
typedef void (*tsfp_guest_entry_t)(void);
tsfp_guest_entry_t recomp_lookup_manual(uint32_t target);
tsfp_guest_entry_t recomp_lookup(uint32_t target);
tsfp_guest_entry_t recomp_lookup_kernel(uint32_t target);

static inline int tsfp_record_guest_call(uint32_t target) {
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE - 1u)] = target;
    ++g_icall_trace_idx;
    ++g_icall_count;
    return RECOMP_ICALL_IS_CODE(target);
}

static inline tsfp_guest_entry_t tsfp_guest_target(uint32_t target) {
    tsfp_guest_entry_t resolved = recomp_lookup_manual(target);
    if (resolved) return resolved;
    resolved = recomp_lookup(target);
    return resolved ? resolved : recomp_lookup_kernel(target);
}

/* Diagnostics inspect the original call frame. They may also change guest
 * state, so failure recovery must run after the diagnostic returns. */
static inline int tsfp_invoke_guest_target(uint32_t target, uint32_t form) {
    tsfp_guest_entry_t resolved = tsfp_guest_target(target);
    if (!resolved) {
        RECOMP_ICALL_OBSERVE(target, RECOMP_ICALL_SEEN_UNRESOLVED);
        g_icall_saved_esp = g_esp;
        g_icall_dispatch_form = form;
        recomp_icall_fail_log(target);
        return 0;
    }
    RECOMP_ICALL_OBSERVE(target, RECOMP_ICALL_SEEN_RESOLVED);
    RECOMP_ABI_CALL(target, resolved);
    return 1;
}

#define TSFP_GUEST_CALL(target, report_stack, recover, observe_site) do { \
    const uint32_t tsfp_call_target_ = (uint32_t)(target); \
    if (tsfp_record_guest_call(tsfp_call_target_)) { \
        observe_site; \
        if (!tsfp_invoke_guest_target(tsfp_call_target_, 1u)) { \
            recover; \
            eax = 0; \
        } \
    } else { \
        recomp_icall_not_code_log(tsfp_call_target_, (report_stack)); \
        recover; \
        eax = 0; \
    } \
} while (0)
/* TSFP indirect dispatch support end. */

#define RECOMP_ICALL(xbox_va) \
    TSFP_GUEST_CALL(xbox_va, g_esp + 4u, g_esp += 4u, (void)0)


/* TSFP HLE frames begin.
 * Register-argument thunks stage at most 24 words in ordinary guest stack
 * memory. Keep the caller's return address and entry stack separate from any
 * register changes made while evaluating cleanup, selecting or calling HLE.
 */
#define RECOMP_HLE_MAX_ARGS 24u
typedef struct {
    uint32_t count, return_address, arguments, entry, work, cleanup;
} tsfp_hle_frame;

static inline tsfp_hle_frame tsfp_begin_hle_frame(uint32_t count) {
    tsfp_hle_frame frame;
    frame.count = count;
    frame.return_address = MEM32(g_esp);
    frame.arguments = g_esp + 4u;
    frame.entry = g_esp;
    frame.work = g_esp - 4u * (count + 4u);
    frame.cleanup = 0;
    return frame;
}

static inline void tsfp_run_hle_frame(const tsfp_hle_frame *frame,
                                      const uint32_t *arguments, void (*target)(void)) {
    uint32_t index = 0;
    MEM32(frame->work) = frame->return_address;
    while (index < frame->count && index < RECOMP_HLE_MAX_ARGS) {
        MEM32(frame->work + 4u * (index + 1u)) = arguments[index];
        ++index;
    }
    g_esp = frame->work;
    target();
    g_esp = frame->entry + frame->cleanup;
}

#define RECOMP_HLE_STACK(index) MEM32(tsfp_hle_frame_.arguments + 4u * (uint32_t)(index))
#define RECOMP_HLE_LTCG_CALL(n, fn, total_pop) { \
    tsfp_hle_frame tsfp_hle_frame_ = tsfp_begin_hle_frame((uint32_t)(n)); \
    tsfp_hle_frame_.cleanup = (uint32_t)(total_pop); \
    uint32_t a[RECOMP_HLE_MAX_ARGS]; \
    void (*tsfp_hle_target_)(void) = (fn);
#define RECOMP_HLE_LTCG_END \
    tsfp_run_hle_frame(&tsfp_hle_frame_, a, tsfp_hle_target_); \
}
/* TSFP HLE frames end. */


/* TSFP indirect dispatch wrappers begin.
 * A saved stack is consulted only for failure. For an invalid address it is
 * consulted before the diagnostic and again afterwards, as in the public ABI.
 * Tail jumps reuse the existing frame and do not enter the call trace ring.
 */
#define RECOMP_ICALL_SAFE(xbox_va, saved_esp) \
    TSFP_GUEST_CALL(xbox_va, saved_esp, g_esp = (saved_esp), (void)0)
#define RECOMP_ICALL_SAFE_AT(xbox_va, saved_esp, site) \
    TSFP_GUEST_CALL(xbox_va, saved_esp, g_esp = (saved_esp), \
        RECOMP_ICALL_OBSERVE_SITE((site), tsfp_call_target_))
#define RECOMP_ITAIL(xbox_va) do { \
    const uint32_t tsfp_tail_target_ = (uint32_t)(xbox_va); \
    if (!tsfp_invoke_guest_target(tsfp_tail_target_, 2u)) { \
        g_esp += 4u; \
        g_eax = 0; \
    } \
} while (0)
/* TSFP indirect dispatch wrappers end. */


/* ================================================================
 * Register name aliases for generated code
 *
 * Map x86 volatile register names to global variables.
 * These #defines allow the generated code to use natural register
 * names (eax, ecx, edx, esp) which the preprocessor maps to the
 * corresponding globals (g_eax, g_ecx, g_edx, g_esp).
 *
 * Only active when RECOMP_GENERATED_CODE is defined (in generated
 * .c files) to avoid polluting hand-written code.
 * ================================================================ */

/* New generator files declare this protocol before including the header.
 * Legacy/manual sources keep direct global aliases. References point at the
 * same thread-local cells: no register values are copied, delayed or flushed. */
#ifndef RECOMP_GPR_ANCHORS
#define RECOMP_GPR_ANCHORS(site) 0
#endif
#if defined(RECOMP_GENERATED_CODE) && defined(RECOMP_GPR_REFS_SUPPORTED) && \
    defined(RECOMP_USE_GPR_REFS) && RECOMP_USE_GPR_REFS
#define RECOMP_DECLARE_GPRS(site) \
    const int _recomp_use_refs = !!RECOMP_GPR_ANCHORS(site); \
    uint32_t *const _recomp_eax=&g_eax; (void)_recomp_eax; \
    uint32_t *const _recomp_ecx=&g_ecx; (void)_recomp_ecx; \
    uint32_t *const _recomp_edx=&g_edx; (void)_recomp_edx; \
    uint32_t *const _recomp_esp=&g_esp; (void)_recomp_esp; \
    uint32_t *const _recomp_ebx=&g_ebx; (void)_recomp_ebx; \
    uint32_t *const _recomp_esi=&g_esi; (void)_recomp_esi; \
    uint32_t *const _recomp_edi=&g_edi; (void)_recomp_edi; \
    RecompXmm *const _recomp_xmm0=&g_xmm0; (void)_recomp_xmm0; \
    RecompXmm *const _recomp_xmm1=&g_xmm1; (void)_recomp_xmm1; \
    RecompXmm *const _recomp_xmm2=&g_xmm2; (void)_recomp_xmm2; \
    RecompXmm *const _recomp_xmm3=&g_xmm3; (void)_recomp_xmm3; \
    RecompXmm *const _recomp_xmm4=&g_xmm4; (void)_recomp_xmm4; \
    RecompXmm *const _recomp_xmm5=&g_xmm5; (void)_recomp_xmm5; \
    RecompXmm *const _recomp_xmm6=&g_xmm6; (void)_recomp_xmm6; \
    RecompXmm *const _recomp_xmm7=&g_xmm7; (void)_recomp_xmm7; \
    (void)_recomp_use_refs
#define eax (*(_recomp_use_refs ? _recomp_eax : &g_eax))
#define ecx (*(_recomp_use_refs ? _recomp_ecx : &g_ecx))
#define edx (*(_recomp_use_refs ? _recomp_edx : &g_edx))
#define esp (*(_recomp_use_refs ? _recomp_esp : &g_esp))
#define ebx (*(_recomp_use_refs ? _recomp_ebx : &g_ebx))
#define esi (*(_recomp_use_refs ? _recomp_esi : &g_esi))
#define edi (*(_recomp_use_refs ? _recomp_edi : &g_edi))
#else
#define RECOMP_DECLARE_GPRS(site) ((void)0)
#ifdef RECOMP_GENERATED_CODE
#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define esp g_esp
#define ebx g_ebx
#define esi g_esi
#define edi g_edi
#endif
#endif

#ifdef RECOMP_GENERATED_CODE

#define xmm0 g_xmm0
#define xmm1 g_xmm1
#define xmm2 g_xmm2
#define xmm3 g_xmm3
#define xmm4 g_xmm4
#define xmm5 g_xmm5
#define xmm6 g_xmm6
#define xmm7 g_xmm7
#define mm0 g_mm0
#define mm1 g_mm1
#define mm2 g_mm2
#define mm3 g_mm3
#define mm4 g_mm4
#define mm5 g_mm5
#define mm6 g_mm6
#define mm7 g_mm7
/* ebp is NOT global - it's local in each function.
 * For __SEH_prolog/epilog, use g_seh_ebp to bridge. */
#endif

/* ================================================================
 * Forward declarations for translated functions
 *
 * These are generated by the recompiler and included per-file.
 * The recomp_funcs.h header (generated) declares all translated
 * function prototypes.
 * ================================================================ */

/* An instruction the lifter has no translation for.
 *
 * The lifter used to emit a bare "TODO: mnemonic" C comment at such a site,
 * and a comment is a no-op: the instruction vanished, the guest carried
 * on, and nothing at runtime could say the site had been reached. Wreckless
 * died inside RtlAllocateHeap because `bsf` was a comment; Half-Life 2 painted
 * its intro red because `cvtpi2ps` was. Each was found by working backwards
 * from a subsystem failure that had nothing to do with the cause.
 *
 * Now the site calls this, with the instruction text and its guest address.
 * The instruction is STILL a no-op -- this changes no behaviour -- but the run
 * names the untranslated sites it reached (the first fifty hits, in
 * src/kernel/recomp_trace.c), and with RECOMP_UNIMPL_TRAP=1 it stops at the
 * first one. The lifter
 * keeps the comment beside the call so `grep TODO:` over a gen tree still
 * enumerates every site whether or not it is ever reached. */
void recomp_unimpl(const char *text, uint32_t va);
#define RECOMP_UNIMPL(_text, _va) recomp_unimpl((_text), (_va))

#endif /* RECOMP_TYPES_H */

/* Intentionally outside the type guard: generated function prefixes reinclude
 * this header with a site. Unselected bodies retain their direct aliases before
 * optimization, avoiding conditional vector-expression growth in large code. */
#if defined(RECOMP_GENERATED_CODE) && defined(RECOMP_REGISTER_SITE)
#undef xmm0
#undef xmm1
#undef xmm2
#undef xmm3
#undef xmm4
#undef xmm5
#undef xmm6
#undef xmm7
#if defined(RECOMP_GPR_REFS_SUPPORTED) && defined(RECOMP_USE_GPR_REFS) && RECOMP_USE_GPR_REFS && RECOMP_GPR_ANCHORS(RECOMP_REGISTER_SITE)
#define xmm0 (*_recomp_xmm0)
#define xmm1 (*_recomp_xmm1)
#define xmm2 (*_recomp_xmm2)
#define xmm3 (*_recomp_xmm3)
#define xmm4 (*_recomp_xmm4)
#define xmm5 (*_recomp_xmm5)
#define xmm6 (*_recomp_xmm6)
#define xmm7 (*_recomp_xmm7)
#else
#define xmm0 g_xmm0
#define xmm1 g_xmm1
#define xmm2 g_xmm2
#define xmm3 g_xmm3
#define xmm4 g_xmm4
#define xmm5 g_xmm5
#define xmm6 g_xmm6
#define xmm7 g_xmm7
#endif
#undef RECOMP_REGISTER_SITE
#endif
