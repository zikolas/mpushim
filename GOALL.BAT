@echo off
rem ==========================================================================
rem  GOALL.BAT - the unified MPU-401 facade, BOTH worlds in one boot:
rem  real-mode games (V86 via QPI) AND DPMI/DOS4GW games (via HDPMI32i)
rem  get an MPU-401 at 330h from the single resident MPUSHIM.EXE.
rem
rem  Everything needed lives in this directory.  JEMM is loaded dynamically
rem  and HDPMI32i takes its memory through VCPI (-v) - the recipe VSBPCM's
rem  GO.BAT ships on this box.  (Without -v, a dynamically loaded Jemm
rem  claims the XMS and HDPMI fails with "insufficient memory".)  With JEMM
rem  loaded from CONFIG.SYS instead, drop the JEMM386 line and the -v.
rem  Never JEMM386 UNLOAD with this stack up - that wedges the machine.
rem
rem  Set the game's music to General MIDI / MPU-401 at port 330, then start
rem  the game normally.  Removal: reboot.
rem ==========================================================================
C:\MPUSHIM\JEMM386.EXE LOAD NOEMS X=DC00-DFFF
C:\MPUSHIM\EXPG3GO.COM /PCIC /W=DC00
cd \MPUSHIM
JLOAD QPIEMU.DLL
C:\MPUSHIM\HDPMI32I.EXE -r -x -v
C:\MPUSHIM\MPUSHIM.EXE /UART=250 %1 %2
cd \
echo.
echo Facade resident for BOTH worlds. Start the game normally.
echo Smoke tests: C:\MPUSHIM\PMPOKE.EXE (PM side), DOSMID /mpu=330 (V86 side).
