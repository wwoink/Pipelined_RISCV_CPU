#include <ap_int.h>
#include <iostream>
#include <cstring>
#include "core.h" 
#include <cstdio>

// ------------------------------------------------------------
// Global Debug Switch
// ------------------------------------------------------------
#ifdef __SYNTHESIS__
#define CORE_DEBUG false
#else
bool CORE_DEBUG = true;
#endif // This must be toggled in the testbench

#define ZERO_BASE_TEST 1

// ------------------------------------------------------------  
// Global Configuration Switch
// ------------------------------------------------------------
// If false, the M-extension logic is completely pruned during synthesis.
const bool ENABLE_M_EXTENSION = false;
const bool ENABLE_A_EXTENSION = true; // Toggle for Atomics

#define ENABLE_FORWARDING 1
#define EXIT_ON_TRAP 0

// Enable for RISC-V ISA tests and ELF benchmarks that report completion
// through an odd value written to 0x80001000 <tohost>.
// Disable for Linux or other programs that may use 0x80001000 normally.
#define ENABLE_TOHOST_EXIT 1

// Enable for ELF benchmarks that use HTIF requests during C-simulation or
// RTL co-simulation. An even write to <tohost> is acknowledged by writing 1
// to <fromhost> at 0x80001040. Disable for Linux and normal FPGA deployment.
#define ENABLE_HTIF_EMULATION 1

// ------------------------------------------------------------
// Inter-Stage Data Structs
// ------------------------------------------------------------

struct FetchOut {
    ap_uint<32> instr;
    ap_uint<32> pc;
};

struct DecodeOut {
    ap_uint<7>  opcode;
    ap_uint<5>  rd;
    ap_uint<3>  funct3;
    ap_uint<5>  rs1;
    ap_uint<5>  rs2;
    ap_uint<7>  funct7;
    ap_int<32>  imm;
    ap_int<32>  pc;
    ap_int<32>  rs1_val;
    ap_int<32>  rs2_val;
    ap_uint<32> instr;
};

struct ExecOut {
    ap_int<32>  alu_result;
    ap_uint<5>  rd;
    bool        mem_read;
    bool        mem_write;
    bool        reg_write;
    ap_uint<3>  funct3;
    ap_int<32>  store_val;
    bool        is_trap;
    bool        is_atomic;
    ap_uint<5>  atomic_op;
    // Control signals
    bool        branch_taken;
    ap_uint<32> next_pc;
    bool        finished;
};

struct MemOut {
    ap_int<32>  value;
    ap_uint<5>  rd;
    bool        reg_write;
    bool        is_trap;
};

// ------------------------------------------------------------
// Address Translation Helper
// ------------------------------------------------------------
// Synthesis: m_axi base=0, index = full_byte_addr / 4
//   Allows core to reach DDR (0x80000000) AND UART (0x10000000)
// Simulation: ram[] is flat 128MB array, index = offset within DDR
static unsigned addr_to_idx(unsigned byte_addr) {
    #pragma HLS INLINE
    #if ZERO_BASE_TEST
        // Zero-base bring-up mode:
        //   0x00000000 -> index 0
        //   0x80000000 -> index 0
        //
        // This lets directed zero-base tests and ELF-generated
        // absolute DRAM addresses both work in the same test mode.
        if (byte_addr >= DRAM_BASE) {
            return (byte_addr - DRAM_BASE) >> 2;
        } else {
            return byte_addr >> 2;
        }
    #else
        #ifdef __SYNTHESIS__
            return byte_addr >> 2;
        #else
            return ((byte_addr & 0x07FFFFFF) - (DRAM_BASE & 0x07FFFFFF)) >> 2;
        #endif
    #endif
}

// ------------------------------------------------------------
// Global Architectural State
// ------------------------------------------------------------
#ifdef __SYNTHESIS__
    ap_uint<32> pc = 0x80000000; 
#else
    ap_uint<32> pc = 0;          
#endif
ap_int<32>  regfile[32];
bool is_finished = false;

// --- Load Reserved / Store Conditional State ---
ap_uint<32> lr_addr = 0;
bool lr_valid = false;

// --- CSRs ---
ap_uint<32> csr_mtvec = 0;
ap_uint<32> csr_mepc = 0;
ap_uint<32> csr_mcause = 0;
ap_uint<32> csr_mscratch = 0;

ap_uint<64> csr_mcycle = 0;   // Cycle Counter (0xB00/0xB80)
ap_uint<64> csr_minstret = 0; // Instructions Retired (0xB02/0xB82)
ap_uint<32> csr_mstatus = 0;  // Status Register (0x300)

// --- Interrupts & Timer ---
ap_uint<32> csr_mie = 0; // Interrupt Enable Register (0x304)
ap_uint<32> csr_mip = 0; // Interrupt Pending Register (0x344)
ap_uint<64> mtimecmp = 0xFFFFFFFFFFFFFFFF;

// --- Sink CSRs (accept writes, minimal/no effect in M-mode-only core) ---
ap_uint<32> csr_mtval = 0;         // Trap Value (0x343)
ap_uint<32> csr_medeleg = 0;       // Exception Delegation (0x302) - no S-mode, sink
ap_uint<32> csr_mideleg = 0;       // Interrupt Delegation (0x303) - no S-mode, sink
ap_uint<32> csr_mcountinhibit = 0; // Counter Inhibit (0x320)
ap_uint<32> csr_satp = 0;          // S-mode Address Translation (0x180) - sink

// --- Global Variable Master Definitions ---
ap_uint<32> ENTRY_PC;
ap_uint<32> DTB_ADDR;

// ------------------------------------------------------------
// Initialization Function
// ------------------------------------------------------------
void riscv_init() {
    pc = ENTRY_PC;
    for(int i=0; i<32; i++) regfile[i] = 0;

    regfile[1] = (ap_int<32>)0xDEADBEEF;       
    regfile[0] = 0;                           
    regfile[2] = (ap_int<32>) (unsigned)DMEM_STACK_TOP;

    //Linux setup
    regfile[10] = 0;         // a0 = Hart ID (0)
    regfile[11] = 0x80800000; // a1 = Device Tree Address
    
    if(CORE_DEBUG) {
        std::cout << "[INIT] Core Reset. PC=0x" << std::hex << (unsigned)pc 
                  << ", SP=0x" << (unsigned)regfile[2] << std::dec << std::endl;
    }

    is_finished = false;

    csr_mcycle = 0;
    csr_minstret = 0;
    csr_mstatus = 0;
    
    // Reset Timer state
    csr_mie = 0;
    csr_mip = 0;
    mtimecmp = 0xFFFFFFFFFFFFFFFF;

    // Reset Sink CSRs
    csr_mtval = 0;
    csr_medeleg = 0;
    csr_mideleg = 0;
    csr_mcountinhibit = 0;
    csr_satp = 0;

    // Reset Atomic State
    lr_valid = false;
    lr_addr = 0;
}

// ------------------------------------------------------------
// Helper Functions: Immediate Extractors
// ------------------------------------------------------------
ap_int<32> sextI(ap_uint<32> insn) {
    #pragma HLS INLINE
    ap_int<12> imm12 = (ap_int<12>)(insn >> 20);
    return (ap_int<32>)imm12;
}

ap_int<32> sextS(ap_uint<32> insn) {
    #pragma HLS INLINE
    ap_uint<7> hi = insn.range(31,25);
    ap_uint<5> lo = insn.range(11,7);
    ap_uint<12> u = ((ap_uint<12>)hi << 5) | (ap_uint<12>)lo;
    ap_int<12> s = (ap_int<12>)u;
    return (ap_int<32>)s;
}

