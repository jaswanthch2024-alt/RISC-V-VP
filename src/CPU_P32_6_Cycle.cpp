// SPDX-License-Identifier: GPL-3.0-or-later
#include "CPU_P32_6_Cycle.h"
#include "spdlog/spdlog.h"
#include <iostream>
#include <iomanip>

namespace riscv_tlm {

CPURV32P6_Cycle::CPURV32P6_Cycle(sc_core::sc_module_name const& name,
                                 BaseType PC,
                                 bool debug)
    : CPU(name, debug) {

    // Initialize Register Bank and Memory Interface
    register_bank = new Registers<BaseType>();
    mem_intf = new MemoryInterface();

    // Set Initial State
    register_bank->setPC(PC);
    register_bank->setValue(Registers<BaseType>::sp, (Memory::SIZE / 4) - 1);
    int_cause = 0;

    // Register callback for DMI invalidations
    instr_bus.register_invalidate_direct_mem_ptr(this, &CPU::invalidate_direct_mem_ptr);

    // Initialize ISA Extensions
    base_inst = new BASE_ISA<BaseType>(0, register_bank, mem_intf);
    m_inst    = new M_extension<BaseType>(0, register_bank, mem_intf);

    next_pc = PC;

    // Start the main simulation thread
    SC_THREAD(cycle_thread);

    logger->info("Created CPURV32P6_Cycle (6-stage CVA6-aligned) CPU");
}

CPURV32P6_Cycle::~CPURV32P6_Cycle() {
    delete register_bank;
    delete mem_intf;
    delete base_inst;
    delete m_inst;
}

void CPURV32P6_Cycle::set_clock(sc_core::sc_clock* c) {
    clk = c;
    if (clk) clock_period = clk->period();
}

// =============================================================================
// Main Simulation Loop
// =============================================================================

void CPURV32P6_Cycle::cycle_thread() {
    stats.cycles = 0;
    stats.instructions = 0;

    while (true) {
        // Synchronize with clock
        if (clk) {
            sc_core::wait(clk->posedge_event());
        } else {
            sc_core::wait(clock_period);
        }

        // Pipeline Latch Transfer
        ex_commit_reg = ex_commit_next;
        issue_ex_reg = issue_ex_next;
        id_issue_reg = id_issue_next;
        fetch_id_reg = fetch_id_next;
        pcgen_fetch_reg = pcgen_fetch_next;

        // Execute stages in reverse order
        Commit_stage();
        EX_stage();
        
        // Capture flush flag here: EX_stage() sets it, PCGen_stage() will reset it.
        bool this_cycle_flush = flush_pipeline;

        Issue_stage();
        ID_stage();
        Fetch_stage();
        PCGen_stage();
        
        stats.cycles++;

        // Termination check
        // During a pipeline flush/redirect or while in WFI stall waiting for interrupts, the pipeline
        // may be temporarily empty but we must NOT terminate the simulation.
        if (stats.cycles > 100 && 
            !this_cycle_flush &&
            !pcgen_fetch_reg.valid && 
            !pcgen_fetch_next.valid &&
            !fetch_id_reg.valid && 
            !id_issue_reg.valid && 
            !issue_ex_reg.valid && 
            rob.is_empty()) {
            
            std::cout << "Pipeline Empty & ROB Empty. Stopping Simulation." << std::endl;
            printStats();
            sc_core::sc_stop();
            break;
        }
    }
}

// =============================================================================
// PCGen Stage
// =============================================================================

void CPURV32P6_Cycle::PCGen_stage() {
    if (flush_pipeline) {
        next_pc = pc_redirect_target;
        flush_pipeline = false;
        pc_redirect_valid = false;
        pcgen_fetch_next.valid = false;
        return;
    }

    if (stall_pcgen) {
        return;
    }

    uint32_t current_pc = next_pc;
    pcgen_fetch_next.pc = current_pc;
    pcgen_fetch_next.valid = true;

    // --- Branch Prediction (CVA6-Aligned) ---
    uint32_t btb_idx = (current_pc >> 2) % 128; // Word-aligned indexing
    uint32_t bht_idx = (current_pc >> 2) % 256;
    
    bool predict_taken = false;
    uint32_t pt_target = 0;
    
    if (btb[btb_idx].valid) {
        if (btb[btb_idx].is_return) {
            // RAS Prediction
            if (!ras.empty()) {
                pt_target = ras.back();
                ras.pop_back(); // Speculatively pop
                predict_taken = true;
            }
        } else if (bht[bht_idx] >= 2) {
            // Predict Taken
            pt_target = btb[btb_idx].target;
            predict_taken = true;
        }
    }
    
    pcgen_fetch_next.predicted_taken = predict_taken;
    pcgen_fetch_next.predicted_target = pt_target;
    
    if (predict_taken) {
        next_pc = pt_target;
    } else {
        next_pc = current_pc + 4; // Default increment
    }
}

// =============================================================================
// Fetch Stage
// =============================================================================

void CPURV32P6_Cycle::Fetch_stage() {
    // Flush has highest priority — must invalidate fetch_id_next even if stalled,
    // otherwise a ghost instruction leaks into ID when flush and stall coincide.
    if (flush_pipeline) {
        fetch_id_next.valid = false;
        return;
    }

    if (stall_fetch) {
        return;
    }

    if (!pcgen_fetch_reg.valid) {
        fetch_id_next.valid = false;
        return;
    }

    uint32_t current_pc = pcgen_fetch_reg.pc;
    uint32_t instr = 0;

    if (fetch_instruction(current_pc, instr)) {
        fetch_id_next.pc = current_pc;
        fetch_id_next.instr = instr;
        fetch_id_next.valid = true;
        
        // Pass Branch Prediction Metadata
        fetch_id_next.predicted_taken = pcgen_fetch_reg.predicted_taken;
        fetch_id_next.predicted_target = pcgen_fetch_reg.predicted_target;
    } else {
        fetch_id_next.valid = false;
    }
}

bool CPURV32P6_Cycle::fetch_instruction(uint32_t addr, uint32_t& data) {
    if (dmi_ptr_valid && addr >= dmi_start_addr && (addr + 4) <= dmi_end_addr) {
        std::memcpy(&data, dmi_ptr + (addr - dmi_start_addr), 4);
        return true;
    }

    tlm::tlm_generic_payload trans;
    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;

    trans.set_command(tlm::TLM_READ_COMMAND);
    trans.set_address(addr);
    trans.set_data_ptr(reinterpret_cast<unsigned char*>(&data));
    trans.set_data_length(4);
    trans.set_streaming_width(4);
    trans.set_byte_enable_ptr(nullptr);
    trans.set_dmi_allowed(false);
    trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

    instr_bus->b_transport(trans, delay);

    if (trans.is_response_error()) return false;

    if (trans.is_dmi_allowed()) {
        tlm::tlm_dmi dmi_data;
        if (instr_bus->get_direct_mem_ptr(trans, dmi_data)) {
            dmi_ptr_valid = true;
            dmi_ptr = dmi_data.get_dmi_ptr();
            dmi_start_addr = dmi_data.get_start_address();
            dmi_end_addr = dmi_data.get_end_address();
        }
    }
    return true;
}

// =============================================================================
// ID Stage (Decode)
// =============================================================================

void CPURV32P6_Cycle::ID_stage() {
    if (flush_pipeline) {
        id_issue_next.valid = false;
        return;
    }

    if (stall_issue) {
        return;
    }

    if (!fetch_id_reg.valid) {
        id_issue_next.valid = false;
        return;
    }

    uint32_t instr = fetch_id_reg.instr;
    
    id_issue_next.pc = fetch_id_reg.pc;
    id_issue_next.instr = instr;
    id_issue_next.opcode = instr & 0x7F;
    id_issue_next.funct3 = (instr >> 12) & 0x7;
    id_issue_next.rs1 = (instr >> 15) & 0x1F;
    id_issue_next.rs2 = (instr >> 20) & 0x1F;
    
    // Pass Branch Prediction Metadata
    id_issue_next.predicted_taken = fetch_id_reg.predicted_taken;
    id_issue_next.predicted_target = fetch_id_reg.predicted_target;
    id_issue_next.is_ras_call = false;
    id_issue_next.is_ras_return = false;
    
    // Detect function calls and returns for RAS
    if (id_issue_next.opcode == 0x6F || id_issue_next.opcode == 0x67) {
        bool is_call = (id_issue_next.rd == 1 || id_issue_next.rd == 5);
        bool is_ret = (id_issue_next.opcode == 0x67 && (id_issue_next.rs1 == 1 || id_issue_next.rs1 == 5) && id_issue_next.rs1 != id_issue_next.rd);
        id_issue_next.is_ras_call = is_call;
        id_issue_next.is_ras_return = is_ret;
    }
    
    // Mask unused registers to prevent false data hazard stalls
    if (id_issue_next.opcode == 0x37 || id_issue_next.opcode == 0x17 || id_issue_next.opcode == 0x6F) {
        id_issue_next.rs1 = 0;
        id_issue_next.rs2 = 0;
    } else if (id_issue_next.opcode == 0x13 || id_issue_next.opcode == 0x03 || id_issue_next.opcode == 0x67 || id_issue_next.opcode == 0x73) {
        id_issue_next.rs2 = 0;
    }
    
    id_issue_next.funct7 = (instr >> 25) & 0x7F;

    // rd = 0 for Store (0x23) and Branch (0x63)
    if (id_issue_next.opcode == 0x23 || id_issue_next.opcode == 0x63) {
        id_issue_next.rd = 0;
    } else {
        id_issue_next.rd = (instr >> 7) & 0x1F;
    }

    // Decode immediate
    switch (id_issue_next.opcode) {
        case 0x13: case 0x03: case 0x67: // I-type
            id_issue_next.imm = static_cast<int32_t>(instr) >> 20;
            break;
        case 0x23: // S-type
            id_issue_next.imm = static_cast<int32_t>(((instr >> 25) << 5) | ((instr >> 7) & 0x1F)) << 20 >> 20;
            break;
        case 0x63: // B-type
            id_issue_next.imm = static_cast<int32_t>(
                ((instr >> 31) << 12) | (((instr >> 7) & 1) << 11) |
                (((instr >> 25) & 0x3F) << 5) | (((instr >> 8) & 0xF) << 1)) << 19 >> 19;
            break;
        case 0x37: case 0x17: // U-type
            id_issue_next.imm = static_cast<int32_t>(instr & 0xFFFFF000);
            break;
        case 0x6F: // J-type
            id_issue_next.imm = static_cast<int32_t>(
                ((instr >> 31) << 20) | (((instr >> 12) & 0xFF) << 12) |
                (((instr >> 20) & 1) << 11) | (((instr >> 21) & 0x3FF) << 1)) << 11 >> 11;
            break;
        case 0x73: // SYSTEM
            id_issue_next.imm = static_cast<int32_t>(instr) >> 20;
            break;
        default:
            id_issue_next.imm = 0;
    }

    id_issue_next.valid = true;
}

// =============================================================================
// Issue Stage
// =============================================================================

void CPURV32P6_Cycle::Issue_stage() {
    stall_pcgen = false;
    stall_fetch = false;
    stall_issue = false;

    if (flush_pipeline) {
        issue_ex_next.valid = false;
        return;
    }

    if (!id_issue_reg.valid) {
        issue_ex_next.valid = false;
        return;
    }

    // Hazard Detection (Scoreboard)
    if (scoreboard[id_issue_reg.rs1] || scoreboard[id_issue_reg.rs2]) {
        stall_issue = true;
        stall_fetch = true;
        stall_pcgen = true;
        issue_ex_next.valid = false;
        stats.stalls++;
        return;
    }

    // --- Store Buffer Full Check ---
    // Stall if this is a store and the store buffer is full — prevents silent data loss.
    if (id_issue_reg.opcode == 0x23 && store_buffer.is_full()) {
        stall_issue = true;
        stall_fetch = true;
        stall_pcgen = true;
        issue_ex_next.valid = false;
        stats.stalls++;
        return;
    }

    // ROB Allocation
    int rob_idx = rob.allocate();
    if (rob_idx < 0) {
        stall_issue = true;
        stall_fetch = true;
        stall_pcgen = true;
        issue_ex_next.valid = false;
        stats.stalls++;
        return;
    }

    // ROB Setup
    rob[rob_idx].pc = id_issue_reg.pc;
    rob[rob_idx].is_store = (id_issue_reg.opcode == 0x23);
    rob[rob_idx].is_branch = (id_issue_reg.opcode == 0x63 || id_issue_reg.opcode == 0x6F || id_issue_reg.opcode == 0x67);

    // --- Illegal Instruction Check ---
    // Trap on all-zero instruction word — unmapped memory returns 0x00000000.
    if (id_issue_reg.instr == 0x00000000) {
        std::cerr << "[TRAP] Illegal instruction (0x00000000) @ PC=0x" << std::hex
                  << (uint32_t)id_issue_reg.pc << std::dec << std::endl;
        rob.flush();
        std::fill(std::begin(scoreboard), std::end(scoreboard), false);
        issue_ex_next.valid = false;
        printStats();
        sc_core::sc_stop();
        return;
    }

    // Dispatch
    issue_ex_next.pc = id_issue_reg.pc;
    issue_ex_next.rs1_val = register_bank->getValue(id_issue_reg.rs1);
    issue_ex_next.rs2_val = register_bank->getValue(id_issue_reg.rs2);
    issue_ex_next.imm = id_issue_reg.imm;
    issue_ex_next.rd = id_issue_reg.rd;
    issue_ex_next.opcode = id_issue_reg.opcode;
    issue_ex_next.funct3 = id_issue_reg.funct3;
    issue_ex_next.funct7 = id_issue_reg.funct7;
    issue_ex_next.rob_index = rob_idx;
    issue_ex_next.valid = true;
    
    // Pass Branch Prediction Metadata
    issue_ex_next.predicted_taken = id_issue_reg.predicted_taken;
    issue_ex_next.predicted_target = id_issue_reg.predicted_target;
    issue_ex_next.is_ras_call = id_issue_reg.is_ras_call;
    issue_ex_next.is_ras_return = id_issue_reg.is_ras_return;

    // Update Scoreboard
    if (id_issue_reg.rd != 0) {
        scoreboard[id_issue_reg.rd] = true;
    }
}

// =============================================================================
// EX Stage (Execute & Memory Access)
// =============================================================================

void CPURV32P6_Cycle::EX_stage() {
    if (!issue_ex_reg.valid) {
        ex_commit_next.valid = false;
        return;
    }

    uint32_t result = 0;
    bool branch_taken = false;
    uint32_t branch_target = 0;
    
    switch (issue_ex_reg.opcode) {
        case 0x33: // R-type
            switch (issue_ex_reg.funct3) {
                case 0x0: 
                    if (issue_ex_reg.funct7 == 0x20) result = issue_ex_reg.rs1_val - issue_ex_reg.rs2_val;
                    else result = issue_ex_reg.rs1_val + issue_ex_reg.rs2_val;
                    break;
                case 0x1: result = issue_ex_reg.rs1_val << (issue_ex_reg.rs2_val & 0x1F); break;
                case 0x2: result = (static_cast<int32_t>(issue_ex_reg.rs1_val) < static_cast<int32_t>(issue_ex_reg.rs2_val)); break;
                case 0x3: result = (issue_ex_reg.rs1_val < issue_ex_reg.rs2_val); break;
                case 0x4: result = issue_ex_reg.rs1_val ^ issue_ex_reg.rs2_val; break;
                case 0x5: 
                    if (issue_ex_reg.funct7 == 0x20) result = static_cast<int32_t>(issue_ex_reg.rs1_val) >> (issue_ex_reg.rs2_val & 0x1F);
                    else result = issue_ex_reg.rs1_val >> (issue_ex_reg.rs2_val & 0x1F);
                    break;
                case 0x6: result = issue_ex_reg.rs1_val | issue_ex_reg.rs2_val; break;
                case 0x7: result = issue_ex_reg.rs1_val & issue_ex_reg.rs2_val; break;
            }
            break;

        case 0x13: // I-type ALU
            switch (issue_ex_reg.funct3) {
                case 0x0: result = issue_ex_reg.rs1_val + issue_ex_reg.imm; break;
                case 0x2: result = (static_cast<int32_t>(issue_ex_reg.rs1_val) < issue_ex_reg.imm); break;
                case 0x3: result = (issue_ex_reg.rs1_val < static_cast<uint32_t>(issue_ex_reg.imm)); break;
                case 0x4: result = issue_ex_reg.rs1_val ^ issue_ex_reg.imm; break;
                case 0x6: result = issue_ex_reg.rs1_val | issue_ex_reg.imm; break;
                case 0x7: result = issue_ex_reg.rs1_val & issue_ex_reg.imm; break;
                case 0x1: result = issue_ex_reg.rs1_val << (issue_ex_reg.imm & 0x1F); break;
                case 0x5: 
                    if ((issue_ex_reg.imm & 0x400) != 0) result = static_cast<int32_t>(issue_ex_reg.rs1_val) >> (issue_ex_reg.imm & 0x1F);
                    else result = issue_ex_reg.rs1_val >> (issue_ex_reg.imm & 0x1F);
                    break;
            }
            break;

        case 0x37: result = issue_ex_reg.imm; break; // LUI
        case 0x17: result = issue_ex_reg.pc + issue_ex_reg.imm; break; // AUIPC

        case 0x6F: // JAL
            result = issue_ex_reg.pc + 4;
            branch_target = issue_ex_reg.pc + issue_ex_reg.imm;
            branch_taken = true;
            break;

        case 0x67: // JALR
            result = issue_ex_reg.pc + 4;
            branch_target = (issue_ex_reg.rs1_val + issue_ex_reg.imm) & ~1;
            branch_taken = true;
            break;

        case 0x63: // Branch
            stats.branches++;
            branch_target = issue_ex_reg.pc + issue_ex_reg.imm;
            switch (issue_ex_reg.funct3) {
                case 0x0: branch_taken = (issue_ex_reg.rs1_val == issue_ex_reg.rs2_val); break;
                case 0x1: branch_taken = (issue_ex_reg.rs1_val != issue_ex_reg.rs2_val); break;
                case 0x4: branch_taken = (static_cast<int32_t>(issue_ex_reg.rs1_val) < static_cast<int32_t>(issue_ex_reg.rs2_val)); break;
                case 0x5: branch_taken = (static_cast<int32_t>(issue_ex_reg.rs1_val) >= static_cast<int32_t>(issue_ex_reg.rs2_val)); break;
                case 0x6: branch_taken = (issue_ex_reg.rs1_val < issue_ex_reg.rs2_val); break;
                case 0x7: branch_taken = (issue_ex_reg.rs1_val >= issue_ex_reg.rs2_val); break;
            }
            break;
    }

    // Load/Store (LSU)
    if (issue_ex_reg.opcode == 0x03) { // Load
        uint32_t addr = issue_ex_reg.rs1_val + issue_ex_reg.imm;
        switch (issue_ex_reg.funct3) {
            case 0x0: result = static_cast<int32_t>(static_cast<int8_t>(mem_intf->readDataMem(addr, 1))); break;
            case 0x1: result = static_cast<int32_t>(static_cast<int16_t>(mem_intf->readDataMem(addr, 2))); break;
            case 0x2: result = mem_intf->readDataMem(addr, 4); break;
            case 0x4: result = mem_intf->readDataMem(addr, 1); break;
            case 0x5: result = mem_intf->readDataMem(addr, 2); break;
        }
    }
    // Note: Store handling moved to Commit_stage via EX-Commit latch

    // Branch Prediction Verification & Training
    bool is_branch_instruction = (issue_ex_reg.opcode == 0x63 || issue_ex_reg.opcode == 0x6F || issue_ex_reg.opcode == 0x67);
    
    if (is_branch_instruction) {
        // Verify Prediction
        bool mispredict = (branch_taken != issue_ex_reg.predicted_taken) || 
                          (branch_taken && (branch_target != issue_ex_reg.predicted_target));
                          
        // Train Predictor
        uint32_t btb_idx = (issue_ex_reg.pc >> 2) % 128;
        uint32_t bht_idx = (issue_ex_reg.pc >> 2) % 256;
        
        if (issue_ex_reg.opcode == 0x63) { // Conditional Branch
            uint8_t current_bht = bht[bht_idx];
            if (branch_taken) {
                if (current_bht < 3) bht[bht_idx]++;
            } else {
                if (current_bht > 0) bht[bht_idx]--;
            }
            btb[btb_idx].target = branch_target;
            btb[btb_idx].valid = true;
            btb[btb_idx].is_return = false;
        } else { // Unconditional Jump
            btb[btb_idx].target = branch_target;
            btb[btb_idx].valid = true;
            bht[bht_idx] = 3; // Force Strongly Taken
            
            // RAS Maintenance
            if (issue_ex_reg.is_ras_call) {
                ras.push_back(issue_ex_reg.pc + 4);
                if (ras.size() > 4) ras.erase(ras.begin());
            }
            if (issue_ex_reg.is_ras_return) {
                btb[btb_idx].is_return = true;
            }
        }
        
        // Redirect on Mispredict
        if (mispredict) {
            pc_redirect_target = branch_taken ? branch_target : (issue_ex_reg.pc + 4);
            pc_redirect_valid = true;
            flush_pipeline = true;
            stats.branch_mispredicts++;
            ras.clear(); // Clear RAS on mispredict to be safe
        }
    }
    
    // ECALL Handling
    if (issue_ex_reg.opcode == 0x73) {
        if (issue_ex_reg.funct3 == 0 && issue_ex_reg.imm == 0) {
            uint32_t syscall_num = register_bank->getValue(17); // a7
            uint32_t a0 = register_bank->getValue(10);
            uint32_t a1 = register_bank->getValue(11);
            uint32_t a2 = register_bank->getValue(12);

            switch (syscall_num) {
                case 64: { // write(fd, buf, count)
                    FILE* out = (a0 == 1) ? stdout : (a0 == 2) ? stderr : nullptr;
                    if (out && a2 > 0) {
                        for (uint32_t i = 0; i < a2; i++) {
                            uint8_t byte = static_cast<uint8_t>(mem_intf->readDataMem(a1 + i, 1));
                            fputc(byte, out);
                        }
                        fflush(out);
                    }
                    register_bank->setValue(10, a2); // return bytes written
                    break;
                }
                case 93: // exit(code)
                case 1:  // legacy exit
                    std::cout << "\n[ECALL] exit(" << a0 << ")" << std::endl;
                    sc_core::sc_stop();
                    break;
                case 214: // brk
                    register_bank->setValue(10, 0); // return success
                    break;
                default:
                    // Unhandled syscall — ignore silently
                    register_bank->setValue(10, static_cast<uint32_t>(-38)); // -ENOSYS
                    break;
            }
        }
    }

    // Output to EX-Commit latch (deferred ROB completion)
    ex_commit_next.result = result;
    ex_commit_next.rd = issue_ex_reg.rd;
    ex_commit_next.rob_index = issue_ex_reg.rob_index;
    ex_commit_next.is_store = (issue_ex_reg.opcode == 0x23);
    if (ex_commit_next.is_store) {
        ex_commit_next.store_addr = issue_ex_reg.rs1_val + issue_ex_reg.imm;
        ex_commit_next.store_data = issue_ex_reg.rs2_val;
        switch (issue_ex_reg.funct3) {
            case 0x0: ex_commit_next.store_size = 1; break;
            case 0x1: ex_commit_next.store_size = 2; break;
            case 0x2: ex_commit_next.store_size = 4; break;
            default: ex_commit_next.store_size = 0;
        }
    }
    ex_commit_next.valid = true;
}

// =============================================================================
// Commit Stage
// =============================================================================

void CPURV32P6_Cycle::Commit_stage() {
    // 1. Complete ROB entry from EX-Commit latch
    if (ex_commit_reg.valid && ex_commit_reg.rob_index >= 0) {
        rob.complete(ex_commit_reg.rob_index, ex_commit_reg.result, ex_commit_reg.rd);
        
        // Handle stores: add to store buffer
        if (ex_commit_reg.is_store) {
            store_buffer.add_store(ex_commit_reg.store_addr, ex_commit_reg.store_data, 
                                   ex_commit_reg.store_size, ex_commit_reg.rob_index);
        }
    }
    
    // 2. Retire the oldest instruction if ready
    if (rob.head_ready()) {
        const ROBEntry& entry = rob.get_head();
        
        // Commit Stores to memory
        if (entry.is_store) {
            store_buffer.commit_store(rob.get_head_index());
            uint64_t addr, data;
            int size;
            if (store_buffer.drain_one(addr, data, size)) {
                mem_intf->writeDataMem(addr, static_cast<uint32_t>(data), size);
            }
        }
        
        // Commit Register Results (and release scoreboard)
        if (entry.dest_reg != 0) {
            register_bank->setValue(entry.dest_reg, static_cast<uint32_t>(entry.result));
            scoreboard[entry.dest_reg] = false;
        }
        
        stats.instructions++;
        if (perf) perf->instructionsInc();

        rob.retire();
    }
}

// =============================================================================
// Helpers / Boilerplate
// =============================================================================

bool CPURV32P6_Cycle::cpu_process_IRQ() {
    return false;
}

void CPURV32P6_Cycle::call_interrupt(tlm::tlm_generic_payload& m_trans, sc_core::sc_time& delay) {
    interrupt = true;
    memcpy(&int_cause, m_trans.get_data_ptr(), sizeof(BaseType));
    delay = sc_core::SC_ZERO_TIME;
}

std::uint64_t CPURV32P6_Cycle::getStartDumpAddress() {
    return register_bank->getValue(Registers<BaseType>::t0);
}

std::uint64_t CPURV32P6_Cycle::getEndDumpAddress() {
    return register_bank->getValue(Registers<BaseType>::t1);
}

void CPURV32P6_Cycle::printStats() const {
    std::cout << "  Architecture: RV32 (CVA6 6-Stage Aligned)\n";
    std::cout << "  Cycles:       " << stats.cycles << "\n";
    std::cout << "  Instructions: " << stats.instructions << "\n";
    std::cout << "  CPI:          " << std::fixed << std::setprecision(2) << stats.get_cpi() << "\n";
    std::cout << "  IPC:          " << std::fixed << std::setprecision(3) << stats.get_ipc() << "\n";
    std::cout << "  Stalls:       " << stats.stalls << "\n";
    std::cout << "  Branches:     " << stats.branches << "\n";
    std::cout << "  Mispredicts:  " << stats.branch_mispredicts << "\n";
    if (stats.branches > 0) {
        std::cout << "  Predict Rate: " << std::fixed << std::setprecision(1)
                  << (100.0 * (stats.branches - stats.branch_mispredicts) / stats.branches) << "%\n";
    }
}

} // namespace riscv_tlm
