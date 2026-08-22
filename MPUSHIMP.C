/* MPUSHIMP - protected-mode companion to MPUSHIM: an MPU-401 (UART mode)
 * facade for DOS games that run as 32-bit DPMI clients (DOS4GW & friends)
 * and do their MPU I/O from ring 3, which the real-mode QPI trap that
 * MPUSHIM.COM uses cannot see.
 *
 * It traps the MPU ports through HDPMI32i's documented I/O-trap API and runs
 * the same facade as MPUSHIM.COM: a correct MPU-401 UART-mode status byte,
 * the 0FEh reset/UART-mode ACK, and MIDI data forwarded to a real serial
 * UART (the EXP GAME/MIDI G3's hidden UART at 250h, or a COM port + an
 * MPU-232 dongle).
 *
 *     MPUSHIMP [/UART=250] [/MPU=330] [/DIV=n] [/NOTX] [/NOCLI] [/IF]
 *
 * It installs the trap and stays resident; games are then started NORMALLY.
 * Prereqs: HDPMI32i resident as the DPMI host (-r -x), and the UART already
 * brought up (EXPG3GO /PCIC for the G3; a COM port needs no enabler).
 * Build: ./build-pm.sh (DJGPP).  Remove: reboot.  MIT (c) 2026 zikolas.
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
 *    (DPMI 0901h) instead of letting them latch off.  /NOCLI disables it
 *    for bisecting.  PROVENANCE NOTE: the fn-9 ABI and the IOPL-0 POPFD
 *    behaviour are published (HDPMIAPI.TXT, Intel SDM); the idea of keying
 *    the heal on the PUSHFD idiom appears in vsbhda (GPL) - this file's
 *    implementation is our own, but do not publish before Nick rules on
 *    that provenance.
 *
 * Clean-room: the HDPMI port-trap ABI is from Baron-von-Riedesel's published
 * HDPMIAPI.TXT (AX=168Ah "HDPMI" -> API entry; fn 5 context mode, fn 6
 * install {in,out FARPROCs}, fn 7 remove, fn 9 CLI/STI trap; the error-code
 * bit layout; the handler is called "like an exception handler proc" and
 * MUST advance the faulting EIP).  The MPU-401 UART-mode and 16550 register
 * models are published hardware standards.  No third-party source copied.
 */

#include <dpmi.h>
#include <go32.h>
#include <crt0.h>
#include <pc.h>
#include <dos.h>
#include <unistd.h>
#include <sys/farptr.h>
#include <sys/exceptn.h>

/* Lock every page of this program at startup and keep sbrk from moving the
 * image.  The trap handler can be entered at any time - including while
 * another DPMI client owns the machine - so none of it may ever be absent. */
int _crt0_startup_flags = _CRT0_FLAG_LOCK_MEMORY | _CRT0_FLAG_NONMOVE_SBRK;

/* ---- facade state, shared with the asm handlers ------------------------ */
volatile unsigned short g_uart = 0x250;  /* serial UART base I/O port       */
volatile unsigned short g_data = 0x330;  /* MPU data port                   */
volatile unsigned short g_stat = 0x331;  /* MPU status/command port         */
volatile unsigned char  g_ack  = 0;      /* 1 = 0FEh ACK waiting to be read */
volatile unsigned char  g_busy = 0;      /* 1 = a handler is already active */
volatile unsigned char  g_notx = 0;      /* /NOTX: answer, but never touch the UART */
volatile unsigned char  g_nobc = 0;      /* /NOBC: skip the all-notes-off broadcast on FFh */
volatile unsigned char  g_forceif = 0;   /* /IF: force IF set in the resumed EFLAGS */

extern unsigned short   g_ds_st;         /* our DS - lives IN .text (see below) */
extern void mpushim_out_handler(void);
extern void mpushim_in_handler(void);
extern void mpushim_cli_handler(void);

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
 * FFh and SoftMPU emulates it, so it stays - but the 2026-08-22 /NOBC
 * regression showed it is NOT load-bearing for DOOM/DMX: DMX sends its
 * own note-offs at song stop, GM gear (Yamaha QY70) honours them, and
 * the CM-32L's droning note across DOOM's track changes persists WITH
 * the broadcast because LA-era Rolands ignore this style of silencing
 * altogether.  Curing that would take SoftMPU's approach - track active
 * notes and send per-note offs on reset (v0.3 candidate).  48 bytes at
 * wire speed is ~15ms inside this one trap, fine for an event this rare. */