ap_int<32> sextB(ap_uint<32> insn) {
    #pragma HLS INLINE
    ap_uint<13> imm;
    imm[12]       = insn[31];       
    imm[11]       = insn[7];        
    imm.range(10,5) = insn.range(30,25); 
    imm.range(4,1)  = insn.range(11,8);  
    imm[0]          = 0;            
    return (ap_int<32>)((ap_int<13>)imm);
}

ap_int<32> sextJ(ap_uint<32> insn) {
    #pragma HLS INLINE
    ap_uint<21> u = ((ap_uint<21>)insn[31]        << 20) |
                    ((ap_uint<21>)insn.range(30,21) << 1 ) |
                    ((ap_uint<21>)insn[20]        << 11) |
                    ((ap_uint<21>)insn.range(19,12) << 12);
    ap_int<21> s = (ap_int<21>)u;
    return (ap_int<32>)s;
}

// ------------------------------------------------------------
// CSR Read/Write Helpers
// ------------------------------------------------------------
ap_uint<32> csr_read(unsigned addr) {
    #pragma HLS INLINE
    switch (addr) {
        // Machine Information
        case 0xF11: return 0;                                          // mvendorid
        case 0xF12: return 0;                                          // marchid
        case 0xF13: return 0;                                          // mimpid
        case 0xF14: return 0;                                          // mhartid
        // Machine ISA
        case 0x301: return 0x40001101;                                 // misa: RV32IMA
        // Machine Trap Setup
        case 0x300: return csr_mstatus;                                // mstatus
        case 0x302: return csr_medeleg;                                // medeleg
        case 0x303: return csr_mideleg;                                // mideleg
        case 0x304: return csr_mie;                                    // mie
        case 0x305: return csr_mtvec;                                  // mtvec
        case 0x320: return csr_mcountinhibit;                          // mcountinhibit
        // Machine Trap Handling
        case 0x340: return csr_mscratch;                               // mscratch
        case 0x341: return csr_mepc;                                   // mepc
        case 0x342: return csr_mcause;                                 // mcause
        case 0x343: return csr_mtval;                                  // mtval
        case 0x344: return csr_mip;                                    // mip
        // Machine Counters
        case 0xB00: return (ap_uint<32>)csr_mcycle;                    // mcycle (low)
        case 0xB80: return (ap_uint<32>)(csr_mcycle >> 32);            // mcycleh (high)
        case 0xB02: return (ap_uint<32>)csr_minstret;                  // minstret (low)
        case 0xB82: return (ap_uint<32>)(csr_minstret >> 32);          // minstreth (high)
        // User Counter Aliases (read-only mirrors)
        case 0xC00: return (ap_uint<32>)csr_mcycle;                    // cycle
        case 0xC80: return (ap_uint<32>)(csr_mcycle >> 32);            // cycleh
        case 0xC02: return (ap_uint<32>)csr_minstret;                  // instret
        case 0xC82: return (ap_uint<32>)(csr_minstret >> 32);          // instreth
        // S-mode (sinks)
        case 0x180: return csr_satp;                                   // satp
        default:    return 0;
    }
}

void csr_write(unsigned addr, ap_uint<32> val) {
    #pragma HLS INLINE
    switch (addr) {
        // Machine Trap Setup
        case 0x300: csr_mstatus = val; break;        // mstatus
        case 0x302: csr_medeleg = val; break;         // medeleg (sink)
        case 0x303: csr_mideleg = val; break;         // mideleg (sink)
        case 0x304: csr_mie = val; break;             // mie
        case 0x305: csr_mtvec = val; break;           // mtvec
        case 0x320: csr_mcountinhibit = val; break;   // mcountinhibit (sink)
        // Machine Trap Handling
        case 0x340: csr_mscratch = val; break;        // mscratch
        case 0x341: csr_mepc = val; break;            // mepc
        case 0x342: csr_mcause = val; break;          // mcause
        case 0x343: csr_mtval = val; break;           // mtval
        case 0x344: csr_mip = val; break;             // mip
        // S-mode (sinks)
        case 0x180: csr_satp = val; break;            // satp (sink)
        // Read-only CSRs (misa, mhartid, counters) — silently ignore writes
        default: break;
    }
}

// ------------------------------------------------------------
// Instruction Line Buffer
// ------------------------------------------------------------
// Each fetch would otherwise be a single-beat m_axi read of one word, and
// because the instruction loop is not pipelined those round-trips are fully
// serialized -- one AXI latency per instruction. This buffer fills a whole
// line (FETCH_LINE_WORDS words) in one burst on a miss, then serves sequential
// PCs from on-chip storage with no bus traffic until the line is exhausted or
// a redirect jumps outside it. Straight-line code and tight loops that fit in
// a line drop from one AXI read per instruction to one burst per line.
//
// Assumes imem is read-only during execution (stores target the separate dmem
// bundle, so there is no self-modifying code through this port). The line is
// reset at the start of riscv_step; FENCE.I is a no-op here for that reason.
#define FETCH_LINE_WORDS 16                 // 64-byte line, must be power of 2
#define FETCH_LINE_MASK  (FETCH_LINE_WORDS - 1)

static ap_uint<32> ic_line[FETCH_LINE_WORDS]; // cached line contents
static unsigned     ic_base_idx = 0;          // word index of ic_line[0]
static bool         ic_valid    = false;      // is ic_line populated?

// ------------------------------------------------------------
// Stage: Fetch
// ------------------------------------------------------------
FetchOut fetch(volatile uint32_t* imem, ap_uint<32> fetch_pc) {
    #pragma HLS INLINE off
    FetchOut f;
    f.pc = fetch_pc;

    unsigned im_idx    = addr_to_idx((unsigned)fetch_pc);
    unsigned line_base = im_idx & ~((unsigned)FETCH_LINE_MASK); // align down
    unsigned line_off  = im_idx &  ((unsigned)FETCH_LINE_MASK);

    // Miss: the requested word is not in the current line. Refill in one
    // contiguous pass so HLS can infer an AXI burst read.
    if (!ic_valid || line_base != ic_base_idx) {
        // Read the line through a NON-volatile view of imem. The interface is
        // declared volatile (so MMIO-style accesses stay un-coalesced), but a
        // volatile access cannot be merged into a burst -- it forces one
        // serialized single-beat read per word. Casting volatile away here lets
        // HLS coalesce the contiguous refill into a single AXI burst. Safe
        // because imem is read-only during execution.
        uint32_t* isrc = const_cast<uint32_t*>(imem);

        FILL_LINE: for (int i = 0; i < FETCH_LINE_WORDS; i++) {
            #pragma HLS PIPELINE II=1
            unsigned idx = line_base + i;
            #ifdef __SYNTHESIS__
            ic_line[i] = (ap_uint<32>)isrc[idx];
            #else
            ic_line[i] = (idx < RAM_SIZE) ? (ap_uint<32>)isrc[idx] : (ap_uint<32>)0;
            #endif
        }
        ic_base_idx = line_base;
        ic_valid    = true;
    }

    f.instr = ic_line[line_off];

    if(CORE_DEBUG) {
        std::cout << "\n------------------------------------------------------------\n";
        std::cout << "[FETCH] PC=0x" << std::hex << (unsigned)fetch_pc
                  << " Instr=0x" << (unsigned)f.instr << std::dec << "\n";
    }
    return f;
}

