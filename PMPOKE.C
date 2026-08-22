/* PMPOKE - minimal 32-bit DPMI client that exercises an MPU-401 at 330h
 * exactly the way a game's init does, one step at a time, with prints.
 * Run it under MPUSHIMP to verify the protected-mode trap end to end:
 *
 *     MPUSHIMP /UART=250 PMPOKE.EXE
 *
 * Expected with the facade working: status reads 80, reset ACK FE arrives,
 * UART-mode ACK FE arrives, note-on bytes accepted (and heard, ch2).
 * MIT (c) 2026 zikolas.
 */
#include <stdio.h>
#include <pc.h>
#include <dos.h>
#include <dpmi.h>

#define DATA 0x330
#define STAT 0x331

static int wait_dsr(void)                 /* bit7 low = data ready */
{
    int i;
    for (i = 0; i < 100000; i++)
        if (!(inportb(STAT) & 0x80)) return 1;
    return 0;
}

int main(void)
{
    unsigned char v;
    /* undo DJGPP's DPMI 0E01h startup call: under HDPMI it edits the REAL
     * CR0 machine-wide and is never restored on exit, which breaks DOS/4GW
     * FPU detection for every later program (the Duke3D exception-07h bug) */
    __dpmi_set_coprocessor_emulation(0);
    printf("PMPOKE: initial status = %02X\n", inportb(STAT));
    printf("PMPOKE: sending MPU reset (FF)...\n");
    outportb(STAT, 0xFF);
    if (wait_dsr()) {
        v = inportb(DATA);
        printf("PMPOKE: reset ACK = %02X %s\n", v, v == 0xFE ? "(GOOD)" : "(BAD)");
    } else
        printf("PMPOKE: reset ACK NEVER arrived (status %02X)\n", inportb(STAT));
    printf("PMPOKE: entering UART mode (3F)...\n");
    outportb(STAT, 0x3F);
    if (wait_dsr()) {
        v = inportb(DATA);
        printf("PMPOKE: UART ACK = %02X %s\n", v, v == 0xFE ? "(GOOD)" : "(BAD)");
    } else
        printf("PMPOKE: UART ACK NEVER arrived (status %02X)\n", inportb(STAT));
    printf("PMPOKE: sending note-on ch2 (91 3C 7F)...\n");
    outportb(DATA, 0x91); outportb(DATA, 0x3C); outportb(DATA, 0x7F);
    delay(400);
    outportb(DATA, 0x81); outportb(DATA, 0x3C); outportb(DATA, 0x40);
    printf("PMPOKE: done, final status = %02X\n", inportb(STAT));
    return 0;
}
