/* OPL4ENG.H - the OPL4/YRW801 General MIDI engine, protected-mode build.
 *
 * This is the engine from OPL4SYN.C in the vew212-opl4 repo (the resident
 * V86 synth), compiled into MPUSHIM's DJGPP image so the protected-mode
 * trap handler can render trapped MPU writes on the OPL4 directly - no
 * cross-world delivery, no UART.  Keep the two in step when the engine
 * changes; the voice programming rules were proven on hardware there
 * (tone bit 8 in 20h BEFORE the 08h write that starts the header fetch;
 * envelope overrides only after the LD wait; CH=0 - only DO2 is wired).
 *
 * Instruments: OPL4TAB.H, ported from the Linux ALSA opl4 driver
 * (sound/drivers/opl4/yrw801.c and opl4_synth.c, (c) 2003 Clemens
 * Ladisch, dual-licensed BSD-2-clause / GPL v2).
 *
 * CONCURRENCY: unlike the V86 build, the nesting guards here are NO-OPS,
 * deliberately.  Under HDPMI the port-trap handler is dispatched with the
 * real IF clear, so the engine can never be preempted - and a ring-3
 * pushf/cli/.../popf under HDPMI at IOPL 0 is the exact PUSHFD/CLI/POPFD
 * hole the CLI-heal handler exists for: our own fn-9 trap would see the
 * 9C..FA pattern and force interrupts ON inside the handler.  If a VDPMI
 * build ever calls this engine (VDPMI CAN preempt a handler), the guards
 * must become real by some VDPMI-safe means first.
 *
 * (C) 2026 zikolas.  GNU General Public License v2 (see COPYING).
 */

#include "OPL4TAB.H"

static unsigned syn_base = 0x388;

static void syn_iod(unsigned n){ while (n--) (void)inportb(0x80); }

/* Register pacing: iod(2), the value the V86 build settled on after
 * iod(40) ate timer ticks inside the trap and slowed every sequencer. */
#define SYN_IODLY 2

static unsigned char syn_wvget(unsigned char r)
{
    outportb(syn_base + 4, r); syn_iod(SYN_IODLY);
    return (unsigned char)inportb(syn_base + 5);
}
static void syn_wvput(unsigned char r, unsigned char v)
{
    outportb(syn_base + 4, r); syn_iod(SYN_IODLY);
    outportb(syn_base + 5, v); syn_iod(SYN_IODLY);
}
static void syn_new2(int on)
{
    outportb(syn_base + 2, 0x05); syn_iod(SYN_IODLY);
    outportb(syn_base + 3, on ? 0x03 : 0x00); syn_iod(SYN_IODLY);
}

#define SYN_NVOICE 24
typedef struct {
    int ch, note, on;
    unsigned long age;
    unsigned char misc;
    unsigned short tone;
    unsigned char ksc;
    short pofs;
    unsigned char pnote;
} SYNVOICE;
static SYNVOICE syn_vc[SYN_NVOICE];
static unsigned long syn_agec = 1;

static int syn_chprog[16], syn_chvol[16], syn_chpan[16];
static short syn_chbend[16];

static void syn_voice_off(int v)
{
    syn_vc[v].misc &= 0x7F;
    syn_wvput((unsigned char)(0x68 + v), syn_vc[v].misc);
    syn_vc[v].on = 0;
}

static int syn_voice_alloc(void)
{
    int i, best = 0;
    unsigned long oldest = 0xFFFFFFFFUL;
    for (i = 0; i < SYN_NVOICE; i++) if (!syn_vc[i].on) return i;
    for (i = 0; i < SYN_NVOICE; i++)
        if (syn_vc[i].age < oldest) { oldest = syn_vc[i].age; best = i; }
    syn_voice_off(best);
    return best;
}

static void syn_all_off(void)
{
    int i;
    for (i = 0; i < SYN_NVOICE; i++) syn_voice_off(i);
}

