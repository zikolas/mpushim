# MPUSHIM — an MPU-401 facade over a plain serial UART

Makes a bare serial-UART MIDI interface look like an **MPU-401 at 330h**, so
DOS games can use it for music. One resident binary covers both trap worlds —
real-mode (V86) games and 32-bit DPMI/DOS4GW games — under any of the three
host arrangements below.

## The problem

Some MIDI interfaces — PC Cards, serial dongles, on-board UARTs — are a raw
16550 and nothing more. A DOS *player* can drive one directly, but *games*
speak the MPU-401 handshake: reset command, `0xFE` ACK, and a status port
whose bits report data-ready and write-ready. A bare UART answers none of it,
and a game's "drain pending input" loop spins forever against a status port
that never says "no data" — so it hangs before drawing a frame.

## What it does

Traps the two MPU ports (`330h` data, `331h` status/command) and:

- returns a correct UART-mode **status byte** — always write-ready, data-ready
  only while an ACK is pending, so drain loops terminate;
- answers **reset (`FFh`)** and **enter-UART-mode (`3Fh`)** with the `FEh` ACK,
  and broadcasts All-Notes-Off on reset as real MPU-401 silicon does;
- **forwards MIDI bytes** to the UART transmitter, paced on the Line Status
  Register. Bytes are never dropped.

## Interfaces

Anything whose MIDI output is a 16550-compatible UART. Two worked examples:

