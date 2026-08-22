@echo off
rem ==========================================================================
rem  GOMIDIPM.BAT - protected-mode game MIDI on the EXP GAME/MIDI G3.
rem  For DOS4GW/DPMI games (DOOM, Duke3D, Descent, Warcraft II...).
rem  After this runs, START THE GAME NORMALLY - MPUSHIMP is resident.
rem
rem  Set the game's music to General MIDI / MPU-401 at port 330.
rem  Removal: reboot.
rem ==========================================================================
C:\DEV\EXPG3GO.COM /PCIC
C:\VSBPCM\HDPMI32I.EXE -r -x
C:\DEV\MPUSHIMP.EXE /UART=250
echo.
echo Smoke test:  C:\DEV\PMPOKE.EXE     (expect FE ACKs + an audible note)
echo Then start the game normally.
