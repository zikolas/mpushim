;==========================================================================
;  MPUSHM16 - the 16-BIT protected-mode half of MPUSHIM.
;
;  MPUSHIM.EXE is a 32-bit DJGPP program, so it can only ever register with
;  the DPMI host that serves 32-bit clients.  HDPMI16 and HDPMI32 are
;  SEPARATE hosts built from the same source with ?32BIT=0 / ?32BIT=1
;  (HDPMI16I.MAK vs HDPMI32I.MAK), and int 2Fh/31h AX=168Ah returns the API
;  of the host serving the CALLING client - so a 16-bit DPMI game (Borland
;  RTM titles such as Tyrian, Phar Lap 286 and Rational DOS/16M programs)
;  can only be trapped by a client that is itself 16-bit.  That is this
;  program: a plain .COM that becomes a 16-bit DPMI client, installs the
;  port trap through HDPMI16i's vendor API with FAR16 handlers, locks
;  itself down and stays resident.  Games are then started normally.
;
;      MPUSHM16 [/UART=250] [/MPU=330]
;
;  Prereq: HDPMI16I resident (-r -x).  Both HDPMI16i and HDPMI32i may be
;  installed at once (they share the TLB; HDPMI.TXT recommends installing
;  HDPMI16 first), so this can sit alongside MPUSHIM.EXE and cover the
;  16-bit and 32-bit catalogues together.  On a Pentium, VDPMI covers all
;  three worlds by itself and neither is needed - but VDPMI is Pentium-only
;  and the 386/486 fleet has no other route to a 16-bit PM game.
;
;  HANDLER CONTRACT (read out of the HDPMI source, Src/HDPMI/HDPMI.ASM:
;  the trap proc is parked in pseudo exception vector 20h and dispatched
;  through lpms_call_exc, which builds the DPMIEXC frame with EVERY DWORD
;  REPLACED BY A WORD when ?32BIT=0).  So this is exactly the 32-bit frame
;  MPUSHIM.C documents, halved:
;
;      after  push bp / mov bp,sp
;        [bp+2]  return IP     [bp+4]  return CS
;        [bp+6]  error code    [bp+8]  faulting IP   <- MUST be advanced
;        [bp+10] faulting CS   [bp+12] FLAGS
;        [bp+14] SP            [bp+16] SS
;      exit with a 16-bit RETF, SP back at the error code.
;
;  Error code (HDPMIAPI.TXT): bits 0-2 instruction size, bit 3 string I/O,
;  bits 4-5 operand size, bit 6 "port is in bits 8-15" (else client DX),
;  bit 7 REP.  The handler runs at ring 3 with the REAL IF clear, on the
;  host's 4K locked stack.  An IN returns its byte in the client's AL, so
;  we patch the saved AX slot before popping.
;
;  THE RULES, all of them learned on hardware and all of them the same as
;  the 32-bit side (see MPUSHIM.C):
;    - ADVANCE THE FAULTING IP ON EVERY PATH, bail-outs included, or the
;      same instruction traps for ever;
;    - NEVER DROP A MIDI BYTE: the transmit waits for THRE and gives up
;      only if the UART is dead.  Safe here because HDPMI dispatches with
;      interrupts off - nothing can preempt this handler;
;    - STATUS ALWAYS SAYS WRITE-READY (bit 6 = 0).  DMX-class drivers poll
;      with short timeouts and skip bytes that read busy;
;    - CS is a code selector in protected mode and therefore NOT writable,
;      so unlike the real-mode core this keeps its state in DS and fetches
;      our own DS through a CS-relative word (the client's DS is live on
;      entry and is none of our business).
;
;  (C) 2026 zikolas.  GNU General Public License v2 - see COPYING.
;  Provenance: the HDPMI vendor API and its trap-dispatch contract are from
;  Baron-von-Riedesel's published HDPMIAPI.TXT and the HX source (freeware,
;  "may be used for any purpose", (C) Japheth); DPMI 0.9 mode switching and
;  the MPU-401 / 16550 register models are published standards.
;==========================================================================
        cpu  386
        org  100h

%define VERSION '0.1'

start:  jmp  init

;--------------------------------------------------------------------------
; resident state.  Reached through DS, which the handlers load from the
; CS-relative word below - our own DS, stamped in at install time.
;--------------------------------------------------------------------------
my_ds     dw 0                       ; our data selector (CS-readable)
uart_base dw 0250h                   ; serial UART base I/O port
mpu_data  dw 0330h                   ; MPU data port
mpu_stat  dw 0331h                   ; MPU status/command port
ackpend   db 0                       ; 1 = 0FEh ACK waiting to be read
uartmode  db 0                       ; informational
drops     dw 0                       ; TX bytes dropped (UART dead)

