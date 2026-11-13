// passthru.asm - the converters on the LPC link, both ways.
//
// What an audio interface needs from the SHARC and nothing more: the inputs
// go to the LPC, and what the LPC sends goes to the outputs. Every gain, mix
// and effect is the LPC's business (or the host's); the SHARC moves samples.
//
// audio_init.asm, appended by tools/midi_control_sharc.py, sets up the routing, the
// clock generators and the converters' reset as the stock program does. This
// then turns on, I2S, 32-bit words, without DMA:
//   receive   SPORT0 A and B (DAI 4 and 8, the two ADCs), 48 kHz
//             SPORT1 A (DAI 10, from the LPC), 192 kHz
//   transmit  SPORT4 A and B and SPORT5 A (DAI 5, 6 and 16, 7, the DACs), 48 kHz
//             SPORT6 A (DAI 9, to the LPC), 192 kHz
// with the stock control values less their DMA bits.
//
// Each 48 kHz frame is two halves, each with one word for every converter
// port and four words each way on the link.
//
// To the LPC (docs/10 section 2), from the previous frame's inputs:
//   slot 0  ADC A left, bit 0 set: the frame marker
//   slot 1  ADC A right
//   slot 2  ADC B left
//   slot 3  ADC B right
//   4-7     zero
// The converters' 24 bits are the top of each word; the bottom byte is
// cleared, so bit 0 is clear in every slot but 0.
//
// From the LPC: the LPC marks its own slot 0 with bit 0, as the SHARC does, so
// this aligns on that bit - a marked word is slot 0, and the slots count on
// from there modulo 8. Slot 0 goes to the left of every DAC, slot 1 to the
// right, with the bottom byte cleared. Before the LPC is aligned it sends zero,
// so the outputs are silent until it is.
//
// The guitar is on ADC A right, link slot 1 (measured). Which DAC feeds which
// jack is not known, so every DAC gets the same left and right.
//
// FLAG0 (DSP_TO_MCU_INT) toggles every 480 frames, 100 times a second, which
// is how the LPC sees that the program runs.
//
// Assembled with hx-stomp-nam's tools/sharc/sasm.py and put behind ADI's
// kernel by tools/midi_control_sharc.py. Entry: start. Registers: R0-R15, I4, M0,
// L4.

.equ FLG0,    0x1               // FLAG0 value
.equ FLG0O,   0x2               // FLAG0 output enable

.equ SPCTL0,  0xc00
.equ SPCTL1,  0xc01
.equ SPCTL4,  0x800
.equ SPCTL5,  0x801
.equ SPCTL6,  0x4800
.equ RXSP0A,  0xc61
.equ RXSP0B,  0xc63
.equ RXSP1A,  0xc65
.equ TXSP4A,  0x860
.equ TXSP4B,  0x862
.equ TXSP5A,  0x864
.equ TXSP6A,  0x4860
.equ RX_AB,   0x010129f1        // stock SPCTL0 0x013d29f1 less SDEN/SCHEN A and B
.equ RX_A,    0x000129f1        // stock SPCTL1 0x000d29f1 less SDEN_A/SCHEN_A
.equ TX_AB,   0x030129f1        // stock SPCTL4 0x033d29f1 less SDEN/SCHEN A and B
.equ TX_A,    0x020129f1        // stock SPCTL5/6 0x020d29f1 less SDEN_A/SCHEN_A
.equ BUF_A,   0xc0000000        // DXS_A: both set = transmit full; either = a word to read
.equ BUF_B,   0x18000000        // DXS_B: the same for B
.equ BEAT,    480               // frames per FLAG0 toggle

// R2 zero          R3 word to send     R4-R7 ADC A L, A R, B L, B R
// R8 DAC left      R9 BUF_A            R10 BUF_B       R11 one
// R12 link slot    R13 0xffffff00      R14 DAC right   R15 frames to toggle

