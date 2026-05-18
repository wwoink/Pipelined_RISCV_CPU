#include <iostream>
#include <iomanip>
#include <cstring>
#include <cstdint>
#include <ap_int.h>
#include "core.h"

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
// Main
// ------------------------------------------------------------
int main() {
    std::memset(imem, 0, sizeof(imem));
    std::memset(dmem, 0, sizeof(dmem));

    CORE_DEBUG = true;

    const uint32_t entry_pc = 0;

    write_instr(entry_pc + 0x00, 0x00000513); // addi x10,x0,0
    write_instr(entry_pc + 0x04, 0x00B00213); // addi x4,x0,11

    write_instr(entry_pc + 0x08, 0x00000013); // nop
    write_instr(entry_pc + 0x0C, 0x00000013); // nop
    write_instr(entry_pc + 0x10, 0x00452023); // sw   x4,0(x10)
    write_instr(entry_pc + 0x14, 0x00052283); // lw   x5,0(x10)

    write_instr(entry_pc + 0x18, 0x00000013); // nop
    write_instr(entry_pc + 0x1C, 0x00552223); // sw   x5,4(x10)

    write_instr(entry_pc + 0x20, 0x00000013); // nop
    write_instr(entry_pc + 0x24, 0x00000013); // nop

    ENTRY_PC = entry_pc;
    DTB_ADDR = 0;

    std::cout << "[TB] Initializing core\n";
    riscv_init();

    int final_cycle_count = 0;

    std::cout << "[TB] Running simple directed test\n";
    riscv_step((volatile uint32_t*)imem,
               (volatile uint32_t*)dmem,
               30,
               &final_cycle_count,
               entry_pc,
               0,
               1);

    std::cout << "\n[TB] Done\n";
    std::cout << "[TB] Cycles = " << final_cycle_count << "\n";

    uint32_t result0 = (uint32_t)dmem[0];
    uint32_t result1 = (uint32_t)dmem[1];

    std::cout << "[TB] DMEM[0] / [0x0] = 0x" << std::hex << result0
              << std::dec << " (" << result0 << ")\n";
    std::cout << "[TB] DMEM[1] / [0x4] = 0x" << std::hex << result1
              << std::dec << " (" << result1 << ")\n";

    if (result0 == 11 && result1 == 11) {
        std::cout << "[TB] PASS\n";
        return 0;
    } else {
        std::cout << "[TB] FAIL: expected 11 in both DMEM[0] and DMEM[1]\n";
        return 1;
    }
}