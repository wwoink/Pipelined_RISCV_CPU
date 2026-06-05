##############################################################################
# uart_test.tcl — UART validation loader for KCU116 RISC-V
# Loads a minimal UART test program and runs it.
# Success = seeing 'A' followed by CR+LF on your terminal at 115200 baud.
##############################################################################

# ---- CHANGE THIS TO YOUR BINARY PATH ----
set BINARY_PATH {I:/Vitis_Files/Pipeline_Tests/Global_Core_Revised/Benchmarks/uart_test/uart_test.bin}

set LOAD_ADDR   0x80000000
set ENTRY_PC    0x80000000

# ---- Read binary file ----
puts ">>> UART Validation Test"
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
    set addr   [expr {$LOAD_ADDR + $offset}]

    set b0 0; set b1 0; set b2 0; set b3 0

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

    if {($i % 100) == 0 && $i > 0} {
        puts "  ... $i / $num_words words written"
    }
}

puts ">>> Upload complete: $num_words words written."

# ---- Verify first 4 words ----
puts ">>> Verify first 4 words:"
mrd $LOAD_ADDR 4

# ---- Set control registers ----
mwr 0x40000010 0xFFFFFFFF
mwr 0x40000028 $ENTRY_PC
mwr 0x40000030 0x00000000

puts ""
puts "============================================="
puts "  uart_test loaded ($num_words words)"
puts "  entry_pc = $ENTRY_PC"
puts "  Press and hold GPIO_SW_N to start core."
puts "  Expected output on terminal: A<CR><LF>"
puts "  Terminal settings: 115200 8N1"
puts "============================================="
puts ""
puts ">>> Waiting 10 seconds for UART output..."
puts ">>> Watch your terminal now."
after 10000
puts ">>> Done. Check terminal for 'A' character."
puts ">>> If nothing appeared, check:"
puts ">>>   1. AXI UART16550 clock frequency set to 200MHz in IP config"
puts ">>>   2. XDC has G19 (TX) and F19 (RX) with LVCMOS18"
puts ">>>   3. UART TX port connected through block design to top-level port"