"o_reset:                                                           \n"
"   cmp  byte ptr [_g_nobc], 0 # /NOBC: ACK only, no broadcast      \n"
"   jne  o_ack                                                      \n"
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
"   cmp  byte ptr [_g_forceif], 0                                    \n"
"   je   o_noif                                                      \n"
"   or   byte ptr [ebp+0x19], 2   # set IF (bit 9) in resumed EFLAGS \n"
"o_noif:                                                             \n"
"   pop  ds                                                         \n"
"   pop  edx                                                        \n"
"   pop  ecx                                                        \n"
"   pop  ebx                                                        \n"
"   pop  eax                                                        \n"
"   pop  ebp                                                        \n"
"   retf                                                            \n"
"                                                                   \n"
/* BL -> the UART transmitter.  Wait for THRE with a ~1.5-byte-time bound
 * (ECX is disposable here: the client's ECX is restored from the saved
 * frame slot at handler exit).  Real IF is 0 for the whole handler, so
 * the wait cannot be preempted; the bound only guards dead hardware.     */
"o_tx:                                                              \n"
"   push eax                                                        \n"
"   push edx                                                        \n"
"   cmp  byte ptr [_g_notx], 0 # /NOTX: swallow, touch no hardware  \n"
"   jne  o_tx_done                                                  \n"
"   mov  dx, [_g_uart]                                              \n"
"   add  dx, 5                 # LSR                                \n"
"   mov  ecx, 800              # ~1ms of ISA reads >> one byte time  \n"
"o_tx_wait:                                                         \n"
"   in   al, dx                                                     \n"
"   test al, 0x20              # THRE: room for a byte?             \n"
"   jnz  o_tx_send                                                  \n"
"   dec  ecx                                                        \n"
"   jnz  o_tx_wait                                                  \n"
"   jmp  o_tx_done             # UART dead: drop, never hang        \n"
"o_tx_send:                                                         \n"
"   mov  dx, [_g_uart]         # THR                                \n"
"   mov  al, bl                                                     \n"
"   out  dx, al                                                     \n"
"o_tx_done:                                                         \n"
"   pop  edx                                                        \n"
"   pop  eax                                                        \n"
"   ret                                                             \n"
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
"   cmp  byte ptr [_g_forceif], 0                                    \n"
"   je   i_noif                                                      \n"
"   or   byte ptr [ebp+0x19], 2   # set IF (bit 9) in resumed EFLAGS \n"
"i_noif:                                                             \n"
"   pop  ds                                                         \n"
"   pop  edx                                                        \n"
"   pop  ecx                                                        \n"
"   pop  ebx                                                        \n"
"   pop  eax                   # AL is now the returned value       \n"
"   pop  ebp                                                        \n"
"   retf                                                            \n"
"                                                                   \n"
/* --------- CLI trap: heal the IOPL-0 PUSHFD/CLI...POPFD hole ----------- *
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
"   .att_syntax prefix                                              \n"
);

/* ---- HDPMI vendor API -------------------------------------------------
 * AX=168Ah with DS:ESI -> "HDPMI" returns AL=0 and the API entry in ES:EDI.
 * (int 2Fh is the call that answers here; int 31h does not.)  The entry is
 * then reached by an lcall through a 16:32 far pointer. */
static struct { unsigned long off; unsigned short sel; } __attribute__((packed)) hdpmi_entry;

static int get_hdpmi(void)
{
    unsigned char ok = 1;
    unsigned short sel = 0;
    unsigned long off = 0;
    static const char sig[] = "HDPMI";
    const char *psig = sig;
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
        : "r"(psig)
        : "eax", "esi", "edi");
    if (ok != 0) return 0;
    hdpmi_entry.off = off;
    hdpmi_entry.sel = sel;
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

