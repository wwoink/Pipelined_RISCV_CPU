##############################################################################
# run_benchmark.tcl — XSCT script for RISC-V softcore on KCU116
# Core uses ap_ctrl_none + external push button (GPIO_SW_N) to start
##############################################################################

# ---- Configuration — edit these per benchmark ----
set BINARY_PATH  {I:/Vitis_Files/Pipeline_Tests/Global_Core_Revised/Benchmarks/rv32ui-p-benchmarks/qsort.bin}
set LOAD_ADDR    0x80000000
set ENTRY_PC     0x80000000
set TOHOST_ADDR  0x80001000

# ---- Address map ----
set CTRL_BASE    0x40000000
set MAX_CYCLES   [expr {$CTRL_BASE + 0x10}]
set CYCLES_OUT   [expr {$CTRL_BASE + 0x18}]
set CYCLES_VLD   [expr {$CTRL_BASE + 0x1C}]
set ENTRY_PC_REG [expr {$CTRL_BASE + 0x28}]
set DTB_ADDR_REG [expr {$CTRL_BASE + 0x30}]
set DDR_BASE     0x80000000

# ---- Connect ----
puts ">>> Connecting to hw_server..."
connect
after 500

puts ">>> Available targets:"
targets

# Select JTAG2AXI target — adjust number if needed
puts ">>> Selecting JTAG2AXI target (target 3)..."
target 3
after 200

# ---- Bus sanity check ----
puts ">>> Bus sanity check..."
mwr $DDR_BASE 0xDEADBEEF
set readback [mrd -value $DDR_BASE]
if {$readback != 0xDEADBEEF} {
    puts "!!! BUS ERROR: Wrote 0xDEADBEEF, read back $readback"
    puts "!!! Reset the board and try again."
    return
}
puts ">>> Bus OK (read back 0xDEADBEEF)"

# ---- Write control registers ----
puts ">>> Setting max_cycles = 0xFFFFFFFF (run forever)..."
mwr $MAX_CYCLES 0xFFFFFFFF

puts ">>> Setting entry_pc = $ENTRY_PC..."
mwr $ENTRY_PC_REG $ENTRY_PC

puts ">>> Setting dtb_addr = 0x00000000 (bare-metal, no DTB)..."
mwr $DTB_ADDR_REG 0x00000000

# ---- Download binary ----
puts ">>> Downloading binary: $BINARY_PATH"
puts ">>>   to address $LOAD_ADDR ..."
dow -data $BINARY_PATH $LOAD_ADDR
after 500

# ---- Clear tohost AFTER download (binary overlaps 0x80001000) ----
puts ">>> Clearing tohost at $TOHOST_ADDR..."
mwr $TOHOST_ADDR 0x00000000

# ---- Verify tohost is clear ----
set tohost_check [mrd -value $TOHOST_ADDR]
if {$tohost_check != 0} {
    puts "!!! WARNING: tohost not zero after clear (got $tohost_check)"
} else {
    puts ">>> tohost cleared OK"
}

# ---- Snapshot input data before core runs ----
puts ""
puts ">>> Snapshot of input_data at 0x80005D88 BEFORE button press:"
mrd 0x80005D88 4

# ---- Poll for core activity ----
puts ""
puts "============================================="
puts "  Setup complete. Binary loaded."
puts "  Press GPIO_SW_N (north button) on the"
puts "  KCU116 board to start the RISC-V core."
puts "============================================="
puts ""
puts ">>> Polling tohost and cycles_vld (1/sec, 120s timeout)..."

set core_done 0
for {set i 0} {$i < 120} {incr i} {
    set th [mrd -value $TOHOST_ADDR]
    set cv [mrd -value $CYCLES_VLD]
    puts "  poll [format %3d $i]: tohost=$th  cycles_vld=$cv"
    if {$th != 0} {
        puts ">>> Core wrote to tohost!"
        set core_done 1
        break
    }
    if {$cv != 0} {
        puts ">>> cycles_output valid flag set!"
        set core_done 1
        break
    }
    after 1000
}

if {!$core_done} {
    puts ""
    puts "!!! Timeout — core did not finish in 120 seconds."
    puts "!!! Check that the button is wired correctly."
    puts ""
}

# ---- Read results ----
puts ""
puts ">>> Reading results..."
puts "-----------------------------------------------"

set cycles_vld [mrd -value $CYCLES_VLD]
set cycles     [mrd -value $CYCLES_OUT]
set tohost     [mrd -value $TOHOST_ADDR]

puts "  cycles_output_vld : $cycles_vld"
puts "  cycles_output     : $cycles"
puts "  tohost            : $tohost"

# ---- Decode tohost ----
if {$tohost == 1} {
    puts ""
    puts "  >>> PASS <<<"
} elseif {$tohost == 0} {
    puts ""
    puts "  >>> Core may still be running (tohost = 0)"
} else {
    set fail_test [expr {$tohost >> 1}]
    puts ""
    puts "  >>> FAIL — failing test case: $fail_test"
}

# ---- Snapshot input data after core runs ----
puts ""
puts ">>> Snapshot of input_data at 0x80005D88 AFTER core ran:"
mrd 0x80005D88 4

puts "-----------------------------------------------"
puts ">>> Done."
