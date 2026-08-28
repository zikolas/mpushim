@echo off
rem card DIN: onboard 16550 at 320h, registers 2 apart, divisor 5
C:\MPUSHIM\JLOAD.EXE C:\MPUSHIM\QPIEMU.DLL
C:\MPUSHIM\MPUSHIMR.COM /UART=320 /STRIDE=2 /DIV=5
