/* MPUSHIM 0.4 - the WHOLE MPU-401 (UART mode) facade in one binary: an
 * MPU-401 at 330h over a plain serial UART (the EXP GAME/MIDI G3's hidden
 * UART at 250h, or a COM port + an MPU-232 dongle), for BOTH worlds:
 *
 *  - V86 (real-mode) games - SCUMM/iMUSE, Sierra, Miles: a 199-byte
 *    real-mode core (MPUSHIMR.ASM, embedded as a blob) is planted in a
 *    small DOS block of its own and registered with the QPI port-trap
 *    host (Jemm's QPIEMU or QEMM);
 *  - 32-bit DPMI/DOS4GW games - the General MIDI era: FAR32 handlers
 *    registered through HDPMI32i's documented I/O-trap API.
 *
 * ...and, on a Pentium or better, through EITHER of those or through a
 * third arrangement that replaces both:
 *
 *  - VDPMI, crazii's DPMI host with its own V86 monitor, traps ring-3 and
 *    V86 I/O in ONE table, so a single registration there covers the whole
 *    catalogue with one resident host instead of three (JEMM386 + QPIEMU +
 *    HDPMI32i).  It is detected first and used alone; /NOVD ignores it and
 *    goes back to the pair.  VDPMI executes RDTSC and RDMSR unguarded, so
 *    it is Pentium-only - the 386/486 fleet keeps the QPI + HDPMI stack,
 *    which runs unchanged on a Pentium as well.
 *
 * Any one host is enough; the sides install and report independently (a
 * PM-only boot just notes there is no QPI host, and vice versa).  All
 * three facades behave identically: correct MPU-401 UART-mode status
 * (always write-ready), the 0FEh reset/UART-mode ACK, All-Notes-Off
 * broadcast on reset, and MIDI data paced onto the real UART, never
 * dropped.  This supersedes the separate MPUSHIM.COM + MPUSHIMP.EXE pair
 * (the standalone .COM remains in the repo for QPI-only stacks without any
 * DPMI host).
 *
 *   MPUSHIM [/UART=250] [/MPU=330] [/DIV=n] [/NORM] [/NOPM] [/NOVD] [/NOCLI]
 *
 * It installs its traps and stays resident; games are then started
 * NORMALLY.  Prereqs: VDPMI resident, or HDPMI32i (-r -x) for the PM side
 * and a QPI host for the V86 side; and the UART already brought up by its
 * card enabler (a COM port needs no enabler).
 * Build: ./build-pm.sh (nasm + DJGPP).  Remove: reboot.
 *
 * Copyright (C) 2026 zikolas.  GNU General Public License v2 (see
 * COPYING).  This program is free software; you can redistribute it
 * and/or modify it under the terms of the GPL as published by the Free
 * Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  It is distributed WITHOUT ANY WARRANTY.
 *
 * TWO HOST-INTERACTION BUGS, found 2026-08-22 by reading the HDPMI source
 * (HX repo, public), explain why every DJGPP test client passed while both
 * DOS/4GW games failed:
 *
 * 1) "Duke3D dies with DOS/4GW error 2001, exception 07h".  DJGPP's CRT
 *    (_npxsetup) calls DPMI 0E01h with EMv|MPv on a machine with no FPU.
 *    HDPMI implements 0E01h as a raw LMSW: it edits the REAL CR0.EM/MP,
 *    machine-globally, and never restores it when a client exits or TSRs.
 *    So ANY DJGPP program leaves CR0.EM=1 behind on the FPU-less 486SX;
 *    DOS/4GW's real-mode FPU probe then faults (EM=1 makes ESC opcodes
 *    trap in real mode too), it misdetects, never arms its TSF32 emulator,
 *    and the game's first FPU instruction is an unhandled exception 07h.
 *    FIX: __dpmi_set_coprocessor_emulation(0) at startup - repairs the
 *    machine no matter how we exit.  vsbhda does the same before its TSR
 *    (and it was written by HDPMI's own author).
 *
 * 2) "DOOM wedges seconds into MPU music, CAPS LOCK dead".  HDPMI32i runs
 *    clients at IOPL 0 and emulates CLI by clearing the REAL IF.  STI and
 *    interrupt-handler exit are healed by the host, but the Watcom/DOS4G
 *    critical-section idiom PUSHFD / POP EAX / CLI ... PUSH EAX / POPFD is
 *    not healable there: at IOPL 0 a ring-3 POPFD silently drops the IF
 *    bit, so interrupts stay off forever - a genuinely dead machine, which
 *    is exactly the observed wedge.  DMX only runs those sections when MPU
 *    music is pumping, which is why DOOM was stable with music off.
 *    FIX: register a CLI trap handler via HDPMI's documented vendor fn 9;
 *    when the CLI immediately follows a PUSHFD (or PUSHFD/POP EAX), the
 *    critical section will be exited with POPFD, so re-enable interrupts
 *    (DPMI 0901h) instead of letting them latch off.  /NOCLI disables it.
 *    That fix is taken from VSBHDA - see the attribution at the handler.
 *
 * Provenance.  The HDPMI port-trap ABI is from Baron-von-Riedesel's
 * published HDPMIAPI.TXT (AX=168Ah "HDPMI" -> API entry; fn 5 context mode,
 * fn 6 install {in,out FARPROCs}, fn 7 remove, fn 9 CLI/STI trap; the
 * error-code bit layout; the handler is called "like an exception handler
 * proc" and MUST advance the faulting EIP).  HDPMI itself is freeware "for
 * any purpose", (C) Japheth.  The MPU-401 UART-mode and 16550 register
 * models are published hardware standards.
 *
 * This program is GPL v2 because two pieces of it are derived from GPL v2
 * sources, each attributed at the function it belongs to:
 * mpushim_cli_handler() follows VSBHDA's _hdpmi_CliHandler
 * (src/stackio.asm), (C) Baron-von-Riedesel, and mpushim_vd_handler()
 * follows the trap-handler contract that SBEMU's client driver for VDPMI
 * (vdpmi.c, (C) crazii) documents by implementing it - VDPMI's own
 * vendor API is not published anywhere else.
 */

#include <dpmi.h>
#include <go32.h>
#include <crt0.h>
#include <pc.h>
#include <dos.h>
#include <unistd.h>
#include <string.h>
#include <sys/farptr.h>
#include <sys/movedata.h>
#include <sys/segments.h>
#include <sys/exceptn.h>
#include <sys/nearptr.h>   /* __djgpp_base_address: our offsets -> linear */

/* The real-mode (V86) resident core, assembled flat from MPUSHIMR.ASM and
 * embedded here (build-pm.sh: nasm -> xxd -i).  At install it is copied
 * into a small DOS-memory block of its own (this client's, so it survives
 * the TSR), patched, and registered with the QPI port-trap host, so ONE
 * binary covers both worlds: V86 (real-mode) games through QPI and ring-3
 * DPMI games through HDPMI.  Each world runs its own facade state, exactly
 * like the previously separate .COM + .EXE pair proved out. */
#include "mpushimr.h"
#define RBLOB_UART   4    /* state-header offsets, must match MPUSHIMR.ASM */
#define RBLOB_DATA   6
#define RBLOB_STAT   8
#define RBLOB_SYNTH 14    /* MIDI sink: 0 = the UART, else the INT 2Fh AX  */
#define RBLOB_LSR   16    /* LSR offset from the UART base: 5, or 10 at stride 2 */
#define RBLOB_ENTRY  18   /* QPI handler entry offset inside the blob */

/* Lock every page of this program at startup and keep sbrk from moving the
 * image.  The trap handler can be entered at any time - including while
 * another DPMI client owns the machine - so none of it may ever be absent. */
int _crt0_startup_flags = _CRT0_FLAG_LOCK_MEMORY | _CRT0_FLAG_NONMOVE_SBRK;

/* ---- facade state, shared with the asm handlers ------------------------ */
volatile unsigned short g_uart = 0x250;  /* serial UART base I/O port       */
volatile unsigned short g_lsr  = 0x255;  /* its LSR: g_uart + (5 << stride
                                          * shift) - 10 for a 16550 with
                                          * registers 2 apart (/STRIDE=2,
                                          * the TDK MC-8000's DIN)        */
volatile unsigned short g_data = 0x330;  /* MPU data port                   */
volatile unsigned short g_stat = 0x331;  /* MPU status/command port         */
/* MIDI sink: 0 = the UART, else the AX handed to INT 2Fh - AH a resident
 * software synth's multiplex id, AL 01h its "byte in DL" function
 * (/SYNTH).  The V86 blob calls the synth natively; the PM facade reaches
 * the SAME real-mode synth through DPMI 0300h (synth_rm_byte below), so
 * one resident synth TSR serves every world and this binary carries no
 * synth engine of its own. */
unsigned short g_synth = 0;
volatile unsigned char  g_ack  = 0;      /* 1 = 0FEh ACK waiting to be read */
volatile unsigned char  g_busy = 0;      /* 1 = a handler is already active */
/* Diagnostics (v0.4, for the Tyrian stall hunt - see the trace notes below) */
volatile unsigned char  g_ackall = 0;    /* 1 = ACK every command, not just FF/3F */
volatile unsigned char  g_trace  = 0;    /* 1 = record traffic into the DOS block */
volatile unsigned short g_dosds  = 0;    /* a base-0 selector, to reach that block */
volatile unsigned long  g_tr_lin = 0;    /* its linear address                     */
volatile unsigned long  g_tr_idx = 0;    /* next entry to write (ring of 1024)     */
volatile unsigned char  g_nofifo = 0;    /* 1 = the old drop-on-nesting behaviour  */
volatile unsigned char  g_tr1    = 0;    /* 1 = stop tracing once the ring fills   */
/* The transmit queue.  256 bytes, byte indices, so the ring wraps by itself. */
volatile unsigned char  g_fifo[256];
volatile unsigned char  g_fhead = 0;
volatile unsigned char  g_ftail = 0;
volatile unsigned char  g_drain = 0;     /* 1 = somebody is already draining it */

