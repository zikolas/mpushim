@echo off
REM MPUSHIM on a Pentium: VDPMI alone covers BOTH trap worlds - real-mode
REM (V86) games and DOS/4GW games - so there is no JEMM386, no QPIEMU and
REM no HDPMI32i in this stack.  VDPMI needs HIMEMX (or HIMEM.SYS) loaded
REM from CONFIG.SYS first, and a Pentium: it uses RDTSC/RDMSR unguarded.
REM /X keeps the UMA window the card enabler wants out of VDPMI hands;
REM add /DPMIMEM=nn if 64M of pre-allocated XMS is more than this box has.
C:\MPUSHIM\VDPMI.EXE /X=DC00-DFFF
C:\MPUSHIM\EXPGXGO.COM /PCIC /W=DC00
C:\MPUSHIM\MPUSHIM.EXE /UART=250 %1 %2
