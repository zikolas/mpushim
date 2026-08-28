@echo off
rem Jemm386 lives in CONFIG.SYS - do not load it here as well
C:\MPUSHIM\JLOAD.EXE C:\MPUSHIM\QPIEMU.DLL
C:\TDK8000\TDKSYN.EXE %1 %2
C:\MPUSHIM\MPUSHIMR.COM /SYNTH