/* /SYNTH, PM side: hand one MIDI byte to the resident real-mode INT 2Fh
 * synth through DPMI 0300h - the reflection MPUSHM16 0.2 proved safe from
 * inside a trap handler (precedent for DPMI calls in that context: the
 * CLI-heal's own int 31h fn 0901h).  The static regs block is safe
 * because HDPMI dispatches handlers with the real IF clear, so this can
 * never be re-entered - and /SYNTH refuses VDPMI, which could preempt. */
static __dpmi_regs synth_regs;
void __attribute__((used)) synth_rm_byte(unsigned b)
{
    synth_regs.x.ax = g_synth;
    synth_regs.x.dx = (unsigned short)(b & 0xFF);
    synth_regs.x.flags = 0;
    synth_regs.x.es = synth_regs.x.ds = 0;
    synth_regs.x.ss = synth_regs.x.sp = 0;
    __dpmi_simulate_real_mode_interrupt(0x2F, &synth_regs);
}

extern unsigned short   g_ds_st;         /* our DS - lives IN .text (see below) */
extern unsigned long    g_vd_old;        /* VDPMI: the handler before us,  */
extern unsigned short   g_vd_old_sel;    /*        offset + selector       */
extern void mpushim_out_handler(void);
extern void mpushim_in_handler(void);
extern void mpushim_cli_handler(void);
extern void mpushim_vd_handler(void);
extern char mpushim_code_end[];          /* just past the last handler */

/* ---- the HDPMI trap handlers ------------------------------------------
 *
 * Entered by a far call, exception-handler style, with the client's
 * registers live and this frame (after "push ebp; mov ebp,esp"):
 *     [ebp+0x04] return EIP     [ebp+0x08] return CS
 *     [ebp+0x0C] error code     [ebp+0x10] faulting EIP  <- must advance
 *     [ebp+0x14] faulting CS    [ebp+0x18] EFLAGS
 * (Verified against the host source: HDPMI parks the handler in a pseudo
 * exception vector 20h and dispatches it through its normal DPMI-0.9
 * exception path, on its 4K locked stack - or on the interrupted stack
 * when nested - with real IF cleared.  A 32-bit RETF with everything
 * popped, ESP back at the error code, resumes the client from the frame's
 * EIP/CS/EFLAGS with the live registers.)
 * Error code: bits 0-2 instruction size, bit 3 string I/O, bits 4-5 operand
 * size, bit 6 "port is in error-code bits 8-15" (else in the client's DX).
 * A byte value for OUT arrives in the client's AL; an IN returns its byte by
 * leaving it in AL at the far return, so we patch the saved EAX slot.
 *
 * Saved-register frame: [ebp-4] EAX  [ebp-8] EBX  [ebp-12] ECX
 *                       [ebp-16] EDX [ebp-20] DS
 *
 * Rules learned on hardware:
 *  - ADVANCE THE FAULTING EIP ON EVERY PATH, including the bail-outs; if it
 *    is not advanced the same I/O instruction traps again forever.
 *  - NEVER DROP MIDI BYTES.  The first build polled THRE once and dropped
 *    the byte if the transmitter was busy, on a "games re-send" theory.
 *    They do not: MIDI messages are 2-3 bytes written back-to-back at CPU
 *    speed against a 320us-per-byte wire, so that policy forwarded the
 *    first byte of each message and ate the rest - fragments the synth
 *    ignores, stuck notes, a jammed receiver (Duke3D SETUP bench,
 *    2026-08-22).  Fix: the transmit path waits for THRE with a bound of
 *    several byte-times and drops only if the UART is genuinely dead.
 *    The wait is safe: HDPMI dispatches this handler with real IF=0, so
 *    nothing can preempt it - the bound exists only so absent hardware
 *    cannot hang the machine.  FIFOs are forced OFF at install so THRE
 *    means "room for one byte", not "all 16 slots empty".
 *  - STATUS must always say WRITE-READY.  An interim build reflected the
 *    UART THR state into DRR (bit 6) "like real hardware"; DOOM promptly
 *    grew stuck notes - DMX-class drivers poll status with short
 *    timeouts and skip bytes that stay busy for a wire-length 320us, and
 *    a skipped note-off drones forever (bench 2026-08-22).  SoftMPU and
 *    vsbhda's VMPU report instantly-ready for the same reason.  Pacing
 *    lives in the transmit path only.
 *  - String I/O (bit 3) is not modelled; absorb it rather than corrupt the
 *    client's ESI/EDI/ECX.
 *  - DS belongs to the client on entry.  Our own DS is kept as a word
 *    inside .text and fetched with a CS override - the vsbhda pattern -
 *    so the read never depends on the CS limit covering .data. */
