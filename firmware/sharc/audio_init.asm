// audio_init.asm - the stock audio set-up, shared by the programs that do audio.
//
// Appended to a program by tools/midi_control_sharc.py, so it has no .section of its
// own and lands after the program's code. Everything it writes is a value read
// out of the stock program (the parent project's docs/09 section 9, "The stock audio set-up"),
// applied in the stock program's order:
//   1. signal routing: serial-port clocks and frame syncs from the clock
//      generators, the data pins both ways, the clock outputs, the pin
//      enables; and DPI pin 14, the converters' reset, driven low
//   2. the clock generators, from CLKIN (24.576 MHz): A = 12.288 MHz / 192 kHz
//      (the LPC link, and the 256 fs master clock on DAI pin 1), B = 3.072 MHz
//      / 48 kHz (the converters)
//   3. a wait, then DPI pin 14 high
// It leaves alone the serial ports, which each program turns on itself, the
// sample-rate converter, the SPI routing the boot set up, and the PLL.
// Measured working on the unit, 2026-11-13, in the parent project.
//
// Call with the serial ports off. Uses R0-R2, I4; sets M0 = 0 and L4 = 0.

audio_init:
        M0 = 0;
        L4 = 0;

        // 1. Routing, each register (old AND keep) OR set. The stock table
        // as {mask, value}; keep is NOT mask.
        I4 = 0x2430; R1 = 0xc00f8000; R2 = 0x3bd0779d; CALL sru;  // SRU_CLK0: SPORT0,2,4,5 = PCG B, SPORT1 = PCG A
        I4 = 0x2450; R1 = 0xc00f8000; R2 = 0x3bd0779d; CALL sru;  // SRU_FS0: the same frame syncs
        I4 = 0x2435; R1 = 0xffffffe0; R2 = 0x0000001c; CALL sru;  // SRU_CLK5: SPORT6 = PCG A
        I4 = 0x2454; R1 = 0xffffffe0; R2 = 0x0000001c; CALL sru;  // SRU_FS4: SPORT6 = PCG A
        I4 = 0x2440; R1 = 0xc0fc0000; R2 = 0x200091c3; CALL sru;  // SRU_DAT0: SPORT0 A, B = DAI 4, 8; SPORT1 A = DAI 10
        I4 = 0x2460; R1 = 0xffe00000; R2 = 0x000edcb8; CALL sru;  // SRU_PIN0: DAI 1-3 = CLKA, CLKB, FSB
        I4 = 0x2461; R1 = 0xffe00000; R2 = 0x00078e9c; CALL sru;  // SRU_PIN1: DAI 5-7 = SPORT4 A, 4 B, 5 A
        I4 = 0x2462; R1 = 0xf01fff80; R2 = 0x0700002c; CALL sru;  // SRU_PIN2: DAI 9 = SPORT6 A, 12 = CLKA
        I4 = 0x2463; R1 = 0xf01fff80; R2 = 0x03a0003a; CALL sru;  // SRU_PIN3: DAI 13 = FSA, 16 = SPORT4 B
        I4 = 0x2478; R1 = 0xc0fc0000; R2 = 0x01001041; CALL sru;  // SRU_PBEN0: DAI 1, 2, 3, 5 driven
        I4 = 0x2479; R1 = 0xff03f000; R2 = 0x00040041; CALL sru;  // SRU_PBEN1: 6, 7, 9
        I4 = 0x247a; R1 = 0xfffc003f; R2 = 0x00001040; CALL sru;  // SRU_PBEN2: 12, 13
        I4 = 0x247b; R1 = 0xffffffc0; R2 = 0x00000001; CALL sru;  // SRU_PBEN3: 16
        I4 = 0x1c12; R1 = 0xff03ffff; R2 = 0x00000000; CALL sru;  // SRU2_PIN2: DPI 14 = low
        I4 = 0x1c22; R1 = 0xff03ffff; R2 = 0x00040000; CALL sru;  // SRU2_PBEN2: DPI 14 driven
        CALL wait;

        // 2. The clock generators, as the stock program writes them.
        R0 = 0;
        DM(0x24c0) = R0;
        DM(0x24c2) = R0;
        DM(0x24c6) = R0;
        DM(0x24c8) = R0;
        DM(0x24c9) = R0;
        R0 = 0x00100002;                // PCG_CTLA1: clock A = CLKIN / 2, frame-sync phase 1
        DM(0x24c1) = R0;
        R0 = 0x00400008;                // PCG_CTLB1: clock B = CLKIN / 8, phase 4
        DM(0x24c3) = R0;
        R0 = 0x01000040;                // PCG_PW: frame syncs high for half their period
        DM(0x24c4) = R0;
        R0 = 0xc0000080;                // PCG_CTLA0: on, frame sync A = CLKIN / 128
        DM(0x24c0) = R0;
        R0 = 0xc0000200;                // PCG_CTLB0: on, frame sync B = CLKIN / 512
        DM(0x24c2) = R0;
        R0 = 0;
        DM(0x24c6) = R0;
        DM(0x24c7) = R0;
        DM(0x24c8) = R0;
        DM(0x24c9) = R0;

        // 3. Clocks stable, then the converters out of reset.
        CALL wait;
        I4 = 0x1c12; R1 = 0xff03ffff; R2 = 0x00040000; CALL sru;  // SRU2_PIN2: DPI 14 = high
        RTS;

sru:    R0 = DM(I4, M0);                // I4 = register, R1 = keep, R2 = set
        R0 = R0 AND R1;
        R0 = R0 OR R2;
        DM(I4, M0) = R0;
        RTS;

wait:   R0 = 16;                        // 16 * 65535 passes: at least 2.6 ms at 400 MHz
outer:  LCNTR = 0xffff, DO inner UNTIL LCE;
inner:    NOP;
        R0 = R0 - 1;
        IF NE JUMP outer;
        RTS;
