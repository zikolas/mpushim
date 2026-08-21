# UARTMPU — an MPU-401 facade over a plain serial UART

A tiny resident DOS tool that lets **games** talk to a bare serial-UART MIDI
interface (like the one hiding inside the EXP GAME/MIDI G3 PC Card) as though
it were an **MPU-401 at 330h**.

## The problem

Some MIDI cards expose only a raw 16550-class UART — no MPU-401 protocol
engine. A DOS *player* (DOSMid `/com=`) drives such a UART directly and works
fine. But *games* speak the MPU-401 handshake: they send a reset command,
wait for the `0xFE` ACK, and poll a status port whose bits mean
"data-ready / write-ready". A bare UART answers none of that — and, fatally,
a game's "drain any pending input" loop spins forever against a status port
that never reports "no data", so the game hangs on a blank screen before it
draws its first frame. (This is exactly what wedged Monkey Island against the
EXP G3's UART.)

## What UARTMPU does

It traps the two MPU I/O ports (`330h` data, `331h` status/command) and:

- returns a **correct MPU-401 UART-mode status byte** — write-ready always,
  data-ready only while an ACK is pending — so the drain loop terminates and
  write-ready polling behaves;
- answers the **reset (`0xFF`) and enter-UART-mode (`0x3F`)** commands with
  the `0xFE` ACK games expect;
- **forwards outgoing MIDI bytes** to the real UART's transmitter, paced on
  the Line Status Register.

It is a plain real-mode DOS TSR (~1.6 KB). It does the port trapping through
the **QPI** interface — the QEMM Programming Interface, a published standard
implemented by QEMM386, Jemm's QPIEMU, and crazii's VDPMI alike — so it is
*not itself* a protected-mode DPMI client, which keeps it small and robust.

## Coverage

QPI's real-mode trap covers **V86-mode (real-mode) programs** — which is
precisely the MPU-401 era: SCUMM/iMUSE, Sierra SCI, Miles-driver games. That
is the target. Protected-mode DPMI games that do their MPU I/O from ring-3,
and VCPI-only extenders, are out of scope (the usual residue every solution
in this space shares).

## Host requirement — pick the one your machine can run

UARTMPU needs a QPI provider resident. Two options, same result:

- **JEMM386 + QPIEMU** — runs on any 386+; the right choice on older/slower
  hardware (e.g. the 486-class IBM PC110, where MPU-era games run at the
  correct speed).
- **VDPMI** — a modern DPMI server + V86 monitor in one. Cleaner, but it
  **requires a Pentium and 64 MB RAM**, so it is for faster boxes only.

## Usage

```
UARTMPU [/UART=250] [/MPU=330] [/DIV=24] [/U] [/?]
  /UART=hex  serial UART base I/O port (default 250h)
  /MPU=hex   MPU-401 base the game expects (default 330h; status = base+1)
  /DIV=dec   (re)program the UART divisor for 31250 baud (G3 wants 24);
             omit to leave the UART exactly as the enabler set it
  /U         unload a resident UARTMPU
  /?         help
```

## Bench recipe (EXP G3 on the IBM PC110)

```
JEMM386 LOAD NOEMS X=D000-EFFF   :: V86 monitor; exclude the card window
JLOAD QPIEMU.DLL                 :: QPI port-trap provider
EXPG3GO /PCIC                    :: card up, UART native at 250h (NO /MPU:
                                 :: presenting 330h is UARTMPU's job now)
UARTMPU                          :: trap 330h/331h -> UART at 250h
```

then start the game and choose **Roland MPU-401 / MT-32** music.

On a Pentium-class machine, swap the first two lines for a single
`DEVICE=VDPMI.EXE` in CONFIG.SYS (after HIMEMX).

## Build

```
nasm -f bin UARTMPU.ASM -o UARTMPU.COM
```

(Builds byte-identical with the on-box NASM at `C:\NASM`.)

## Clean-room provenance

The QPI port-trap ABI used here is the public QEMM programming interface
(entry via `int 67h AX=3F00h` or `int 2Fh AX=1684h`; far-call functions
`1A06h`/`1A07h` get/set trap handler, `1A08h` status, `1A09h`/`1A0Ah`
trap/untrap a port; real-mode handler contract `DX`=port, `CL` bit 2 = OUT,
`AL` = value). The MPU-401 UART-mode protocol and the 16550 register model
are published hardware standards. No third-party source was copied.

## License

MIT © zikolas