* **EXP Game Traveler PC Cards** (the GAME/MIDI variants) — the card's MIDI is
  a bare UART with no MPU engine behind it. Bring the card up with
  [EXPGXGO](https://github.com/zikolas/expgxgo) (a separate tool, MIT) first,
  then point the shim at the UART base it reports (`/UART=250` is that card's
  default).
* **Serdaco [MPU-232](https://www.serdashop.com/MPU-232)** — an RS-232-to-MIDI
  dongle on a plain COM port, so a machine with no sound card and no free slot
  still gets game MIDI. DIP switches all OFF (38400 baud, binary):
  `/UART=3F8 /DIV=3`, as in `GOC232.BAT`.
* **Resident software synths** - `/SYNTH[=id]` delivers each byte to an
  INT 2Fh synth TSR instead of a UART, in every world (the protected-mode
  sides reflect through DPMI 0300h). Two exist so far: [OPL4SYN](https://github.com/zikolas/vew21xgo/tree/master/synth)
  (the CF-VEW212's OPL4 wavetable, shipped with its enabler) and TDKSYN
  (the TDK MC-8000/DMC-9000's EMU8000; still to be released).

## Requirements

A trap host. On a 386 or 486 that means one per world — either alone works,
each side installs and reports separately. On a Pentium, VDPMI can do both by
itself.

| World | Host | Shim | CPU |
|---|---|---|---|
| Real-mode (V86) games | JEMM386 + QPIEMU, or QEMM | `MPUSHIM.EXE` | 386+ |
| 32-bit DPMI / DOS4GW games | `HDPMI32I -r -x` | `MPUSHIM.EXE` | 386+ |
| 16-bit DPMI games | `HDPMI16I -r -x` | `MPUSHM16.COM` | 386+ |
| **all three, one host** | **VDPMI** | `MPUSHIM.EXE` | **Pentium+** |

[VDPMI](https://github.com/crazii/SBEMU/releases) is crazii's DPMI host with
its own V86 monitor; its port traps fire for ring-3 and V86 clients through
one table, so MPUSHIM makes a single registration and covers everything —
one resident instead of three, and no HDPMI CLI heal needed (VDPMI's virtual
interrupts do not have the IOPL-0 `POPFD` hole that heal exists for). It is
detected first and used alone; `/NOVD` ignores it and uses the QPI + HDPMI
pair instead. VDPMI executes RDTSC and RDMSR with no CPUID guard, so it needs
a Pentium; the QPI + HDPMI stack is 386+ and runs on a Pentium unchanged.

The UART must already be enabled and set to 31250 baud — by its card enabler,
or with `/DIV`. If a dynamically loaded JEMM has claimed extended memory, give
HDPMI32i `-v` so it takes memory via VCPI instead.

## Usage

```
MPUSHIM [/UART=250] [/MPU=330] [/DIV=n] [/NORM] [/NOPM] [/NOVD] [/NOCLI]
  /UART  serial UART base I/O port (default 250)
  /MPU   MPU-401 base the game expects (default 330; status = base+1)
  /DIV   reprogram the UART divisor for 31250 baud
  /NORM  skip the real-mode side      /NOPM  skip the protected-mode side
  /NOVD  ignore VDPMI; use the QPI + HDPMI pair instead
  /NOCLI skip the DOS/4G PUSHFD/CLI/POPFD interrupt heal
```

Installs its traps, stays resident, and prints what it armed. Start games
normally afterwards and select **MPU-401 / General MIDI at 330**. Removal is a
reboot.

```
legacy/MPUSHIM.COM [/UART=250] [/MPU=330] [/DIV=24] [/U]
```

A standalone 1.6 KB real-mode TSR, for QPI-only stacks with no DPMI host.
Unlike the EXE it can unload itself. Keep it out of the shim's own directory:
DOS runs `MPUSHIM.COM` in preference to `MPUSHIM.EXE` when both are present
and you type the bare name.

## Card recipes

The launchers in `go/` wrap these, but the switches stand on their own.
Load order is always: the card's enabler, then the synth TSR if one is
involved, then the trap hosts (see Requirements), then the shims.

**CF-VEW212 internal wavetable**
([vew21xgo](https://github.com/zikolas/vew21xgo) enabler + its OPL4SYN):

    VEW21XGO
    OPL4SYN
    ...trap hosts...
    MPUSHM16 /SYNTH
    MPUSHIM  /SYNTH

Both synth TSRs answer the default multiplex id (BDh), so bare `/SYNTH`
is the whole configuration; `/SYNTH=xx` pairs with a synth loaded with
`/ID=xx`.

**TDK MC-8000/DMC-9000 internal wavetable**
([mc8kgo](https://github.com/zikolas/mc8kgo) enabler + its TDKSYN):

    MC8KGO
    TDKSYN
    ...trap hosts...
    MPUSHM16 /SYNTH
    MPUSHIM  /SYNTH

**TDK MC-8000/DMC-9000 MIDI DIN** — external gear (a real MT-32/CM-32L
or GM module) on the card's DIN socket:

    MC8KGO
    ...trap hosts...
    MPUSHM16 /UART=320 /STRIDE=2
    MPUSHIM  /UART=320 /STRIDE=2 /DIV=5

The card's DIN is an onboard 16550 living in a 16-bit window, so its
registers sit 2 bytes apart — that is `/STRIDE=2`, and every shim aimed
at this UART needs it. Its clock wants divisor 5 for MIDI's 31250 baud;
`/DIV=5` programs the UART once, from whichever shim carries it — the
.EXE in the recipe above, MPUSHM16 if it is the only shim loaded, or the
real-mode `legacy` .COM on a V86-only boot (`/UART=320 /STRIDE=2
/DIV=5` there too).

**EXP Game Traveler / MPU-232**: see Interfaces — `/UART=250` (the EXP
card's default) and `/UART=3F8 /DIV=3` respectively, both at the normal
register stride.

Each shim has ONE sink per boot: the wavetable and the DIN are a choice,
made per shim at load time.

## Files

| | |
|---|---|
| `MPUSHIM.EXE` | the shim |
| `MPUSHM16.COM` | the 16-bit protected-mode shim |

The GO launchers live in `go/` and carry a card letter after `GO`:
**E** = EXP GAME/MIDI (UART at 250), **C** = MPU-232 dongle on a COM
port, **W** = CF-VEW212 wavetable (enabler + OPL4SYN synth:
[vew21xgo](https://github.com/zikolas/vew21xgo)), **T** = TDK MC-8000/DMC-9000 (enabled by
[MC8KGO](https://github.com/zikolas/mc8kgo)). They assume everything in
one directory on the box; adjust paths to taste.

| | |
|---|---|
| `go/GOEALL.BAT` | EXP: the full stack, all three worlds at once |
| `go/GOEPM.BAT` | EXP: 32-bit protected mode only |
| `go/GOERM.BAT` | EXP: real mode only |
| `go/GOEVDP.BAT` | EXP: all three worlds through VDPMI alone (Pentium) |
| `go/GOE16.BAT` | EXP: 16-bit protected-mode games (HDPMI16i) |
| `go/GOC232.BAT` | serial-MIDI dongle on a COM port |
| `go/GOWMIDI.BAT` | VEW212: wavetable MIDI, all three worlds |
| `go/GOW32.BAT` | VEW212: wavetable MIDI + VSBPCM digital (32-bit games) |
| `go/GOW16.BAT` | VEW212: wavetable MIDI + VSBPCM16 digital (16-bit games) |
| `go/GOTSYN.BAT` | TDK: MIDI to the card's own EMU8000 synth |
| `go/GOTDIN.BAT` | TDK: MIDI out of the card's DIN socket |
| `tools/PMPOKE.EXE` | protected-mode smoke test |
| `tools/PMSTORM`, `PMISR`, `PMHOG` | trap throughput, ISR-context, memory pressure |
| `legacy/MPUSHIM.COM` | the standalone real-mode TSR |

## Build

```
./build-pm.sh                       MPUSHIM.EXE + MPUSHM16.COM
cd legacy && ./BUILD.BAT            the standalone real-mode TSR (nasm)
```

`build-pm.sh` assembles the embedded real-mode core (`MPUSHIMR.ASM`) flat,
embeds it with `xxd -i`, cross-compiles `MPUSHIM.C`, and assembles the 16-bit
shim (`MPUSHM16.ASM`) as a .COM.

## Notes

- The reset broadcast is CC 123, which LA-era Roland synths ignore; a General
  MIDI score can leave a note ringing on those at song changes. Use a GM module
  for GM titles.
- A dongle fed at 38400 takes dense SysEx slightly faster than MIDI drains.
  Fine for game music.
- Under VDPMI, if a DOS/4GW game stutters or wedges once music is playing,
  load VDPMI with `/PVI=0`. crazii's own VDPMI.TXT names the case ("some old
  dos extenders or programs (e.g. DMX in doom) may have glitches/sound
  stutters with /PVI=1"), and it is the same critical-section-versus-virtual-
  interrupts problem the HDPMI CLI heal exists for, reached from the other
  side: with PVI on, a ring-3 `POPFD` cannot restore the virtual interrupt
  flag its matching `PUSHFD` saved.
- **16-bit DPMI clients need `MPUSHM16.COM`, not `MPUSHIM.EXE`.** HDPMI16
  and HDPMI32 are separate hosts built from one source (`?32BIT=0/1`), and
  `AX=168Ah` returns the API of the host serving the *calling* client — so a
  32-bit program can never register a trap for a 16-bit game. `MPUSHM16` is a
  plain .COM that becomes a 16-bit DPMI client itself and installs FAR16
  handlers. Both may be resident together; load HDPMI16 first.
- Not covered: MPU-401 **intelligent mode**, and VCPI-only extenders.

## Provenance and licence

**MPUSHIM is GNU General Public License v2** (see `COPYING`), Copyright (C)
2026 zikolas.

It is GPL rather than permissive because two pieces of it derive from GPL v2
sources, each attributed at the function it belongs to.

First, the DOS/4G CLI heal
in `mpushim_cli_handler()` is derived from **VSBHDA**'s `_hdpmi_CliHandler`
(`src/stackio.asm`), Copyright (C) Baron-von-Riedesel, GPL v2 — the same
register discipline, the same "was this CLI preceded by a PUSHFD?" test with
the same opcode constants, and the same re-enable through DPMI 0901h. The
resident-TSR teardown sequence follows VSBHDA's too. Copyright in those parts
stays with their author.

Second, **VDPMI's vendor API is not documented anywhere** — VDPMI.TXT does not
mention it and VDPMI itself is not yet open source. The ABI `mpushim_vd_handler()`
and the `vd_*` calls implement is the one **SBEMU**'s client driver for VDPMI
(`vdpmi.c`, Copyright (C) crazii, GPL v2) describes by using it: the vendor
signature, the function numbers, the trap handler's argument frame and far
return, and the chain-to-the-previous-handler convention. Copyright in that
description stays with its author.

Everything else is original work against published interfaces:

* the **HDPMI** port-trap ABI, from Baron-von-Riedesel's published
  `HDPMIAPI.TXT` (`int 2Fh AX=168Ah` "HDPMI"; fn 5 context mode, fn 6/7
  install/remove traps, fn 9 CLI/STI trap; the error-code layout and the
  advance-EIP obligation). HDPMI itself is freeware "for any purpose",
  Copyright Japheth.
* the **QPI** port-trap ABI, the public QEMM programming interface (`int 67h
  AX=3F00h` or `int 2Fh AX=1684h` for the entry; far-call `1A06h`/`1A07h`
  get/set trap handler, `1A08h` status, `1A09h`/`1A0Ah` trap/untrap).
* **MPU-401 UART mode** and the **16550** register model, published
  hardware standards.
