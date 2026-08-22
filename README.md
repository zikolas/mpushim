# MPUSHIM — an MPU-401 facade over a plain serial UART

A tiny resident DOS tool that lets **games** talk to a bare serial-UART MIDI
interface (like the one hiding inside the EXP GAME/MIDI G3 PC Card) as though
it were an **MPU-401 at 330h**.

**Status: bench-proven, both eras, two backends**, all on an IBM PC110
(486SX). `MPUSHIMP.EXE` is the one to use — **a single binary that covers
both trap worlds**: real-mode (V86) games through the QPI port-trap
interface, and 32-bit DPMI/DOS4GW games through HDPMI32i. Either host
alone is enough; each side installs and reports separately.

- **Real-mode era** — *The Secret of Monkey Island*'s Roland MT-32 music
  plays through the facade on the EXP GAME/MIDI G3 (its hidden UART at
  250h), the same setup that hard-wedges without MPUSHIM. DOSMid in real
  MPU mode (`/mpu=330`) detects and plays too.
- **DOS4GW / General MIDI era** — **Duke Nukem 3D, DOOM and DOOM II** play
  their GM scores through the facade to a Yamaha QY70.
- **serdashop MPU-232** serial-MIDI dongle on a plain COM port: game MIDI
  on a machine with **no sound card and no free slot** — nothing but a
  serial port.

Pair the synth to the score: a **GM module** (QY70 et al.) for the
DOS4GW/General MIDI catalogue, a **CM-32L/MT-32** for the Roland-native
titles. `MPUSHIM.COM`, the standalone 1.6 KB real-mode TSR, remains in the
repo for QPI-only stacks with no DPMI host at all.

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

Both **UART-mode** worlds are covered, by the one `MPUSHIMP.EXE`:

- **Real-mode (V86) games** — the MPU-401 era proper: SCUMM/iMUSE, Sierra
  SCI, Miles-driver titles. A 199-byte real-mode core (`MPUSHIMR.ASM`,
  embedded in the EXE) is planted in a DOS block and registered with the
  QPI port-trap host, so a V86 status poll is answered in real mode with
  no mode switch at all.
- **32-bit protected-mode games** (DOS4GW/DPMI titles doing MPU I/O from
  ring-3 — the General MIDI generation: Duke3D, DOOM, Descent, Warcraft
  II, ...), through HDPMI32i's documented I/O-trap API.

```
MPUSHIMP /UART=250      :: arms whichever hosts are present
DUKE3D.EXE              :: then start games normally
```

