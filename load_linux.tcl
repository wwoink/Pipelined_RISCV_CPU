##############################################################################
# load_linux.tcl — Linux boot loader for KCU116 RISC-V HLS Core
#                  (MicroBlaze bulk-transfer version)
#
# Memory Layout:
#   0x80000000 — Kernel Image (~3.6MB)
#   0x80800000 — Device Tree Blob (system.dtb)
#
# Targets (verify in `targets` listing — IDs may shift):
#   target 3 — MicroBlaze (used for fast dow -data bulk transfer)
#   target 5 — JTAG-to-AXI (used for peripheral mwr/mrd)
#
# Note: Core sits in WAIT_FOR_START spin loop until GPIO_SW_N is pressed,
#       so no GPIO reset is required for first boot after bitstream load.
#
# Usage:
#   1. Run script in XSCT
#   2. Watch UART terminal (115200 8N1) for boot output
#   3. Press and hold GPIO_SW_N to start core
##############################################################################

# ---- Paths ----
set BASE_DIR     {I:/Vitis_Files/Pipeline_Tests/Global_Core_Revised}
set KERNEL_PATH  "${BASE_DIR}/Image"
set DTB_PATH     "${BASE_DIR}/system.dtb"

# ---- Memory layout ----
set KERNEL_ADDR  0x80000000
set DTB_ADDR     0x80800000
set ENTRY_PC     0x80000000

# ============================================================
# STEP 1: Load Kernel Image via MicroBlaze bulk transfer
# ============================================================
puts ">>> Loading Kernel Image (~3.6MB) via MicroBlaze bulk transfer..."
puts ">>>   Source: $KERNEL_PATH"
puts ">>>   Dest:   [format 0x%08X $KERNEL_ADDR]"
target 3
dow -data $KERNEL_PATH $KERNEL_ADDR
puts ">>> Kernel upload complete."

puts ">>> Verify kernel first 4 words:"
mrd $KERNEL_ADDR 4

# ============================================================
# STEP 2: Load Device Tree Blob via MicroBlaze bulk transfer
# ============================================================
puts ">>> Loading Device Tree Blob..."
puts ">>>   Source: $DTB_PATH"
puts ">>>   Dest:   [format 0x%08X $DTB_ADDR]"
dow -data $DTB_PATH $DTB_ADDR
puts ">>> DTB upload complete."

# Valid DTB magic = 0xD00DFEED big-endian (= 0xEDFE0DD0 little-endian)
puts ">>> Verify DTB magic word (expect 0xEDFE0DD0):"
mrd $DTB_ADDR 1
puts ">>> Verify DTB first 4 words:"
mrd $DTB_ADDR 4

# ============================================================
# STEP 3: Switch to JTAG-AXI for peripheral writes
# ============================================================
target 5

# ============================================================
# STEP 4: Initialize UART (200MHz, 115200 baud, 8N1)
# ============================================================
puts ">>> Initializing UART (200MHz, 115200 baud, 8N1)..."
mwr 0x10000004 0x00
mwr 0x1000000C 0x80
mwr 0x10000000 0x6C
mwr 0x10000004 0x00
mwr 0x1000000C 0x03
mwr 0x10000008 0x07

# ============================================================
# STEP 5: Set HLS control registers
# ============================================================
puts ">>> Setting control registers..."
mwr 0x40000010 0xFFFFFFFF    ;# max_cycles = run forever
mwr 0x40000028 $ENTRY_PC     ;# entry_pc   = 0x80000000
mwr 0x40000030 $DTB_ADDR     ;# dtb_addr   = 0x80800000

puts ""
puts "============================================="
puts "  Linux Boot Loader Ready"
puts "  Kernel:    [format 0x%08X $KERNEL_ADDR]"
puts "  DTB:       [format 0x%08X $DTB_ADDR]"
puts "  entry_pc:  [format 0x%08X $ENTRY_PC]"
puts "  dtb_addr:  [format 0x%08X $DTB_ADDR]"
puts ""
puts "  Linux does not use tohost -- watch UART terminal"
puts "  (115200 8N1) for boot messages."
puts ""
puts "  Press and hold GPIO_SW_N to start core."
puts "============================================="
puts ""
puts ">>> Done."