// ------------------------------------------------------------
// Stage: Decode
// ------------------------------------------------------------
DecodeOut decode(const FetchOut& f) {
    #pragma HLS INLINE off
    DecodeOut d;
    ap_uint<32> instr = f.instr;

    d.opcode = instr.range(6, 0);
    d.rd     = instr.range(11, 7);
    d.funct3 = instr.range(14, 12);
    d.rs1    = instr.range(19, 15);
    d.rs2    = instr.range(24, 20);
    d.funct7 = instr.range(31, 25);
    d.instr  = instr;

    // Uniform Switch for Immediate Extraction
    switch (d.opcode) {
        case 0x23: d.imm = sextS(instr); break; // Store
        case 0x63: d.imm = sextB(instr); break; // Branch
        case 0x6F: d.imm = sextJ(instr); break; // JAL
        case 0x2F: d.imm = 0; break;            // Atomics (No immediate)
        default:   d.imm = sextI(instr); break; // All others (I-type, JALR, Load, ALU-I)
    }
    
    d.pc = f.pc;
    d.rs1_val = (d.rs1 == 0) ? (ap_int<32>)0 : regfile[d.rs1];
    d.rs2_val = (d.rs2 == 0) ? (ap_int<32>)0 : regfile[d.rs2];

    if (CORE_DEBUG) {
        std::cout << "[DECODE] Opcode=0x" << std::hex << (int)d.opcode 
                  << " Rd=" << (int)d.rd << std::dec << "\n";
    }
    return d;
}

