# run_test.tcl
# XSCT script for RISC-V softcore benchmark test
# 
# Usage:
#   1. Open XSCT (after fresh board reset)
#   2. Connect and select JTAG2AXI target manually:
#        connect
#        targets
#        target 3
#   3. Source this script:
#        source {I:/Vitis_Files/Pipeline_Tests/Global_Core_Revised/Benchmarks/rv32ui-p-benchmarks/run_test.tcl}
#
# WARNING: If the core gets stuck, the bus will be corrupted when 
#          max_cycles kills it. You will need to reset the board.

# ============================================
# Configuration - edit these as needed
# ============================================
set BIN_FILE   {I:/Vitis_Files/Pipeline_Tests/Global_Core_Revised/Benchmarks/rv32ui-p-benchmarks/qsort.bin}
set DDR_BASE    0x80000000
set CTRL_BASE   0x40000000
set TOHOST      0x80001000

# qsort specific addresses
set INPUT_DATA  0x80005D88
set VERIFY_DATA 0x80003D88

# Control register offsets
set CTRL_REG    [expr {$CTRL_BASE + 0x00}]
set MAX_CYCLES  [expr {$CTRL_BASE + 0x10}]
set ENTRY_PC    [expr {$CTRL_BASE + 0x28}]
set DTB_ADDR    [expr {$CTRL_BASE + 0x30}]

# Readback registers
set CYCLES_OUT      [expr {$CTRL_BASE + 0x18}]
set CYCLES_OUT_CTRL [expr {$CTRL_BASE + 0x1C}]

# ============================================
# Step 1: Sanity check - make sure bus is clean
# ============================================
puts "\n=== Sanity check: testing DDR access ==="
mwr $DDR_BASE 0xDEADBEEF
set sanity [mrd -value $DDR_BASE]
if {$sanity != 0xDEADBEEF} {
    puts "  ERROR: DDR read back [format 0x%08X $sanity] instead of 0xDEADBEEF"
    puts "  Bus may be corrupted. Reset the board and try again."
    return
}
puts "  Bus is clean"

# ============================================
# Step 2: Stop core and clear CTRL
# ============================================
puts "\n=== Stopping core and clearing CTRL ==="
mwr $CTRL_REG 0x00
after 100
set ctrl_rd [mrd -value $CTRL_REG]
puts "  CTRL after clear: [format 0x%08X $ctrl_rd]"

# ============================================
# Step 3: Set up control registers
# ============================================
puts "\n=== Setting up control registers ==="

# Set max_cycles very high so core runs until ecall
mwr $MAX_CYCLES 0xFFFFFFFF
puts "  max_cycles  = 0xFFFFFFFF (~4.3 billion cycles)"

mwr $ENTRY_PC $DDR_BASE
puts "  entry_pc    = [format 0x%08X $DDR_BASE]"

# Verify
set rd_max [mrd -value $MAX_CYCLES]
set rd_pc  [mrd -value $ENTRY_PC]
puts "  max_cycles  readback: $rd_max"
puts "  entry_pc    readback: $rd_pc"

# ============================================
# Step 4: Download binary to DDR
# ============================================
puts "\n=== Downloading binary ==="
puts "  File: $BIN_FILE"
puts "  Dest: [format 0x%08X $DDR_BASE]"

dow -data $BIN_FILE $DDR_BASE

# ============================================
# Step 5: Clear tohost AFTER download (binary overwrites it)
# ============================================
puts "\n=== Clearing tohost (after download) ==="
mwr $TOHOST 0x00000000
set tohost_rd [mrd -value $TOHOST]
puts "  tohost after clear: [format 0x%08X $tohost_rd]"
if {$tohost_rd != 0} {
    puts "  WARNING: tohost did not clear! Value: [format 0x%08X $tohost_rd]"
}

# ============================================
# Step 6: Verify first 8 words of DDR
# ============================================
puts "\n=== Verifying first 8 words of DDR ==="
for {set i 0} {$i < 8} {incr i} {
    set addr [expr {$DDR_BASE + $i * 4}]
    set val [mrd -value $addr]
    puts "  [format 0x%08X $addr]: [format 0x%08X $val]"
}

# ============================================
# Step 7: Show first 4 input_data words before sort
# ============================================
puts "\n=== Input data before sort (first 4 words) ==="
for {set i 0} {$i < 4} {incr i} {
    set addr [expr {$INPUT_DATA + $i * 4}]
    set val [mrd -value $addr]
    puts "  [format 0x%08X $addr]: [format 0x%08X $val]"
}

# ============================================
# Step 8: Start core
# ============================================
puts "\n=== Starting core (AP_START = 0x01) ==="
mwr $CTRL_REG 0x01
set ctrl_rd [mrd -value $CTRL_REG]
puts "  CTRL after start: [format 0x%08X $ctrl_rd]"

if {$ctrl_rd == 0x04 || $ctrl_rd == 0x0E} {
    puts "  WARNING: Core did not start (still idle). Aborting."
    puts "  Consider triggering AP_START from a physical button instead."
    return
}

# ============================================
# Step 9: Poll for completion
# ============================================
puts "\n=== Waiting for core to finish (timeout 60s) ==="
set timeout 60
set elapsed 0
set done 0

while {$elapsed < $timeout} {
    after 1000
    incr elapsed

    set vld [mrd -value $CYCLES_OUT_CTRL]
    if {$vld == 1} {
        set done 1
        puts "\n  Core finished after ~${elapsed}s"
        break
    }
    puts -nonewline "."
    flush stdout
}

if {!$done} {
    puts "\n  WARNING: Core did not finish within ${timeout}s"
    puts "  The core may be stuck. Reading status anyway..."
    puts "  NOTE: If reads return garbage, the bus is corrupted. Reset board."
}

# ============================================
# Step 10: Read results
# ============================================
puts "\n=== Results ==="
set status  [mrd -value $CTRL_REG]
set cycles  [mrd -value $CYCLES_OUT]
set cyc_vld [mrd -value $CYCLES_OUT_CTRL]
set tohost  [mrd -value $TOHOST]

puts "  CTRL status       : [format 0x%08X $status]"
puts "  cycles_output     : [format 0x%08X $cycles] ([expr {$cycles}] cycles)"
puts "  cycles_output_vld : $cyc_vld"
puts "  tohost            : [format 0x%08X $tohost]"

if {$tohost == 1} {
    puts "\n  >>> BENCHMARK PASSED <<<"
} elseif {$tohost == 0} {
    puts "\n  >>> tohost not written - core may not have reached ecall <<<"
} else {
    set fail_case [expr {$tohost >> 1}]
    puts "\n  >>> BENCHMARK FAILED - test case $fail_case <<<"
}

# ============================================
# Step 11: Compare sorted input_data vs verify_data
# ============================================
if {$done} {
    puts "\n=== Comparing sorted data vs expected (first 16 words) ==="
    set mismatches 0
    for {set i 0} {$i < 16} {incr i} {
        set addr_in  [expr {$INPUT_DATA + $i * 4}]
        set addr_exp [expr {$VERIFY_DATA + $i * 4}]
        set val_in  [mrd -value $addr_in]
        set val_exp [mrd -value $addr_exp]
        set match [expr {$val_in == $val_exp ? "OK" : "MISMATCH"}]
        if {$val_in != $val_exp} {incr mismatches}
        puts "  \[$i\] sorted=[format 0x%08X $val_in]  expected=[format 0x%08X $val_exp]  $match"
    }
    if {$mismatches == 0} {
        puts "  All 16 checked words match!"
    } else {
        puts "  $mismatches mismatches in first 16 words"
    }
}

puts "\n=== Done ==="