static void syn_note_off(int ch, int note)
{
    int v;
    for (v = 0; v < SYN_NVOICE; v++)
        if (syn_vc[v].on && syn_vc[v].ch == ch && syn_vc[v].note == note)
            syn_voice_off(v);
}

static void syn_voice_pitch(int v)
{
    long pitch;
    int octv;
    unsigned f;
    pitch = (((long)(syn_vc[v].pnote - 60) << 7) * syn_vc[v].ksc) / 100 + (60L << 7);
    pitch += syn_vc[v].pofs + syn_chbend[syn_vc[v].ch];
    if (pitch < 0)        pitch = 0;
    if (pitch >= 0x6000L) pitch = 0x5FFFL;
    octv = (int)(pitch / 0x600) - 8;
    f = pitch_map[(unsigned)(pitch % 0x600)];
    syn_wvput((unsigned char)(0x20 + v),
              (unsigned char)(((f & 0x7F) << 1) | ((syn_vc[v].tone >> 8) & 1)));
    syn_wvput((unsigned char)(0x38 + v),
              (unsigned char)(((octv & 0x0F) << 4) | ((f >> 7) & 0x07)));
}

static void syn_start_region(int ch, int note, int pnote, int vel,
                             const REGION *rg)
{
    int v, att, pan;

    v = syn_voice_alloc();
    syn_vc[v].ch = ch; syn_vc[v].note = note; syn_vc[v].on = 1;
    syn_vc[v].age = syn_agec++;
    syn_vc[v].tone = rg->tone; syn_vc[v].ksc = rg->ksc;
    syn_vc[v].pofs = rg->pofs; syn_vc[v].pnote = (unsigned char)pnote;

    /* tone bit 8 BEFORE the 08h write that triggers the header fetch */
    syn_wvput((unsigned char)(0x20 + v), (unsigned char)((rg->tone >> 8) & 1));
    syn_wvput((unsigned char)(0x08 + v), (unsigned char)(rg->tone & 0xFF));

    pan = rg->pan + syn_chpan[ch];
    if (pan < -7) pan = -7;
    if (pan >  7) pan =  7;
    syn_vc[v].misc = (unsigned char)(0x20 | (pan & 0x0F));
    syn_wvput((unsigned char)(0x68 + v), syn_vc[v].misc);

    syn_voice_pitch(v);

    att = rg->att + vol_tab[syn_chvol[ch] & 0x7F] + vol_tab[vel & 0x7F];
    att = 0x7F - ((0x7F - att) * rg->vf) / 0xFE;
    if (att < 0)    att = 0;
    if (att > 0x7E) att = 0x7E;
    syn_wvput((unsigned char)(0x50 + v), (unsigned char)((att << 1) | 1));

    /* envelope overrides only after the header load ends */
    { int t = 400; while ((inportb(syn_base) & 0x02) && --t) syn_iod(1); }
    syn_wvput((unsigned char)(0x80 + v), rg->lfovib);
    syn_wvput((unsigned char)(0x98 + v), rg->ad1);
    syn_wvput((unsigned char)(0xB0 + v), rg->ld2);
    syn_wvput((unsigned char)(0xC8 + v), rg->rc);
    syn_wvput((unsigned char)(0xE0 + v), rg->trem);

    syn_vc[v].misc = (unsigned char)((syn_vc[v].misc & 0x1F) | 0x80);
    syn_wvput((unsigned char)(0x68 + v), syn_vc[v].misc);
}

static void syn_note_on(int ch, int note, int vel)
{
    int i, n = 0, prog;
    unsigned base, cnt;
    if (vel == 0) { syn_note_off(ch, note); return; }
    prog = ch == 9 ? 128 : (syn_chprog[ch] & 0x7F);
    base = alsa_prog[prog].base;
    cnt  = alsa_prog[prog].n;
    for (i = 0; i < (int)cnt && n < 2; i++) {
        const REGION *rg = &alsa_reg[base + i];
        if (note >= rg->lo && note <= rg->hi) {
            syn_start_region(ch, note, ch == 9 ? 60 : note, vel, rg);
            n++;
        }
    }
}

