##############################################################################
# load_and_run.tcl -- MicroBlaze bulk loader for KCU116 RISC-V
# Change TEST_NAME below to switch tests
##############################################################################

# ---- CHANGE THIS TO SWITCH TESTS ----
set TEST_NAME  "qsort"
# Options: rv32ui-p-add, rv32ui-p-beq, rv32ui-p-bge, rv32ui-p-lui

# ---- Derived paths ----
set BASE_DIR   {I:/Vitis_Files/Pipeline_Tests/Global_Core_Revised/Benchmarks/rv32ui-p-benchmarks}
set BINARY_PATH "${BASE_DIR}/${TEST_NAME}_baremetal.bin"
set LOAD_ADDR    0x80000000
set ENTRY_PC     0x80000000
set TOHOST_ADDR  0x80001000

# ---- Assert HLS core reset via GPIO ----
puts ">>> Asserting HLS core reset..."
target 3
mwr 0x00010000 0x00000000
after 500

# ---- Load binary via MicroBlaze bulk transfer ----
puts ">>> Test: $TEST_NAME"
puts ">>> Loading binary via MicroBlaze: $BINARY_PATH"
dow -data $BINARY_PATH $LOAD_ADDR
puts ">>> Upload complete."

# ---- Verify first 4 words ----
puts ">>> Verify first 4 words:"
mrd $LOAD_ADDR 4

# ---- Switch to JTAG for peripheral writes ----
target 5

# ---- Clear tohost ----
puts ">>> Clearing tohost..."
mwr $TOHOST_ADDR 0x00000000

# ---- Initialize UART ----
puts ">>> Initializing UART..."
mwr 0x10000004 0x00
mwr 0x1000000C 0x80
mwr 0x10000000 0x6C
mwr 0x10000004 0x00
mwr 0x1000000C 0x03
mwr 0x10000008 0x07

# ---- Release HLS core reset ----
puts ">>> Releasing HLS core reset..."
target 3
mwr 0x00010000 0x00000001
after 500

# ---- Set HLS control registers ----
target 5
mwr 0x40000010 0xFFFFFFFF
mwr 0x40000028 $ENTRY_PC
mwr 0x40000030 0x00000000

puts ""
puts "============================================="
puts "  $TEST_NAME loaded"
puts "  entry_pc = $ENTRY_PC"
puts "  Press and hold GPIO_SW_N to start core."
puts "============================================="
puts ""

# ---- Poll ----
puts ">>> Polling tohost..."
for {set i 0} {$i < 120} {incr i} {
    set th [mrd -value $TOHOST_ADDR]
    if {($i % 5) == 0} {
        puts "  poll [format %3d $i]: tohost=$th"
    }
    if {$th != 0} {
        puts ">>> tohost changed! Value: $th"
        break
    }
    after 1000
}

# ---- Results ----
puts ""
set tohost [mrd -value $TOHOST_ADDR]
puts "  tohost: $tohost"

if {$tohost == 1} {
    puts "\n  >>> PASS - $TEST_NAME <<<"
} elseif {$tohost == 0} {
    puts "\n  >>> TIMEOUT - core may still be running"
} else {
    puts "\n  >>> FAIL - $TEST_NAME - test case: [expr {$tohost >> 1}]"
}
puts ">>> Done."
