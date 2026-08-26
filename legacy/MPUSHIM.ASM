;==========================================================================
;  MPUSHIM 0.1 - a resident MPU-401 (UART mode) facade over a plain serial
;  UART, for DOS games that expect an MPU-401 at 330h.
;
;  Many PCMCIA "MIDI" cards (e.g. the EXP GAME/MIDI G3) expose only a raw
;  16550-class UART, with NO MPU-401 protocol engine.  A DOS MIDI PLAYER
;  (DOSMid /com=) drives such a UART directly, but GAMES speak the MPU-401
;  handshake - reset command, the 0FEh ACK, and a status port whose bits
;  they poll - and a bare UART answers none of that.  Worse, a game's
;  "drain pending input" loop spins forever against a status port that
;  never says "no data", so the game hangs on a blank screen before it
;  ever draws.
;
;  MPUSHIM sits between the two.  It traps the MPU I/O ports (330h/331h)
;  through the QEMM-compatible QPI port-trap interface - the SAME API that
;  QEMM386, Jemm's QPIEMU, and crazii's VDPMI all provide - and:
;    - presents a correct MPU-401 UART-mode STATUS byte (so the drain
;      loop terminates and write-ready polling works),
;    - answers the reset/UART-mode commands with the 0FEh ACK,
;    - forwards outgoing MIDI data bytes to the real UART's transmitter,
;      paced on the Line Status Register.
;
;  Because it rides the QPI interface it needs one of those hosts loaded
;  (VDPMI is the modern choice: a DPMI server + V86 monitor in one, no
;  JEMM/HDPMI stack).  It is a plain real-mode DOS TSR - it does NOT need
;  to be a protected-mode DPMI client itself, which keeps it small and
;  robust.  It covers V86-mode (real-mode) games; that is exactly the
;  MPU-401 era (SCUMM/iMUSE, Sierra SCI, Miles).
;
;  Usage:
;    MPUSHIM [/UART=250] [/MPU=330] [/DIV=24] [/U] [/?]
;      /UART=hex  base I/O port of the serial UART (default 250h - where
;                 EXPG3GO leaves the EXP G3's MIDI UART natively)
;      /MPU=hex   MPU-401 base the game expects (default 330h; the status/
;                 command port is base+1)
;      /DIV=dec   if given, (re)program the UART divisor for 31250 baud
;                 (the G3's ~12 MHz UART wants 24); omit to leave the UART
;                 exactly as the enabler set it
;      /U         unload a resident MPUSHIM
;      /?         help
;
;  Prereqs (EXP G3 example):  VDPMI resident, then
;      EXPG3GO /PCIC            (UART native at 250h - do NOT use /MPU:
;                               the 330h presence is MPUSHIM's job now)
;      MPUSHIM                  (traps 330h/331h -> UART at 250h)
;
;  Build:  nasm -f bin MPUSHIM.ASM -o MPUSHIM.COM
;
;  (C) 2026 zikolas.  GNU General Public License v2 - see ../COPYING.
;  Clean-room: the QPI port-trap ABI is the public
;  QEMM programming interface (int 67h AX=3F00h entry; far-call functions
;  1A06h get / 1A07h set trap handler, 1A08h status, 1A09h/1A0Ah trap/
;  untrap port; real-mode handler contract DX=port, CL bit2=OUT, AL=value),
;  and the MPU-401 UART-mode + 16550 register models are published
;  hardware standards.  No third-party source was copied.
;==========================================================================
        cpu  386
        org  100h

section .text
        jmp  init                     ; 3 bytes; sig lands at 103h

;==========================================================================
;  RESIDENT DATA + CODE  (kept after TSR)
;==========================================================================
; installation signature at a fixed offset (103h) so /U can recognise a
; resident copy and read its saved state out of the resident segment.
sig       db 'MSHM'                   ; @103
sigver    dw 0001h                    ; @107

; --- resident state (accessed CS-relative from the trap handler) ---------
oldtrap   dd 0                        ; previous QPI trap handler (far)
qpient    dd 0                        ; QPI entry point (far), for /U
uart_base dw 0250h                    ; serial UART base I/O port
mpu_data  dw 0330h                    ; MPU data port
mpu_stat  dw 0331h                    ; MPU status/command port
ackpend   db 0                        ; 1 = 0FEh ACK waiting to be read
uartmode  db 0                        ; 1 = UART mode entered (informational)
drops     dw 0                        ; TX bytes dropped (UART stayed full)
lsr_off   dw 5                        ; LSR offset from uart_base. 5 for an
                                      ; ordinary UART; 10 where the registers
                                      ; are spaced 2 bytes apart (/STRIDE=2),
                                      ; as on the TDK DMC-8000's onboard 16550
                                      ; at 320h. Precomputed at install so the
                                      ; trap handler stays a single ADD.
synthax   dw 0                        ; MIDI sink: 0 = the UART, else the AX
                                      ; for INT 2Fh - AH a resident software
                                      ; synth's multiplex id, AL its "MIDI
                                      ; byte in DL" function (/SYNTH)

;==========================================================================
;  trap_handler - the QPI real-mode port-trap callback.
;
;  Entry (QEMM QPI real-mode handler contract, see PTRAP.C / rmcode1.asm):
;    DX    = port being accessed
;    CL    bit 2 (04h) : 1 = OUT (write), 0 = IN (read)
;          bits 3-5 (38h): word/dword/string access (0 = byte)
;    AL    = value, for an OUT
;  Return:
;    RETF.  For an IN, AL = the byte to hand back to the guest.
;    To decline (let the underlying handler/hardware see it): jump to the
;    saved old handler.
;
;  Runs in the guest's V86 context with UNKNOWN DS/SS - every memory
;  reference is CS-relative.  Preserves every register except AL on an IN
;  (the ABI's return path).
;==========================================================================
trap_handler:
        ; A byte access to one of our two ports is the only thing that can
        ; arrive here (we trapped exactly 330h/331h).  There is no real MPU
        ; hardware behind them, so anything we don't model - including the
        ; nonsensical word/dword access - is simply absorbed (RETF), never
        ; chained (the prior QPI handler may be null, and passing a phantom
        ; MPU port to hardware is wrong regardless).
        test cl, 38h                  ; word/dword/string access?
        jnz  .absorb                  ; not a byte MPU access - eat it
        cmp  dx, cs:[mpu_data]
        je   .data
        cmp  dx, cs:[mpu_stat]
        je   .stat
.absorb:
        retf

; ---- MPU data port (330h) -----------------------------------------------
.data:
        test cl, 4
        jz   .data_in
        ; OUT: a MIDI data byte -> the UART transmitter
        jmp  .send
.data_in:
        ; IN: hand back the ACK if one is pending, else nothing (0).
        test byte cs:[ackpend], 1
        jz   .din_zero
        mov  byte cs:[ackpend], 0
        mov  al, 0FEh
        retf
.din_zero:
        xor  al, al
        retf

; ---- MPU status/command port (331h) -------------------------------------
.stat:
        test cl, 4
        jz   .stat_in
        ; OUT: an MPU command byte.
        cmp  al, 0FFh                 ; reset
        je   .cmd_ack
        cmp  al, 3Fh                  ; enter UART mode
        je   .cmd_uart
        retf                          ; other commands: absorb (ignore)
.cmd_uart:
        mov  byte cs:[uartmode], 1
.cmd_ack:
        mov  byte cs:[ackpend], 1     ; both reset and 3Fh queue the FEh ACK
        retf
.stat_in:
        ; IN: MPU-401 status byte.
        ;   bit 7 (80h): 0 = a data byte is ready to READ; 1 = none.
        ;   bit 6 (40h): 0 = ready to WRITE a command/data byte; 1 = busy.
        ; We are always write-ready (bit6=0).  Data-ready (bit7=0) only
        ; while the ACK is pending - so a guest's "drain input" loop, which
        ; reads data WHILE bit7=0, terminates instead of spinning forever.
        mov  al, 80h                  ; idle: no data, write-ready
        test byte cs:[ackpend], 1
        jz   .st_done
        xor  al, al                   ; ACK pending: data-ready + write-ready
.st_done:
        retf

; ---- forward AL to the UART transmitter, paced on LSR THRE ---------------
.send:
        push ax
        push bx
        push cx
        push dx
        mov  bl, al                   ; the MIDI byte
        mov  ax, cs:[synthax]         ; a resident synth instead of a UART?
        test ax, ax
        jnz  .send_synth
        mov  dx, cs:[uart_base]
        add  dx, cs:[lsr_off]         ; LSR
        mov  cx, 0FFFFh               ; bounded spin (~ one byte-time margin)
.send_wait:
        in   al, dx
        test al, 20h                  ; THRE: room in the TX holding/FIFO?
        jnz  .send_go
        loop .send_wait
        inc  word cs:[drops]          ; UART never drained - drop (no hang)
        jmp  .send_done
.send_go:
        mov  dx, cs:[uart_base]       ; THR
        mov  al, bl
        out  dx, al
.send_done:
        pop  dx
        pop  cx
        pop  bx
        pop  ax
        retf

; ---- hand the byte to a resident INT 2Fh software synth instead ----------
; AX is already synthax.  .send has saved AX/BX/CX/DX; the callee synthesises
; the note on the guest's stack and is only contracted to preserve what a
; multiplex handler normally does, so save the rest as well.
.send_synth:
        mov  dl, bl
        push si
        push di
        push bp
        push ds
        push es
        int  2Fh
        pop  es
        pop  ds
        pop  bp
        pop  di
        pop  si
        jmp  .send_done

resident_end:                         ; everything below is discarded on TSR

;==========================================================================
;  INIT  (transient)
;==========================================================================
init:
        cld
        mov  si, m_banner
        call prstr

        call parse_args               ; sets uart_base/mpu_*/flags
        cmp  byte [f_help], 0
        je   .nohelp
        mov  si, m_help
        call prstr
        int  20h
.nohelp:

        ; derive the status port from the MPU base
        mov  ax, [mpu_data]
        inc  ax
        mov  [mpu_stat], ax

        call find_qpi                 ; -> qpient (far), or error
        cmp  word [qpient+2], 0
        jne  .haveqpi
        mov  si, m_noqpi
        call errout
.haveqpi:

        cmp  byte [f_unload], 0
        je   .install
        jmp  do_unload

.install:
        ; Publish the LSR offset the trap handler will use, so the hot path is
        ; one ADD rather than a shift per byte.
        push cx
        mov  cl, [ushift]
        mov  ax, 5
        shl  ax, cl
        mov  [lsr_off], ax
        pop  cx

        ; A synth sink has no UART behind it, so neither the divisor program
        ; nor the LSR probe applies - and both would poke an I/O port that
        ; on this machine may belong to something else entirely.
        cmp  word [synthax], 0
        jne  .nouart

        ; optional divisor (re)program
        cmp  byte [f_div], 0
        je   .nodiv
        call uart_setdiv
.nodiv:
        ; sanity: is the UART actually decoding?  LSR of a dead/absent port
        ; floats FFh.  Warn but continue (the game test is the real check).
        mov  al, 5                    ; LSR, wherever the stride puts it
        call uart_reg
        in   al, dx
        mov  [r_lsr], al
        cmp  al, 0FFh
        jne  .live
        mov  si, m_deaduart
        call prstr
.live:
.nouart:

        ; --- get + save the current QPI trap handler --------------------
        mov  ax, 1A06h
        call far [qpient]
        jc   .qpifail
        mov  [oldtrap], di
        mov  [oldtrap+2], es

        ; --- install ours (ES:DI = trap_handler in our segment) ---------
        push cs
        pop  es
        mov  di, trap_handler
        mov  ax, 1A07h
        call far [qpient]
        jc   .qpifail

        ; --- trap the two MPU ports -------------------------------------
        mov  dx, [mpu_data]
        mov  ax, 1A09h
        call far [qpient]
        mov  dx, [mpu_stat]
        mov  ax, 1A09h
        call far [qpient]

        ; --- report -----------------------------------------------------
        mov  si, m_ok1
        call prstr
        mov  ax, [mpu_data]
        call prhex3
        mov  al, '/'
        call prchar
        mov  ax, [mpu_stat]
        call prhex3
        cmp  word [synthax], 0
        jne  .rep_synth
        mov  si, m_ok2
        call prstr
        mov  ax, [uart_base]
        call prhex3
        cmp  byte [ushift], 0
        je   .rep_end
        mov  si, m_stride2
        call prstr
        jmp  .rep_end
.rep_synth:
        mov  si, m_oksyn
        call prstr
        mov  al, [synthax+1]          ; the multiplex id
        call prhex2
.rep_end:
        mov  si, m_ok3
        call prstr

        ; --- go resident, keeping through resident_end ------------------
        mov  dx, (resident_end - $$ + 100h + 15) >> 4
        mov  ax, 3100h
        int  21h

.qpifail:
        mov  si, m_qpifail
        call errout

;==========================================================================
;  find_qpi - locate the QPI entry point.
;  Method 1: QEMM-style int 67h AX=3F00h, CX='QE', DX='MM' -> ES:DI.
;  Method 2: QPIEMU-style int 2Fh AX=1684h, BX=4354h      -> ES:DI.
;  Leaves qpient = far entry (segment 0 if not found).
;==========================================================================
find_qpi:
        mov  word [qpient], 0
        mov  word [qpient+2], 0

        ; int 67h vector present? (EMS/QEMM host)
        push es
        xor  ax, ax
        mov  es, ax
        mov  ax, [es:67h*4]
        mov  bx, [es:67h*4+2]
        pop  es
        mov  cx, ax
        or   cx, bx
        jz   .try2f
        mov  ax, 3F00h
        mov  cx, 5145h                ; 'QE'
        mov  dx, 4D4Dh                ; 'MM'
        int  67h
        or   ah, ah
        jnz  .try2f
        mov  ax, es
        or   ax, ax
        jz   .try2f
        mov  [qpient], di
        mov  [qpient+2], es
        ret
.try2f:
        mov  ax, 1684h
        mov  bx, 4354h                ; QPIEMU device id
        push es
        push di
        xor  di, di
        mov  es, di
        int  2Fh
        mov  ax, es
        mov  cx, di
        pop  di
        pop  es
        or   ax, ax
        jz   .none
        mov  [qpient], cx
        mov  [qpient+2], ax
.none:
        ret

;==========================================================================
;  do_unload - restore a resident MPUSHIM's trapping and free it.
;  Reads the CURRENT QPI trap handler; if it points at a segment carrying
;  our 'MSHM' signature, that segment is our resident copy: pull its saved
;  old handler + ports out, restore the handler, untrap the ports, free it.
;==========================================================================
do_unload:
        mov  ax, 1A06h                ; current trap handler -> ES:DI
        call far [qpient]
        jc   .fail
        mov  bx, es                   ; candidate resident segment
        ; verify signature at 103h
        cmp  word [es:103h], 'MS'
        jne  .notours
        cmp  word [es:105h], 'HM'
        jne  .notours

        ; restore the handler the resident saved (ES:DI from its oldtrap)
        push es
        mov  di, [es:oldtrap]
        mov  ax, [es:oldtrap+2]
        pop  es
        push bx                       ; keep resident seg
        mov  es, ax
        mov  ax, 1A07h
        call far [qpient]
        pop  bx

        ; untrap the two ports the resident registered
        push bx
        mov  es, bx
        mov  dx, [es:mpu_data]
        mov  ax, 1A0Ah
        call far [qpient]
        mov  es, bx                   ; call may have clobbered ES
        mov  dx, [es:mpu_stat]
        mov  ax, 1A0Ah
        call far [qpient]
        pop  bx

        ; free the resident segment
        mov  es, bx
        mov  ax, 4900h
        int  21h

        mov  si, m_unloaded
        call prstr
        int  20h
.notours:
        mov  si, m_notres
        call errout
.fail:
        mov  si, m_qpifail
        call errout

;==========================================================================
;  uart_setdiv - program [uart_base] to 8N1 at divisor [o_div], FIFOs on,
;  MCR/IER 0.  Only used when /DIV was given; normally EXPG3GO has already
;  set the UART and we leave it untouched.
;==========================================================================
; DX = uart_base + (AL << ushift), i.e. the port of logical 16550 register AL.
; Registers are one byte apart on an ordinary UART and two on a card that puts
; an 8-bit UART on a 16-bit bus, so nothing here may use INC DX to step from
; one register to the next.  Clobbers AX and CX.
uart_reg:
        push cx
        mov  cl, [ushift]
        xor  ah, ah
        shl  ax, cl
        add  ax, [uart_base]
        mov  dx, ax
        pop  cx
        ret

uart_setdiv:
        push ax
        push cx
        push dx
        mov  al, 3                    ; LCR: DLAB on
        call uart_reg
        mov  al, 80h
        out  dx, al
        mov  al, 0                    ; divisor low
        call uart_reg
        mov  al, [o_div]
        out  dx, al
        mov  al, 1                    ; divisor high = 0
        call uart_reg
        xor  al, al
        out  dx, al
        mov  al, 3                    ; LCR: 8N1, DLAB off
        call uart_reg
        mov  al, 03h
        out  dx, al
        mov  al, 4                    ; MCR = 0
        call uart_reg
        xor  al, al
        out  dx, al
        mov  al, 1                    ; IER = 0
        call uart_reg
        xor  al, al
        out  dx, al
        mov  al, 2                    ; FCR: FIFOs OFF
        call uart_reg
        ; With the FIFO enabled, LSR bit 5 only sets once the WHOLE transmit
        ; FIFO has drained, so the send loop would wait a full byte-time
        ; before every byte and halve throughput on dense MIDI. 16450
        ; semantics - THRE means room for one more byte - is what pacing
        ; wants, and we never receive.
        xor  al, al
        out  dx, al
        pop  dx
        pop  cx
        pop  ax
        ret

;==========================================================================
;  COMMAND LINE
;==========================================================================
parse_args:
        mov  si, 81h
.loop:
        lodsb
        cmp  al, 0Dh
        je   .done
        cmp  al, '/'
        je   .sw
        cmp  al, '-'
        je   .sw
        jmp  .loop
.sw:
        lodsb
        cmp  al, '?'
        je   .help
        and  al, 0DFh
        cmp  al, 'U'
        je   .u
        cmp  al, 'M'
        je   .m
        cmp  al, 'D'
        je   .d
        cmp  al, 'S'
        je   .s
        jmp  .loop
.s:                                   ; /SYNTH vs /STRIDE, on the 2nd letter
        mov  al, [si]
        and  al, 0DFh
        cmp  al, 'T'
        je   .stride
        jmp  .synth
.u:
        ; /U (unload) vs /UART=
        mov  al, [si]
        and  al, 0DFh
        cmp  al, 'A'
        je   .uart
        mov  byte [f_unload], 1
        jmp  .loop
.uart:
        call eat_eq
        call hexnum
        test dx, dx
        jz   .loop
        mov  [uart_base], dx
        jmp  .loop
.m:                                   ; /MPU=
        call eat_eq
        call hexnum
        test dx, dx
        jz   .loop
        mov  [mpu_data], dx
        jmp  .loop
.d:                                   ; /DIV=
        call eat_eq
        call decnum
        test dx, dx
        jz   .loop
        mov  [o_div], dl
        mov  byte [f_div], 1
        jmp  .loop
.stride:                              ; /STRIDE=1|2  (16550 register spacing)
        call eat_eq
        call decnum
        mov  byte [ushift], 0
        cmp  dx, 2
        jne  .loop
        mov  byte [ushift], 1
        jmp  .loop
.synth:                               ; /SYNTH[=ah]
        call eat_eq
        call hexnum
        test dx, dx
        jnz  .s_have
        mov  dx, 0BDh                 ; the default multiplex id
.s_have:
        mov  dh, dl                   ; id -> AH
        mov  dl, 01h                  ; AL = "MIDI byte in DL"
        mov  [synthax], dx
        jmp  .loop
.help:
        mov  byte [f_help], 1
.done:
        ret

; skip to after '=' or ':' (letters of the switch name are consumed)
eat_eq:
        lodsb
        cmp  al, '='
        je   .r
        cmp  al, ':'
        je   .r
        and  al, 0DFh
        cmp  al, 'A'
        jb   .back
        cmp  al, 'Z'
        jbe  eat_eq
.back:
        dec  si
.r:
        ret

hexnum:                               ; DS:SI hex -> DX (0 if none)
        xor  dx, dx
.h:
        lodsb
        and  al, 0DFh
        cmp  al, 10h                  ; '0'&DF..'9'&DF = 10h..19h
        jb   .done
        cmp  al, 19h
        jbe  .dig
        cmp  al, 'A'
        jb   .done
        cmp  al, 'F'
        ja   .done
        sub  al, 'A'-10
        jmp  .acc
.dig:
        sub  al, 10h
.acc:
        push cx
        mov  cl, 4
        shl  dx, cl
        pop  cx
        xor  ah, ah
        add  dx, ax
        jmp  .h
.done:
        dec  si
        ret

decnum:                               ; DS:SI decimal -> DX
        xor  dx, dx
.d:
        lodsb
        cmp  al, '0'
        jb   .done
        cmp  al, '9'
        ja   .done
        sub  al, '0'
        xor  ah, ah
        push ax
        mov  ax, dx
        mov  dx, 10
        mul  dx
        mov  dx, ax
        pop  ax
        add  dx, ax
        jmp  .d
.done:
        dec  si
        ret

;==========================================================================
;  OUTPUT HELPERS
;==========================================================================
prchar:
        push ax
        push dx
        mov  dl, al
        mov  ah, 2
        int  21h
        pop  dx
        pop  ax
        ret
prstr:
        push ax
.l:
        lodsb
        or   al, al
        jz   .r
        call prchar
        jmp  .l
.r:
        pop  ax
        ret
prhex3:                               ; AX as 3 hex digits (AH low nibble + AL)
        push ax
        mov  al, ah
        and  al, 0Fh
        call prnyb
        pop  ax
        call prhex2
        ret
prhex2:                               ; AL as 2 hex digits
        push ax
        push cx
        mov  cl, 4
        shr  al, cl
        call prnyb
        pop  cx
        pop  ax
        push ax
        call prnyb
        pop  ax
        ret
prnyb:
        push ax
        and  al, 0Fh
        add  al, '0'
        cmp  al, '9'
        jbe  .p
        add  al, 7
.p:
        call prchar
        pop  ax
        ret

errout:
        call prstr
        int  20h

;==========================================================================
;  TRANSIENT DATA
;==========================================================================
f_unload  db 0
f_help    db 0
f_div     db 0
o_div     db 24
r_lsr     db 0
ushift    db 0                        ; 0 = registers 1 byte apart (/STRIDE=1),
                                      ; 1 = 2 bytes apart (/STRIDE=2)

m_banner    db 'MPUSHIM 0.1 - MPU-401 facade, real-mode (V86) only',13,10,0
m_help      db 'Usage: MPUSHIM [/UART=250] [/MPU=330] [/DIV=24] [/SYNTH]',13,10
            db '               [/STRIDE=n] [/U] [/?]',13,10
            db '  /UART=hex  serial UART base (default 250h)',13,10
            db '  /MPU=hex   MPU-401 base the game expects (default 330h)',13,10
            db '  /DIV=dec   reprogram UART divisor for 31250 (G3 wants 24)',13,10
            db '  /SYNTH[=ah] send MIDI to a resident INT 2Fh software synth',13,10
            db '             instead of a UART (default id BDh)',13,10
            db '  /STRIDE=n  UART register spacing: 1 (normal) or 2, for an',13,10
            db '             8-bit UART on a 16-bit bus (TDK DMC-8000 at 320h)',13,10
            db '  /U         unload   /?  help',13,10
            db 'Needs VDPMI (or QEMM / Jemm+QPIEMU) resident, and the card',13,10
            db 'enabled first (EXPG3GO /PCIC - native UART, no /MPU).',13,10,0
m_noqpi     db 'No QPI port-trap host found. Load VDPMI (or QEMM/QPIEMU).',13,10,0
m_qpifail   db 'QPI call failed - cannot (un)install the port trap.',13,10,0
m_deaduart  db 'WARNING: UART LSR reads FF - is the card enabled? '
            db '(run EXPG3GO /PCIC)',13,10,0
m_ok1       db 'Installed: MPU-401 at ',0
m_ok2       db ' -> UART ',0
m_oksyn     db ' -> synth INT 2Fh AH=',0
m_stride2   db ' (regs 2 apart)',0
m_ok3       db '. Resident.',13,10,0
m_unloaded  db 'MPUSHIM unloaded.',13,10,0
m_notres    db 'No resident MPUSHIM found to unload.',13,10,0
