#include <iostream>
#include <iomanip>
#include <cstring>
#include <cstdint>
#include <ap_int.h>
#include "core.h"

// ------------------------------------------------------------
// Select which directed branch test to run
// ------------------------------------------------------------
// 1 = Branch not taken
// 2 = Branch taken, wrong-path store should be flushed
// 3 = JAL taken, wrong-path store should be flushed
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
// Directed Branch Test Programs
// ------------------------------------------------------------
static uint32_t load_test_program(uint32_t entry_pc) {
    uint32_t expected = 0;

    if (TEST_CASE == 1) {
        std::cout << "[TB] Test Case 1: Branch not taken\n";

        // Goal:
        // x1 = 1
        // x2 = 2
        // beq x1,x2,target is NOT taken
        // sequential path stores 11 to DMEM[0]
        //
        // DMEM[1] should remain 0 because no wrong-path sentinel store should occur.
        //
        // NOP spacing is intentional to isolate branch behavior from RAW hazards.
        write_instr(entry_pc + 0x00, 0x00100093); // addi x1,x0,1
        write_instr(entry_pc + 0x04, 0x00200113); // addi x2,x0,2
        write_instr(entry_pc + 0x08, 0x00000013); // nop
        write_instr(entry_pc + 0x0C, 0x00000013); // nop

        write_instr(entry_pc + 0x10, 0x00208663); // beq  x1,x2,+12 -> target at 0x1C, not taken

        write_instr(entry_pc + 0x14, 0x00B00193); // addi x3,x0,11
        write_instr(entry_pc + 0x18, 0x00302023); // sw   x3,0(x0)

        write_instr(entry_pc + 0x1C, 0x00000013); // target: nop
        write_instr(entry_pc + 0x20, 0x00000013); // nop
        write_instr(entry_pc + 0x24, 0x00000013); // nop

        expected = 11;
    }
    else if (TEST_CASE == 2) {
        std::cout << "[TB] Test Case 2: Branch taken, wrong-path store flushed\n";

        // Goal:
        // x1 = 5
        // x2 = 5
        // beq x1,x2,target is taken
        //
        // Wrong path:
        //   x3 = 99
        //   sw x3,4(x0)      -> sentinel write to DMEM[1]
        //
        // Target path:
        //   x4 = 11
        //   sw x4,0(x0)      -> expected write to DMEM[0]
        //
        // Expected final values:
        //   DMEM[0] = 11
        //   DMEM[1] = 0      -> proves wrong-path store was flushed
        //
        // NOP spacing before the branch is intentional to isolate branch flush behavior
        // from RAW hazards on x1/x2.
        write_instr(entry_pc + 0x00, 0x00500093); // addi x1,x0,5
        write_instr(entry_pc + 0x04, 0x00500113); // addi x2,x0,5
        write_instr(entry_pc + 0x08, 0x00000013); // nop
        write_instr(entry_pc + 0x0C, 0x00000013); // nop

        write_instr(entry_pc + 0x10, 0x00208863); // beq  x1,x2,+16 -> target at 0x20, taken

        write_instr(entry_pc + 0x14, 0x06300193); // addi x3,x0,99   // wrong path
        write_instr(entry_pc + 0x18, 0x00302223); // sw   x3,4(x0)   // wrong-path sentinel
        write_instr(entry_pc + 0x1C, 0x00000013); // nop             // wrong path / padding

        write_instr(entry_pc + 0x20, 0x00B00213); // target: addi x4,x0,11
        write_instr(entry_pc + 0x24, 0x00402023); // sw     x4,0(x0)

        write_instr(entry_pc + 0x28, 0x00000013); // nop
        write_instr(entry_pc + 0x2C, 0x00000013); // nop
        write_instr(entry_pc + 0x30, 0x00000013); // nop

        expected = 11;
    }
    else if (TEST_CASE == 3) {
        std::cout << "[TB] Test Case 3: JAL taken, wrong-path store flushed\n";

        // Goal:
        // jal x0,target is always taken
        //
        // Wrong path:
        //   x3 = 99
        //   sw x3,4(x0)      -> sentinel write to DMEM[1]
        //
        // Target path:
        //   x4 = 22
        //   sw x4,0(x0)      -> expected write to DMEM[0]
        //
        // Expected final values:
        //   DMEM[0] = 22
        //   DMEM[1] = 0      -> proves wrong-path store was flushed
        write_instr(entry_pc + 0x00, 0x0100006F); // jal  x0,+16 -> target at 0x10

        write_instr(entry_pc + 0x04, 0x06300193); // addi x3,x0,99   // wrong path
        write_instr(entry_pc + 0x08, 0x00302223); // sw   x3,4(x0)   // wrong-path sentinel
        write_instr(entry_pc + 0x0C, 0x00000013); // nop             // wrong path / padding

        write_instr(entry_pc + 0x10, 0x01600213); // target: addi x4,x0,22
        write_instr(entry_pc + 0x14, 0x00402023); // sw     x4,0(x0)

        write_instr(entry_pc + 0x18, 0x00000013); // nop
        write_instr(entry_pc + 0x1C, 0x00000013); // nop
        write_instr(entry_pc + 0x20, 0x00000013); // nop

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

    std::cout << "[TB] Running branch directed test\n";
    riscv_step((volatile uint32_t*)imem,
               (volatile uint32_t*)dmem,
               60,
               &final_cycle_count,
               entry_pc,
               0,
               1);

    std::cout << "\n[TB] Done\n";
    std::cout << "[TB] Cycles = " << final_cycle_count << "\n";

    uint32_t result0 = (uint32_t)dmem[0]; // target-path result
    uint32_t result1 = (uint32_t)dmem[1]; // wrong-path sentinel location

    std::cout << "[TB] DMEM[0] / [0x0] = 0x" << std::hex << result0
              << std::dec << " (" << result0 << ")\n";

    std::cout << "[TB] DMEM[1] / [0x4] = 0x" << std::hex << result1
              << std::dec << " (" << result1 << ")\n";

    std::cout << "[TB] Expected DMEM[0] = " << expected << "\n";
    std::cout << "[TB] Expected DMEM[1] = 0"
              << "  // wrong-path sentinel should remain unchanged\n";

    if (result0 == expected && result1 == 0) {
        std::cout << "[TB] PASS\n";
        return 0;
    } else {
        std::cout << "[TB] FAIL\n";

        if (result0 != expected) {
            std::cout << "[TB] ERROR: target-path result is incorrect\n";
        }

        if (result1 != 0) {
            std::cout << "[TB] ERROR: wrong-path store was not flushed\n";
        }

        return 1;
    }
}