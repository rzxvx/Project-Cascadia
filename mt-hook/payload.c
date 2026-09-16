typedef unsigned int u32;
/* No output. Enable SPI1/PWM PMGR gates+clocks, then read PWM then SPI1.
   If a block is unpowered the read faults -> panic fault_addr names it.
   If we return cleanly, boot continues to shell = both powered. */
void mt_peek_dump(u32(*rd)(u32), void(*wr)(u32,u32)){
    wr(0x3F100020, rd(0x3F100020)|0x180);   /* clk SPI1A */
    wr(0x3F10002C, rd(0x3F10002C)|0x180);   /* clk SPI1B */
    wr(0x3F100010, rd(0x3F100010)|0x180);   /* clk PWM   */
    wr(0x3F101110, rd(0x3F101110)|0xF); wr(0x3F101118, rd(0x3F101118)|0xF);  /* gate 68 SPI1 */
    wr(0x3F10114C, rd(0x3F10114C)|0xF); wr(0x3F101154, rd(0x3F101154)|0xF);  /* gate 83 PWM  */
    (void)rd(0x33500300);   /* probe PWM grape  */
    (void)rd(0x32100000);   /* probe SPI1 ctrl  */
}