.section pm 0x8c100
start:  BIT SET FLAGS FLG0;     // FLAG0 high, as the stock program brings it up
        BIT SET FLAGS FLG0O;
        R0 = 0;
        DM(SPCTL0) = R0;
        DM(SPCTL1) = R0;
        DM(SPCTL4) = R0;
        DM(SPCTL5) = R0;
        DM(SPCTL6) = R0;
        CALL audio_init;

        R0 = RX_AB;
        DM(SPCTL0) = R0;
        R0 = RX_A;
        DM(SPCTL1) = R0;
        R0 = TX_AB;
        DM(SPCTL4) = R0;
        R0 = TX_A;
        DM(SPCTL5) = R0;
        DM(SPCTL6) = R0;

        R2 = 0;
        R4 = 0;
        R5 = 0;
        R6 = 0;
        R7 = 0;
        R8 = 0;
        R14 = 0;
        R9 = BUF_A;
        R10 = BUF_B;
        R11 = 1;
        R12 = 0;
        R13 = 0xffffff00;
        R15 = BEAT;

frame:  // First half: the link's audio slots, then the left of everything.
        R3 = R4 OR R11;                 // slot 0, marked
        CALL link;
        R3 = PASS R5;
        CALL link;
        R3 = PASS R6;
        CALL link;
        R3 = PASS R7;
        CALL link;
        CALL r0a;
        R4 = R0 AND R13;
        CALL r0b;
        R6 = R0 AND R13;
        R3 = PASS R8;
        CALL dacs;

        // Second half: the link's empty slots, then the right.
        R3 = PASS R2;
        CALL link;
        CALL link;
        CALL link;
        CALL link;
        CALL r0a;
        R5 = R0 AND R13;
        CALL r0b;
        R7 = R0 AND R13;
        R3 = PASS R14;
        CALL dacs;

        R15 = R15 - 1;
        IF NE JUMP frame;
        R15 = BEAT;
        BIT TGL FLAGS FLG0;
        JUMP frame;

// One word each way on the link: send R3, receive into R8 or R14 by slot.
// Uses R0, R1.
link:   R1 = DM(SPCTL6);
        R1 = R1 AND R9;
        COMP(R1, R9);
        IF EQ JUMP link;                // no room to send
        DM(TXSP6A) = R3;
lrx:    R1 = DM(SPCTL1);
        R1 = R1 AND R9;
        IF EQ JUMP lrx;                 // nothing received yet
        R0 = DM(RXSP1A);
        R1 = R0 AND R11;
        IF EQ JUMP count;
        R12 = 0;                        // the LPC's marker: this is slot 0
count:  R1 = R0 AND R13;                // the sample, marker cleared
        R0 = PASS R12;
        IF NE JUMP not0;
        R8 = PASS R1;                   // slot 0: left
        JUMP next;
not0:   R0 = R12 - R11;
        IF NE JUMP next;
        R14 = PASS R1;                  // slot 1: right
next:   R12 = R12 + 1;
        R12 = LSHIFT R12 BY 29;         // modulo 8: keep the low three bits
        R12 = LSHIFT R12 BY -29;
        RTS;

// One word from each ADC port into R0. Uses R1.
r0a:    R1 = DM(SPCTL0);
        R1 = R1 AND R9;
        IF EQ JUMP r0a;
        R0 = DM(RXSP0A);
        RTS;
r0b:    R1 = DM(SPCTL0);
        R1 = R1 AND R10;
        IF EQ JUMP r0b;
        R0 = DM(RXSP0B);
        RTS;

// R3 to every DAC port. Uses R1.
dacs:   R1 = DM(SPCTL4);
        R1 = R1 AND R9;
        COMP(R1, R9);
        IF EQ JUMP dacs;
        DM(TXSP4A) = R3;
d4b:    R1 = DM(SPCTL4);
        R1 = R1 AND R10;
        COMP(R1, R10);
        IF EQ JUMP d4b;
        DM(TXSP4B) = R3;
d5a:    R1 = DM(SPCTL5);
        R1 = R1 AND R9;
        COMP(R1, R9);
        IF EQ JUMP d5a;
        DM(TXSP5A) = R3;
        RTS;