asm(
"   .text                                                          \n"
"   .intel_syntax noprefix                                         \n"
"   .globl _mpushim_out_handler                                    \n"
"   .globl _mpushim_in_handler                                     \n"
"   .globl _mpushim_cli_handler                                    \n"
"   .globl _g_ds_st                                                \n"
"_g_ds_st:                                                         \n"
"   .word 0                                                        \n"
   /* The port-trap handler VDPMI had before us: ports that are not ours
    * are passed down this chain, so several drivers can share the host's
    * one global handler slot (SBEMU for the sound card, us for MIDI).  A
    * 16:32 far pointer, selector 0 meaning nobody was there.  It lives in
    * .text next to g_ds_st so the far jump reaches it through CS. */
"   .balign 4                                                      \n"
"   .globl _g_vd_old                                               \n"
"   .globl _g_vd_old_sel                                           \n"
"_g_vd_old:                                                        \n"
"   .long 0                                                        \n"
"_g_vd_old_sel:                                                    \n"
"   .word 0                                                        \n"
"   .balign 4                                                      \n"
"                                                                  \n"
/* --------- OUT: the client wrote a byte to a trapped MPU port ---------- */
"_mpushim_out_handler:                                             \n"
"   push ebp                                                       \n"
"   mov  ebp, esp                                                  \n"
"   push eax                                                       \n"
"   push ebx                                                       \n"
"   push ecx                                                       \n"
"   push edx                                                       \n"
"   push ds                                                        \n"
"   mov  eax, [ebp+0x0C]       # error code                        \n"
"   mov  ebx, eax                                                  \n"
"   and  ebx, 7                # instruction size                  \n"
"   add  [ebp+0x10], ebx       # advance faulting EIP (every path)  \n"
"   mov  bx, cs:[_g_ds_st]                                          \n"
"   mov  ds, bx                                                    \n"
"   test al, 0x08              # string I/O? absorb it              \n"
"   jnz  o_exit                                                    \n"
"   cmp  byte ptr [_g_busy], 0                                      \n"
"   jne  o_exit                # re-entrant: drop this byte         \n"
"   mov  byte ptr [_g_busy], 1                                      \n"
"   test al, 0x40              # port in the error code?            \n"
"   jz   o_dxport                                                   \n"
"   movzx edx, ah                                                   \n"
"   jmp  o_haveport                                                 \n"
"o_dxport:                                                          \n"
"   mov  edx, [ebp-16]         # the client's DX                    \n"
"   and  edx, 0xFFFF                                                \n"
"o_haveport:                                                        \n"
"   mov  eax, [ebp-4]          # the client's EAX (value in AL)     \n"
"   mov  cx, [_g_stat]                                              \n"
"   cmp  dx, cx                                                     \n"
"   je   o_cmd                                                      \n"
"   mov  cx, [_g_data]         # must be OUR data port, else absorb \n"
"   cmp  dx, cx                                                     \n"
"   je   o_data                                                     \n"
"   jmp  o_clear                                                    \n"
"o_cmd:                                                             \n"
"   cmp  al, 0xFF              # reset: notes-off bcast + ACK       \n"
"   je   o_reset                                                    \n"
"   cmp  al, 0x3F              # enter UART mode -> queue the ACK   \n"
"   je   o_ack                                                      \n"
"   jmp  o_clear               # any other command: absorb          \n"
/* RESET does what the real chip does: broadcast All Notes Off (CC 123 on
 * every channel).  Real MPU-401 silicon transmits this when it receives
 * FFh and SoftMPU emulates it, so it stays - but a 2026-08-22 bench
 * regression showed it is NOT load-bearing for DOOM/DMX: DMX sends its
 * own note-offs at song stop, GM gear (Yamaha QY70) honours them, and
 * the CM-32L's droning note across DOOM's track changes persists WITH
 * the broadcast because LA-era Rolands ignore this style of silencing
 * altogether.  Curing that would take SoftMPU's approach - track active
 * notes and send per-note offs on reset (v0.3 candidate).  48 bytes at
 * wire speed is ~15ms inside this one trap, fine for an event this rare. */
"o_reset:                                                           \n"
"   push ebx                   # o_tx eats BL; preserved across it  \n"
"   mov  bh, 0xB0                                                   \n"
"o_rst1:                                                            \n"
"   mov  bl, bh                # Bn: control change, channel n      \n"
"   call o_tx                                                       \n"
"   mov  bl, 0x7B              # CC 123: all notes off              \n"
"   call o_tx                                                       \n"
"   mov  bl, 0x00                                                   \n"
"   call o_tx                                                       \n"
"   inc  bh                                                         \n"
"   cmp  bh, 0xC0                                                   \n"
"   jne  o_rst1                                                     \n"
"   pop  ebx                                                        \n"
"o_ack:                                                             \n"
"   mov  byte ptr [_g_ack], 1                                       \n"
"   jmp  o_clear                                                    \n"
"o_data:                                                            \n"
"   mov  bl, al                                                     \n"
"   call o_tx                                                       \n"
"   mov  ecx, [ebp+0x0C]       # word/dword OUT? send the rest too  \n"
"   test cl, 0x30                                                   \n"
"   jz   o_clear                                                    \n"
"   mov  eax, [ebp-4]                                               \n"
"   mov  bl, ah                                                     \n"
"   call o_tx                                                       \n"
"o_clear:                                                           \n"
"   mov  byte ptr [_g_busy], 0                                      \n"
"o_exit:                                                            \n"
"   pop  ds                                                         \n"
"   pop  edx                                                        \n"
"   pop  ecx                                                        \n"
"   pop  ebx                                                        \n"
"   pop  eax                                                        \n"
"   pop  ebp                                                        \n"
"   retf                                                            \n"
"                                                                   \n"
/* BL -> the UART transmitter, through a 256-byte queue.
 *
 * The queue exists because under VDPMI a trap handler CAN be preempted -
 * unlike HDPMI, which dispatches with the real IF clear.  A game whose music
 * ISR writes MIDI while its main thread also touches the MPU then re-enters
 * this code mid-byte.  The first design answered that with a reentrancy flag
 * that made the nested call give up, which breaks the project's oldest law:
 * NEVER DROP A MIDI BYTE (2026-08-22, Tyrian).  So now every byte is queued
 * and the FIRST caller in drains the whole queue; a nested caller appends
 * and returns immediately.  Order is preserved, nothing is lost, and the
 * inner caller is never made to wait.
 *
 * The drain waits for THRE with a ~1.5-byte-time bound and drops a byte only
 * if the UART is genuinely dead - the bound guards absent hardware, nothing
 * else.  /NOFIFO restores the old behaviour for A/B tests.               */
"o_tx:                                                              \n"
/* /SYNTH: the byte goes to the resident RM synth via DPMI 0300h, not a
 * UART.  DS is ours (every caller set it); give the C code ES=DS too and
 * preserve the rest.  Bypasses g_fifo - under HDPMI the handler cannot
 * be preempted, and /SYNTH refuses VDPMI at install.                  */
"   cmp  word ptr [_g_synth], 0                                     \n"
"   je   o_tx_hw                                                    \n"
"   pushad                                                          \n"
"   push es                                                         \n"
"   push ds                                                         \n"
"   pop  es                                                         \n"
"   movzx eax, bl                                                   \n"
"   push eax                                                        \n"
"   call _synth_rm_byte                                             \n"
"   add  esp, 4                                                     \n"
"   pop  es                                                         \n"
"   popad                                                           \n"
"   ret                                                             \n"
"o_tx_hw:                                                           \n"
"   push eax                                                        \n"
"   push ebx                                                        \n"
"   push ecx                                                        \n"
"   push edx                                                        \n"
"   cmp  byte ptr [_g_nofifo], 0                                    \n"
"   jne  o_tx_direct                                                \n"
"   movzx eax, byte ptr [_g_fhead]                                  \n"
"   mov  cl, al                                                     \n"
"   inc  cl                                                         \n"
"   cmp  cl, [_g_ftail]        # queue full? (256 bytes: never seen)\n"
"   je   o_tx_ret                                                   \n"
"   mov  [_g_fifo+eax], bl                                          \n"
"   mov  [_g_fhead], cl                                             \n"
"   cmp  byte ptr [_g_drain], 0                                     \n"
"   jne  o_tx_ret              # nested: the outer drain takes it   \n"
"   mov  byte ptr [_g_drain], 1                                     \n"
"o_tx_loop:                                                         \n"
"   mov  al, [_g_ftail]                                             \n"
"   cmp  al, [_g_fhead]                                             \n"
"   je   o_tx_end              # queue empty: done                  \n"
"   mov  dx, [_g_lsr]          # LSR, wherever the stride puts it   \n"
"   mov  ecx, 800              # ~1ms of reads >> one byte time     \n"
"o_tx_wait:                                                         \n"
"   in   al, dx                                                     \n"
"   test al, 0x20              # THRE: room for a byte?             \n"
"   jnz  o_tx_pop                                                   \n"
"   dec  ecx                                                        \n"
"   jnz  o_tx_wait                                                  \n"
"   movzx eax, byte ptr [_g_ftail]   # UART dead: drop one, stop    \n"
"   inc  al                                                         \n"
"   mov  [_g_ftail], al                                             \n"
"   jmp  o_tx_end                                                   \n"
"o_tx_pop:                                                          \n"
"   movzx eax, byte ptr [_g_ftail]                                  \n"
"   mov  bl, [_g_fifo+eax]                                          \n"
"   inc  al                                                         \n"
"   mov  [_g_ftail], al                                             \n"
"   mov  dx, [_g_uart]         # THR                                \n"
"   mov  al, bl                                                     \n"
"   out  dx, al                                                     \n"
"   jmp  o_tx_loop                                                  \n"
"o_tx_end:                                                          \n"
"   mov  byte ptr [_g_drain], 0                                     \n"
"o_tx_ret:                                                          \n"
"   pop  edx                                                        \n"
"   pop  ecx                                                        \n"
"   pop  ebx                                                        \n"
"   pop  eax                                                        \n"
"   ret                                                             \n"
"o_tx_direct:                                                       \n"
"   mov  dx, [_g_lsr]                                               \n"
"   mov  ecx, 800                                                   \n"
"o_txd_wait:                                                        \n"
"   in   al, dx                                                     \n"
"   test al, 0x20                                                   \n"
"   jnz  o_txd_send                                                 \n"
"   dec  ecx                                                        \n"
"   jnz  o_txd_wait                                                 \n"
"   jmp  o_tx_ret                                                   \n"
"o_txd_send:                                                        \n"
"   mov  dx, [_g_uart]                                              \n"
"   mov  al, bl                                                     \n"
"   out  dx, al                                                     \n"
"   jmp  o_tx_ret                                                   \n"
"                                                                   \n"
/* --------- IN: the client read a byte from a trapped MPU port ---------- */
"_mpushim_in_handler:                                               \n"
"   push ebp                                                        \n"
"   mov  ebp, esp                                                   \n"
"   push eax                                                        \n"
"   push ebx                                                        \n"
"   push ecx                                                        \n"
"   push edx                                                        \n"
"   push ds                                                         \n"
"   mov  eax, [ebp+0x0C]                                            \n"
"   mov  ebx, eax                                                   \n"
"   and  ebx, 7                                                     \n"
"   add  [ebp+0x10], ebx       # advance faulting EIP (every path)  \n"
"   mov  bx, cs:[_g_ds_st]                                          \n"
"   mov  ds, bx                                                     \n"
"   mov  bl, 0x80              # default: no data, write-ready      \n"
"   test al, 0x08              # string I/O? absorb it              \n"
"   jnz  i_ret                                                      \n"
"   cmp  byte ptr [_g_busy], 0                                      \n"
"   jne  i_ret                 # re-entrant: safe idle status       \n"
"   mov  byte ptr [_g_busy], 1                                      \n"
"   test al, 0x40                                                   \n"
"   jz   i_dxport                                                   \n"
"   movzx edx, ah                                                   \n"
"   jmp  i_haveport                                                 \n"
"i_dxport:                                                          \n"
"   mov  edx, [ebp-16]                                              \n"
"   and  edx, 0xFFFF                                                \n"
"i_haveport:                                                        \n"
"   mov  cx, [_g_stat]                                              \n"
"   cmp  dx, cx                                                     \n"
"   je   i_status                                                   \n"
"   mov  cx, [_g_data]         # must be OUR data port, else FFh    \n"
"   cmp  dx, cx                                                     \n"
"   je   i_data                                                     \n"
"   mov  bl, 0xFF              # open bus for anything else         \n"
"   jmp  i_clear                                                    \n"
/* Status ALWAYS reports write-ready (bit 6 = 0).  An earlier build
 * reflected the UART THR state into DRR "like real hardware" - and DOOM
 * grew stuck notes: DMX-class drivers poll status with short timeouts
 * and skip bytes that stay "busy" for a wire-length 320us, and a skipped
 * note-off drones forever.  SoftMPU and vsbhda's VMPU both report
 * instantly-ready for the same reason; the pacing belongs in the
 * transmit path (o_tx waits), never in the status byte.               */
"i_status:                                                          \n"
"   mov  bl, 0x80              # bit7=1 no data, bit6=0 write-ready \n"
"   cmp  byte ptr [_g_ack], 0                                       \n"
"   je   i_clear                                                    \n"
"   xor  bl, bl                # ACK pending: data ready            \n"
"   jmp  i_clear                                                    \n"
"i_data:                                                            \n"
"   xor  bl, bl                                                     \n"
"   cmp  byte ptr [_g_ack], 0                                       \n"
"   je   i_clear                                                    \n"
"   mov  byte ptr [_g_ack], 0                                       \n"
"   mov  bl, 0xFE              # the ACK the game is waiting for    \n"
"i_clear:                                                           \n"
"   mov  byte ptr [_g_busy], 0                                      \n"
"i_ret:                                                             \n"
"   mov  [ebp-4], bl           # patch saved EAX low byte = our AL  \n"
"   pop  ds                                                         \n"
"   pop  edx                                                        \n"
"   pop  ecx                                                        \n"
"   pop  ebx                                                        \n"
"   pop  eax                   # AL is now the returned value       \n"
"   pop  ebp                                                        \n"
"   retf                                                            \n"
"                                                                   \n"
/* --------- the traffic recorder (diagnostic) ---------------------------- *
 *
 * Writes what the games actually do into a plain DOS-memory block, where
 * COMrade can read it straight out of conventional memory - no dump tool and
 * no second client needed.  Entries are 8 bytes:
 *     +0 value   +1 tag (0 data out, 1 command out, 2 data in, 3 status in)
 *     +2 repeat count   +4 BIOS tick when the run started
 * Consecutive identical (tag,value) pairs COALESCE into the repeat count -
 * without that, one poll loop would flood the ring and hide the commands
 * around it; with it, a stall shows up as a single "status 80 x 48000"
 * entry and the tick stamps either side say how long it lasted.
 * Block header: +0 'MTRC'  +4 ring index  +8 total events.
 * AH = tag, AL = value.  Preserves every register.                        */
"tr_rec:                                                            \n"
"   cmp  byte ptr [_g_trace], 0                                     \n"
"   je   tr_ret                                                     \n"
"   push eax                                                        \n"
"   push ebx                                                        \n"
"   push ecx                                                        \n"
"   push edi                                                        \n"
"   push es                                                         \n"
"   mov  bx, [_g_dosds]                                             \n"
"   mov  es, bx                                                     \n"
"   mov  edi, [_g_tr_lin]                                           \n"
"   mov  ebx, [_g_tr_idx]                                           \n"
"   mov  ecx, ebx              # the entry before this one          \n"
"   dec  ecx                                                        \n"
"   and  ecx, 1023                                                  \n"
"   lea  ecx, [edi+ecx*8+16]                                        \n"
"   cmp  word ptr es:[ecx], ax # same thing again? just count it    \n"
"   jne  tr_new                                                     \n"
"   inc  word ptr es:[ecx+2]                                        \n"
"   jmp  tr_tot                                                     \n"
"tr_new:                                                            \n"
"   lea  ecx, [edi+ebx*8+16]                                        \n"
"   mov  es:[ecx], ax                                               \n"
"   mov  word ptr es:[ecx+2], 1                                     \n"
"   push edx                                                        \n"
"   mov  edx, es:[0x46C]       # the BIOS tick count, at linear 46Ch\n"
"   mov  es:[ecx+4], edx                                            \n"
"   pop  edx                                                        \n"
"   inc  ebx                                                        \n"
"   and  ebx, 1023                                                  \n"
"   mov  [_g_tr_idx], ebx                                           \n"
"   mov  es:[edi+4], ebx                                            \n"
"   test ebx, ebx              # ring just wrapped?                 \n"
"   jnz  tr_tot                                                     \n"
"   cmp  byte ptr [_g_tr1], 0  # /TRACE1: keep the FIRST 1024 only  \n"
"   je   tr_tot                                                     \n"
"   mov  byte ptr [_g_trace], 0                                     \n"
"tr_tot:                                                            \n"
"   inc  dword ptr es:[edi+8]                                       \n"
"   pop  es                                                         \n"
"   pop  edi                                                        \n"
"   pop  ecx                                                        \n"
"   pop  ebx                                                        \n"
"   pop  eax                                                        \n"
"tr_ret:                                                            \n"
"   ret                                                             \n"
"                                                                   \n"
/* --------- VDPMI: ONE handler, BOTH worlds ----------------------------- *
 *
 * VDPMI (crazii's DPMI host with its own V86 monitor and virtual PIC,
 * Pentium-only) traps V86 and ring-3 protected-mode I/O through the SAME
 * table, so under it a single registration covers real-mode and DOS/4GW
 * games alike - no QPI blob, no HDPMI, one resident host instead of three.
 *
 * Entered by a FAR CALL with a pure argument frame - the client's own
 * registers are the host's business, not ours, and DS is undefined:
 *     [ebp+0x04] return EIP     [ebp+0x08] return CS
 *     [ebp+0x0C] the byte the client's IN will read   <- we write this
 *     [ebp+0x10] port           [ebp+0x14] flags (bit 0 = OUT)
 *     [ebp+0x18] the byte an OUT wrote
 * Finish with a far RET and the host pops the frame.  A port that is not
 * ours is handed on to the handler registered before us with the frame
 * exactly as it arrived.
 *
 * The facade is the same one the HDPMI pair above implements, and so are
 * its hard-won rules: never drop a MIDI byte, always report write-ready.
 * There is no EIP to advance here and no error code to decode: the host
 * resumes the client itself and hands us the port, the direction and the
 * value ready-made.  Only bit 0 of the flags word has a published meaning,
 * so - exactly like SBEMU - this models byte accesses only; if a client
 * ever writes a word to an MPU port, the high half goes nowhere.  No game
 * does, and guessing at the rest of that word could do real damage.
 *
 * DERIVED WORK.  VDPMI's vendor API is not in VDPMI.TXT; this handler's
 * calling contract - the argument frame, the far-return, the "return with
 * bit 31 set / jump to the previous handler" chaining - is read off
 * SBEMU's client-side driver for it (vdpmi.c, (C) crazii, GNU GPL v2),
 * which is the published description of that ABI.  Another reason this
 * program is GPL v2; see the header.                                      */
"   .globl _mpushim_vd_handler                                       \n"
"_mpushim_vd_handler:                                               \n"
"   push ebp                                                        \n"
"   mov  ebp, esp                                                   \n"
"   push eax                                                        \n"
"   push ebx                                                        \n"
"   push ecx                                                        \n"
"   push edx                                                        \n"
"   push ds                                                         \n"
"   mov  bx, cs:[_g_ds_st]                                          \n"
"   mov  ds, bx                                                     \n"
"   mov  edx, [ebp+0x10]       # the port                           \n"
"   mov  cx, [_g_stat]                                              \n"
"   cmp  dx, cx                                                     \n"
"   je   v_mine                                                     \n"
"   mov  cx, [_g_data]         # must be OUR port, else pass it on  \n"
"   cmp  dx, cx                                                     \n"
"   jne  v_chain                                                    \n"
"v_mine:                                                            \n"
"   mov  ebx, 0x80             # idle status: no data, write-ready  \n"
/* No reentrancy guard on this path any more.  VDPMI can preempt a trap
 * handler (HDPMI cannot - it dispatches with the real IF clear), and the
 * guard turned that into a DROPPED byte and a LIE: a nested status read
 * answered "no data" even with an ACK pending, and a nested command was
 * swallowed whole, so a game that never saw its ACK sat in its retry
 * timeout for seconds.  That is exactly the Tyrian symptom - fine in the
 * jukebox where only the music ISR touches the MPU, stalling in the menu
 * where the main thread touches it too.  The transmit queue in o_tx makes
 * nesting harmless, so the honest answer is always given.  /NOFIFO puts
 * the old behaviour back, to demonstrate the fault on demand.          */
"   cmp  byte ptr [_g_nofifo], 0                                    \n"
"   je   v_go                                                       \n"
"   cmp  byte ptr [_g_busy], 0                                      \n"
"   jne  v_nested                                                   \n"
"   mov  byte ptr [_g_busy], 1                                      \n"
"v_go:                                                              \n"
"   test byte ptr [ebp+0x14], 1                                     \n"
"   jnz  v_out                                                      \n"
"   mov  cx, [_g_stat]         # ---- IN ----                       \n"
"   cmp  dx, cx                                                     \n"
"   je   v_i_stat                                                   \n"
"   mov  ah, 2                 # trace tag: data read               \n"
"   xor  ebx, ebx              # data port: the ACK once, then 0    \n"
"   cmp  byte ptr [_g_ack], 0                                       \n"
"   je   v_i_done                                                   \n"
"   mov  byte ptr [_g_ack], 0                                       \n"
"   mov  ebx, 0xFE                                                  \n"
"   jmp  v_i_done                                                   \n"
"v_i_stat:                                                          \n"
"   mov  ah, 3                 # trace tag: status read             \n"
"   mov  ebx, 0x80             # bit7=1 no data, bit6=0 write-ready \n"
"   cmp  byte ptr [_g_ack], 0                                       \n"
"   je   v_i_done                                                   \n"
"   xor  ebx, ebx              # ACK pending: data ready            \n"
"v_i_done:                                                          \n"
"   mov  al, bl                                                     \n"
"   call tr_rec                                                     \n"
"   mov  byte ptr [_g_busy], 0                                      \n"
"   jmp  v_ret                                                      \n"
"v_out:                                                             \n"
"   mov  eax, [ebp+0x18]       # the byte written                   \n"
"   mov  cx, [_g_stat]                                              \n"
"   cmp  dx, cx                                                     \n"
"   je   v_o_cmd                                                    \n"
"   mov  ah, 0                 # trace tag: data written            \n"
"   call tr_rec                                                     \n"
"   mov  bl, al                # data: straight out to the UART     \n"
"   call o_tx                                                       \n"
"   jmp  v_o_done                                                   \n"
"v_o_cmd:                                                           \n"
"   mov  ah, 1                 # trace tag: command written         \n"
"   call tr_rec                                                     \n"
"   cmp  al, 0xFF              # reset: notes-off bcast + ACK       \n"
"   je   v_o_reset                                                  \n"
"   cmp  al, 0x3F              # enter UART mode -> queue the ACK   \n"
"   je   v_o_ack                                                    \n"
"   cmp  byte ptr [_g_ackall], 0   # /ACKALL: answer everything     \n"
"   jne  v_o_ack                                                    \n"
"   jmp  v_o_done              # any other command: absorb          \n"
"v_o_reset:                                                         \n"
"   mov  bh, 0xB0                                                   \n"
"v_o_rst1:                                                          \n"
"   mov  bl, bh                # Bn: control change, channel n      \n"
"   call o_tx                                                       \n"
"   mov  bl, 0x7B              # CC 123: all notes off              \n"
"   call o_tx                                                       \n"
"   mov  bl, 0x00                                                   \n"
"   call o_tx                                                       \n"
"   inc  bh                                                         \n"
"   cmp  bh, 0xC0                                                   \n"
"   jne  v_o_rst1                                                   \n"
"v_o_ack:                                                           \n"
"   mov  byte ptr [_g_ack], 1                                       \n"
"v_o_done:                                                          \n"
"   mov  byte ptr [_g_busy], 0                                      \n"
"   xor  ebx, ebx              # an OUT reads back nothing          \n"
"   jmp  v_ret                 # (do NOT fall into v_nested)        \n"
"v_nested:                    # /NOFIFO only: record what gets lost \n"
"   mov  ah, 4                                                      \n"
"   mov  al, [ebp+0x18]                                             \n"
"   call tr_rec                                                     \n"
"   mov  ebx, 0x80                                                  \n"
"v_ret:                                                             \n"
"   mov  [ebp+0x0C], ebx                                            \n"
"   pop  ds                                                         \n"
"   pop  edx                                                        \n"
"   pop  ecx                                                        \n"
"   pop  ebx                                                        \n"
"   pop  eax                                                        \n"
"   pop  ebp                                                        \n"
"   retf                                                            \n"
/* Not ours: hand the frame on exactly as it arrived.  If there was no
 * handler before us the port cannot be one we enabled, so answer open bus
 * rather than jump through a null far pointer.                          */
"v_chain:                                                           \n"
"   cmp  word ptr cs:[_g_vd_old_sel], 0                             \n"
"   jne  v_chain_go                                                 \n"
"   mov  ebx, 0xFF                                                  \n"
"   jmp  v_ret                                                      \n"
"v_chain_go:                                                        \n"
"   pop  ds                                                         \n"
"   pop  edx                                                        \n"
"   pop  ecx                                                        \n"
"   pop  ebx                                                        \n"
"   pop  eax                                                        \n"
"   pop  ebp                                                        \n"
"   .byte 0x2E, 0xFF, 0x2D     # jmp fword ptr cs:[g_vd_old]        \n"
"   .long _g_vd_old                                                 \n"
"                                                                   \n"
/* --------- CLI trap: heal the IOPL-0 PUSHFD/CLI...POPFD hole ----------- *
 *
 * DERIVED WORK.  This handler follows VSBHDA's _hdpmi_CliHandler
 * (src/stackio.asm), (C) Baron-von-Riedesel, GNU GPL v2 - the same register
 * discipline, the same "was the CLI preceded by PUSHFD?" test with the same
 * 9Ch / 589Ch opcode constants, and the same re-enable via DPMI 0901h.  His
 * comment there names the case it exists for: "needed by ID games' DOS/4G".
 * That is why this program is GPL v2 rather than MIT.
 *
 * Registered through HDPMI vendor fn 9.  Contract (HDPMIAPI.TXT): the host
 * has ALREADY cleared IF; no stack switch - we run on the interrupted
 * stack, which holds a plain IRETD frame [EIP after CLI][CS][EFLAGS]; all
 * registers must be preserved; exit with IRETD straight back to the client.
 * If the CLI directly follows PUSHFD (9C) or PUSHFD/POP EAX (9C 58), the
 * section will be exited with POPFD - which at IOPL 0 CANNOT restore IF -
 * so re-enable interrupts now via DPMI 0901h (that call flips the live IF;
 * the IF bit in our IRETD image is powerless at ring 3, which is exactly
 * the hole being healed).  Client flags are restored from the IRETD image,
 * so our compares don't leak into the client's arithmetic flags.          */
"_mpushim_cli_handler:                                              \n"
"   push ds                                                         \n"
"   push esi                                                        \n"
"   lds  esi, [esp+8]          # client CS:EIP from the IRETD frame \n"
"   cmp  esi, 3                # too close to segment start?        \n"
"   jb   c_leave                                                    \n"
"   cmp  word ptr [esi-3], 0x589C  # pushfd / pop eax / cli         \n"
"   je   c_sti                                                      \n"
"   cmp  byte ptr [esi-2], 0x9C    # pushfd / cli                   \n"
"   jne  c_leave                                                    \n"
"c_sti:                                                             \n"
"   push eax                                                        \n"
"   mov  ax, 0x0901            # DPMI: virtual ints = enabled       \n"
"   int  0x31                                                       \n"
"   pop  eax                                                        \n"
"c_leave:                                                           \n"
"   pop  esi                                                        \n"
"   pop  ds                                                         \n"
"   iretd                                                           \n"
"   .globl _mpushim_code_end                                        \n"
"_mpushim_code_end:                                                 \n"
"   .att_syntax prefix                                              \n"
);