;==========================================================================
; OUT handler - the client wrote a byte to a trapped MPU port
;==========================================================================
out_handler:
        push bp
        mov  bp, sp
        push ax
        push bx
        push cx
        push dx
        push ds
        mov  ax, cs:[my_ds]
        mov  ds, ax
        mov  cx, [bp+6]              ; error code
        mov  bx, cx
        and  bx, 7                   ; instruction size
        add  [bp+8], bx              ; ADVANCE THE FAULTING IP (every path)
        test cl, 8                   ; string I/O: absorb, never corrupt
        jnz  o_exit                  ;   the client's SI/DI/CX
        test cl, 40h                 ; port in the error code?
        jz   o_dxport
        mov  dl, ch
        xor  dh, dh
        jmp  o_haveport
o_dxport:
        mov  dx, [bp-8]              ; the client's DX
o_haveport:
        mov  ax, [bp-2]              ; the client's AX (value in AL)
        cmp  dx, [mpu_stat]
        je   o_cmd
        cmp  dx, [mpu_data]
        jne  o_exit                  ; not ours: absorb
;--- MIDI data byte -------------------------------------------------------
        mov  bl, al
        call send_bl
        test cl, 30h                 ; word/dword OUT: send AH as well
        jz   o_exit
        mov  bl, ah
        call send_bl
        jmp  o_exit
;--- command --------------------------------------------------------------
o_cmd:
        cmp  al, 0FFh                ; reset
        je   o_reset
        cmp  al, 3Fh                 ; enter UART mode
        je   o_uart
        jmp  o_exit                  ; any other command: absorb
o_uart:
        mov  byte [uartmode], 1
        jmp  o_ack
; Real MPU-401 silicon broadcasts All Notes Off when it is reset, and
; SoftMPU emulates it; kept for parity with the other two facades.  48
; bytes at wire speed is ~15ms inside this one trap, fine for an event
; this rare.  (Bench-proven NOT load-bearing for DMX, but LA-era Rolands
; ignore CC 123 anyway - see MPUSHIM.C.)
o_reset:
        mov  bh, 0B0h
o_bc:
        mov  bl, bh                  ; Bn: control change, channel n
        call send_bl
        mov  bl, 7Bh                 ; CC 123: all notes off
        call send_bl
        mov  bl, 0
        call send_bl
        inc  bh
        cmp  bh, 0C0h
        jne  o_bc
o_ack:
        mov  byte [ackpend], 1
o_exit:
        pop  ds
        pop  dx
        pop  cx
        pop  bx
        pop  ax
        pop  bp
        retf

;==========================================================================
; IN handler - the client read a byte from a trapped MPU port
;==========================================================================
in_handler:
        push bp
        mov  bp, sp
        push ax
        push bx
        push cx
        push dx
        push ds
        mov  ax, cs:[my_ds]
        mov  ds, ax
        mov  cx, [bp+6]
        mov  bx, cx
        and  bx, 7
        add  [bp+8], bx              ; ADVANCE THE FAULTING IP (every path)
        mov  bl, 80h                 ; default: no data, write-ready
        test cl, 8                   ; string I/O: absorb
        jnz  i_ret
        test cl, 40h
        jz   i_dxport
        mov  dl, ch
        xor  dh, dh
        jmp  i_haveport
i_dxport:
        mov  dx, [bp-8]
i_haveport:
        cmp  dx, [mpu_stat]
        je   i_stat
        cmp  dx, [mpu_data]
        je   i_data
        mov  bl, 0FFh                ; open bus for anything else
        jmp  i_ret
; Status ALWAYS reports write-ready (bit 6 = 0); data-ready (bit 7 = 0)
; only while the ACK is pending, which is what kills the drain-loop wedge.
i_stat:
        mov  bl, 80h
        cmp  byte [ackpend], 0
        je   i_ret
        xor  bl, bl                  ; ACK pending: data ready
        jmp  i_ret
i_data:
        xor  bl, bl
        cmp  byte [ackpend], 0
        je   i_ret
        mov  byte [ackpend], 0
        mov  bl, 0FEh                ; the ACK the game is waiting for
