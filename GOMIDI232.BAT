@echo off
rem ==========================================================================
rem  GOMIDI232.BAT - MPU-401 at 330h over an MPU-232 serial-MIDI dongle on
rem  COM1 (3F8h).  No PCMCIA card involved: MPUSHIM forwards to the COM
rem  port UART at 38400 (divisor 3) and the MPU-232 converts to MIDI.
rem  MPU-232 DIP switches: all OFF (binary mode, 38400).
rem
rem  COM1 must be FREE: reboot and do NOT start comrade first - the link
rem  cable and the dongle share the port.  Unplug the link cable, plug the
rem  MPU-232 into the serial port, MIDI DIN to the synth, then run this.
rem ==========================================================================
C:\VSBPCM\JEMM386.EXE LOAD NOEMS X=DC00-DFFF
cd \VSBPCM
JLOAD QPIEMU.DLL
cd \
C:\DEV\MPUSHIM.COM /UART=3F8 /DIV=3
echo.
echo MPU-401 at 330h -^> MPU-232 on COM1 (38400).
echo Try:  C:\DOSMID\DOSMID.EXE /mpu=330 /noxms C:\DOSMID\CANYON.MID
echo Then: a game with Roland MT-32 / General MIDI music at 330.