/* ---- finding a host's vendor API --------------------------------------
 * AX=168Ah with DS:ESI -> a host's signature returns AL=0 and that host's
 * API entry in ES:EDI.  "HDPMI" answers on HDPMI32i, "VDPMI" on VDPMI.
 * (int 2Fh is the call that answers here; int 31h does not.)  The entry is
 * then reached by an lcall through the returned 16:32 far pointer. */
typedef struct { unsigned long off; unsigned short sel; } __attribute__((packed)) FARPTR32;
static FARPTR32 hdpmi_entry, vdpmi_entry;

static int get_vendor_api(const char *sig, FARPTR32 *entry)
{
    unsigned char ok = 1;
    unsigned short sel = 0;
    unsigned long off = 0;
    __asm__ __volatile__(
        "pushl %%es         \n"
        "movl  %3, %%esi    \n"
        "xorl  %%edi, %%edi \n"
        "movw  $0x168a, %%ax\n"
        "int   $0x2f        \n"
        "movb  %%al, %0     \n"
        "movw  %%es, %1     \n"
        "movl  %%edi, %2    \n"
        "popl  %%es         \n"
        : "=m"(ok), "=m"(sel), "=m"(off)
        : "r"(sig)
        : "eax", "esi", "edi");
    if (ok != 0) return 0;
    entry->off = off;
    entry->sel = sel;
    return (sel != 0);
}

