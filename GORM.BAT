@echo off
rem ==========================================================================
rem  GORM.BAT - real-mode (V86) games only: SCUMM/iMUSE, Sierra, Miles
rem  titles get an MPU-401 at 330h on the EXP GAME/MIDI G3.  No DPMI host
rem  needed - use GOALL.BAT if you also want DOS4GW games covered.
rem
rem  Choose "Roland MPU-401 / MT-32" music in the game, port 330.
rem  Removal: reboot (or MPUSHIM.COM /U if you used the standalone TSR).
rem ==========================================================================
C:\MPUSHIM\JEMM386.EXE LOAD NOEMS X=DC00-DFFF
C:\MPUSHIM\EXPG3GO.COM /PCIC /W=DC00
cd \MPUSHIM
JLOAD QPIEMU.DLL
C:\MPUSHIM\MPUSHIM.EXE /UART=250 /NOPM %1 %2
cd \
