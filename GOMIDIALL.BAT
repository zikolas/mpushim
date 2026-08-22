@echo off
rem ==========================================================================
rem  GOMIDIALL.BAT - both shims in one boot: real-mode games AND DPMI/DOS4GW
rem  games get an MPU-401 at 330h.
rem
rem  ** PREREQ: JEMM386 MUST BE LOADED FROM CONFIG.SYS (DEVICE=...), NOT here.
rem     A command-line `JEMM386 LOAD` claims all extended memory, and then
rem     HDPMI32i fails with "insufficient memory" (same root cause as DOSMid
rem     needing /noxms under a dynamically loaded Jemm).  With Jemm loaded at
rem     boot, JEMM + QPIEMU + HDPMI32i coexist happily - that is the stack
rem     vsbpcmcia ships.
rem     Never `JEMM386 UNLOAD` with QPIEMU/MPUSHIM stacked on it: that wedges
rem     the machine.
rem
rem  Set the game's music to General MIDI / MPU-401 at port 330, then start
rem  the game normally.  Removal: reboot.
rem ==========================================================================
C:\DEV\EXPG3GO.COM /PCIC /W=DC00
cd \VSBPCM
JLOAD QPIEMU.DLL
cd \
C:\DEV\MPUSHIM.COM
C:\VSBPCM\HDPMI32I.EXE -r -x
C:\DEV\MPUSHIMP.EXE /UART=250 %1 %2
echo.
echo Both shims resident. Start the game normally (music: General MIDI @ 330).