static void syn_ch_bend(int ch, int lo7, int hi7)
{
    int v;
    syn_chbend[ch] = (short)((((hi7 << 7) | lo7) - 8192) / 32);
    for (v = 0; v < SYN_NVOICE; v++)
        if (syn_vc[v].on && syn_vc[v].ch == ch) syn_voice_pitch(v);
}

static void syn_ch_off(int ch)
{
    int v;
    for (v = 0; v < SYN_NVOICE; v++)
        if (syn_vc[v].on && syn_vc[v].ch == ch) syn_voice_off(v);
}

static void syn_reset_state(void)
{
    int i;
    for (i = 0; i < 16; i++) {
        syn_chprog[i] = 0; syn_chvol[i] = 100;
        syn_chpan[i] = 0;  syn_chbend[i] = 0;
    }
    syn_all_off();
}

/* ---- the MIDI byte stream (identical to the V86 build) ------------------ */
static unsigned char syn_st = 0, syn_d0 = 0, syn_have = 0, syn_need = 0;

static void syn_byte1(unsigned char b)
{
    int ch;
    if (b >= 0xF8) return;
    if (b & 0x80) {
        if (b >= 0xF0) { syn_st = 0; return; }
        syn_st = b; syn_have = 0;
        syn_need = ((b & 0xE0) == 0xC0) ? 1 : 2;
        return;
    }
    if (!syn_st) return;
    if (syn_have == 0 && syn_need == 2) { syn_d0 = b; syn_have = 1; return; }
    syn_have = 0;
    ch = syn_st & 0x0F;
    switch (syn_st & 0xF0) {
    case 0x80: syn_note_off(ch, syn_d0); break;
    case 0x90: syn_note_on(ch, syn_d0, b); break;
    case 0xA0: break;
    case 0xB0:
        if      (syn_d0 ==   7) syn_chvol[ch] = b;
        else if (syn_d0 ==  10) syn_chpan[ch] = (b - 64) >> 3;
        else if (syn_d0 == 120 || syn_d0 == 123) syn_ch_off(ch);
        else if (syn_d0 == 121) { syn_chbend[ch] = 0; syn_chpan[ch] = 0; }
        break;
    case 0xC0: syn_chprog[ch] = b; break;
    case 0xD0: break;
    case 0xE0: syn_ch_bend(ch, syn_d0, b); break;
    }
}

/* Queue kept for structure; under HDPMI it is never contended (see the
 * header - real IF is clear for the whole handler, guards are no-ops). */
static volatile unsigned char syn_q[64], syn_qh = 0, syn_qt = 0, syn_busy = 0;

void __attribute__((used)) syn_byte_eng(unsigned b)
{
    syn_q[syn_qh] = (unsigned char)b;
    syn_qh = (unsigned char)((syn_qh + 1) & 63);
    if (syn_busy) return;
    syn_busy = 1;
    while (syn_qt != syn_qh) {
        unsigned char c = syn_q[syn_qt];
        syn_qt = (unsigned char)((syn_qt + 1) & 63);
        syn_byte1(c);
    }
    syn_busy = 0;
}

/* Install-time bring-up.  Returns 0 and leaves the gate closed if no OPL4
 * answers (a closed NEW2 bus floats - the check only means anything with
 * the gate freshly opened). */
static int syn_init(unsigned base)
{
    unsigned char id;
    syn_base = base;
    syn_new2(1);
    id = syn_wvget(0x02);
    if ((id & 0xF0) != 0x20) { syn_new2(0); return 0; }
    syn_wvput(0x02, 0x00);                       /* sound generation mode */
    syn_wvput(0xF9, 0x00);                       /* PCM mix 0 dB          */
    syn_reset_state();
    return 1;
}
