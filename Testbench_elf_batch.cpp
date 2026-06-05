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

// --- OS Specific Headers (Batch Runner Only) ---
#include <windows.h> // Required for directory scanning (FindFirstFile)

// ============================================================================
//  USER CONFIGURATION SWITCHES
// ============================================================================

// 1. Default folder if no command-line argument is provided
#define DEFAULT_TEST_FOLDER "I:/Vitis_Files/Pipeline_Tests/Pipelined_Core/Benchmarks/rv32ui-p"

// 2. Per-test cycle limit
//    Each ELF is loaded, the core is initialized, and riscv_step runs once
//    for this many cycles. (upped to 1500 for ld_st)
const int MAX_CYCLES = 1500;

// 3. Debug Switches
const bool ENABLE_CORE_DEBUG = false;
const bool PRINT_LOAD_DETAILS = false;

// ============================================================================

// Separate instruction and data memories for the pipelined core.
// The ELF is first loaded into load_mem, then copied into both imem and dmem.
// This matches the current working hardcoded ELF testbench.
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

// ============================================================================
// Directory Scanner (Windows)
// ============================================================================
static std::vector<std::string> get_test_files(const std::string& folder) {
    std::vector<std::string> files;

    std::string search_path = folder + "/*";
    WIN32_FIND_DATA fd;
    HANDLE hFind = ::FindFirstFile(search_path.c_str(), &fd);

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                std::string filename = fd.cFileName;

                // Filter:
                //   - skip .dump files
                //   - skip hidden/current/parent style names
                //   - skip common generated text/log files
                if (filename.find(".dump") == std::string::npos &&
                    filename.find(".txt")  == std::string::npos &&
                    filename.find(".log")  == std::string::npos &&
                    filename.find(".") != 0) {
                    files.push_back(filename);
                }
            }
        } while (::FindNextFile(hFind, &fd));

        ::FindClose(hFind);
    }

    std::sort(files.begin(), files.end());
    return files;
}

// ============================================================================
// Run One ELF Test
// ============================================================================
static bool run_one_test(const std::string& full_path,
                         const std::string& test_name,
                         int& cycles_out,
                         uint32_t& tohost_out,
                         int& exit_code_out) {
    cycles_out = 0;
    tohost_out = 0;
    exit_code_out = -1;

    // 1. Clear memories for this test.
    std::memset(load_mem, 0, sizeof(load_mem));
    std::memset(imem,     0, sizeof(imem));
    std::memset(dmem,     0, sizeof(dmem));

    // 2. Load ELF into temporary unified memory.
    ElfFile loader(full_path.c_str());
    ap_uint<32> elf_entry_pc = loader.load_to_mem(load_mem, RAM_SIZE);

    if (elf_entry_pc == 0) {
        std::cout << std::left << std::setw(35) << test_name
                  << " : SKIP/FAIL (invalid ELF or entry PC = 0)\n";
        return false;
    }

    // 3. Copy loaded memory image into both IMEM and DMEM.
    for (unsigned i = 0; i < RAM_SIZE; i++) {
        imem[i] = load_mem[i];
        dmem[i] = load_mem[i];
    }

    // 4. Convert entry PC to current zero-base execution model.
    ap_uint<32> zero_entry_pc = to_zero_base_pc(elf_entry_pc);

    // 5. Dynamic .tohost calculation.
    unsigned tohost_idx = 0;
    if (loader.tohost_addr_found != 0) {
        tohost_idx = (loader.tohost_addr_found - DRAM_BASE) >> 2;
    } else {
        tohost_idx = (0x80001000 - DRAM_BASE) >> 2;
    }

    if (tohost_idx >= RAM_SIZE) {
        std::cout << std::left << std::setw(35) << test_name
                  << " : FAIL (.tohost index outside RAM_SIZE)\n";
        return false;
    }

    if (PRINT_LOAD_DETAILS) {
        std::cout << "\n[LOAD] " << test_name << "\n";
        std::cout << "       ELF entry PC       = 0x" << std::hex << (uint32_t)elf_entry_pc << "\n";
        std::cout << "       Zero-base entry PC = 0x" << std::hex << (uint32_t)zero_entry_pc << "\n";
        std::cout << "       .tohost index      = " << std::dec << tohost_idx << "\n";
    }

    // 6. Initialize core for this individual test.
    CORE_DEBUG = ENABLE_CORE_DEBUG;

    ENTRY_PC = zero_entry_pc;
    DTB_ADDR = 0;

    riscv_init();

    // 7. Run one hardware transaction for this test.
    int final_cycle_count = 0;

    riscv_step((volatile uint32_t*)imem,
               (volatile uint32_t*)dmem,
               MAX_CYCLES,
               &final_cycle_count,
               zero_entry_pc,
               0x0,
               1);

    cycles_out = final_cycle_count;

    // 8. Check .tohost from DMEM because stores go through the data-memory port.
    tohost_out = (uint32_t)dmem[tohost_idx];

    if (tohost_out & 1) {
        exit_code_out = (int)(tohost_out >> 1);
        return (exit_code_out == 0);
    }

    return false;
}

// ============================================================================
// Main Batch Loop
// ============================================================================
int main(int argc, char* argv[])
{
    std::string folder_path = DEFAULT_TEST_FOLDER;

    if (argc > 1) {
        folder_path = argv[1];
    }

    std::vector<std::string> tests = get_test_files(folder_path);

    std::cout << "\n==================================================================\n";
    std::cout << "  RISC-V ELF BATCH RUNNER - SPLIT IMEM/DMEM PIPELINED CORE\n";
    std::cout << "==================================================================\n";
    std::cout << "[BATCH] Folder: " << folder_path << "\n";
    std::cout << "[BATCH] Found " << tests.size() << " potential tests\n";
    std::cout << "[BATCH] Per-test cycle limit: " << MAX_CYCLES << "\n";
    std::cout << "==================================================================\n\n";

    if (tests.empty()) {
        std::cout << "[BATCH] ERROR: No tests found.\n";
        return 1;
    }

    int total_pass = 0;
    int total_fail = 0;

    for (const auto& test_name : tests) {
        std::string full_path = folder_path + "/" + test_name;

        int cycles = 0;
        uint32_t tohost = 0;
        int exit_code = -1;

        bool passed = run_one_test(full_path, test_name, cycles, tohost, exit_code);

        if (passed) {
            std::cout << std::left << std::setw(35) << test_name
                      << " : PASS"
                      << "  cycles=" << std::setw(6) << cycles
                      << "  tohost=0x" << std::hex << tohost << std::dec
                      << "\n";
            total_pass++;
        } else {
            std::cout << std::left << std::setw(35) << test_name
                      << " : FAIL";

            if (tohost & 1) {
                std::cout << "  code=" << exit_code;
            } else {
                std::cout << "  timeout/no-tohost";
            }

            std::cout << "  cycles=" << std::setw(6) << cycles
                      << "  tohost=0x" << std::hex << tohost << std::dec
                      << "\n";
            total_fail++;
        }
    }

    std::cout << "\n==================================================================\n";
    std::cout << "SUMMARY: " << total_pass << " PASSED, " << total_fail << " FAILED\n";
    std::cout << "==================================================================\n";

    return (total_fail == 0) ? 0 : 1;
}