/* ---- optional UART (re)program: 8N1, given divisor, FIFOs on ----------- */
static void uart_setdiv(unsigned base, unsigned div)
{
    outportb(base + 3, 0x80);            /* DLAB */
    outportb(base + 0, div & 0xFF);
    outportb(base + 1, (div >> 8) & 0xFF);
    outportb(base + 3, 0x03);            /* 8N1 */
    outportb(base + 4, 0x00);            /* MCR = 0 */
    outportb(base + 1, 0x00);            /* IER = 0 */
    outportb(base + 2, 0x00);            /* FCR: FIFOs OFF (see below) */
}

int main(int argc, char **argv)
{
    unsigned long handle;
    unsigned long psp;
    unsigned div = 0;
    int nocli = 0;
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
        else if (keymatch(a, "NOTX"))  g_notx = 1;
        else if (keymatch(a, "NOCLI")) nocli = 1;
        else if (keymatch(a, "NOBC"))  g_nobc = 1;
        else if (keymatch(a, "IF"))    g_forceif = 1;
        else {
            outs("MPUSHIMP - protected-mode MPU-401 facade over a serial UART\r\n"
                 "  MPUSHIMP [/UART=250] [/MPU=330] [/DIV=n] [/NOTX] [/NOCLI]\r\n"
                 "  /NOTX  = diagnostic: answer the handshake, send no MIDI\r\n"
                 "  /NOCLI = diagnostic: skip the DOS4G PUSHFD/CLI/POPFD heal\r\n"
                 "  /NOBC  = diagnostic: no all-notes-off broadcast on reset\r\n"
                 "  /IF    = diagnostic: force interrupts on at trap return\r\n"
                 "Installs the trap and stays resident; then start games normally.\r\n"
                 "Needs HDPMI32i resident (-r -x) and the UART already up.\r\n");
            return 0;
        }
    }
    g_stat = g_data + 1;

    if (!get_hdpmi()) {
        outs("MPUSHIMP: HDPMI vendor API not found - is HDPMI32i loaded (-r -x)?\r\n");
        return 2;
    }
    __asm__ __volatile__("movw %%ds, %0" : "=m"(g_ds_st));
    if (div) uart_setdiv(g_uart, div);

    /* FIFOs OFF regardless of who programmed the UART: with the FIFO on,
     * LSR bit 5 only sets when ALL 16 slots are empty, which turns both
     * our DRR status bit and the transmit wait into "wait for the whole
     * FIFO to drain".  16450 semantics (THRE = room for one byte) is what
     * MIDI pacing wants, and we never use RX at all. */
    if (!g_notx) outportb(g_uart + 2, 0x00);

    /* _CRT0_FLAG_LOCK_MEMORY already locked the image; lock the handler code
     * and the facade state again explicitly so a future build that drops the
     * crt0 flag still cannot fault inside a trap. */
    {
        __dpmi_meminfo m;
        m.address = (unsigned long)&mpushim_out_handler;
        m.size    = 2048;                /* all three handlers + g_ds_st */
        __dpmi_lock_linear_region(&m);
        m.address = (unsigned long)&g_uart;
        m.size    = 64;
        __dpmi_lock_linear_region(&m);
    }

    hdpmi_set_context_mode(0);
    handle = hdpmi_install(g_data, 2);          /* the two MPU ports */
    if (!handle) {
        outs("MPUSHIMP: HDPMI trap install failed (ports already trapped?).\r\n");
        return 4;
    }

    outs("MPUSHIMP: MPU-401 ");
    outhex(g_data, 3);
    outs("/");
    outhex(g_stat, 3);
    outs(" -> UART ");
    outhex(g_uart, 3);
    outs(", trap ");
    outhex((unsigned)handle, 8);
    if (g_notx) outs(" [NOTX: no MIDI will be sent]");
    if (g_nobc) outs(" [NOBC: no reset broadcast]");
    if (g_forceif) outs(" [IF: forcing interrupts on]");
    if (nocli)
        outs(" [NOCLI]");
    else if (hdpmi_set_cli(&mpushim_cli_handler))
        outs(" [DOS4G CLI fix]");
    else
        outs(" [no fn9: CLI fix unavailable]");
    outs("\r\nMPUSHIMP: resident - start your game normally.\r\n");

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
    r.x.dx = 0x10;              /* keep only the PSP in conventional memory */
    r.x.ss = r.x.sp = 0;
    __dpmi_simulate_real_mode_interrupt(0x21, &r);
    return 0;                          /* not reached */
}