i_ret:
        mov  [bp-2], bl              ; patch saved AX low byte = our AL
        pop  ds
        pop  dx
        pop  cx
        pop  bx
        pop  ax                      ; AL is now the returned value
        pop  bp
        retf

;==========================================================================
; BL -> the UART transmitter.  Waits for THRE with a bound of several byte
; times and drops only if the UART is genuinely dead.  Preserves AX/CX/DX
; so the handlers can keep the error code in CX across a call.
;==========================================================================
send_bl:
        push ax
        push cx
        push dx
        mov  dx, [uart_base]
        add  dx, 5                   ; LSR
        mov  cx, 800                 ; >> one byte time; guards dead hw
sb_wait:
        in   al, dx
        test al, 20h                 ; THRE: room for a byte?
        jnz  sb_go
        loop sb_wait
        inc  word [drops]
        jmp  sb_done
sb_go:
        mov  dx, [uart_base]         ; THR
        mov  al, bl
        out  dx, al
sb_done:
        pop  dx
        pop  cx
        pop  ax
        ret

;==========================================================================
;                            installer (real mode)
;==========================================================================
switch_ptr:
switch_off dw 0
switch_seg dw 0
api_ptr:
api_off    dw 0
api_sel    dw 0
priv_para  dw 0
hostflags  dw 0                       ; BX from int 2Fh AX=1687h

trapprocs:                           ; DS:SI -> this for vendor fn 6
tp_in_off  dw 0                      ; FARPROC is FAR16 for a 16-bit client
tp_in_sel  dw 0
tp_out_off dw 0
tp_out_sel dw 0

hdpmi_sig  db 'HDPMI',0

rmregs:    times 50 db 0             ; DPMI 0300h real-mode call structure

m_banner   db 'MPUSHM16 ',VERSION,' - MPU-401 facade for 16-bit DPMI games',13,10,'$'
m_nodpmi   db 'No DPMI host. Load HDPMI16I -r -x first.',13,10,'$'
m_no32     db 'This host serves 32-bit clients; HDPMI16I is the 16-bit one.',13,10,'$'
m_nomem    db 'Not enough memory for the DPMI host data.',13,10,'$'
m_nopm     db 'Could not enter protected mode.',13,10,'$'
m_noapi    db 'No HDPMI vendor API (is this HDPMI16i, the IOPL-0 build?).',13,10,'$'
m_notrap   db 'Trap refused - ports already trapped, or no free range.',13,10,'$'
m_ok       db 'PM trap: armed. Resident.',13,10,'$'
m_help     db 'MPUSHM16 [/UART=250] [/MPU=330]',13,10
           db '  Traps the MPU ports for 16-BIT DPMI clients (Borland RTM,',13,10
           db '  Phar Lap 286, DOS/16M).  Needs HDPMI16I -r -x resident.',13,10
           db '  32-bit DOS4GW games are the job of MPUSHIM.EXE; both may',13,10
           db '  be resident together.',13,10,'$'

;--- print DS:DX ----------------------------------------------------------
say:
        mov  ah, 9
        int  21h
        ret

;--- parse a hex number at DS:SI into AX (stops at the first non-hex) -----
gethex:
        xor  ax, ax
gh_loop:
        mov  bl, [si]
        cmp  bl, '0'
        jb   gh_done
        cmp  bl, '9'
        jbe  gh_digit
        and  bl, 0DFh                ; upper-case
        cmp  bl, 'A'
        jb   gh_done
        cmp  bl, 'F'
        ja   gh_done
        sub  bl, 'A'-10
        jmp  gh_add
gh_digit:
        sub  bl, '0'
gh_add:
        shl  ax, 4
        xor  bh, bh
        add  ax, bx
        inc  si
        jmp  gh_loop
gh_done:
        ret

init:
        mov  sp, stack_top           ; stack inside the image, so the
                                     ; shrink below cannot cut it off
        mov  dx, m_banner
        call say

;--- command line ---------------------------------------------------------
        mov  si, 81h
cl_loop:
        lodsb
        cmp  al, 13
        je   cl_done
        cmp  al, '/'
        je   cl_opt
        cmp  al, '-'
        je   cl_opt
        or   al, al
        jz   cl_done
        jmp  cl_loop
cl_opt:
        mov  al, [si]
        and  al, 0DFh
        cmp  al, 'U'                 ; /UART=
        je   cl_uart
        cmp  al, 'M'                 ; /MPU=
        je   cl_mpu
        mov  dx, m_help              ; anything else: help and quit
        call say
        mov  ax, 4C00h
        int  21h
