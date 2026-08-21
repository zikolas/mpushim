@echo off
rem  Build UARTMPU.COM with NASM (host or on-box C:\NASM).
nasm -f bin UARTMPU.ASM -o UARTMPU.COM
