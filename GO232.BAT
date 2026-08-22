@echo off
rem ==========================================================================
rem  GO232.BAT - the serdashop MPU-232 serial-MIDI dongle on COM1, for a
rem  machine with no sound card and no free slot.  DIP switches all OFF =
rem  binary mode, 38400 baud; /DIV=3 on a standard 1.8432 MHz COM UART = 38400,
rem  and the dongle converts to MIDI's 31250.  No card enabler needed.
rem
rem  NOTE: the dongle and the COMrade serial link share COM1 - either/or.
rem  Removal: reboot.
rem ==========================================================================
C:\MPUSHIM\JEMM386.EXE LOAD NOEMS
cd \MPUSHIM
JLOAD QPIEMU.DLL
C:\MPUSHIM\HDPMI32I.EXE -r -x -v
C:\MPUSHIM\MPUSHIM.EXE /UART=3F8 /DIV=3 %1 %2
cd \
echo.
echo MPU-232 facade resident. Start the game (music: MPU-401 / GM at 330).
