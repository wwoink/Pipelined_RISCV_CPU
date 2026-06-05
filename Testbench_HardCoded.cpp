// --- Standard C++ Headers ---
#include <iostream>
#include <iomanip>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>

// --- Vitis HLS Headers ---
#include <ap_int.h>

// --- Local Project Headers ---
#include "elf.h"
#include "elfFile.h"
#include "core.h"

// ============================================================================
//  USER CONFIGURATION SWITCHES
// ============================================================================

// 1. Hardcoded Path
//    Start with a small bare-metal ELF before trying larger benchmarks.
#define ELF_PATH "I:/Vitis_Files/Pipeline_Tests/Pipelined_Core/Benchmarks/rv32ui-p/rv32ui-p-sltu"
// "I:/Vitis_Files/Pipeline_Tests/Pipelined_Core/Benchmarks/rv32ui-p-benchmarks/qsort.riscv"

// 2. Debug Switches
const bool ENABLE_CORE_DEBUG = false;
const bool ENABLE_MEMORY_INSPECTION = false;

// 3. Cycle Limit
const int MAX_CYCLES = 2000;
// ============================================================================

// Separate instruction and data memories for the pipelined core.
// The ELF is first loaded into load_mem, then copied into both imem and dmem.
// This keeps the current split-port core interface while preserving the original
// ELF-loaded memory image.
ap_uint<32> load_mem[RAM_SIZE];
ap_uint<32> imem[RAM_SIZE];
ap_uint<32> dmem[RAM_SIZE];

extern void riscv_init();

extern void riscv_step(volatile uint32_t* imem,
                       volatile uint32_t* dmem,
                       int max_cycles,
                       int* cycles_output,
                       ap_uint<32> entry_pc,
                       ap_uint<32> dtb_addr,
                       ap_uint<1> start);

extern bool CORE_DEBUG;

// Convert a normal RISC-V DRAM address into the current zero-base test address.
// Example: 0x80000000 -> 0x00000000.
static ap_uint<32> to_zero_base_pc(ap_uint<32> addr) {
    return (ap_uint<32>)((uint32_t)addr - (uint32_t)DRAM_BASE);
}

int main(int argc, char* argv[])
{
    const char* elf_filename = ELF_PATH;

    std::cout << "[TESTBENCH] Loading ELF: " << elf_filename << "\n";

    // Clear memories
    std::memset(load_mem, 0, sizeof(load_mem));
    std::memset(imem,     0, sizeof(imem));
    std::memset(dmem,     0, sizeof(dmem));

    // Load ELF into a temporary unified memory image using the existing loader.
    ElfFile loader(elf_filename);
    ap_uint<32> elf_entry_pc = loader.load_to_mem(load_mem, RAM_SIZE);

    if (elf_entry_pc == 0) {
        std::cout << "[TESTBENCH] CRITICAL ERROR: Could not load ELF (ENTRY_PC is 0).\n";
        return 1;
    }

    // Copy the loaded memory image into both IMEM and DMEM.
    // This is a zero-base split-memory bring-up model:
    //   - fetch reads from imem[]
    //   - loads/stores use dmem[]
    //   - both begin with the same ELF-loaded contents
    for (unsigned i = 0; i < RAM_SIZE; i++) {
        imem[i] = load_mem[i];
        dmem[i] = load_mem[i];
    }

    // Convert the ELF entry point to the current zero-base core address model.
    ap_uint<32> zero_entry_pc = to_zero_base_pc(elf_entry_pc);

    std::cout << "[TESTBENCH] ELF entry PC       = 0x" << std::hex << (uint32_t)elf_entry_pc << "\n";
    std::cout << "[TESTBENCH] Zero-base entry PC = 0x" << std::hex << (uint32_t)zero_entry_pc << std::dec << "\n";

    // Dynamic tohost calculation.
    // The loader reports the normal ELF/DRAM address, but dmem[] is indexed from DRAM_BASE.
    unsigned tohost_idx = 0;
    if (loader.tohost_addr_found != 0) {
        tohost_idx = (loader.tohost_addr_found - DRAM_BASE) >> 2;
        std::cout << "[TESTBENCH] Detected .tohost at 0x" << std::hex << loader.tohost_addr_found
                  << " (Index " << std::dec << tohost_idx << ")\n";
    } else {
        std::cout << "[TESTBENCH] WARNING: .tohost section not found, using default 0x80001000\n";
        tohost_idx = (0x80001000 - DRAM_BASE) >> 2;
    }

    if (tohost_idx >= RAM_SIZE) {
        std::cout << "[TESTBENCH] CRITICAL ERROR: tohost index is outside RAM_SIZE.\n";
        return 1;
    }

    std::cout << "\n[TESTBENCH] Initializing Core...\n";

    CORE_DEBUG = ENABLE_CORE_DEBUG;

    // The current core is using ZERO_BASE_TEST, so the architectural entry point
    // for this testbench must be the zero-base translated PC.
    ENTRY_PC = zero_entry_pc;
    DTB_ADDR = 0;

    riscv_init();

    std::cout << "\n[TESTBENCH] Starting Simulation...\n";

    bool passed = false;
    int final_cycle_count = 0;

    // Single call to hardware.
    riscv_step((volatile uint32_t*)imem,
               (volatile uint32_t*)dmem,
               MAX_CYCLES,
               &final_cycle_count,
               zero_entry_pc,
               0x0,
               1);

    std::cout << "--------------------------------------------------\n";
    std::cout << "[TESTBENCH] Hardware Finished.\n";
    std::cout << "[TESTBENCH] Total Cycles Executed: " << final_cycle_count << "\n";
    std::cout << "--------------------------------------------------\n";

    // Check .tohost in DMEM because the core writes data memory through dmem[].
    uint32_t tohost = (uint32_t)dmem[tohost_idx];

    std::cout << "[TESTBENCH] tohost = 0x" << std::hex << tohost << std::dec << "\n";

    if (tohost & 1) {
        int exit_code = tohost >> 1;
        if (exit_code == 0) {
            std::cout << "[TESTBENCH] PASS (Hardware exited via tohost)\n";
            passed = true;
        } else {
            std::cout << "[TESTBENCH] FAIL (Code: " << exit_code << ")\n";
            passed = false;
        }
    } else {
        std::cout << "[TESTBENCH] ERROR (Hardware returned, but tohost is 0 - Unknown Error)\n";
        passed = false;
    }

    // ================================================================
    // MEMORY INSPECTION
    // ================================================================
    if (passed && ENABLE_MEMORY_INSPECTION) {
        std::cout << "\n[INSPECTION] Checking selected memory region...\n";
        uint32_t data_addr = 0x80005d94;

        if (data_addr != 0) {
            unsigned ram_idx = (data_addr - DRAM_BASE) >> 2;
            std::cout << "Reading from 0x" << std::hex << data_addr << std::dec << "\n";
            for (int k = 0; k < 20; k++) {
                if (ram_idx + k < RAM_SIZE) {
                    int value = (int)dmem[ram_idx + k];
                    std::cout << "[" << k << "] " << value << "\n";
                }
            }
        }
    }

    if (passed) {
        return 0;
    } else {
        return 1;
    }
}