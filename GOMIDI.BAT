@echo off
rem ==========================================================================
rem  GOMIDI.BAT - bring up the EXP GAME/MIDI G3 as a game-usable MPU-401@330
rem  on the IBM PC110 (486-class: JEMM386 + QPIEMU, since VDPMI needs a
rem  Pentium).  After this, start a game and pick Roland MPU-401 / MT-32.
rem
rem  Windows to exclude: EXPG3GO's attribute window (here DC00, matching the
rem  vsbhda fleet convention).  JEMM's UMB scan over a live card window wedges
rem  the machine, so the X= is MANDATORY.
rem ==========================================================================

rem --- V86 monitor (drop this line if JEMM already loads from CONFIG.SYS) ---
C:\VSBPCM\JEMM386.EXE LOAD NOEMS X=DC00-DFFF

rem --- enable the card: UART native at 250h.  NO /MPU here - presenting the
rem     MPU at 330h is MPUSHIM's job in software now. ---------------------
C:\DEV\EXPG3GO.COM /PCIC /W=DC00

rem --- QPI port-trap provider ------------------------------------------------
cd \VSBPCM
JLOAD QPIEMU.DLL
cd \

rem --- the facade: trap 330h/331h, forward to the UART at 250h ---------------
C:\DEV\MPUSHIM.COM

echo.
echo Ready.  Start your game and choose Roland MPU-401 / MT-32 music.
echo   (to remove: C:\DEV\MPUSHIM.COM /U  then  C:\VSBPCM\JEMM386.EXE UNLOAD)