// ------------------------------------------------------------
// Stage: Execute
// ------------------------------------------------------------
ExecOut execute(const DecodeOut& d) {
    #pragma HLS INLINE
    ap_int<32> rs1_val = d.rs1_val;
    ap_int<32> rs2_val = d.rs2_val;

    ExecOut e;
    e.alu_result   = 0;
    e.rd           = d.rd;
    e.mem_read     = false;
    e.mem_write    = false;
    e.reg_write    = false;
    e.store_val    = rs2_val;
    e.funct3       = d.funct3;
    e.is_trap      = false;
    e.is_atomic    = false;
    e.atomic_op    = 0;
    e.branch_taken = false;
    e.next_pc      = 0;
    e.finished     = false;

    switch ((unsigned)d.opcode) {
    case 0x2F: { // A-Extension (Atomics)
        if (ENABLE_A_EXTENSION) {
            if (d.funct3 == 0x2) {
                e.is_atomic  = true;
                e.atomic_op  = d.funct7.range(6, 2);
                e.alu_result = rs1_val; // Memory Address
                e.store_val  = rs2_val;
                e.reg_write  = true;
            } else {
                e.is_trap = true;
            }
        } else {
            e.is_trap = true;
        }
        break;
    }
    case 0x33: { // R-type
            ap_uint<5> shamt = rs2_val.range(4,0);
            e.reg_write = true;
            
            switch ((unsigned)d.funct7) {
                case 0x00:
                    switch ((unsigned)d.funct3) {
                        case 0x0: e.alu_result = rs1_val + rs2_val; break; // ADD
                        case 0x1: e.alu_result = rs1_val << shamt;  break; // SLL
                        case 0x2: e.alu_result = (rs1_val < rs2_val) ? 1 : 0; break; // SLT
                        case 0x3: e.alu_result = ((ap_uint<32>)rs1_val < (ap_uint<32>)rs2_val) ? 1 : 0; break; // SLTU
                        case 0x4: e.alu_result = rs1_val ^ rs2_val; break; // XOR
                        case 0x5: e.alu_result = (ap_uint<32>)rs1_val >> shamt; break; // SRL
                        case 0x6: e.alu_result = rs1_val | rs2_val; break; // OR
                        case 0x7: e.alu_result = rs1_val & rs2_val; break; // AND
                        default:  e.reg_write = false; e.is_trap = true; break; 
                    }
                    break;
                case 0x20:
                    switch ((unsigned)d.funct3) {
                        case 0x0: e.alu_result = rs1_val - rs2_val; break; // SUB
                        case 0x5: e.alu_result = rs1_val >> shamt; break; // SRA
                        default:  e.reg_write = false; e.is_trap = true; break;
                    }
                    break;
                case 0x01: // M-Extension (MUL, DIV, REM)
                    if (ENABLE_M_EXTENSION) {
                        bool is_div  = (d.funct3 == 0x4);
                        bool is_divu = (d.funct3 == 0x5);
                        bool is_rem  = (d.funct3 == 0x6);
                        bool is_remu = (d.funct3 == 0x7);
                        
                        bool is_div_op = is_div || is_divu || is_rem || is_remu;
                        bool is_signed = is_div || is_rem;

                        bool sign_a = rs1_val[31];
                        bool sign_b = rs2_val[31];
                        
                        ap_uint<32> u_a = (is_signed && sign_a) ? (ap_uint<32>)(-rs1_val) : (ap_uint<32>)rs1_val;
                        ap_uint<32> u_b = (is_signed && sign_b) ? (ap_uint<32>)(-rs2_val) : (ap_uint<32>)rs2_val;

                        ap_uint<32> u_quot = 0;
                        ap_uint<32> u_rem  = 0;

                        if (is_div_op) {
                            if (u_b != 0) {
                                u_quot = u_a / u_b;
                                u_rem = u_a - (u_quot * u_b);
                            } else {
                                u_quot = -1;
                                u_rem  = u_a;
                            }
                        }

                        ap_int<32> res_div = 0;
                        ap_int<32> res_rem = 0;

                        if (is_signed) {
                            res_div = ((sign_a ^ sign_b) && u_b != 0) ? (ap_int<32>)-u_quot : (ap_int<32>)u_quot;
                            res_rem = (sign_a && u_b != 0) ? (ap_int<32>)-u_rem : (ap_int<32>)u_rem;
                            
                            if (u_b == 1 && sign_b && !sign_a && rs1_val == (ap_int<32>)0x80000000) {
                                res_div = rs1_val;
                                res_rem = 0;
                            }
                        } else {
                            res_div = (ap_int<32>)u_quot;
                            res_rem = (ap_int<32>)u_rem;
                        }

                        switch ((unsigned)d.funct3) {
                            case 0x0: e.alu_result = rs1_val * rs2_val; break; // MUL
                            case 0x1: e.alu_result = (ap_int<32>)(((ap_int<64>)rs1_val * (ap_int<64>)rs2_val) >> 32); break; // MULH
                            case 0x2: e.alu_result = (ap_int<32>)(((ap_int<64>)rs1_val * (ap_uint<64>)((ap_uint<32>)rs2_val)) >> 32); break; // MULHSU
                            case 0x3: e.alu_result = (ap_int<32>)(((ap_uint<64>)((ap_uint<32>)rs1_val) * (ap_uint<64>)((ap_uint<32>)rs2_val)) >> 32); break; // MULHU
                            case 0x4: e.alu_result = res_div; break; // DIV
                            case 0x5: e.alu_result = res_div; break; // DIVU
                            case 0x6: e.alu_result = res_rem; break; // REM
                            case 0x7: e.alu_result = res_rem; break; // REMU
                        }
                    } else {
                        e.reg_write = false; 
                        e.is_trap = true; 
                    }
                    break;
                default:
                    e.reg_write = false; 
                    e.is_trap = true; 
                    break;
            }
            break;
    }
    case 0x17: { // AUIPC
            ap_int<32> imm20 = (ap_int<32>)(d.instr & 0xFFFFF000);
            e.alu_result = (ap_int<32>)d.pc + imm20;
            e.reg_write  = (d.rd != 0);
            break;
    }
    case 0x13: { // I-type
            if (d.rd == 0 && d.rs1 == 0 && d.imm == 0) { // NOP
                e.reg_write = false;
            } else {
                e.reg_write = true; 
            }
            ap_uint<5> shamt = d.imm.range(4,0); 
            
            switch ((unsigned)d.funct3) {
                case 0x0: e.alu_result = rs1_val + d.imm; break; // ADDI
                case 0x1: e.alu_result = rs1_val << shamt;  break; // SLLI
                case 0x2: e.alu_result = (rs1_val < d.imm) ? 1 : 0; break; // SLTI
                case 0x3: e.alu_result = ((ap_uint<32>)rs1_val < (ap_uint<32>)d.imm) ? 1 : 0; break; // SLTIU
                case 0x4: e.alu_result = rs1_val ^ d.imm; break; // XORI
                case 0x5: 
                    if (d.instr[30] == 0) e.alu_result = (ap_uint<32>)rs1_val >> shamt; // SRLI
                    else e.alu_result = rs1_val >> shamt; // SRAI
                    break;
                case 0x6: e.alu_result = rs1_val | d.imm; break; // ORI
                case 0x7: e.alu_result = rs1_val & d.imm; break; // ANDI
                default: e.reg_write = false; e.is_trap = true; break;
            }
            break;
    }
    case 0x03: { // Load
            ap_int<32> addr = rs1_val + d.imm;

            // Alignment check: LW needs 4-byte, LH/LHU need 2-byte alignment.
            // LB/LBU are always aligned. Misaligned loads trap (mcause 4).
            unsigned align_mask = 0;
            if (d.funct3 == 0x2)                          align_mask = 0x3; // LW
            else if (d.funct3 == 0x1 || d.funct3 == 0x5)  align_mask = 0x1; // LH/LHU

            if (((unsigned)addr & align_mask) != 0) {
                e.is_trap      = true;
                e.reg_write    = false;
                csr_mepc       = d.pc;
                csr_mcause     = 4;               // Load address misaligned
                csr_mtval      = (ap_uint<32>)addr;
                e.next_pc      = csr_mtvec;
                e.branch_taken = true;
            } else {
                e.alu_result = addr;
                e.mem_read   = true;
                e.reg_write  = true;
            }
            break;
    }
    case 0x23: { // Store
            ap_int<32> addr = rs1_val + d.imm;

            // Alignment check: SW needs 4-byte, SH needs 2-byte alignment.
            // SB is always aligned. Misaligned stores trap (mcause 6).
            unsigned align_mask = 0;
            if (d.funct3 == 0x2)       align_mask = 0x3; // SW
            else if (d.funct3 == 0x1)  align_mask = 0x1; // SH

            if (((unsigned)addr & align_mask) != 0) {
                e.is_trap      = true;
                e.reg_write    = false;
                csr_mepc       = d.pc;
                csr_mcause     = 6;               // Store/AMO address misaligned
                csr_mtval      = (ap_uint<32>)addr;
                e.next_pc      = csr_mtvec;
                e.branch_taken = true;
            } else {
                e.alu_result = addr;
                e.mem_write  = true;
            }
            break;
    }
    case 0x63: { // Branch
            // Branch direction/target are resolved in ID now.
            // Let the branch flow through EX as a no-op so it can retire
            // without causing a second redirect.
            e.reg_write = false;
            break;
    }
    case 0x6F: { // JAL
            // The redirect is resolved in ID. EX still produces rd = PC + 4.
            e.alu_result    = (ap_int<32>)(d.pc + 4);
            e.reg_write     = (d.rd != 0);
            break;
    }
    case 0x67: { // JALR
            // The redirect is resolved in ID. EX still produces rd = PC + 4.
            e.alu_result    = (ap_int<32>)(d.pc + 4); 
            e.reg_write     = (d.rd != 0);
            break;
    }
    case 0x37:{ // LUI
            e.alu_result = (ap_int<32>)(d.instr & 0xFFFFF000);
            e.reg_write  = (d.rd != 0);
            break;
    }
    case 0x73: { // System
        unsigned csr_addr = d.imm.range(11,0); 
        ap_uint<5> rs1_imm = d.rs1; 
        ap_uint<32> trap_cause = 0;
        
        ap_uint<32> csr_read_val = csr_read(csr_addr);
        
        e.reg_write = false; 

        switch ((unsigned)d.funct3) {
            case 0x0: // ECALL / EBREAK / WFI / MRET
                if (d.imm == 0x000) { 
                    #ifndef __SYNTHESIS__
                    if (regfile[17] == 93) {
                        e.finished = true;
                        std::cout << "[CORE DEBUG] Exit Condition Met! Stopping Simulation." << std::endl;
                    }
                    #endif
                    e.is_trap = true; 
                    trap_cause = 11; 
                } // ECALL
                else if (d.imm == 0x001) { e.is_trap = true; trap_cause = 3; } // EBREAK
                else if (d.imm == 0x105) { // WFI (Wait For Interrupt)
                    #ifndef __SYNTHESIS__
                    bool global_enable = (csr_mstatus >> 3) & 1;
                    bool timer_enable  = (csr_mie >> 7) & 1;

                    if (csr_mcycle > 500000) { 
                        mtimecmp = csr_mcycle + 100;
                        
                        std::cout << "[SIM-HACK] WFI at Cycle " << std::hex << (uint64_t)csr_mcycle 
                                  << " | MIE (Global): " << (int)global_enable 
                                  << " | MTIE (Timer): " << (int)timer_enable 
                                  << std::dec << std::endl << std::flush;
                    }
                    #endif
                }
                else if (d.imm == 0x302) { // MRET
                    e.next_pc = (ap_uint<32>)csr_mepc;
                    e.branch_taken = true;

                    bool mpie = (csr_mstatus >> 7) & 1;
                    if(mpie) csr_mstatus |= (1 << 3);
                    else     csr_mstatus &= ~(1 << 3);
                    csr_mstatus |= (1 << 7);

                    e.reg_write = false; 
                    if(CORE_DEBUG) std::cout << "[MRET] Returning to 0x" << std::hex << (int)e.next_pc << std::dec << "\n";
                }
                break;
            case 0x1: // CSRRW
                e.alu_result = csr_read_val; 
                e.reg_write = (d.rd != 0);
                csr_write(csr_addr, (ap_uint<32>)rs1_val);
                break;
            case 0x2: // CSRRS
                e.alu_result = csr_read_val; 
                e.reg_write = (d.rd != 0);
                if (d.rs1 != 0)
                    csr_write(csr_addr, csr_read_val | (ap_uint<32>)rs1_val);
                break;
            case 0x3: // CSRRC
                e.alu_result = csr_read_val;
                e.reg_write = (d.rd != 0);
                if (d.rs1 != 0)
                    csr_write(csr_addr, csr_read_val & ~(ap_uint<32>)rs1_val);
                break;
            case 0x5: // CSRRWI
                e.alu_result = csr_read_val; 
                e.reg_write = (d.rd != 0);
                csr_write(csr_addr, (ap_uint<32>)rs1_imm);
                break;
            case 0x6: // CSRRSI
                e.alu_result = csr_read_val;
                e.reg_write = (d.rd != 0);
                if (rs1_imm != 0)
                    csr_write(csr_addr, csr_read_val | (ap_uint<32>)rs1_imm);
                break;
            case 0x7: // CSRRCI
                e.alu_result = csr_read_val;
                e.reg_write = (d.rd != 0);
                if (rs1_imm != 0)
                    csr_write(csr_addr, csr_read_val & ~(ap_uint<32>)rs1_imm);
                break;
            default:
                e.is_trap = true;
                trap_cause = 2; 
                break;
        }

        if (e.is_trap) {
            csr_mepc = d.pc;       
            csr_mcause = trap_cause;  
            e.next_pc = csr_mtvec;      
            e.branch_taken = true; 
            e.reg_write = false; 
        }
        break;
    } 
    case 0x0F: { // FENCE and FENCE.I
        e.reg_write = false; 

        switch ((unsigned)d.funct3) {
            case 0x1: // FENCE.I
                if(CORE_DEBUG) std::cout << "[FENCE.I] Synchronizing Instruction Stream\n";
                break;
            default: // FENCE
                if(CORE_DEBUG) std::cout << "[FENCE] Memory Barrier\n";
                break;
        }
        break;
    }

    default:
            e.is_trap = true;
            csr_mepc = d.pc;       
            csr_mcause = 2; 
            e.next_pc = csr_mtvec;     
            e.branch_taken = true; 
            e.reg_write = false; 
            break;
    }

    if (CORE_DEBUG) {
        std::cout << "[EXEC] ALU=0x" << std::hex << (int)e.alu_result << std::dec << "\n";
    }
    return e;
}