/* fn 5: clear the "separate address contexts" flag (HDPMI=32 / -a).  Our
 * resident handlers live in THIS client's address space, so every client
 * whose I/O we trap must share it.  Unconditional, like vsbhda: the 08-22
 * bisect proved the call harmless (identical behaviour with and without),
 * and with HDPMI=32 set in someone's environment it is load-bearing. */
static void hdpmi_set_context_mode(unsigned char mode)
{
    __asm__ __volatile__(
        "movw  $5, %%ax         \n"
        "movb  %0, %%bl         \n"
        "lcall *%1              \n"
        :
        : "g"(mode), "m"(hdpmi_entry)
        : "eax", "ebx");
}

/* fn 6: trap [start, start+count).  DS:ESI -> the two FAR32 handler procs.
 * CF means failure (no free range slot, or a port already trapped - e.g.
 * VSBPCM resident with /P) and EAX is then garbage, so the carry must be
 * checked: treating it as a handle would report success while trapping
 * nothing. */
static struct {
    unsigned long  in_off;  unsigned short in_sel;
    unsigned long  out_off; unsigned short out_sel;
} __attribute__((packed)) tp;

static unsigned long hdpmi_install(unsigned start, unsigned count)
{
    unsigned long handle = 0;
    unsigned char err = 0;
    unsigned short mycs;
    __asm__ __volatile__("movw %%cs, %0" : "=r"(mycs));
    tp.in_off  = (unsigned long)&mpushim_in_handler;  tp.in_sel  = mycs;
    tp.out_off = (unsigned long)&mpushim_out_handler; tp.out_sel = mycs;
    __asm__ __volatile__(
        "leal  %2, %%esi        \n"
        "movw  $6, %%ax         \n"
        "movl  %3, %%edx        \n"
        "movl  %4, %%ecx        \n"
        "lcall *%5              \n"
        "setc  %1               \n"
        "movl  %%eax, %0        \n"
        : "=m"(handle), "=m"(err)
        : "m"(tp), "g"(start), "g"(count), "m"(hdpmi_entry)
        : "eax", "ecx", "edx", "esi");
    return err ? 0 : handle;
}

/* ---- QPI (the QEMM Programming Interface) - the V86 port-trap host -----
 * Provided by QEMM386, Jemm's QPIEMU, and VDPMI alike.  Public ABI: entry
 * via int 67h AX=3F00h CX='QE' DX='MM' -> ES:DI, or int 2Fh AX=1684h
 * BX=4354h -> ES:DI; far-call functions 1A06h get / 1A07h set trap
 * handler, 1A08h port status, 1A09h/1A0Ah trap/untrap; CF = error. */
static void outs(const char *s);               /* defined below */
static unsigned short qpi_seg, qpi_ip;         /* far entry; seg 0 = absent */

