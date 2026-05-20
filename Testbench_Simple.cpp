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

    write_instr(entry_pc + 0x00, 0x00000093); // addi x1,x0,0      // x1 = sum = 0
    write_instr(entry_pc + 0x04, 0x00100113); // addi x2,x0,1      // x2 = i = 1
    write_instr(entry_pc + 0x08, 0x00600193); // addi x3,x0,6      // x3 = limit = 6

    // loop:
    // sum += i
    // i++
    // if i != 6, loop again
    write_instr(entry_pc + 0x0C, 0x002080B3); // add  x1,x1,x2     // sum += i
    write_instr(entry_pc + 0x10, 0x00110113); // addi x2,x2,1      // i++
    write_instr(entry_pc + 0x14, 0xFE311CE3); // bne  x2,x3,-8     // branch to 0x0C if i != 6

    // Check sum == 15
    write_instr(entry_pc + 0x18, 0x00F00213); // addi x4,x0,15     // expected sum
    write_instr(entry_pc + 0x1C, 0x02409263); // bne  x1,x4,+36    // if sum wrong, branch to bad

    // Check final i - 1 == 5
    write_instr(entry_pc + 0x20, 0x00500293); // addi x5,x0,5      // expected loop count
    write_instr(entry_pc + 0x24, 0xFFF10313); // addi x6,x2,-1     // x6 = i - 1
    write_instr(entry_pc + 0x28, 0x00531C63); // bne  x6,x5,+24    // if count wrong, branch to bad

    // Success path
    write_instr(entry_pc + 0x2C, 0x07B00393); // addi x7,x0,123    // success marker
    write_instr(entry_pc + 0x30, 0x00102023); // sw   x1,0(x0)     // DMEM[0] = sum = 15
    write_instr(entry_pc + 0x34, 0x00602223); // sw   x6,4(x0)     // DMEM[1] = count = 5
    write_instr(entry_pc + 0x38, 0x00702423); // sw   x7,8(x0)     // DMEM[2] = success marker = 123
    write_instr(entry_pc + 0x3C, 0x00C0006F); // jal  x0,+12       // jump over bad block

    // bad:
    write_instr(entry_pc + 0x40, 0x06300493); // addi x9,x0,99     // failure marker
    write_instr(entry_pc + 0x44, 0x00902623); // sw   x9,12(x0)    // DMEM[3] = 99 if failure path executes

    // done:
    write_instr(entry_pc + 0x48, 0x00000013); // nop
    write_instr(entry_pc + 0x4C, 0x00000013); // nop
    write_instr(entry_pc + 0x50, 0x00000013); // nop

    ENTRY_PC = entry_pc;
    DTB_ADDR = 0;

    std::cout << "[TB] Initializing core\n";
    riscv_init();

    int final_cycle_count = 0;

    std::cout << "[TB] Running simple directed test\n";
    riscv_step((volatile uint32_t*)imem,
               (volatile uint32_t*)dmem,
               120,
               &final_cycle_count,
               entry_pc,
               0,
               1);

    std::cout << "\n[TB] Done\n";
    std::cout << "[TB] Cycles = " << final_cycle_count << "\n";

    uint32_t result0 = (uint32_t)dmem[0];
    uint32_t result1 = (uint32_t)dmem[1];
    uint32_t result2 = (uint32_t)dmem[2];
    uint32_t result3 = (uint32_t)dmem[3];

    std::cout << "[TB] DMEM[0] / [0x0] = 0x" << std::hex << result0
              << std::dec << " (" << result0 << ")\n";
    std::cout << "[TB] DMEM[1] / [0x4] = 0x" << std::hex << result1
              << std::dec << " (" << result1 << ")\n";
    std::cout << "[TB] DMEM[2] / [0x8] = 0x" << std::hex << result2
              << std::dec << " (" << result2 << ")\n";
    std::cout << "[TB] DMEM[3] / [0xc] = 0x" << std::hex << result3
              << std::dec << " (" << result3 << ")\n";

    if (dmem[0] == 15 && dmem[1] == 5 && dmem[2] == 123 && dmem[3] == 0 ) {
        std::cout << "[TB] PASS\n";
        return 0;
    } else {
        std::cout << "[TB] FAIL\n";
        return 1;
    }
}