cl_uart:
        add  si, 5                   ; past "UART="
        call gethex
        mov  [uart_base], ax
        jmp  cl_loop
cl_mpu:
        add  si, 4                   ; past "MPU="
        call gethex
        mov  [mpu_data], ax
        inc  ax
        mov  [mpu_stat], ax
        jmp  cl_loop
cl_done:

;--- is there a DPMI host, and is it the 16-bit one? ----------------------
        mov  ax, 1687h
        int  2Fh
        or   ax, ax
        jnz  e_nodpmi
        mov  [switch_off], di
        mov  [switch_seg], es
        mov  [priv_para], si
        mov  [hostflags], bx

;--- give the rest of conventional memory back, then take what the host
;--- needs for its private data ------------------------------------------
        mov  bx, (stack_top - $$ + 010Fh) / 16
        mov  ah, 4Ah
        push ds
        pop  es                      ; ES = our PSP
        int  21h
        jc   e_nomem
        mov  bx, [priv_para]
        or   bx, bx
        jz   pm_switch
        mov  ah, 48h
        int  21h
        jc   e_nomem
        mov  es, ax
pm_switch:
        mov  ax, 0                   ; bit 0 clear = a 16-BIT client
        call far [switch_ptr]
        jc   e_nopm
;==========================================================================
;                    from here on we are in 16-bit protected mode
;==========================================================================
        mov  ax, ds
        mov  [my_ds], ax             ; the handlers fetch this through CS

;--- HDPMI vendor API: documented on int 31h, but int 2Fh is what answers
;--- for our 32-bit half, so try both -------------------------------------
        mov  ax, 168Ah
        mov  si, hdpmi_sig
        push ds
        pop  es
        xor  di, di
        int  31h
        or   al, al
        jz   api_got
        mov  ax, 168Ah
        mov  si, hdpmi_sig
        push ds
        pop  es
        xor  di, di
        int  2Fh
        or   al, al
        jnz  e_noapi
api_got:
        mov  [api_off], di
        mov  [api_sel], es
        mov  ax, es
        or   ax, di
        jz   e_noapi

;--- install the trap: fn 6, DS:SI -> two FAR16 procs --------------------
        mov  ax, cs
        mov  [tp_in_sel], ax
        mov  [tp_out_sel], ax
        mov  word [tp_in_off], in_handler
        mov  word [tp_out_off], out_handler
        mov  si, trapprocs
        mov  dx, [mpu_data]
        mov  cx, 2                   ; the two MPU ports
        mov  ax, 6
        call far [api_ptr]
        jc   e_notrap

;--- lock the image: a trap can land at any time, including while another
;--- client owns the machine, so none of it may ever be absent -----------
        mov  ax, 0006h               ; get segment base address
        mov  bx, ds
        int  31h
        jc   lock_done               ; not fatal; carry on
        mov  bx, cx                  ; BX:CX = linear address
        mov  cx, dx
        xor  si, si
        mov  di, stack_top
        mov  ax, 0600h               ; lock linear region
        int  31h
lock_done:

        mov  dx, m_ok
        call say

;--- go resident.  Ask the host to run the real-mode TSR call, exactly as
;--- the 32-bit half does (__dpmi_simulate_real_mode_interrupt) ----------
        mov  word [rmregs+28], 3100h         ; EAX = AH 31h, AL 00
        mov  word [rmregs+20], (stack_top - $$ + 010Fh) / 16   ; EDX = paragraphs kept
        mov  ax, 0300h
        mov  bx, 0021h               ; BL = int 21h, BH = 0
        xor  cx, cx
        push ds
        pop  es
        mov  di, rmregs
        int  31h
;--- not reached ---------------------------------------------------------
        mov  ax, 4C00h
        int  21h

e_nodpmi:
        mov  dx, m_nodpmi
        jmp  e_exit
e_nomem:
        mov  dx, m_nomem
        jmp  e_exit
e_nopm:
        mov  dx, m_nopm
        call say
        test byte [hostflags], 1     ; host advertises 32-bit clients?
        jz   e_bail                  ; then it is probably HDPMI32i
        mov  dx, m_no32
        jmp  e_exit
e_noapi:
        mov  dx, m_noapi
        jmp  e_exit
e_notrap:
        mov  dx, m_notrap
e_exit:
        call say
e_bail:
        mov  ax, 4C01h
        int  21h

        align 2
stack_bottom:
        times 256 dw 0
stack_top:
