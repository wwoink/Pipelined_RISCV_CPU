# Initialize UART directly via JTAG-to-AXI
mwr 0x10000004 0x00       ;# IER - disable interrupts
mwr 0x1000000C 0x80       ;# LCR - DLAB=1
mwr 0x10000000 0x6C       ;# DLL - divisor low (108)
mwr 0x10000004 0x00       ;# DLM - divisor high
mwr 0x1000000C 0x03       ;# LCR - 8N1, DLAB=0
mwr 0x10000008 0x07       ;# FCR - enable FIFOs

# Check LSR - should show 0x60 (TX empty)
mrd 0x10000014

# Send 'A' (0x41)
mwr 0x10000000 0x41
mwr 0x10000000 0x0D       ;# CR
mwr 0x10000000 0x0A       ;# LF