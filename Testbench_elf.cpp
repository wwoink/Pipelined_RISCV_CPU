// --- Standard C++ Headers ---
#include <iostream>
#include <iomanip>
#include <cstring>
#include <cstdint>

// --- Vitis HLS Headers ---
#include <ap_int.h>

// --- Local Project Headers ---
#include "elf.h"
#include "elfFile.h"
#include "core.h"

// ============================================================================
// USER CONFIGURATION
// ============================================================================

// Increase this if median.riscv has not written tohost before the core returns.
#define MAX_CYCLES 500000

const bool ENABLE_CORE_DEBUG = false;
const bool ENABLE_MEMORY_INSPECTION = false;

// ============================================================================
// MEMORY ARRAYS
// ============================================================================
//
// The current pipelined core has separate imem and dmem AXI interfaces.
// The ELF contains both instructions and initialized data, so it is loaded
// into imem first and then copied into dmem before execution.
//
ap_uint<32> imem[RAM_SIZE];
ap_uint<32> dmem[RAM_SIZE];

// Globals provided by the core.
extern ap_uint<32> ENTRY_PC;
extern ap_uint<32> DTB_ADDR;
extern bool CORE_DEBUG;

extern void riscv_init();

extern void riscv_step(
    volatile uint32_t* imem,
    volatile uint32_t* dmem,
    int max_cycles,
    int* cycles_output,
    ap_uint<32> entry_pc,
    ap_uint<32> dtb_addr,
    ap_uint<1> start
);

int main(int argc, char* argv[]) {
    const char* elf_filename = "memcpy.riscv";
    if (argc > 1) {
        elf_filename = argv[1];
    }

    std::cout << "[TESTBENCH] Loading ELF: " << elf_filename << "\n";

    std::memset(imem, 0, sizeof(imem));
    std::memset(dmem, 0, sizeof(dmem));

    // Load the complete ELF image into instruction memory.
    ElfFile loader(elf_filename);
    ENTRY_PC = loader.load_to_mem(imem, RAM_SIZE);

    // The program's initialized data must also be visible through dmem.
    std::memcpy(dmem, imem, sizeof(imem));

    // median.riscv does not require a DTB, but the current core interface
    // expects the argument.
    DTB_ADDR = 0;

    // Locate the tohost word used by the RISC-V test/runtime.
    unsigned tohost_idx = 0;
    if (loader.tohost_addr_found != 0) {
        tohost_idx = (loader.tohost_addr_found - DRAM_BASE) >> 2;

        std::cout << "[TESTBENCH] Detected .tohost at 0x"
                  << std::hex << loader.tohost_addr_found
                  << " (Index " << std::dec << tohost_idx << ")\n";
    } else {
        constexpr uint32_t DEFAULT_TOHOST_ADDR = 0x80001000;
        tohost_idx = (DEFAULT_TOHOST_ADDR - DRAM_BASE) >> 2;

        std::cout << "[TESTBENCH] WARNING: .tohost section not found; "
                  << "using default 0x" << std::hex << DEFAULT_TOHOST_ADDR
                  << std::dec << "\n";
    }

    if (tohost_idx >= RAM_SIZE) {
        std::cerr << "[TESTBENCH] ERROR: tohost index is outside RAM_SIZE.\n";
        return 1;
    }

    CORE_DEBUG = ENABLE_CORE_DEBUG;

#ifndef __SYNTHESIS__
    // C-simulation uses the core's software initialization path.
    riscv_init();
#endif

    int cycles_output = 0;
    ap_uint<1> start = 1;

    std::cout << "[TESTBENCH] Starting simulation for at most "
              << MAX_CYCLES << " core-loop iterations...\n";

    riscv_step(
        reinterpret_cast<volatile uint32_t*>(imem),
        reinterpret_cast<volatile uint32_t*>(dmem),
        MAX_CYCLES,
        &cycles_output,
        ENTRY_PC,
        DTB_ADDR,
        start
    );

    const uint32_t tohost = static_cast<uint32_t>(dmem[tohost_idx]);

    std::cout << "[TESTBENCH] Core returned after "
              << cycles_output << " cycles.\n";
    std::cout << "[TESTBENCH] tohost = 0x"
              << std::hex << tohost << std::dec << "\n";

    bool passed = false;

    if (tohost & 1U) {
        const uint32_t exit_code = tohost >> 1;

        if (exit_code == 0) {
            std::cout << "[TESTBENCH] PASS\n";
            passed = true;
        } else {
            std::cout << "[TESTBENCH] FAIL (Code: "
                      << exit_code << ")\n";
        }
    } else {
        std::cout << "[TESTBENCH] TIMEOUT/INCOMPLETE: "
                  << "the program did not write a completed tohost value "
                  << "before the cycle limit.\n";
    }

    // Optional example inspection area. Update the address and interpretation
    // for the specific benchmark before enabling.
    if (passed && ENABLE_MEMORY_INSPECTION) {
        constexpr uint32_t data_addr = 0x80000000;
        const unsigned data_idx = (data_addr - DRAM_BASE) >> 2;

        std::cout << "\n[INSPECTION] Data beginning at 0x"
                  << std::hex << data_addr << std::dec << ":\n";

        for (int i = 0; i < 20 && data_idx + i < RAM_SIZE; ++i) {
            std::cout << "[" << i << "] "
                      << static_cast<int32_t>(dmem[data_idx + i]) << "\n";
        }
    }

    return passed ? 0 : 1;
}