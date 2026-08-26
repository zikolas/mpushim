;==========================================================================
;  MPUSHIMR - the real-mode (V86) resident core of the unified MPUSHIM.
;
;  Assembled flat (org 0) and embedded in MPUSHIM.EXE as a byte blob
;  (build-pm.sh: nasm -f bin -> xxd -i -> mpushimr.h).  At install time
;  MPUSHIM copies it into a paragraph-aligned DOS block of its own (the
;  block belongs to the resident client, so it survives the TSR), patches
;  the state header, and registers the handler with the QPI port-trap
;  host (QEMM / Jemm's QPIEMU / VDPMI) as segment = block, offset =
;  HANDLER_OFF.  All memory references are CS-relative, so org-0 offsets
;  are correct as-is.
;
;  The MIDI sink is selectable at install time: the UART transmitter, or a
;  resident software synthesiser reached over an INT 2Fh multiplex (MPUSHIM
;  /SYNTH).  The facade above it - status model, ACK, reset broadcast - is
;  identical either way; only send_bl differs.
;
;  This is MPUSHIM.COM 0.1's bench-proven resident core, brought to parity
;  with the protected-mode facade: same status model (always write-ready),
;  ACK on reset/UART-mode, all-notes-off broadcast on reset, and a paced
;  bounded transmit that never drops a byte.
;
;  QPI real-mode handler contract: DX = port, CL bit 2 = OUT (else IN),
;  CL bits 3-5 = word/dword/string (absorbed), AL = value for OUT; RETF,
;  with AL = the byte handed back for an IN.  Runs in the guest's V86
;  context with unknown DS/SS; preserves everything except AL on IN.
;
;  (C) 2026 zikolas.  GNU General Public License v2 - see COPYING.
;  Provenance: see MPUSHIM.C.  This file is original work; the QPI
;  handler contract it implements is the public QEMM interface.
;==========================================================================
        cpu  386
        org  0

; ---- state header: FIXED offsets, patched by the installer --------------
sig       db 'MSHR'                   ; +0   blob identification
uart_base dw 0250h                    ; +4   serial UART base I/O port
mpu_data  dw 0330h                    ; +6   MPU data port
mpu_stat  dw 0331h                    ; +8   MPU status/command port
ackpend   db 0                        ; +10  1 = 0FEh ACK waiting
uartmode  db 0                        ; +11  informational
drops     dw 0                        ; +12  TX bytes dropped (UART dead)
synthax   dw 0                        ; +14  MIDI sink: 0 = the UART, else the
                                      ;      AX for INT 2Fh - AH the resident
                                      ;      synth's multiplex id, AL its
                                      ;      "MIDI byte in DL" function

        times 16-($-$$) db 0          ; assembler guard: entry stays at +16

; ---- the QPI trap handler (entry: blob offset 16) ------------------------
handler:
        test cl, 38h                  ; word/dword/string: not modelled
        jnz  .absorb
        cmp  dx, cs:[mpu_data]
        je   .data
        cmp  dx, cs:[mpu_stat]
        je   .stat
.absorb:
        retf                          ; never chain: no hardware behind us

; ---- MPU data port --------------------------------------------------------
.data:
        test cl, 4
        jz   .data_in
        push ax                       ; OUT: MIDI byte -> UART, paced
        push bx
        push cx
        push dx
        mov  bl, al
        call send_bl
        pop  dx
        pop  cx
        pop  bx
        pop  ax
        retf
.data_in:
        test byte cs:[ackpend], 1     ; IN: the ACK if pending, else 0
        jz   .din_zero
        mov  byte cs:[ackpend], 0
        mov  al, 0FEh
        retf
.din_zero:
        xor  al, al
        retf

; ---- MPU status/command port ----------------------------------------------
.stat:
        test cl, 4
        jz   .stat_in
        cmp  al, 0FFh                 ; reset
        je   .cmd_reset
        cmp  al, 3Fh                  ; enter UART mode
        je   .cmd_uart
        retf                          ; other commands: absorb
.cmd_uart:
        mov  byte cs:[uartmode], 1
        jmp  .cmd_ack
.cmd_reset:
        ; real MPU-401 silicon broadcasts All Notes Off on reset; parity
        ; with the PM facade
        push ax
        push bx
        push cx
        push dx
        mov  bh, 0B0h
.bc:
        mov  bl, bh                   ; Bn: control change, channel n
        call send_bl
        mov  bl, 7Bh                  ; CC 123: all notes off
        call send_bl
        mov  bl, 0
        call send_bl
        inc  bh
        cmp  bh, 0C0h
        jne  .bc
        pop  dx
        pop  cx
        pop  bx
        pop  ax
.cmd_ack:
        mov  byte cs:[ackpend], 1     ; reset and 3Fh both queue the ACK
        retf
.stat_in:
        ; Always write-ready (bit6=0); data-ready (bit7=0) only while the
        ; ACK is pending, so drain-input loops terminate.
        mov  al, 80h
        test byte cs:[ackpend], 1
        jz   .st_done
        xor  al, al
.st_done:
        retf

; ---- BL -> the MIDI sink -------------------------------------------------
; The UART transmitter, bounded THRE spin, dropping only if it is dead; or,
; where the installer patched synthax, a resident software synth over INT 2Fh.
; clobbers AX, CX, DX (callers save around it)
send_bl:
        mov  ax, cs:[synthax]         ; a resident synth instead of a UART?
        test ax, ax
        jnz  send_synth
        mov  dx, cs:[uart_base]
        add  dx, 5                    ; LSR
        mov  cx, 0FFFFh               ; generous bound; never a hang
.wait:
        in   al, dx
        test al, 20h                  ; THRE: room for a byte?
        jnz  .go
        loop .wait
        inc  word cs:[drops]
        jmp  .done
.go:
        mov  dx, cs:[uart_base]       ; THR
        mov  al, bl
        out  dx, al
.done:
        ret

; ---- BL -> a resident INT 2Fh synth (AX is already synthax) ---------------
; The callee synthesises the note on OUR stack and is only contracted to
; preserve what a multiplex handler normally does, so save everything send_bl
; promises its own callers - BX above all, which carries the reset
; broadcast's channel counter in BH.
send_synth:
        mov  dl, bl
        push bx
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
        pop  bx
        ret

blob_end:
