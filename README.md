# MPUSHIM — an MPU-401 facade over a plain serial UART

A tiny resident DOS tool that lets **games** talk to a bare serial-UART MIDI
interface (like the one hiding inside the EXP GAME/MIDI G3 PC Card) as though
it were an **MPU-401 at 330h**.

**Status: bench-proven, two backends.** Both on an IBM PC110 (486SX):

- **EXP GAME/MIDI G3 PC Card** (its hidden UART at 250h) → Roland CM-32L:
  DOSMid in real MPU mode (`/mpu=330`) detects and plays through the
  facade, and **The Secret of Monkey Island's Roland MT-32 music works** —
  the same setup that hard-wedges without MPUSHIM.
- **serdashop MPU-232** serial-MIDI dongle on a plain COM port: DOSMid
  `/mpu=330` plays to an external synth on a machine with **no sound card
  and no free slot** — game MIDI from nothing but a serial port.

(DOSMid note: with Jemm loaded dynamically from the prompt, add `/noxms`.)

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

## What MPUSHIM does

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

## Coverage and roadmap

QPI's real-mode trap covers **V86-mode (real-mode) programs** — which is
precisely the MPU-401 era: SCUMM/iMUSE, Sierra SCI, Miles-driver games. That
is what v0.1 serves, in **UART mode** (the mode virtually everything after
~1990 uses).

Not covered yet, all planned:

- **32-bit protected-mode games** (DOS4GW/DPMI titles doing MPU I/O from
  ring-3 — the General MIDI generation: Duke3D, Descent, Warcraft II, ...):
  needs a second trap registration with the protected-mode host
  (HDPMI32i). Planned as v0.2 — with a GM module on the DIN this opens the
  whole DOS4GW library.
- **16-bit protected-mode games** (the 16-bit DPMI client class —
  Tyrian, for one): DPMI hosts handle 16- and 32-bit clients separately,
  so this is its own registration against HDPMI16i. Also on the roadmap.
- **Intelligent mode** (the MPU-401's onboard 8-track sequencer, used by
  early Sierra AGI/early-SCI titles ~1986-1990): needs a command state
  machine, a timer-driven track engine, and virtual-IRQ delivery to the
  game. Planned as v0.3.

(On Pentium-class machines VDPMI covers V86 plus both 16- and 32-bit DPMI
clients in one host — the 486 fleet is where the per-host registrations
matter.)

VCPI-only extenders remain out of scope (the usual residue every solution
in this space shares).

## Host requirement — pick the one your machine can run

MPUSHIM needs a QPI provider resident. Two options, same result:

- **JEMM386 + QPIEMU** — runs on any 386+; the right choice on older/slower
  hardware (e.g. the 486-class IBM PC110, where MPU-era games run at the
  correct speed).
- **VDPMI** — a modern DPMI server + V86 monitor in one. Cleaner, but it
  **requires a Pentium and 64 MB RAM**, so it is for faster boxes only.

## Usage

```
MPUSHIM [/UART=250] [/MPU=330] [/DIV=24] [/U] [/?]
  /UART=hex  serial UART base I/O port (default 250h)
  /MPU=hex   MPU-401 base the game expects (default 330h; status = base+1)
  /DIV=dec   (re)program the UART divisor for 31250 baud (G3 wants 24);
             omit to leave the UART exactly as the enabler set it
  /U         unload a resident MPUSHIM
  /?         help
```

## Bench recipe (EXP G3 on the IBM PC110) — as proven

```
JEMM386 LOAD NOEMS X=DC00-DFFF   :: V86 monitor; exclude the card window
                                 :: (match the X= to your enabler's /W)
EXPG3GO /PCIC /W=DC00            :: card up, UART native at 250h (NO /MPU:
                                 :: presenting 330h is MPUSHIM's job now)
JLOAD QPIEMU.DLL                 :: QPI port-trap provider
MPUSHIM                          :: trap 330h/331h -> UART at 250h
```

Quick smoke test: `DOSMID /mpu=330 /noxms CANYON.MID` — if that plays, the
whole facade works. Then start the game and choose **Roland MPU-401 /
MT-32** music.

`GOMIDI.BAT` in this repo is that sequence ready to run; `GOMIDI232.BAT`
is the MPU-232 variant. On a Pentium-class machine the JEMM386+JLOAD pair
can be replaced by a single `DEVICE=VDPMI.EXE` in CONFIG.SYS (after
HIMEMX).

## Serial-port dongles: the MPU-232 (tested)

MPUSHIM is not tied to PCMCIA cards — a standard PC COM port is the same
16550 programming model, so a serial-MIDI dongle works too. With a
serdashop **MPU-232** on COM1 (DIP switches all OFF = binary mode, 38400):

```
JEMM386 LOAD NOEMS
JLOAD QPIEMU.DLL
MPUSHIM /UART=3F8 /DIV=3
```

(`/DIV=3` on a standard 1.8432 MHz COM UART = 38400 baud; the dongle
converts to MIDI's 31250.) **Bench-tested** on the IBM PC110's serial
port. Note that dense SysEx bursts arrive at the dongle slightly faster
than MIDI drains (38400 vs 31250) — fine for game music, which never
sustains that; a paced-output option can be added if it ever matters.

## Build

```
nasm -f bin MPUSHIM.ASM -o MPUSHIM.COM
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