// ------------------------------------------------------------
// Stage: Memory
// ------------------------------------------------------------
MemOut memory(volatile uint32_t* dmem, const ExecOut& e) {
    #pragma HLS INLINE
    MemOut m;
    m.is_trap = e.is_trap;

    // Local copies that may be suppressed by trap
    bool mem_read  = e.mem_read;
    bool mem_write = e.mem_write;
    bool reg_write = e.reg_write;

    if (e.is_trap) {
        mem_read  = false;
        mem_write = false;
        reg_write = false; 
    }

    m.value     = e.alu_result;
    m.rd        = e.rd;
    m.reg_write = reg_write;

    // -------------------------------------------------------------
    // FAST PATH: non-memory instruction
    // -------------------------------------------------------------
    // ALU/branch/CSR/etc. produce their result in m.value above and never
    // touch dmem. Return immediately so the scheduler has a path with no
    // m_axi access, instead of budgeting every iteration for the worst-case
    // AXI read/write latency. Trapping loads/stores are also caught here
    // (mem_read/mem_write were cleared on trap), keeping them off the bus.
    if (!e.is_atomic && !mem_read && !mem_write) {
        return m;
    }

    unsigned ea_u    = (unsigned)e.alu_result;
    unsigned phys_ea = ea_u & 0x07FFFFFF;        // Used for CLINT MMIO checks
    unsigned d_idx   = addr_to_idx(ea_u);        // Synthesis: ea_u>>2, Sim: array-relative
    unsigned byte_off = ea_u & 0x3;

    // =============================================================
    // ATOMIC MEMORY OPERATIONS (A-EXTENSION)
    // =============================================================
    if (e.is_atomic) {
        #ifndef __SYNTHESIS__
        if (d_idx >= RAM_SIZE) { 
            m.value = 0; // Fault
        } else 
        #endif
        {
            uint32_t loaded_raw = dmem[d_idx];
            ap_int<32> loaded_val = (ap_int<32>)loaded_raw;
            ap_int<32> write_val = 0;
            bool do_write = false;

            // Load Reserved (LR.W)
            if (e.atomic_op == 0x02) { 
                lr_addr = ea_u;
                lr_valid = true;
                m.value = loaded_val;
                do_write = false;
                if(CORE_DEBUG) std::cout << "[AMO] LR at 0x" << std::hex << ea_u << std::dec << "\n";
            } 
            // Store Conditional (SC.W)
            else if (e.atomic_op == 0x03) {
                if (lr_valid && lr_addr == ea_u) {
                    write_val = e.store_val;
                    do_write = true;
                    m.value = 0; // Success
                    lr_valid = false;
                } else {
                    do_write = false;
                    m.value = 1; // Failure
                }
                if(CORE_DEBUG) std::cout << "[AMO] SC at 0x" << std::hex << ea_u << (do_write ? " Success" : " Fail") << std::dec << "\n";
            }
            // AMO Read-Modify-Write Operations
            else {
                ap_int<32> op_b = e.store_val;
                do_write = true;
                switch (e.atomic_op) {
                    case 0x01: write_val = op_b; break; // AMOSWAP
                    case 0x00: write_val = loaded_val + op_b; break; // AMOADD
                    case 0x04: write_val = loaded_val ^ op_b; break; // AMOXOR
                    case 0x0C: write_val = loaded_val & op_b; break; // AMOAND
                    case 0x08: write_val = loaded_val | op_b; break; // AMOOR
                    case 0x10: write_val = (loaded_val < op_b) ? loaded_val : op_b; break; // AMOMIN
                    case 0x14: write_val = (loaded_val > op_b) ? loaded_val : op_b; break; // AMOMAX
                    case 0x18: write_val = ((ap_uint<32>)loaded_val < (ap_uint<32>)op_b) ? loaded_val : op_b; break; // AMOMINU
                    case 0x1C: write_val = ((ap_uint<32>)loaded_val > (ap_uint<32>)op_b) ? loaded_val : op_b; break; // AMOMAXU
                    default: do_write = false; break;
                }
                m.value = loaded_val; // AMOs write original value to Rd
            }

            if (do_write) {
                dmem[d_idx] = (uint32_t)write_val;
                lr_valid = false; 
            }
        }
    }
    // =============================================================
    // NORMAL LOAD
    // =============================================================
    else if (mem_read) {
        // ----------------------------------------------------------------
        // MMIO READ: UART
        // ----------------------------------------------------------------
        if ((ea_u & 0xFFFFF000) == 0x10000000) {
            #ifdef __SYNTHESIS__
                m.value = (ap_int<32>)dmem[d_idx]; // Read from real UART via AXI
            #else
                m.value = 0x60; // Sim: always "TX Ready"
            #endif
            m.reg_write = true;
            return m;
        }
        
        // ----------------------------------------------------------------
        // MMIO: CLINT (emulated internally)
        // ----------------------------------------------------------------
        // mtimecmp (0x2004000) - Timer Compare Register
        if (ea_u == 0x02004000) {
            m.value = (ap_int<32>)(mtimecmp & 0xFFFFFFFF);
            m.reg_write = true;
            return m;
        }
        if (ea_u == 0x02004004) {
            m.value = (ap_int<32>)(mtimecmp >> 32);
            m.reg_write = true;
            return m;
        }
        
        // mtime (0x200BFF8) - Current Time (Aliased to csr_mcycle)
        if (ea_u == 0x0200BFF8) {
            m.value = (ap_int<32>)(csr_mcycle & 0xFFFFFFFF);
            m.reg_write = true;
            return m;
        }
        if (ea_u == 0x0200BFFC) {
            m.value = (ap_int<32>)(ap_uint<32>)(csr_mcycle >> 32);
            m.reg_write = true;
            return m;
        }

        // ----------------------------------------------------------------
        // STANDARD RAM READ
        // ----------------------------------------------------------------
        #ifndef __SYNTHESIS__
        if (d_idx >= RAM_SIZE) {
            if(CORE_DEBUG) std::cerr << "[MEM] Load OOB: EA=0x" << std::hex << ea_u << "\n";
            m.value = 0;
        } else 
        #endif
        {
            // Aligned access: the addressed element lives entirely within
            // word0. byte_off only selects the byte/halfword lane for
            // LB/LBU/LH/LHU; it never crosses into the next word.
            uint32_t raw_w0 = dmem[d_idx];
            ap_uint<32> word0 = (ap_uint<32>)raw_w0;

            ap_uint<32> raw_val_32 = word0 >> (byte_off * 8);
            ap_int<32> loaded_val = 0;
            m.reg_write = true;

            switch ((unsigned)e.funct3) { 
                case 0: loaded_val = (ap_int<32>)((ap_int<8>)raw_val_32.range(7, 0)); break; // LB
                case 1: loaded_val = (ap_int<32>)((ap_int<16>)raw_val_32.range(15, 0)); break; // LH
                case 2: loaded_val = (ap_int<32>)raw_val_32; break; // LW
                case 4: loaded_val = (ap_int<32>)((ap_uint<32>)raw_val_32.range(7, 0)); break; // LBU
                case 5: loaded_val = (ap_int<32>)((ap_uint<32>)raw_val_32.range(15, 0)); break; // LHU
                default: m.reg_write = false; break; 
            }
            m.value = loaded_val; 
        }
    }
    // =============================================================
    // NORMAL STORE
    // =============================================================
    else if (mem_write) {
        // Any standard write invalidates a Load Reservation
        lr_valid = false;
        
        // ----------------------------------------------------------------
        // MMIO: UART Write
        // ----------------------------------------------------------------
        if ((ea_u & 0xFFFFF000) == 0x10000000) { 
            #ifdef __SYNTHESIS__
                // Hardware: Direct word write to UART via AXI (no read-modify-write!)
                dmem[d_idx] = (uint32_t)(ap_uint<32>)e.store_val;
            #else
                // Simulation: Print character
                if ((ea_u & 0xFF) == 0x00) {
                    char c = (char)(e.store_val & 0xFF);
                    std::cout << c << std::flush; 
                }
            #endif
            m.reg_write = false; 
            return m;
        }

        // ----------------------------------------------------------------
        // MMIO: CLINT (Timer Compare)
        // ----------------------------------------------------------------
        if (ea_u == 0x02004000) {
            mtimecmp = (mtimecmp & 0xFFFFFFFF00000000) | (ap_uint<32>)e.store_val;
            if(CORE_DEBUG) std::cout << "[CLINT] mtimecmp Low Update: " << std::hex << mtimecmp << std::dec << "\n";
            return m;
        }
        if (ea_u == 0x02004004) {
             mtimecmp = (mtimecmp & 0x00000000FFFFFFFF) | ((ap_uint<64>)e.store_val << 32);
             if(CORE_DEBUG) std::cout << "[CLINT] mtimecmp High Update: " << std::hex << mtimecmp << std::dec << "\n";
             return m;
        }

        // ----------------------------------------------------------------
        // STANDARD RAM STORE
        // ----------------------------------------------------------------
        #ifndef __SYNTHESIS__
        if (d_idx >= RAM_SIZE) {
            if(CORE_DEBUG) std::cerr << "[MEM] Store OOB: EA=0x" << std::hex << ea_u << "\n";
        } else 
        #endif
        {
            // Aligned access: the stored element lives entirely within word0,
            // so a single read-modify-write of one word suffices. byte_off only
            // selects the byte/halfword lane for SB/SH; SW always lands at
            // byte_off == 0.
            uint32_t raw_w0 = dmem[d_idx];
            ap_uint<32> word0 = (ap_uint<32>)raw_w0;

            ap_uint<32> store_val = (ap_uint<32>)e.store_val;
            ap_uint<32> mask0 = 0;

            switch ((unsigned)e.funct3) {
                case 0: mask0 = (ap_uint<32>)0xFF   << (byte_off * 8); break; // SB
                case 1: mask0 = (ap_uint<32>)0xFFFF << (byte_off * 8); break; // SH
                case 2: mask0 = 0xFFFFFFFF;                            break; // SW
            }

            // Modify the single addressed word.
            word0 = (word0 & ~mask0) | ((store_val << (byte_off * 8)) & mask0);
            dmem[d_idx] = (uint32_t)word0;

            // ----------------------------------------------------------------
            // OPTIONAL HTIF / TOHOST COMPLETION DETECTOR
            // ----------------------------------------------------------------
            // Standard RISC-V tests and ELF benchmarks report final completion
            // by writing an odd value to 0x80001000 <tohost>:
            //   0x1                  -> pass
            //   (exit_code << 1) | 1 -> fail
            //
            // Some programs also write even HTIF request values to tohost
            // during normal execution. Those must not stop the core.
            //
            // Disable ENABLE_TOHOST_EXIT when running Linux or any program
            // that may use 0x80001000 as ordinary memory.
#if ENABLE_TOHOST_EXIT
            if (ea_u == 0x80001000 &&
                (((ap_uint<32>)e.store_val & 1) != 0)) {

                is_finished = true;

                if (CORE_DEBUG) {
                    std::cout << "[TOHOST] Completion write detected: 0x"
                              << std::hex << (unsigned)(ap_uint<32>)e.store_val
                              << std::dec << "\n";
                }
            }
#endif

            // ----------------------------------------------------------------
            // OPTIONAL HTIF HOST EMULATION
            // ----------------------------------------------------------------
            // Some ELF benchmarks write an even request value to
            // 0x80001000 <tohost>, then poll 0x80001040 <fromhost> until the
            // simulated host acknowledges the request. This logic must also
            // be synthesized for RTL co-simulation, so it is not hidden by
            // __SYNTHESIS__.
#if ENABLE_HTIF_EMULATION
            if (ea_u == 0x80001000 &&
                (((ap_uint<32>)e.store_val & 1) == 0)) {

                const unsigned fromhost_idx = addr_to_idx(0x80001040);

#ifndef __SYNTHESIS__
                if (fromhost_idx < RAM_SIZE) {
                    dmem[fromhost_idx] = 1;
                }
#else
                dmem[fromhost_idx] = 1;
#endif

                if (CORE_DEBUG) {
                    std::cout << "[HTIF] Request 0x"
                              << std::hex << (unsigned)(ap_uint<32>)e.store_val
                              << " acknowledged through fromhost"
                              << std::dec << "\n";
                }
            }
#endif
            // ----------------------------------------------------------------
            
            if(CORE_DEBUG) std::cout << "[MEM] Stored 0x" << std::hex << (int)e.store_val << " to 0x" << ea_u << std::dec << "\n";
        }
    }
    return m;
}