static int qpi_call(__dpmi_regs *r)            /* far-call QPI; 0 = success */
{
    r->x.cs = qpi_seg;
    r->x.ip = qpi_ip;
    r->x.ss = r->x.sp = 0;
    if (__dpmi_simulate_real_mode_procedure_retf(r) != 0) return -1;
    return (r->x.flags & 1) ? -1 : 0;
}

static int find_qpi(void)
{
    __dpmi_regs r;
    unsigned long psp = _go32_info_block.linear_address_of_original_psp;
    /* QEMM / VDPMI answer on int 67h */
    if (_farpeekl(_dos_ds, 0x67 * 4) != 0) {
        memset(&r, 0, sizeof r);
        r.x.ax = 0x3F00; r.x.cx = 0x5145; r.x.dx = 0x4D4D;   /* 'QE' 'MM' */
        r.x.flags = 0x202;
        __dpmi_simulate_real_mode_interrupt(0x67, &r);
        if (r.h.ah == 0 && r.x.es) { qpi_seg = r.x.es; qpi_ip = r.x.di; return 1; }
    }
    /* QPIEMU answers on int 2Fh - which must run as a REAL interrupt, so
     * call a 3-byte INT 2Fh/RETF thunk planted in the PSP's FCB scratch */
    _farpokel(_dos_ds, psp + 0x5C, 0x00CB2FCDUL);
    memset(&r, 0, sizeof r);
    r.x.ax = 0x1684; r.x.bx = 0x4354;
    r.x.cs = (unsigned short)(psp >> 4); r.x.ip = 0x5C;
    r.x.ss = r.x.sp = 0;
    if (__dpmi_simulate_real_mode_procedure_retf(&r) == 0 && r.h.al == 0 && r.x.es) {
        qpi_seg = r.x.es; qpi_ip = r.x.di;
        return 1;
    }
    return 0;
}

/* Plant the V86 blob in a small DOS-memory block of its own and arm the
 * QPI trap.  The block is allocated through the DPMI host and belongs to
 * this client, so it survives the TSR with everything else; measured on
 * the bench, the go32 transfer buffer sits at PSP+100h under HDPMI, so
 * nothing inside our own conventional image is safely reusable.  Returns
 * 1 if armed; 0 (reason printed) if the V86 side is left alone - which
 * is not fatal, the PM side still installs. */
static int rm_install(void)
{
    __dpmi_regs r;
    unsigned short oldseg = 0, oldofs = 0;
    unsigned long home;
    int blkseg, blksel;
    int i;

    /* a standalone MPUSHIM.COM already resident? it already covers V86 */
    memset(&r, 0, sizeof r);
    r.x.ax = 0x1A06;                               /* get current handler */
    if (qpi_call(&r) == 0 && r.x.es) {
        unsigned long h = (unsigned long)r.x.es << 4;
        oldseg = r.x.es;
        oldofs = r.x.di;
        if (_farpeekw(_dos_ds, h + 0x103) == 0x534D &&        /* 'MS' */
            _farpeekw(_dos_ds, h + 0x105) == 0x4D48) {        /* 'HM' */
            outs("V86 trap: not armed - MPUSHIM.COM has it\n");
            return 0;
        }
    }
    /* ports already trapped in V86 (VSBPCM's own MPU emulation)? */
    for (i = 0; i < 2; i++) {
        memset(&r, 0, sizeof r);
        r.x.ax = 0x1A08;
        r.x.dx = (unsigned short)(g_data + i);
        if (qpi_call(&r) == 0 && r.h.bl) {
            outs("V86 trap: not armed - ports already trapped\n");
            return 0;
        }
    }
    blkseg = __dpmi_allocate_dos_memory((mpushimr_bin_len + 15) >> 4, &blksel);
    if (blkseg < 0) {
        outs("V86 trap: not armed - no DOS memory\n");
        return 0;
    }
    home = (unsigned long)blkseg << 4;
    movedata(_my_ds(), (unsigned)mpushimr_bin, _dos_ds, home, mpushimr_bin_len);
    _farpokew(_dos_ds, home + RBLOB_UART, g_uart);
    _farpokew(_dos_ds, home + RBLOB_DATA, g_data);
    _farpokew(_dos_ds, home + RBLOB_STAT, g_stat);
    _farpokew(_dos_ds, home + RBLOB_SYNTH, g_synth);
    _farpokew(_dos_ds, home + RBLOB_LSR, (unsigned short)(g_lsr - g_uart));

    memset(&r, 0, sizeof r);
    r.x.ax = 0x1A07;                               /* set trap handler */
    r.x.es = (unsigned short)blkseg;
    r.x.di = RBLOB_ENTRY;
    if (qpi_call(&r) != 0) {
        __dpmi_free_dos_memory(blksel);
        outs("V86 trap: not armed - QPI rejected it\n");
        return 0;
    }
    for (i = 0; i < 2; i++) {
        memset(&r, 0, sizeof r);
        r.x.ax = 0x1A09;                           /* trap port */
        r.x.dx = (unsigned short)(g_data + i);
        if (qpi_call(&r) != 0) {
            if (i) {                               /* roll the first one back */
                memset(&r, 0, sizeof r);
                r.x.ax = 0x1A0A; r.x.dx = g_data;
                qpi_call(&r);
            }
            memset(&r, 0, sizeof r);               /* restore the old handler */
            r.x.ax = 0x1A07;
            r.x.es = oldseg;
            r.x.di = oldofs;
            qpi_call(&r);
            __dpmi_free_dos_memory(blksel);
            outs("V86 trap: not armed - QPI rejected it\n");
            return 0;
        }
    }
    return 1;
}

/* ---- VDPMI's port-trap API --------------------------------------------
 * Not documented in VDPMI.TXT; these are the calls SBEMU's client driver
 * makes (vdpmi.c, GPL v2 - see the attribution at the handler).  Each is an
 * lcall through the vendor entry with the function number in EAX and CF set
 * on failure:
 *    1  get the current port-trap handler           -> CX:EDX
 *    2  set the port-trap handler       ECX = CS, EDX = offset
 *    3  is this port trapped?           EDX = port  -> EAX
 *    4  trap / untrap one port          EDX = port, ECX = 1 / 0
 * (5 performs an untrapped access on the client's behalf, 6 and 7 are
 * client IRQ handling, 8 maps linear to physical - none of them needed
 * here: our handler talks to the UART with plain IN/OUT, exactly as it
 * does under HDPMI, and we raise no interrupts.)  Nothing published says
 * which registers the entry preserves, so every call below declares EBX,
 * ESI and EDI clobbered as well and lets the compiler save what it cares
 * about.                                                                */
static int vd_get_handler(unsigned long *off, unsigned short *sel)
{
    unsigned char err = 0;
    unsigned long o = 0;
    unsigned short sl = 0;
    __asm__ __volatile__(
        "movl  $1, %%eax    \n"
        "lcall *%3          \n"
        "setc  %2           \n"
        "movl  %%edx, %0    \n"
        "movw  %%cx, %1     \n"
        : "=m"(o), "=m"(sl), "=m"(err)
        : "m"(vdpmi_entry)
        : "eax", "ecx", "edx", "ebx", "esi", "edi");
    *off = o; *sel = sl;
    return !err;
}

static int vd_set_handler(unsigned short sel, unsigned long off)
{
    unsigned char err = 0;
    __asm__ __volatile__(
        "movl  $2, %%eax    \n"
        "movzwl %1, %%ecx   \n"
        "movl  %2, %%edx    \n"
        "lcall *%3          \n"
        "setc  %0           \n"
        : "=m"(err)
        : "m"(sel), "m"(off), "m"(vdpmi_entry)
        : "eax", "ecx", "edx", "ebx", "esi", "edi");
    return !err;
}

static int vd_is_trapped(unsigned port)
{
    unsigned char err = 0;
    unsigned long state = 0;
    __asm__ __volatile__(
        "movl  $3, %%eax    \n"
        "movl  %2, %%edx    \n"
        "lcall *%3          \n"
        "setc  %1           \n"
        "movl  %%eax, %0    \n"
        : "=m"(state), "=m"(err)
        : "g"(port), "m"(vdpmi_entry)
        : "eax", "ecx", "edx", "ebx", "esi", "edi");
    return !err && state != 0;
}

static int vd_set_trap(unsigned port, unsigned on)
{
    unsigned char err = 0;
    __asm__ __volatile__(
        "movl  $4, %%eax    \n"
        "movl  %1, %%edx    \n"
        "movl  %2, %%ecx    \n"
        "lcall *%3          \n"
        "setc  %0           \n"
        : "=m"(err)
        : "g"(port), "g"(on), "m"(vdpmi_entry)
        : "eax", "ecx", "edx", "ebx", "esi", "edi");
    return !err;
}

/* Arm the one registration that covers both worlds.  The handler slot is
 * global to the host, so we chain: remember who was there and pass on
 * anything that is not one of our two ports.  Ports that are trapped
 * already belong to somebody else - a resident SBEMU emulating an MPU, or
 * an MPUSHIM that is already in - and we stand down rather than fight for
 * them, exactly as the HDPMI and QPI sides do.  Returns 1 if armed; 0 with
 * the reason printed. */
