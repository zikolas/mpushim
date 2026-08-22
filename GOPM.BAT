@echo off
rem ==========================================================================
rem  GOPM.BAT - protected-mode only: DPMI/DOS4GW games (DOOM, Duke3D,
rem  Descent, Warcraft II...) get an MPU-401 at 330h on the EXP GAME/MIDI G3.
rem  No JEMM/QPIEMU needed - use GOALL.BAT if you also want real-mode
rem  games covered.  After this runs, START THE GAME NORMALLY.
rem
rem  Set the game's music to General MIDI / MPU-401 at port 330.
rem  Removal: reboot.
rem ==========================================================================
C:\MPUSHIM\EXPG3GO.COM /PCIC
C:\MPUSHIM\HDPMI32I.EXE -r -x
C:\MPUSHIM\MPUSHIM.EXE /UART=250 %1 %2
echo.
echo Smoke test:  C:\MPUSHIM\PMPOKE.EXE   (expect FE ACKs + an audible note)
echo Then start the game normally.