// ------------------------------------------------------------
// Stage: Writeback
// ------------------------------------------------------------
void writeback(const MemOut& m) {
    #pragma HLS INLINE off
    if (m.reg_write && m.rd != 0 && !m.is_trap) {
        regfile[m.rd] = m.value;
        if(CORE_DEBUG) std::cout << "[WB] x" << (int)m.rd << " <= 0x" << std::hex << (int)m.value << std::dec << "\n";
    }
    regfile[0] = 0; 
}

// ============================================================
// Pipeline Register Structs
// ============================================================
struct IFIDReg {
    bool valid;
    FetchOut data;
};

struct IDEXReg {
    bool valid;
    DecodeOut data;
};

struct EXMEMReg {
    bool valid;
    ExecOut data;
};

struct MEMWBReg {
    bool valid;
    MemOut data;
};

// ============================================================
// Bubble Helpers
// ============================================================
static IFIDReg make_ifid_bubble() {
    IFIDReg r;
    r.valid = false;
    return r;
}

static IDEXReg make_idex_bubble() {
    IDEXReg r;
    r.valid = false;
    return r;
}

static EXMEMReg make_exmem_bubble() {
    EXMEMReg r;
    r.valid = false;
    return r;
}

static MEMWBReg make_memwb_bubble() {
    MEMWBReg r;
    r.valid = false;
    return r;
}

// ============================================================
// Forwarding Helpers
// ============================================================
static bool can_forward_from_exmem(const EXMEMReg& ex_mem) {
    if (!ex_mem.valid) return false;
    if (!ex_mem.data.reg_write) return false;
    if (ex_mem.data.rd == 0) return false;

    // Do not forward load data from EX/MEM.
    // For loads, EX/MEM.alu_result is the address, not the loaded data.
    if (ex_mem.data.mem_read) return false;

    return true;
}

static bool can_forward_from_memwb(const MEMWBReg& mem_wb) {
    if (!mem_wb.valid) return false;
    if (!mem_wb.data.reg_write) return false;
    if (mem_wb.data.rd == 0) return false;
    if (mem_wb.data.is_trap) return false;

    return true;
}

// ============================================================
// ID-Stage Redirect Helpers
// ============================================================
struct IDRedirect {
    bool valid;
    ap_uint<32> next_pc;
};

static IDRedirect make_no_id_redirect() {
    IDRedirect r;
    r.valid = false;
    r.next_pc = 0;
    return r;
}

