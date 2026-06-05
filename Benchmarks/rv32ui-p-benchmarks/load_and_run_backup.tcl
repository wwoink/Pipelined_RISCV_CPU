##############################################################################
# load_and_run.tcl — Word-by-word loader for KCU116 RISC-V
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

# ---- Read binary file ----
puts ">>> Test: $TEST_NAME"
puts ">>> Reading binary: $BINARY_PATH"
set fp [open $BINARY_PATH rb]
set bindata [read $fp]
close $fp

set filesize [string length $bindata]
set num_words [expr {($filesize + 3) / 4}]
puts ">>> File size: $filesize bytes ($num_words words)"

# ---- Write word-by-word ----
puts ">>> Writing to DDR at $LOAD_ADDR ..."

for {set i 0} {$i < $num_words} {incr i} {
    set offset [expr {$i * 4}]
    set addr [expr {$LOAD_ADDR + $offset}]

    set b0 0
    set b1 0
    set b2 0
    set b3 0
    if {$offset < $filesize} {
        binary scan $bindata @${offset}c b0
        set b0 [expr {$b0 & 0xFF}]
    }
    if {[expr {$offset + 1}] < $filesize} {
        set off1 [expr {$offset + 1}]
        binary scan $bindata @${off1}c b1
        set b1 [expr {$b1 & 0xFF}]
    }
    if {[expr {$offset + 2}] < $filesize} {
        set off2 [expr {$offset + 2}]
        binary scan $bindata @${off2}c b2
        set b2 [expr {$b2 & 0xFF}]
    }
    if {[expr {$offset + 3}] < $filesize} {
        set off3 [expr {$offset + 3}]
        binary scan $bindata @${off3}c b3
        set b3 [expr {$b3 & 0xFF}]
    }

    set word [expr {($b3 << 24) | ($b2 << 16) | ($b1 << 8) | $b0}]
    mwr $addr $word

    if {($i % 1000) == 0 && $i > 0} {
        puts "  ... $i / $num_words words written"
    }
}

puts ">>> Upload complete: $num_words words written."

# ---- Verify first 4 words ----
puts ">>> Verify first 4 words:"
mrd $LOAD_ADDR 4

# ---- Clear tohost ----
puts ">>> Clearing tohost..."
mwr $TOHOST_ADDR 0x00000000

# Initialize UART before starting core
puts ">>> Initializing UART..."
mwr 0x10000004 0x00
mwr 0x1000000C 0x80
mwr 0x10000000 0x6C
mwr 0x10000004 0x00
mwr 0x1000000C 0x03
mwr 0x10000008 0x07

# ---- Set control registers ----
mwr 0x40000010 0xFFFFFFFF
mwr 0x40000028 $ENTRY_PC
mwr 0x40000030 0x00000000

puts ""
puts "============================================="
puts "  $TEST_NAME loaded ($num_words words)"
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
