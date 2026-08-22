/* PMSTORM - hammer the trapped MPU ports from a 32-bit DPMI client and
 * report how many traps per second the host sustains, in batches, so a
 * hang shows up as a batch that never completes.
 *
 * Run under a resident MPUSHIMP.  Purpose: separate "sustained protected
 * mode trapping is too slow / unstable" from "something specific to the
 * game".  MIT (c) 2026 zikolas.
 */
#include <stdio.h>
#include <pc.h>
#include <dos.h>
#include <dpmi.h>
#include <sys/farptr.h>
#include <go32.h>

#define DATA 0x330
#define STAT 0x331
#define BATCH 2000
#define BATCHES 20

static unsigned long ticks(void)
{
    return _farpeekl(_dos_ds, 0x46c);   /* BIOS tick, 18.2/s */
}

int main(void)
{
    int b, i;
    unsigned long t0, t1, total = 0;
    /* repair CR0 after DJGPP's 0E01h startup call (see MPUSHIMP.C bug 1) */
    __dpmi_set_coprocessor_emulation(0);
    printf("PMSTORM: %d batches x %d writes+reads to %03X/%03X\n",
           BATCHES, BATCH, DATA, STAT);
    printf("(each batch = %d traps; a batch that never prints = the hang)\n",
           BATCH * 2);
    for (b = 0; b < BATCHES; b++) {
        printf("  batch %2d ...", b + 1);
        fflush(stdout);
        t0 = ticks();
        for (i = 0; i < BATCH; i++) {
            outportb(DATA, 0x90);          /* trapped OUT: forwarded byte */
            (void)inportb(STAT);           /* trapped IN: status poll     */
        }
        t1 = ticks();
        total += (t1 - t0);
        printf(" %lu ticks (%lu us/trap)\n", t1 - t0,
               ((t1 - t0) * 54925UL) / (BATCH * 2));
    }
    printf("PMSTORM: survived %d traps in %lu ticks (~%lu us/trap)\n",
           BATCHES * BATCH * 2, total,
           total ? (total * 54925UL) / (BATCHES * BATCH * 2) : 0);
    return 0;
}
