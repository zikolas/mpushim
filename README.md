# MPUSHIM — an MPU-401 facade over a plain serial UART

Makes a bare serial-UART MIDI interface look like an **MPU-401 at 330h**, so
DOS games can use it for music. One resident binary covers both trap worlds:
real-mode (V86) games and 32-bit DPMI/DOS4GW games.

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
  `/UART=3F8 /DIV=3`, as in `GO232.BAT`.

## Requirements

A trap host for each world. Either alone works — each side installs and
reports separately.

| World | Host |
|---|---|
| Real-mode (V86) games | JEMM386 + QPIEMU, QEMM, or VDPMI (386+) |
| DPMI / DOS4GW games | `HDPMI32I -r -x` (the IOPL-0 variant) |

The UART must already be enabled and set to 31250 baud — by its card enabler,
or with `/DIV`. If a dynamically loaded JEMM has claimed extended memory, give
HDPMI32i `-v` so it takes memory via VCPI instead.

## Usage

```
MPUSHIM [/UART=250] [/MPU=330] [/DIV=n] [/NORM] [/NOPM] [/NOCLI]
  /UART  serial UART base I/O port (default 250)
  /MPU   MPU-401 base the game expects (default 330; status = base+1)
  /DIV   reprogram the UART divisor for 31250 baud
  /NORM  skip the real-mode side      /NOPM  skip the protected-mode side
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

## Files

| | |
|---|---|
| `MPUSHIM.EXE` | the shim |
| `GOALL.BAT` | both worlds |
| `GOPM.BAT` | protected mode only |
| `GORM.BAT` | real mode only |
| `GO232.BAT` | serial-MIDI dongle on a COM port |
| `tools/PMPOKE.EXE` | protected-mode smoke test |
| `tools/PMSTORM`, `PMISR`, `PMHOG` | trap throughput, ISR-context, memory pressure |
| `legacy/MPUSHIM.COM` | the standalone real-mode TSR |

## Build

```
./build-pm.sh                       MPUSHIM.EXE (nasm + DJGPP in docker)
cd legacy && ./BUILD.BAT            the standalone TSR (nasm)
```

`build-pm.sh` assembles the embedded real-mode core (`MPUSHIMR.ASM`) flat,
embeds it with `xxd -i`, then cross-compiles `MPUSHIM.C`.

## Notes

- The reset broadcast is CC 123, which LA-era Roland synths ignore; a General
  MIDI score can leave a note ringing on those at song changes. Use a GM module
  for GM titles.
- A dongle fed at 38400 takes dense SysEx slightly faster than MIDI drains.
  Fine for game music.
- Not covered: **16-bit** DPMI clients (a separate registration), MPU-401
  **intelligent mode**, and VCPI-only extenders.

## Provenance and licence

**MPUSHIM is GNU General Public License v2** (see `COPYING`), Copyright (C)
2026 zikolas.

It is GPL rather than permissive for one specific reason: the DOS/4G CLI heal
in `mpushim_cli_handler()` is derived from **VSBHDA**'s `_hdpmi_CliHandler`
(`src/stackio.asm`), Copyright (C) Baron-von-Riedesel, GPL v2 — the same
register discipline, the same "was this CLI preceded by a PUSHFD?" test with
the same opcode constants, and the same re-enable through DPMI 0901h. The
resident-TSR teardown sequence follows VSBHDA's too. Copyright in those parts
stays with their author.

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