static int vdpmi_install(void)
{
    unsigned short mycs, oldsel = 0;
    unsigned long oldoff = 0;
    int i;

    for (i = 0; i < 2; i++) {
        if (vd_is_trapped(g_data + i)) {
            outs("VDPMI trap: not armed - ports already trapped"
                 " (MPUSHIM already resident?)\n");
            return 0;
        }
    }
    __asm__ __volatile__("movw %%cs, %0" : "=r"(mycs));
    if (!vd_get_handler(&oldoff, &oldsel)) { oldoff = 0; oldsel = 0; }
    if (oldsel == mycs) { oldoff = 0; oldsel = 0; }   /* never chain to self */
    g_vd_old     = oldoff;
    g_vd_old_sel = oldsel;

    if (!vd_set_handler(mycs, (unsigned long)&mpushim_vd_handler)) {
        outs("VDPMI trap: not armed - the host refused the handler\n");
        return 0;
    }
    for (i = 0; i < 2; i++) {
        if (!vd_set_trap(g_data + i, 1)) {
            while (i-- > 0) vd_set_trap(g_data + i, 0);
            vd_set_handler(oldsel, oldoff);           /* put the chain back */
            outs("VDPMI trap: not armed - the host refused the ports\n");
            return 0;
        }
    }
    return 1;
}

/* fn 9: trap CLI (BL=0).  CX:EDX = FAR32 handler.  CF set = the resident
 * HDPMI has no CLI/STI trap support (very old build). */
static int hdpmi_set_cli(void (*handler)(void))
{
    unsigned long off = (unsigned long)handler;
    unsigned short mycs;
    unsigned char err = 0;
    __asm__ __volatile__("movw %%cs, %0" : "=r"(mycs));
    __asm__ __volatile__(
        "movw  $9, %%ax         \n"
        "xorl  %%ebx, %%ebx     \n"
        "movzwl %1, %%ecx       \n"
        "movl  %2, %%edx        \n"
        "lcall *%3              \n"
        "setc  %0               \n"
        : "=m"(err)
        : "m"(mycs), "m"(off), "m"(hdpmi_entry)
        : "eax", "ebx", "ecx", "edx");
    return !err;
}

/* ---- tiny console output (no stdio: this image stays resident) --------- */
static void outs(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    write(1, s, n);
}

static void outhex(unsigned v, int digits)
{
    static const char hx[] = "0123456789ABCDEF";
    char b[9];
    int i;
    for (i = digits - 1; i >= 0; i--) { b[i] = hx[v & 15]; v >>= 4; }
    b[digits] = 0;
    outs(b);
}

static void outdec(unsigned long v)
{
    char b[12];
    int i = 11;
    b[11] = 0;
    if (!v) { outs("0"); return; }
    while (v && i) { b[--i] = (char)('0' + (int)(v % 10)); v /= 10; }
    outs(b + i);
}

/* ---- /DUMP=seg: print the trace block ---------------------------------
 * Separate from the resident half on purpose.  Under VDPMI the COMrade
 * link does not survive a DPMI client, so the trace cannot be read off the
 * machine while the evidence is being made; instead this prints it to
 * stdout in the SAME boot ("MPUSHIM /DUMP=2745 > TRACE.TXT"), and the file
 * is collected after a reboot.  Entries come out oldest first. */
static void dump_trace(unsigned seg)
{
    static const char *tagname[5] = {
        "out data", "out CMD ", "in  data", "in  stat", "DROPPED "
    };
    unsigned long lin = (unsigned long)seg << 4;
    unsigned long idx, tot;
    unsigned i;

    if (_farpeekl(_dos_ds, lin) != 0x4352544DUL) {
        outs("MPUSHIM: no trace buffer at that segment.\n");
        return;
    }
    idx = _farpeekl(_dos_ds, lin + 4) & 1023;
    tot = _farpeekl(_dos_ds, lin + 8);
    outs("MPUSHIM trace at ");
    outhex(seg, 4);
    outs(":0000 - ");
    outdec(tot);
    outs(" events\n\nwhat      val  repeat  tick\n");
    for (i = 0; i < 1024; i++) {
        unsigned long a = lin + 16 + (((idx + i) & 1023) * 8);
        unsigned rep = _farpeekw(_dos_ds, a + 2);
        unsigned char tg;
        if (!rep) continue;
        tg = _farpeekb(_dos_ds, a + 1);
        if (tg > 4) continue;
        outs(tagname[tg]);
        outs("   ");
        outhex(_farpeekb(_dos_ds, a), 2);
        outs("  ");
        outdec(rep);
        outs("   ");
        outdec(_farpeekl(_dos_ds, a + 4));
        outs("\n");
    }
}

/* ---- tiny arg helpers -------------------------------------------------- */
static int keymatch(const char *a, const char *key)   /* case-insensitive */
{
    int i;
    for (i = 0; key[i]; i++) {
        char c = a[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c != key[i]) return 0;
    }
    return 1;
}

static unsigned parse_hex(const char *s)
{
    unsigned v = 0;
    for (;;) {
        char c = *s++;
        if (c >= '0' && c <= '9') v = (v << 4) | (unsigned)(c - '0');
        else if (c >= 'A' && c <= 'F') v = (v << 4) | (unsigned)(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') v = (v << 4) | (unsigned)(c - 'a' + 10);
        else break;
    }
    return v;
}

static unsigned parse_dec(const char *s)
{
    unsigned v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned)(*s++ - '0');
    return v;
}

/* ---- optional UART (re)program: 8N1, given divisor, FIFOs on -----------
 * sh = the register-stride shift: 0 for a normal 16550, 1 where the
 * registers sit 2 bytes apart (/STRIDE=2).                               */
static void uart_setdiv(unsigned base, unsigned div, unsigned sh)
{
    outportb(base + (3 << sh), 0x80);    /* DLAB */
    outportb(base + (0 << sh), div & 0xFF);
    outportb(base + (1 << sh), (div >> 8) & 0xFF);
    outportb(base + (3 << sh), 0x03);    /* 8N1 */
    outportb(base + (4 << sh), 0x00);    /* MCR = 0 */
    outportb(base + (1 << sh), 0x00);    /* IER = 0 */
    outportb(base + (2 << sh), 0x00);    /* FCR: FIFOs OFF (see below) */
}

/* Is anything resident on that INT 2Fh multiplex id?  AL=00 is the usual
 * install check and a handler that owns the id answers AL=0FFh.  We do not
 * verify any particular signature: the sink is deliberately generic, and the
 * only thing worth telling the user is that the far end is empty. */
static int synth_present(unsigned short ax)
{
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.h.ah = (unsigned char)(ax >> 8);
    r.h.al = 0x00;
    __dpmi_int(0x2F, &r);
    return (r.h.al == 0xFF);
}

