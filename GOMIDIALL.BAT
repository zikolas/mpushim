@echo off
rem ==========================================================================
rem  GOMIDIALL.BAT - the unified MPU-401 facade, BOTH worlds in one boot:
rem  real-mode games (V86 via QPI) AND DPMI/DOS4GW games (via HDPMI32i)
rem  get an MPU-401 at 330h from the single resident MPUSHIMP.
rem
rem  JEMM is loaded dynamically and HDPMI32i takes its memory through VCPI
rem  (-v) - the exact recipe VSBPCM's GO.BAT ships on this box.  (Without
rem  -v, a dynamically loaded Jemm claims the XMS and HDPMI fails with
rem  "insufficient memory".)  Never JEMM386 UNLOAD with this stack up.
rem
rem  Set the game's music to General MIDI / MPU-401 at port 330, then start
rem  the game normally.  Removal: reboot.
rem ==========================================================================
C:\VSBPCM\JEMM386.EXE LOAD NOEMS X=DC00-DFFF
C:\DEV\EXPG3GO.COM /PCIC /W=DC00
cd \VSBPCM
JLOAD QPIEMU.DLL
cd \
C:\VSBPCM\HDPMI32I.EXE -r -x -v
C:\DEV\MPUSHIMP.EXE /UART=250 %1 %2
echo.
echo Facade resident for BOTH worlds. Start the game normally.