`GOMIDIALL.BAT` brings up the whole stack (JEMM+QPIEMU for V86, HDPMI32i
for PM, then the shim); `GOMIDIPM.BAT` is the PM-only variant. **Bench-
proven on the IBM PC110 with a Yamaha QY70 GM module on the DIN: Duke
Nukem 3D, DOOM and DOOM II all play their General MIDI scores through the
facade.** Getting there took finding two **host-interaction bugs** in the
HDPMI source: DJGPP's startup leaves the machine-global CR0.EM FPU state
altered under HDPMI, which broke DOS/4GW's FPU emulator for every program
started afterwards (Duke3D's "exception 07h" crash — repaired at
startup), and the DOS/4G `PUSHFD/CLI...POPFD` critical sections latch
interrupts off forever at IOPL 0, since a ring-3 POPFD cannot restore IF
(DOOM's hard wedge — healed by a CLI trap handler registered through
HDPMI's vendor fn 9). Plus two MIDI data-path laws learned on the bench:
never drop a MIDI byte (the transmit path waits a bounded byte-time,
FIFOs off so THRE means room-for-one), and the status byte must always
report write-ready (DMX-class drivers skip bytes if it ever says busy).
The facade also broadcasts All-Notes-Off on all 16 channels when it
receives the MPU RESET command, because real MPU-401 silicon does
(`/NOBC` disables it) — note this is CC 123, which LA-era Rolands
(MT-32/CM-32L) ignore, so a GM score can still leave a note ringing on
those synths at song changes; use a true GM module for GM titles.

Not covered yet, both planned:

- **16-bit protected-mode games** (the 16-bit DPMI client class —
  Tyrian, for one): DPMI hosts handle 16- and 32-bit clients separately,
  so this is its own registration against HDPMI16i. Also on the roadmap.
- **Intelligent mode** (the MPU-401's onboard 8-track sequencer, used by
  early Sierra AGI/early-SCI titles ~1986-1990): needs a command state
  machine, a timer-driven track engine, and virtual-IRQ delivery to the
  game. Planned as v0.4.

(On Pentium-class machines VDPMI covers V86 plus both 16- and 32-bit DPMI
clients in one host — the 486 fleet is where the per-host registrations
matter.)

VCPI-only extenders remain out of scope (the usual residue every solution
in this space shares).

## Host requirements

Each world needs its trap host, and either alone is fine — the shim arms
what it finds and says what it could not:

- **V86 (real-mode) side — a QPI provider.** **JEMM386 + QPIEMU** runs on
  any 386+ and is the right choice on older hardware (e.g. the 486-class
  IBM PC110, where MPU-era games run at the correct speed). **VDPMI** is a
  modern DPMI server + V86 monitor in one, but **requires a Pentium and
  64 MB RAM**, so it is for faster boxes only.
- **Protected-mode side — `HDPMI32I -r -x`**, whose IOPL-0 client
  execution is what makes ring-3 I/O trapping possible.

A dynamically loaded JEMM claims the extended memory, so give HDPMI32i
`-v` (take memory via VCPI) when both load from the prompt — the recipe
`GOMIDIALL.BAT` uses. With JEMM in `CONFIG.SYS` instead, the `-v` is
unnecessary. Never `JEMM386 UNLOAD` with this stack on top of it.

## Usage

```
MPUSHIMP [/UART=250] [/MPU=330] [/DIV=n] [/NORM] [/NOPM]
         [/NOTX] [/NOCLI] [/NOBC] [/IF]
  /UART=hex  serial UART base I/O port (default 250h)
  /MPU=hex   MPU-401 base the game expects (default 330h; status = base+1)
  /DIV=dec   (re)program the UART divisor for 31250 baud (G3 wants 24);
             omit to leave the UART exactly as the enabler set it
  /NORM      skip the V86 (QPI) side      /NOPM  skip the protected-mode side
  /NOTX      diagnostic: answer the MPU handshake, send no MIDI
  /NOCLI     diagnostic: skip the DOS/4G PUSHFD/CLI/POPFD interrupt heal
  /NOBC      diagnostic: no all-notes-off broadcast on MPU reset
  /IF        diagnostic: force interrupts on at every trap return
```

Installs its traps and stays resident (remove = reboot); start games
normally afterwards.

```
MPUSHIM [/UART=250] [/MPU=330] [/DIV=24] [/U] [/?]   :: standalone RM .COM
```

## Bench recipe (EXP G3 on the IBM PC110) — as proven

```
JEMM386 LOAD NOEMS X=DC00-DFFF   :: V86 monitor; exclude the card window
                                 :: (match the X= to your enabler's /W)
EXPG3GO /PCIC /W=DC00            :: card up, UART native at 250h (NO /MPU:
                                 :: presenting 330h is MPUSHIM's job now)
JLOAD QPIEMU.DLL                 :: QPI port-trap provider (V86 side)
HDPMI32I -r -x -v                :: DPMI host with I/O trapping (PM side)
MPUSHIMP /UART=250               :: trap 330h/331h -> UART at 250h, both worlds
```

Quick smoke tests: `DOSMID /mpu=330 /noxms CANYON.MID` exercises the
real-mode path, `PMPOKE.EXE` the protected-mode one. Then start the game
and choose **Roland MPU-401 / MT-32** or **General MIDI** music at 330.

`GOMIDIALL.BAT` in this repo is that sequence ready to run;
`GOMIDIPM.BAT` is the protected-mode-only subset, `GOMIDI.BAT` the
real-mode-only one, and `GOMIDI232.BAT` the MPU-232 variant. On a
Pentium-class machine the JEMM386+JLOAD pair can be replaced by a single
`DEVICE=VDPMI.EXE` in CONFIG.SYS (after HIMEMX).

## Serial-port dongles: the MPU-232 (tested)

MPUSHIM is not tied to PCMCIA cards — a standard PC COM port is the same
16550 programming model, so a serial-MIDI dongle works too. With a
serdashop **MPU-232** on COM1 (DIP switches all OFF = binary mode, 38400):

```
JEMM386 LOAD NOEMS
JLOAD QPIEMU.DLL
MPUSHIMP /UART=3F8 /DIV=3
```

(`/DIV=3` on a standard 1.8432 MHz COM UART = 38400 baud; the dongle
converts to MIDI's 31250.) **Bench-tested** on the IBM PC110's serial
port. Note that dense SysEx bursts arrive at the dongle slightly faster
than MIDI drains (38400 vs 31250) — fine for game music, which never
sustains that; a paced-output option can be added if it ever matters.

## Build

```
./build-pm.sh                              (MPUSHIMP.EXE — nasm + DJGPP in docker)
nasm -f bin MPUSHIM.ASM -o MPUSHIM.COM     (the standalone real-mode TSR)
```

`build-pm.sh` assembles `MPUSHIMR.ASM` flat with nasm, embeds it as a C
byte array (`xxd -i`), then compiles `MPUSHIMP.C` with the DJGPP cross
toolchain in a container. (The .COM builds byte-identical with the on-box
NASM at `C:\NASM`.)

## Clean-room provenance

The QPI port-trap ABI used here is the public QEMM programming interface
(entry via `int 67h AX=3F00h` or `int 2Fh AX=1684h`; far-call functions
`1A06h`/`1A07h` get/set trap handler, `1A08h` status, `1A09h`/`1A0Ah`
trap/untrap a port; real-mode handler contract `DX`=port, `CL` bit 2 = OUT,
`AL` = value). The protected-mode side uses the HDPMI vendor API exactly
as documented in HX's published `HDPMIAPI.TXT` (int 31h `AX=168Ah`
"HDPMI"; fn 5 context mode; fn 6/7 install/remove port traps; fn 9
CLI/STI trap; the error-code bit layout and the advance-EIP handler
obligation). The MPU-401 UART-mode protocol and the 16550 register model
are published hardware standards. No third-party source was copied. One
honesty note: keying the CLI heal on the preceding-`PUSHFD` idiom is the
same approach vsbhda (GPL) ships for the identical DOS/4G problem; the
implementation here was written independently against the documented fn-9
ABI and the Intel-documented IOPL-0 `POPFD` behaviour.

## License

MIT © zikolas