int main(int argc, char **argv)
{
    unsigned long handle = 0;
    unsigned long psp;
    unsigned div = 0, ush = 0;
    int nocli = 0, nopm = 0, norm = 0, novd = 0, trace = 0;
    unsigned dumpseg = 0;
    int hdpmi_ok, qpi_ok, vd_ok, pm_armed = 0, rm_armed = 0, vd_armed = 0;
    __dpmi_regs r;
    int i;

    /* Repair the machine-global FPU state FIRST, before any exit path (see
     * bug 1 in the header): DJGPP's CRT already called DPMI 0E01h during
     * startup, and under HDPMI that edited the real CR0 for every program
     * that runs after us - TSR or not.  0 = EM off, MP off, the bare-DOS
     * baseline a no-FPU (and an FPU) machine boots with.  We execute no FP
     * instruction ourselves, so losing DJGPP's emulation hook is free. */
    __dpmi_set_coprocessor_emulation(0);

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (*a != '/' && *a != '-') continue;
        a++;
        if      (keymatch(a, "UART=")) g_uart = (unsigned short)parse_hex(a + 5);
        else if (keymatch(a, "MPU="))  g_data = (unsigned short)parse_hex(a + 4);
        else if (keymatch(a, "DIV="))  div    = parse_dec(a + 4);
        else if (keymatch(a, "STRIDE=")) ush  = (parse_dec(a + 7) == 2) ? 1 : 0;
        else if (keymatch(a, "NOCLI")) nocli = 1;
        else if (keymatch(a, "NOVD"))  novd = 1;
        else if (keymatch(a, "DUMP="))  dumpseg = parse_hex(a + 5);
        else if (keymatch(a, "TRACE1")) { trace = 1; g_tr1 = 1; }
        else if (keymatch(a, "TRACE"))  trace = 1;
        else if (keymatch(a, "ACKALL")) g_ackall = 1;
        else if (keymatch(a, "NOFIFO")) g_nofifo = 1;
        else if (keymatch(a, "NOPM"))  nopm = 1;
        else if (keymatch(a, "NORM"))  norm = 1;
        else if (keymatch(a, "SYNTH="))
            g_synth = (unsigned short)((parse_hex(a + 6) << 8) | 0x01);
        else if (keymatch(a, "SYNTH")) g_synth = 0xBD01;
        else {
            outs("MPUSHIM 0.7 - MPU-401 facade over a serial UART, all trap worlds.\n"
                 "  MPUSHIM [/UART=250] [/MPU=330] [/DIV=n] [/STRIDE=n] [/NORM] [/NOPM]\n"
                 "  /UART  = serial UART base I/O port (default 250)\n"
                 "  /MPU   = MPU-401 base the game expects (default 330)\n"
                 "  /DIV   = reprogram the UART divisor for 31250 baud\n"
                 "  /STRIDE= UART register spacing: 1 (normal) or 2, for a\n"
                 "           16550 on a 16-bit window (TDK DIN: /UART=320\n"
                 "           /STRIDE=2 /DIV=5)\n"
                 "  /NORM  = skip the V86 (QPI) side   /NOPM = skip the PM side\n"
                 "  /SYNTH[=ah] = send MIDI to a resident INT 2Fh software synth\n"
                 "           (OPL4SYN, TDKSYN, ...; default id BDh) instead of\n"
                 "           the UART - every world.  Refuses VDPMI.\n"
                 "  /NOVD  = ignore VDPMI, use the QPI + HDPMI pair instead\n"
                 "  /NOCLI = skip the DOS4G PUSHFD/CLI/POPFD interrupt heal\n"
                 "  diagnostics: /TRACE record traffic  /ACKALL ACK every command\n"
                 "               /NOFIFO drop bytes when a trap nests (the old bug)\n"
                 "               /TRACE1 like /TRACE but keeps the FIRST 1024\n"
                 "               /DUMP=seg print the trace block and exit\n"
                 "Installs its traps and stays resident; start games normally.\n"
                 "Hosts: VDPMI alone covers both worlds (Pentium); otherwise\n"
                 "HDPMI32i -r -x (DOS4GW games) + JEMM+QPIEMU or QEMM (real-mode).\n");
            return 0;
        }
    }
    g_stat = g_data + 1;
    g_lsr  = g_uart + (5 << ush);

    /* /SYNTH covers every world: the V86 blob delivers natively and the PM
     * handler reflects through DPMI 0300h.  VDPMI stays refused - it can
     * preempt a handler mid-reflection and synth_regs is static. */
    if (g_synth) novd = 1;

    if (dumpseg) { dump_trace(dumpseg); return 0; }

    /* VDPMI first, and on its own: it is a DPMI host AND a V86 monitor
     * whose port traps fire for ring-3 and V86 clients through one table,
     * so a single registration there covers everything the QPI blob plus
     * HDPMI cover between them - and it is the only host on the machine
     * (it replaces Jemm/QPIEMU/HDPMI32i rather than joining them).  /NOVD
     * falls back to the pair, which also works under VDPMI's QEMM-style
     * QPI facade for the V86 half. */
    vd_ok    = novd ? 0 : get_vendor_api("VDPMI", &vdpmi_entry);
    hdpmi_ok = (vd_ok || nopm) ? 0 : get_vendor_api("HDPMI", &hdpmi_entry);
    qpi_ok   = (vd_ok || norm) ? 0 : find_qpi();
    if (!vd_ok && !hdpmi_ok && !qpi_ok) {
        outs("MPUSHIM: no trap host at all.\n"
             "  VDPMI covers both worlds by itself (Pentium and up);\n"
             "  otherwise the PM side needs HDPMI32i resident (-r -x)\n"
             "  and the V86 side needs Jemm+QPIEMU or QEMM.\n");
        return 2;
    }
    /* Already resident?  The V86 core carries 'MSHR' at offset 0 of its own
     * block, so ask QPI who owns the trap and look for our marker.  Without
     * this a second run reaches the install path and briefly re-points the
     * live QPI handler before rolling back - and would orphan the first
     * core outright if the port-trap call happened to succeed.  (A PM-only
     * install leaves no such marker; there the honest answer is the
     * "ports already trapped" the install itself reports.) */
    if (qpi_ok) {
        __dpmi_regs q;
        memset(&q, 0, sizeof q);
        q.x.ax = 0x1A06;                       /* get current trap handler */
        if (qpi_call(&q) == 0 && q.x.es &&
            _farpeekl(_dos_ds, (unsigned long)q.x.es << 4) == 0x5248534DUL) {
            outs("MPUSHIM: already installed.\n");
            return 0;
        }
    }

    __asm__ __volatile__("movw %%ds, %0" : "=m"(g_ds_st));
    if (div && !g_synth) uart_setdiv(g_uart, div, ush);

    /* FIFOs OFF regardless of who programmed the UART: with the FIFO on,
     * LSR bit 5 only sets when ALL 16 slots are empty, which turns both
     * our DRR status bit and the transmit wait into "wait for the whole
     * FIFO to drain".  16450 semantics (THRE = room for one byte) is what
     * MIDI pacing wants, and we never use RX at all. */
    if (!g_synth) outportb(g_uart + (2 << ush), 0x00);   /* no UART in synth mode */

    /* _CRT0_FLAG_LOCK_MEMORY already locked the image; lock the handler code
     * and the facade state again explicitly so a future build that drops the
     * crt0 flag still cannot fault inside a trap. */
    {
        __dpmi_meminfo m;
        m.address = __djgpp_base_address + (unsigned long)&g_ds_st;
        m.size    = (unsigned long)mpushim_code_end - (unsigned long)&g_ds_st;
        __dpmi_lock_linear_region(&m);   /* every handler and its .text data */
        m.address = __djgpp_base_address + (unsigned long)&g_uart;
        m.size    = 64;                  /* the facade state */
        __dpmi_lock_linear_region(&m);
    }

    /* The trace block lives in DOS memory so it can be read from outside
     * this client entirely - COMrade dumps it straight out of conventional
     * memory while the game is still running. */
    if (trace) {
        int sel, seg = __dpmi_allocate_dos_memory((8192 + 16 + 15) >> 4, &sel);
        if (seg > 0) {
            unsigned long lin = (unsigned long)seg << 4;
            unsigned i;
            for (i = 0; i < (8192 + 16) / 4; i++) _farpokel(_dos_ds, lin + i * 4, 0);
            _farpokel(_dos_ds, lin, 0x4352544DUL);      /* 'MTRC' */
            g_tr_lin = lin;
            g_dosds  = _dos_ds;
            g_trace  = 1;
        }
    }

    outs("MPUSHIM 0.7: MPU-401 ");
    outhex(g_data, 3);
    outs("/");
    outhex(g_stat, 3);
    if (g_synth) {
        outs(" -> synth INT 2Fh AH=");
        outhex((unsigned)(g_synth >> 8), 2);
        outs(" (PM side via DPMI 0300h)\n");
        if (!synth_present(g_synth))
            outs("          WARNING: nothing answers that id - load the"
                 " synth TSR\n");
    } else {
        outs(" -> UART ");
        outhex(g_uart, 3);
        outs("\n");
    }

    if (vd_ok) {
        vd_armed = vdpmi_install();
        if (vd_armed)
            outs("VDPMI:    armed - one trap, both worlds (V86 + protected)\n");
    }

    if (hdpmi_ok) {
        hdpmi_set_context_mode(0);
        handle = hdpmi_install(g_data, 2);      /* the two MPU ports */
        if (!handle) {
            outs("PM trap:  not armed - ports already trapped\n");
        } else {
            pm_armed = 1;
            outs("PM trap:  armed\n");
            /* The DOS/4G CLI heal is on by default and says nothing when it
             * works - only the exceptions are worth a line: the user turned
             * it off, or this HDPMI is too old to offer fn 9, which leaves
             * DOS/4GW games able to wedge (see the header comment). */
            if (nocli)
                outs("          (CLI heal off: /NOCLI)\n");
            else if (!hdpmi_set_cli(&mpushim_cli_handler))
                outs("          (CLI heal unavailable: this HDPMI has no fn 9"
                     " - DOS/4GW games may wedge)\n");
        }
    } else if (vd_ok) {
        /* VDPMI's one registration is the PM side too - nothing to say. */
    } else if (nopm) {
        outs("PM trap:  skipped (/NOPM)\n");
    } else {
        outs("PM trap:  not armed - no DPMI host (HDPMI32i -r -x)\n");
    }

    if (qpi_ok) {
        rm_armed = rm_install();
        if (rm_armed) outs("V86 trap: armed\n");
    } else if (vd_ok) {
        /* likewise the V86 side */
    } else if (norm) {
        outs("V86 trap: skipped (/NORM)\n");
    } else {
        outs("V86 trap: not armed - no QPI host (JEMM+QPIEMU or QEMM)\n");
    }

    if (!pm_armed && !rm_armed && !vd_armed) {
        outs("MPUSHIM: nothing armed - not going resident.\n");
        return 4;
    }
    if (g_trace) {
        outs("Trace:    buffer at ");
        outhex((unsigned)(g_tr_lin >> 4), 4);
        outs(":0000\n");
    }
    if (g_nofifo) outs("          (/NOFIFO: nested traps drop bytes)\n");
    if (g_ackall) outs("          (/ACKALL: every command is ACKed)\n");
    outs("MPUSHIM: resident.\n");

    /* Go resident, the vsbhda way: give DOS back everything but the PSP.
     * Our resident half is pure protected-mode - it never uses the stub,
     * the transfer buffer or a DOS handle again - so the environment block
     * is freed, handles closed, the transfer buffer disowned, and DJGPP's
     * exception handlers come off (they belong to a program that is about
     * to stop running; a later client's fault must not land in them). */
    psp = _go32_info_block.linear_address_of_original_psp;
    {
        unsigned short envsel = _farpeekw(_dos_ds, psp + 0x2C);
        if (envsel) {
            __dpmi_free_dos_memory(envsel);   /* DPMI hosts stow a selector here */
            _farpokew(_dos_ds, psp + 0x2C, 0);
        }
    }
    for (i = 0; i < 5; i++) _dos_close(i);
    __djgpp_exception_toggle();
    _go32_info_block.size_of_transfer_buffer = 0;
    __asm__ __volatile__(       /* stale selectors must not linger in fs/gs */
        "pushl $0 \n popl %%gs \n pushl $0 \n popl %%fs" ::: "memory");
    r.x.ax = 0x3100;
    r.x.dx = 0x10;              /* keep only the PSP: the V86 blob lives in
                                 * its own DOS block, owned by this client */
    r.x.ss = r.x.sp = 0;
    __dpmi_simulate_real_mode_interrupt(0x21, &r);
    return 0;                          /* not reached */
}
