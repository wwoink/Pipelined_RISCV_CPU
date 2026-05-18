#include <iostream>
#include <iomanip>
#include <cstring>
#include <cstdint>
#include <ap_int.h>
#include "core.h"

// ------------------------------------------------------------
// Select which directed forwarding test to run
// ------------------------------------------------------------
// 1 = ALU result forwarding test
// 2 = ALU dependency chain test
// 3 = Load-use stall + forwarding test
static const int TEST_CASE = 2;

// ------------------------------------------------------------
// Separate Instruction and Data Memories
// ------------------------------------------------------------
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

// ------------------------------------------------------------
// Helper: write instruction into IMEM using byte address
// ------------------------------------------------------------
static void write_instr(uint32_t byte_addr, uint32_t instr_word) {
    unsigned idx = byte_addr >> 2;
    if (idx < RAM_SIZE) {
        imem[idx] = instr_word;
    }
}

// ------------------------------------------------------------
// Directed Test Programs
// ------------------------------------------------------------
static uint32_t load_test_program(uint32_t entry_pc) {
    uint32_t expected = 0;

    if (TEST_CASE == 1) {
        std::cout << "[TB] Test Case 1: ALU forwarding into execute\n";

        // x1 = 5
        // x2 = 7
        // x3 = x1 + x2 = 12
        // store x3 to DMEM[0]
        write_instr(entry_pc + 0x00, 0x00500093); // addi x1,x0,5
        write_instr(entry_pc + 0x04, 0x00700113); // addi x2,x0,7
        write_instr(entry_pc + 0x08, 0x002081B3); // add  x3,x1,x2

        write_instr(entry_pc + 0x0C, 0x00000013); // nop
        write_instr(entry_pc + 0x10, 0x00302023); // sw   x3,0(x0)

        write_instr(entry_pc + 0x14, 0x00000013); // nop
        write_instr(entry_pc + 0x18, 0x00000013); // nop
        write_instr(entry_pc + 0x1C, 0x00000013); // nop

        expected = 12;
    }
    else if (TEST_CASE == 2) {
        std::cout << "[TB] Test Case 2: ALU dependency chain\n";

        // x1 = 5
        // x2 = x1 + 1 = 6
        // x3 = x2 + 1 = 7
        // x4 = x3 + 1 = 8
        // store x4 to DMEM[0]
        write_instr(entry_pc + 0x00, 0x00500093); // addi x1,x0,5
        write_instr(entry_pc + 0x04, 0x00108113); // addi x2,x1,1
        write_instr(entry_pc + 0x08, 0x00110193); // addi x3,x2,1
        write_instr(entry_pc + 0x0C, 0x00118213); // addi x4,x3,1

        write_instr(entry_pc + 0x10, 0x00000013); // nop
        write_instr(entry_pc + 0x14, 0x00402023); // sw   x4,0(x0)

        write_instr(entry_pc + 0x18, 0x00000013); // nop
        write_instr(entry_pc + 0x1C, 0x00000013); // nop
        write_instr(entry_pc + 0x20, 0x00000013); // nop

        expected = 8;
    }
    else if (TEST_CASE == 3) {
        std::cout << "[TB] Test Case 3: Load-use stall remains protected\n";

        // Preload data memory:
        // address 16 maps to DMEM[4]
        dmem[4] = 21;

        // x5 = 16
        // x6 = load DMEM[4] = 21
        // x7 = x6 + 1 = 22
        // store x7 to DMEM[0]
        write_instr(entry_pc + 0x00, 0x01000293); // addi x5,x0,16
        write_instr(entry_pc + 0x04, 0x0002A303); // lw   x6,0(x5)
        write_instr(entry_pc + 0x08, 0x00130393); // addi x7,x6,1

        write_instr(entry_pc + 0x0C, 0x00000013); // nop
        write_instr(entry_pc + 0x10, 0x00702023); // sw   x7,0(x0)

        write_instr(entry_pc + 0x14, 0x00000013); // nop
        write_instr(entry_pc + 0x18, 0x00000013); // nop
        write_instr(entry_pc + 0x1C, 0x00000013); // nop

        expected = 22;
    }
    else {
        std::cerr << "[TB] ERROR: Invalid TEST_CASE = " << TEST_CASE << "\n";
        std::cerr << "[TB] Valid options: 1, 2, or 3\n";
        expected = 0xFFFFFFFF;
    }

    return expected;
}

// ------------------------------------------------------------
// Main
// ------------------------------------------------------------
int main() {
    std::memset(imem, 0, sizeof(imem));
    std::memset(dmem, 0, sizeof(dmem));

    CORE_DEBUG = true;

    const uint32_t entry_pc = 0;
    uint32_t expected = load_test_program(entry_pc);

    if (expected == 0xFFFFFFFF) {
        return 1;
    }

    ENTRY_PC = entry_pc;
    DTB_ADDR = 0;

    std::cout << "[TB] Initializing core\n";
    riscv_init();

    int final_cycle_count = 0;

    std::cout << "[TB] Running forwarding directed test\n";
    riscv_step((volatile uint32_t*)imem,
               (volatile uint32_t*)dmem,
               50,
               &final_cycle_count,
               entry_pc,
               0,
               1);

    std::cout << "\n[TB] Done\n";
    std::cout << "[TB] Cycles = " << final_cycle_count << "\n";

    uint32_t result0 = (uint32_t)dmem[0];

    std::cout << "[TB] DMEM[0] / [0x0] = 0x" << std::hex << result0
              << std::dec << " (" << result0 << ")\n";
    std::cout << "[TB] Expected = " << expected << "\n";

    if (result0 == expected) {
        std::cout << "[TB] PASS\n";
        return 0;
    } else {
        std::cout << "[TB] FAIL\n";
        return 1;
    }
}