static bool is_id_resolved_control(ap_uint<7> opcode) {
    #pragma HLS INLINE
    return opcode == 0x63 || // Branch
           opcode == 0x6F || // JAL
           opcode == 0x67;   // JALR
}

static ap_int<32> forward_value_to_id(ap_uint<5> rs,
                                      ap_int<32> reg_val,
                                      const EXMEMReg& ex_mem,
                                      const MEMWBReg& mem_wb) {
    #pragma HLS INLINE
    ap_int<32> v = reg_val;

#if ENABLE_FORWARDING
    if (rs != 0) {
        // Newest safe value has priority. Do not forward EX/MEM loads here
        // because EX/MEM.alu_result is the load address, not loaded data.
        if (can_forward_from_exmem(ex_mem) && rs == ex_mem.data.rd) {
            v = ex_mem.data.alu_result;
        } else if (can_forward_from_memwb(mem_wb) && rs == mem_wb.data.rd) {
            v = mem_wb.data.value;
        }
    }
#endif

    return v;
}

static IDRedirect compute_id_redirect(const IDEXReg& decoded_id,
                                      const EXMEMReg& ex_mem,
                                      const MEMWBReg& mem_wb) {
    #pragma HLS INLINE
    IDRedirect r = make_no_id_redirect();
    if (!decoded_id.valid) return r;

    DecodeOut d = decoded_id.data;
    ap_uint<7> opcode = d.opcode;

    if (!is_id_resolved_control(opcode)) {
        return r;
    }

    ap_int<32> rs1_val = forward_value_to_id(d.rs1, d.rs1_val, ex_mem, mem_wb);
    ap_int<32> rs2_val = forward_value_to_id(d.rs2, d.rs2_val, ex_mem, mem_wb);

    if (opcode == 0x6F) { // JAL
        r.valid = true;
        r.next_pc = (ap_uint<32>)((ap_int<32>)d.pc + d.imm);
    } else if (opcode == 0x67) { // JALR
        r.valid = true;
        r.next_pc = (ap_uint<32>)(((ap_int<32>)rs1_val + d.imm) & (~1));
    } else if (opcode == 0x63) { // Conditional branch
        bool taken = false;

        switch ((unsigned)d.funct3) {
            case 0x0: taken = (rs1_val == rs2_val); break; // BEQ
            case 0x1: taken = (rs1_val != rs2_val); break; // BNE
            case 0x4: taken = (rs1_val < rs2_val);  break; // BLT
            case 0x5: taken = (rs1_val >= rs2_val); break; // BGE
            case 0x6: taken = ((ap_uint<32>)rs1_val < (ap_uint<32>)rs2_val);  break; // BLTU
            case 0x7: taken = ((ap_uint<32>)rs1_val >= (ap_uint<32>)rs2_val); break; // BGEU
            default:  taken = false; break;
        }

        if (taken) {
            r.valid = true;
            r.next_pc = (ap_uint<32>)((ap_int<32>)d.pc + d.imm);
        }
    }

    if (CORE_DEBUG && r.valid) {
        std::cout << "[ID-REDIRECT] PC <= 0x" << std::hex << (unsigned)r.next_pc << std::dec << "\n";
    }

    return r;
}

// ============================================================
// Pipeline Stage Wrappers
// ============================================================
static IFIDReg run_fetch_stage(volatile uint32_t* imem, ap_uint<32> fetch_pc) {
    IFIDReg out = make_ifid_bubble();
    out.valid = true;
    out.data = fetch(imem, fetch_pc);
    return out;
}

static IDEXReg run_decode_stage(const IFIDReg& if_id) {
    IDEXReg out = make_idex_bubble();
    if (if_id.valid) {
        out.valid = true;
        out.data = decode(if_id.data);
    }
    return out;
}

static EXMEMReg run_execute_stage(const IDEXReg& id_ex,
                                  const EXMEMReg& ex_mem,
                                  const MEMWBReg& mem_wb) {
    EXMEMReg out = make_exmem_bubble();

    if (id_ex.valid) {
        DecodeOut d = id_ex.data;

#if ENABLE_FORWARDING
        // Forward rs1
        if (d.rs1 != 0) {
            if (can_forward_from_exmem(ex_mem) && d.rs1 == ex_mem.data.rd) {
                d.rs1_val = ex_mem.data.alu_result;
            } else if (can_forward_from_memwb(mem_wb) && d.rs1 == mem_wb.data.rd) {
                d.rs1_val = mem_wb.data.value;
            }
        }

        // Forward rs2
        if (d.rs2 != 0) {
            if (can_forward_from_exmem(ex_mem) && d.rs2 == ex_mem.data.rd) {
                d.rs2_val = ex_mem.data.alu_result;
            } else if (can_forward_from_memwb(mem_wb) && d.rs2 == mem_wb.data.rd) {
                d.rs2_val = mem_wb.data.value;
            }
        }
#endif

        out.valid = true;
        out.data = execute(d);
    }

    return out;
}

static MEMWBReg run_memory_stage(volatile uint32_t* dmem, const EXMEMReg& ex_mem) {
    MEMWBReg out = make_memwb_bubble();
    if (ex_mem.valid) {
        out.valid = true;
        out.data = memory(dmem, ex_mem.data);
    }
    return out;
}

// ============================================================
// Hazard Decode Helper
// ============================================================
struct HazardInfo {
    ap_uint<7> opcode;
    ap_uint<5> rs1;
    ap_uint<5> rs2;
    ap_uint<5> rd;
};

static HazardInfo get_hazard_info(const IFIDReg& if_id) {
    HazardInfo h{};
    h.opcode = 0;
    h.rs1 = 0;
    h.rs2 = 0;
    h.rd = 0;

    if (!if_id.valid) return h;

    ap_uint<32> instr = if_id.data.instr;
    h.opcode = instr.range(6, 0);
    h.rd     = instr.range(11, 7);
    h.rs1    = instr.range(19, 15);
    h.rs2    = instr.range(24, 20);

    return h;
}

// ------------------------------------------------------------
// Simple Hazard Handeler
// ------------------------------------------------------------
static bool uses_rs1(ap_uint<7> opcode) {
    switch ((unsigned)opcode) {
        case 0x37: // LUI
        case 0x17: // AUIPC
        case 0x6F: // JAL
            return false;
        default:
            return true;
    }
}

static bool uses_rs2(ap_uint<7> opcode) {
    switch ((unsigned)opcode) {
        case 0x33: // R-type
        case 0x23: // Store
        case 0x63: // Branch
        case 0x2F: // Atomic
            return true;
        default:
            return false;
    }
}

static bool opcode_writes_rd(ap_uint<7> opcode) {
    switch ((unsigned)opcode) {
        case 0x33: // R-type
        case 0x13: // I-type ALU
        case 0x03: // Load
        case 0x37: // LUI
        case 0x17: // AUIPC
        case 0x6F: // JAL
        case 0x67: // JALR
        case 0x73: // CSR reads may write rd
        case 0x2F: // Atomic
            return true;
        default:
            return false;
    }
}

static bool id_control_needs_early_operand(ap_uint<7> opcode) {
    #pragma HLS INLINE
    return opcode == 0x63 || // branch compares rs1/rs2 in ID
           opcode == 0x67;   // JALR computes target from rs1 in ID
}

