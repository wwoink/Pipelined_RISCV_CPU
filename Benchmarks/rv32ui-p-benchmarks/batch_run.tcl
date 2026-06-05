##############################################################################
# batch_run.tcl — Automated batch benchmark runner for KCU116 RISC-V
# Runs all benchmarks in sequence, pausing for button press between each.
# Source this from target 5 after loading MicroBlaze hello world.
##############################################################################

set BASE_DIR   {I:/Vitis_Files/Pipeline_Tests/Global_Core_Revised/Benchmarks/rv32ui-p-benchmarks}
set LOAD_ADDR   0x80000000
set ENTRY_PC    0x80000000
set TOHOST_ADDR 0x80001000

set TESTS {median qsort rsort towers vvadd memcpy multiply dhrystone}

set total [llength $TESTS]
set pass_count 0
set fail_count 0
set results {}

puts ""
puts "############################################"
puts "  KCU116 RISC-V Batch Benchmark Runner"
puts "  $total tests queued"
puts "############################################"
puts ""

set idx 0
foreach TEST_NAME $TESTS {
    incr idx
    set BINARY_PATH "${BASE_DIR}/${TEST_NAME}_baremetal.bin"

    puts "--------------------------------------------"
    puts ">>> Test $idx/$total: $TEST_NAME"
    puts "--------------------------------------------"

    # ---- Assert HLS core reset via GPIO ----
    puts ">>> Asserting HLS core reset..."
    target 3
    mwr 0x00010000 0x00000000
    after 500

    # ---- Load binary via MicroBlaze bulk transfer ----
    puts ">>> Loading binary: $BINARY_PATH"
    dow -data $BINARY_PATH $LOAD_ADDR
    puts ">>> Upload complete."

    # ---- Switch to JTAG for peripheral writes ----
    target 5

    # ---- Clear tohost ----
    mwr $TOHOST_ADDR 0x00000000

    # ---- Initialize UART ----
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
    puts "  Press and hold GPIO_SW_N to start core."
    puts ""

    # ---- Poll tohost ----
    set passed 0
    for {set i 0} {$i < 120} {incr i} {
        set th [mrd -value $TOHOST_ADDR]
        if {($i % 5) == 0} {
            puts "  poll [format %3d $i]: tohost=$th"
        }
        if {$th != 0} {
            puts ">>> tohost changed! Value: $th"
            set passed $th
            break
        }
        after 1000
    }

    # ---- Record result ----
    set tohost [mrd -value $TOHOST_ADDR]
    if {$tohost == 1} {
        puts "\n  >>> PASS - $TEST_NAME <<<"
        incr pass_count
        lappend results "  PASS: $TEST_NAME"
    } elseif {$tohost == 0} {
        puts "\n  >>> TIMEOUT - $TEST_NAME"
        incr fail_count
        lappend results "  TIMEOUT: $TEST_NAME"
    } else {
        set tc [expr {$tohost >> 1}]
        puts "\n  >>> FAIL - $TEST_NAME (test case $tc)"
        incr fail_count
        lappend results "  FAIL: $TEST_NAME (test case $tc)"
    }
    puts ""
}

# ---- Final Summary ----
puts ""
puts "############################################"
puts "  BATCH COMPLETE -- RESULTS: $pass_count/$total PASS"
puts "############################################"
foreach r $results {
    puts $r
}
puts "############################################"
puts ""