static bool decode_has_hazard(const IFIDReg& if_id,
                              const IDEXReg& id_ex,
                              const EXMEMReg& ex_mem) {
    if (!if_id.valid) return false;

    HazardInfo h = get_hazard_info(if_id);

    bool need_rs1 = uses_rs1(h.opcode) && (h.rs1 != 0);
    bool need_rs2 = uses_rs2(h.opcode) && (h.rs2 != 0);

    if (id_ex.valid && id_ex.data.rd != 0) {
        bool depends_on_idex =
            (need_rs1 && h.rs1 == id_ex.data.rd) ||
            (need_rs2 && h.rs2 == id_ex.data.rd);

#if ENABLE_FORWARDING
        // Normal dependent ALU instructions can usually wait until EX and
        // receive forwarded operands there. ID-resolved branches/JALR need
        // operands one stage earlier, so they must stall for any producer
        // currently in ID/EX. Loads also still create the normal load-use stall.
        bool idex_is_load = (id_ex.data.opcode == 0x03);
        bool idex_writes  = opcode_writes_rd(id_ex.data.opcode);
        bool id_needs_early_operand = id_control_needs_early_operand(h.opcode);

        if (depends_on_idex && (idex_is_load || (id_needs_early_operand && idex_writes))) {
            return true;
        }
#else
        // Without forwarding, any dependency on an ID/EX producer must stall.
        bool ex_writes = false;

        switch ((unsigned)id_ex.data.opcode) {
            case 0x33:
            case 0x13:
            case 0x03:
            case 0x37:
            case 0x17:
            case 0x6F:
            case 0x67:
            case 0x73:
            case 0x2F:
                ex_writes = true;
                break;
            default:
                ex_writes = false;
                break;
        }

        if (depends_on_idex && ex_writes) {
            return true;
        }
#endif
    }

    if (ex_mem.valid && ex_mem.data.reg_write && ex_mem.data.rd != 0) {
        bool depends_on_exmem =
            (need_rs1 && h.rs1 == ex_mem.data.rd) ||
            (need_rs2 && h.rs2 == ex_mem.data.rd);

#if ENABLE_FORWARDING
        // With forwarding, only load-use from EX/MEM must stall.
        // ALU-style EX/MEM results can be forwarded into execute.
        if (depends_on_exmem && ex_mem.data.mem_read) {
            return true;
        }
#else
        // Without forwarding, any dependency on EX/MEM must stall.
        if (depends_on_exmem) {
            return true;
        }
#endif
    }

    return false;
}

// ------------------------------------------------------------
// Top-Level Step Function
// ------------------------------------------------------------
void riscv_step(volatile uint32_t* imem, volatile uint32_t* dmem,
                int max_cycles, int* cycles_output,
                ap_uint<32> entry_pc, ap_uint<32> dtb_addr, ap_uint<1> start) {
    #pragma HLS INTERFACE m_axi port=imem offset=off bundle=imem depth=RAM_SIZE
    #pragma HLS INTERFACE m_axi port=dmem offset=off bundle=dmem depth=RAM_SIZE
    #pragma HLS INTERFACE s_axilite port=max_cycles bundle=control
    #pragma HLS INTERFACE s_axilite port=cycles_output bundle=control
    #pragma HLS INTERFACE s_axilite port=entry_pc bundle=control
    #pragma HLS INTERFACE s_axilite port=dtb_addr bundle=control
    #pragma HLS INTERFACE s_axilite port=return bundle=control
    #pragma HLS INTERFACE ap_none port=start
    #pragma HLS BIND_STORAGE variable=regfile type=ram_2p impl=lutram

    WAIT_FOR_START: while (!start) {
        #pragma HLS PIPELINE II=1
    }

    #ifdef __SYNTHESIS__
        pc = entry_pc;
        for (int i = 0; i < 32; i++) regfile[i] = 0;
        regfile[2]  = entry_pc + 0x4000000;
        regfile[10] = 0;
        regfile[11] = dtb_addr;

        csr_mcycle = 0;
        csr_minstret = 0;
        csr_mstatus = 0;
        csr_mtvec = 0;
        csr_mepc = 0;
        csr_mcause = 0;
        csr_mscratch = 0;
        csr_mie = 0;
        csr_mip = 0;
        mtimecmp = 0xFFFFFFFFFFFFFFFF;
        csr_mtval = 0;
        csr_medeleg = 0;
        csr_mideleg = 0;
        csr_mcountinhibit = 0;
        csr_satp = 0;
        lr_valid = false;
        lr_addr = 0;
    #endif

    is_finished = false;

    // Invalidate the instruction line buffer so the first fetch refills it.
    ic_valid    = false;
    ic_base_idx = 0;

    // ---------------- Pipeline Registers ----------------
    IFIDReg  if_id  = make_ifid_bubble();
    IDEXReg  id_ex  = make_idex_bubble();
    EXMEMReg ex_mem = make_exmem_bubble();
    MEMWBReg mem_wb = make_memwb_bubble();

    INSTRUCTION_LOOP: while (true) {
        #pragma HLS LOOP_TRIPCOUNT min=50 max=500000

        csr_mcycle++;

        // =====================================================
        // 1. WRITEBACK: commit oldest instruction
        // =====================================================
        if (mem_wb.valid) {
            writeback(mem_wb.data);

            // Count only instructions that actually retire. A trapping
            // instruction (ECALL/EBREAK, illegal instruction, misaligned
            // load/store) does not complete, so it must not bump minstret.
            if (!mem_wb.data.is_trap) {
                csr_minstret++;
            }
        }

        // =====================================================
        // 2. COMPUTE NEXT PIPELINE REGISTERS FROM OLD ONES
        // =====================================================
        MEMWBReg next_mem_wb = run_memory_stage(dmem, ex_mem);
        EXMEMReg next_ex_mem = run_execute_stage(id_ex, ex_mem, mem_wb);
        IDEXReg  next_id_ex  = run_decode_stage(if_id);
        IFIDReg  next_if_id  = run_fetch_stage(imem, pc);

        // Default next PC
        ap_uint<32> next_pc = pc + 4;

        // =====================================================
        // 2.5 Check for hazard
        // =====================================================
        bool stall = decode_has_hazard(if_id, id_ex, ex_mem);

        if (stall) {
            next_pc  = pc;                    // hold PC
            next_if_id = if_id;               // hold fetched instruction
            next_id_ex = make_idex_bubble();  // bubble into EX
        }

        // =====================================================
        // 3. NEXT-PC LOGIC
        //    Branch/JAL/JALR redirects are now resolved from ID.
        //    Trap/MRET redirects still come from EX.
        // =====================================================
        if (!stall) {
            IDRedirect id_redirect = compute_id_redirect(next_id_ex, ex_mem, mem_wb);

            if (id_redirect.valid) {
                next_pc = id_redirect.next_pc;

                // Kill the sequential instruction fetched after the control
                // instruction. The control instruction itself remains in ID/EX
                // so JAL/JALR can still write rd = PC + 4.
                next_if_id = make_ifid_bubble();
            }
        }

        // Older EX-stage redirects, such as traps and MRET, have priority over
        // any younger ID-stage redirect.
        if (next_ex_mem.valid && next_ex_mem.data.branch_taken) {
            next_pc = next_ex_mem.data.next_pc;

            // Flush instructions younger than the redirecting instruction.
            // The redirecting instruction itself remains in next_ex_mem.
            next_if_id = make_ifid_bubble();
            next_id_ex = make_idex_bubble();

            next_ex_mem.data.branch_taken = false;
        }

        // =====================================================
        // 4. COMMIT PIPELINE REGISTERS
        // =====================================================
        mem_wb = next_mem_wb;
        ex_mem = next_ex_mem;
        id_ex  = next_id_ex;
        if_id  = next_if_id;

        pc = next_pc;

        // =====================================================
        // 5. EXIT CHECK
        // =====================================================
#if EXIT_ON_TRAP
        bool trap_exit = mem_wb.valid && mem_wb.data.is_trap;
#else
        bool trap_exit = false;
#endif

        if (is_finished ||
            trap_exit ||
            (max_cycles > 0 && (int)(ap_uint<32>)csr_mcycle >= max_cycles)) {
            *cycles_output = (int)(ap_uint<32>)csr_mcycle;
            return;
        }
    }
}