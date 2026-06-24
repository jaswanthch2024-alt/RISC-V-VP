/*!
 \file A_extension.h
 \brief Implement A extensions part of the RISC-V
 \author Màrius Montón
 \date December 2018
 */
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef A_EXTENSION__H
#define A_EXTENSION__H

#include "systemc"

#include <unordered_set>

#include "Registers.h"
#include "MemoryInterface.h"
#include "extension_base.h"

namespace riscv_tlm {

    typedef enum {
        OP_A_LR,
        OP_A_SC,
        OP_A_AMOSWAP,
        OP_A_AMOADD,
        OP_A_AMOXOR,
        OP_A_AMOAND,
        OP_A_AMOOR,
        OP_A_AMOMIN,
        OP_A_AMOMAX,
        OP_A_AMOMINU,
        OP_A_AMOMAXU,

        // 64-bit (.d) variants
        OP_A_LR_D,
        OP_A_SC_D,
        OP_A_AMOSWAP_D,
        OP_A_AMOADD_D,
        OP_A_AMOXOR_D,
        OP_A_AMOAND_D,
        OP_A_AMOOR_D,
        OP_A_AMOMIN_D,
        OP_A_AMOMAX_D,
        OP_A_AMOMINU_D,
        OP_A_AMOMAXU_D,

        OP_A_ERROR
    } op_A_Codes;

    typedef enum {
        A_LR = 0b00010,
        A_SC = 0b00011,
        A_AMOSWAP = 0b00001,
        A_AMOADD = 0b00000,
        A_AMOXOR = 0b00100,
        A_AMOAND = 0b01100,
        A_AMOOR = 0b01000,
        A_AMOMIN = 0b10000,
        A_AMOMAX = 0b10100,
        A_AMOMINU = 0b11000,
        A_AMOMAXU = 0b11100,
    } A_Codes;

/**
 * @brief Instruction decoding and fields access
 */
    template<typename T>
    class A_extension : public extension_base<T> {
    public:

        /**
         * @brief Constructor, same as base class
         */
        using extension_base<T>::extension_base;

        using signed_T = typename std::make_signed<T>::type;
        using unsigned_T = typename std::make_unsigned<T>::type;

        /**
         * @brief Access to opcode field
         * @return return opcode field
         */
        inline unsigned_T opcode() const override {
            return static_cast<unsigned_T>(this->m_instr.range(31, 27));
        }

        /**
         * @brief Decodes opcode of instruction
         * @return opcode of instruction
         */
        op_A_Codes decode() const {
            uint32_t funct5 = (this->m_instr >> 27) & 0x1F;
            uint32_t funct3 = (this->m_instr >> 12) & 0x7;
            bool is64 = (funct3 == 0x3);

            switch (static_cast<A_Codes>(funct5)) {
                case A_LR:      return is64 ? OP_A_LR_D      : OP_A_LR;
                case A_SC:      return is64 ? OP_A_SC_D      : OP_A_SC;
                case A_AMOSWAP: return is64 ? OP_A_AMOSWAP_D : OP_A_AMOSWAP;
                case A_AMOADD:  return is64 ? OP_A_AMOADD_D  : OP_A_AMOADD;
                case A_AMOXOR:  return is64 ? OP_A_AMOXOR_D  : OP_A_AMOXOR;
                case A_AMOAND:  return is64 ? OP_A_AMOAND_D  : OP_A_AMOAND;
                case A_AMOOR:   return is64 ? OP_A_AMOOR_D   : OP_A_AMOOR;
                case A_AMOMIN:  return is64 ? OP_A_AMOMIN_D  : OP_A_AMOMIN;
                case A_AMOMAX:  return is64 ? OP_A_AMOMAX_D  : OP_A_AMOMAX;
                case A_AMOMINU: return is64 ? OP_A_AMOMINU_D : OP_A_AMOMINU;
                case A_AMOMAXU: return is64 ? OP_A_AMOMAXU_D : OP_A_AMOMAXU;
                default:        return OP_A_ERROR;
            }
        }

        inline void dump() const override {
            std::cout << std::hex << "0x" << this->m_instr << std::dec << std::endl;
        }

        bool Exec_A_LR() {
            std::uint32_t mem_addr = 0;
            int rd, rs1, rs2;
            std::uint32_t data;

            rd = this->get_rd();
            rs1 = this->get_rs1();
            rs2 = this->get_rs2();

            if (rs2 != 0) {
                std::cout << "ILEGAL INSTRUCTION, LR.W: rs2 != 0" << std::endl;
                this->RaiseException(Exception_cause::ILLEGAL_INSTRUCTION, this->m_instr);

                return false;
            }

            mem_addr = this->regs->getValue(rs1);
            data = this->mem_intf->readDataMem(mem_addr, 4);
            this->perf->dataMemoryRead();
            this->regs->setValue(rd, static_cast<int32_t>(data));

            TLB_reserve(mem_addr);

            this->logger->debug("{} ns. PC: 0x{:x}. A.LR.W: x{:d}(0x{:x}) -> x{:d}(0x{:x}) ",
                                sc_core::sc_time_stamp().value(),
                                this->regs->getPC(),
                                rs1, mem_addr, rd, data);

            return true;
        }

        bool Exec_A_SC() {
            std::uint32_t mem_addr;
            int rd, rs1, rs2;
            std::uint32_t data;

            rd = this->get_rd();
            rs1 = this->get_rs1();
            rs2 = this->get_rs2();

            mem_addr = this->regs->getValue(rs1);
            data = this->regs->getValue(rs2);

            if (TLB_reserved(mem_addr)) {
                this->mem_intf->writeDataMem(mem_addr, data, 4);
                this->perf->dataMemoryWrite();
                this->regs->setValue(rd, 0);  // SC writes 0 to rd on success
            } else {
                this->regs->setValue(rd, 1);  // SC writes nonzero on failure
            }

            this->logger->debug("{} ns. PC: 0x{:x}. A.SC.W: (0x{:x}) <- x{:d}(0x{:x}) ",
                                sc_core::sc_time_stamp().value(),
                                this->regs->getPC(),
                                mem_addr, rs2, data);

            return true;
        }

        bool Exec_A_AMOSWAP() const {
            int rd  = this->get_rd();
            int rs1 = this->get_rs1();
            int rs2 = this->get_rs2();

            uint32_t mem_addr = this->regs->getValue(rs1);
            uint32_t old_val  = this->mem_intf->readDataMem(mem_addr, 4);
            this->perf->dataMemoryRead();
            uint32_t new_val  = this->regs->getValue(rs2);  // read rs2 before writing rd

            this->regs->setValue(rd, static_cast<int32_t>(old_val));
            this->mem_intf->writeDataMem(mem_addr, new_val, 4);
            this->perf->dataMemoryWrite();
            return true;
        }

        bool Exec_A_AMOADD() const {
            int rd  = this->get_rd();
            int rs1 = this->get_rs1();
            int rs2 = this->get_rs2();

            uint32_t mem_addr = this->regs->getValue(rs1);
            uint32_t old_val  = this->mem_intf->readDataMem(mem_addr, 4);
            this->perf->dataMemoryRead();
            uint32_t operand  = this->regs->getValue(rs2);  // read rs2 before writing rd

            this->regs->setValue(rd, static_cast<int32_t>(old_val));
            this->mem_intf->writeDataMem(mem_addr, old_val + operand, 4);
            this->perf->dataMemoryWrite();
            return true;
        }

        bool Exec_A_AMOXOR() const {
            int rd  = this->get_rd();
            int rs1 = this->get_rs1();
            int rs2 = this->get_rs2();

            uint32_t mem_addr = this->regs->getValue(rs1);
            uint32_t old_val  = this->mem_intf->readDataMem(mem_addr, 4);
            this->perf->dataMemoryRead();
            uint32_t operand  = this->regs->getValue(rs2);

            this->regs->setValue(rd, static_cast<int32_t>(old_val));
            this->mem_intf->writeDataMem(mem_addr, old_val ^ operand, 4);
            this->perf->dataMemoryWrite();
            return true;
        }

        bool Exec_A_AMOAND() const {
            int rd  = this->get_rd();
            int rs1 = this->get_rs1();
            int rs2 = this->get_rs2();

            uint32_t mem_addr = this->regs->getValue(rs1);
            uint32_t old_val  = this->mem_intf->readDataMem(mem_addr, 4);
            this->perf->dataMemoryRead();
            uint32_t operand  = this->regs->getValue(rs2);

            this->regs->setValue(rd, static_cast<int32_t>(old_val));
            this->mem_intf->writeDataMem(mem_addr, old_val & operand, 4);
            this->perf->dataMemoryWrite();
            return true;
        }

        bool Exec_A_AMOOR() const {
            int rd  = this->get_rd();
            int rs1 = this->get_rs1();
            int rs2 = this->get_rs2();

            uint32_t mem_addr = this->regs->getValue(rs1);
            uint32_t old_val  = this->mem_intf->readDataMem(mem_addr, 4);
            this->perf->dataMemoryRead();
            uint32_t operand  = this->regs->getValue(rs2);

            this->regs->setValue(rd, static_cast<int32_t>(old_val));
            this->mem_intf->writeDataMem(mem_addr, old_val | operand, 4);
            this->perf->dataMemoryWrite();
            return true;
        }

        bool Exec_A_AMOMIN() const {
            int rd  = this->get_rd();
            int rs1 = this->get_rs1();
            int rs2 = this->get_rs2();

            uint32_t mem_addr = this->regs->getValue(rs1);
            uint32_t old_val  = this->mem_intf->readDataMem(mem_addr, 4);
            this->perf->dataMemoryRead();
            uint32_t operand  = this->regs->getValue(rs2);

            uint32_t new_val = ((int32_t)old_val < (int32_t)operand) ? old_val : operand;
            this->regs->setValue(rd, static_cast<int32_t>(old_val));
            this->mem_intf->writeDataMem(mem_addr, new_val, 4);
            this->perf->dataMemoryWrite();
            return true;
        }

        bool Exec_A_AMOMAX() const {
            int rd  = this->get_rd();
            int rs1 = this->get_rs1();
            int rs2 = this->get_rs2();

            uint32_t mem_addr = this->regs->getValue(rs1);
            uint32_t old_val  = this->mem_intf->readDataMem(mem_addr, 4);
            this->perf->dataMemoryRead();
            uint32_t operand  = this->regs->getValue(rs2);

            uint32_t new_val = ((int32_t)old_val > (int32_t)operand) ? old_val : operand;
            this->regs->setValue(rd, static_cast<int32_t>(old_val));
            this->mem_intf->writeDataMem(mem_addr, new_val, 4);
            this->perf->dataMemoryWrite();
            return true;
        }

        bool Exec_A_AMOMINU() const {
            int rd  = this->get_rd();
            int rs1 = this->get_rs1();
            int rs2 = this->get_rs2();

            uint32_t mem_addr = this->regs->getValue(rs1);
            uint32_t old_val  = this->mem_intf->readDataMem(mem_addr, 4);
            this->perf->dataMemoryRead();
            uint32_t operand  = this->regs->getValue(rs2);

            uint32_t new_val = (old_val < operand) ? old_val : operand;
            this->regs->setValue(rd, static_cast<int32_t>(old_val));
            this->mem_intf->writeDataMem(mem_addr, new_val, 4);
            this->perf->dataMemoryWrite();
            return true;
        }

        bool Exec_A_AMOMAXU() const {
            int rd  = this->get_rd();
            int rs1 = this->get_rs1();
            int rs2 = this->get_rs2();

            uint32_t mem_addr = this->regs->getValue(rs1);
            uint32_t old_val  = this->mem_intf->readDataMem(mem_addr, 4);
            this->perf->dataMemoryRead();
            uint32_t operand  = this->regs->getValue(rs2);

            uint32_t new_val = (old_val > operand) ? old_val : operand;
            this->regs->setValue(rd, static_cast<int32_t>(old_val));
            this->mem_intf->writeDataMem(mem_addr, new_val, 4);
            this->perf->dataMemoryWrite();
            return true;
        }

        bool Exec_LR_D() {
            uint32_t rd  = this->get_rd();
            uint32_t rs1 = this->get_rs1();
            uint64_t mem_addr = static_cast<uint64_t>(this->regs->getValue(rs1));
            uint64_t data = this->mem_intf->readDataMem64(mem_addr, 8);
            this->perf->dataMemoryRead();
            this->regs->setValue(rd, static_cast<int64_t>(data));
            TLB_reserve_64(mem_addr);
            this->logger->debug("{} ns. PC: 0x{:x}. A.LR.D: x{:d}(0x{:x}) -> x{:d}(0x{:x})",
                                sc_core::sc_time_stamp().value(),
                                this->regs->getPC(),
                                rs1, mem_addr, rd, data);
            return true;
        }

        bool Exec_SC_D() {
            uint32_t rd  = this->get_rd();
            uint32_t rs1 = this->get_rs1();
            uint32_t rs2 = this->get_rs2();
            uint64_t mem_addr = static_cast<uint64_t>(this->regs->getValue(rs1));
            if (TLB_reserved_64(mem_addr)) {
                uint64_t data = static_cast<uint64_t>(this->regs->getValue(rs2));
                this->mem_intf->writeDataMem64(mem_addr, data, 8);
                this->perf->dataMemoryWrite();
                this->regs->setValue(rd, 0); // success
            } else {
                this->regs->setValue(rd, 1); // failure
            }
            this->logger->debug("{} ns. PC: 0x{:x}. A.SC.D: (0x{:x}) <- x{:d}",
                                sc_core::sc_time_stamp().value(),
                                this->regs->getPC(),
                                mem_addr, rs2);
            return true;
        }

        bool Exec_AMO_D(op_A_Codes op) {
            uint32_t rd  = this->get_rd();
            uint32_t rs1 = this->get_rs1();
            uint32_t rs2 = this->get_rs2();
            uint64_t mem_addr = static_cast<uint64_t>(this->regs->getValue(rs1));
            uint64_t old_val = this->mem_intf->readDataMem64(mem_addr, 8);
            this->perf->dataMemoryRead();
            uint64_t operand = static_cast<uint64_t>(this->regs->getValue(rs2));
            uint64_t new_val = old_val;
            switch (op) {
                case OP_A_AMOSWAP_D: new_val = operand; break;
                case OP_A_AMOADD_D:  new_val = old_val + operand; break;
                case OP_A_AMOXOR_D:  new_val = old_val ^ operand; break;
                case OP_A_AMOAND_D:  new_val = old_val & operand; break;
                case OP_A_AMOOR_D:   new_val = old_val | operand; break;
                case OP_A_AMOMIN_D:  new_val = (static_cast<int64_t>(old_val) < static_cast<int64_t>(operand)) ? old_val : operand; break;
                case OP_A_AMOMAX_D:  new_val = (static_cast<int64_t>(old_val) > static_cast<int64_t>(operand)) ? old_val : operand; break;
                case OP_A_AMOMINU_D: new_val = (old_val < operand) ? old_val : operand; break;
                case OP_A_AMOMAXU_D: new_val = (old_val > operand) ? old_val : operand; break;
                default: break;
            }
            this->mem_intf->writeDataMem64(mem_addr, new_val, 8);
            this->perf->dataMemoryWrite();
            this->regs->setValue(rd, static_cast<int64_t>(old_val));
            return true;
        }

        void TLB_reserve(std::uint32_t address) {
            TLB_A_Entries.insert(address);
        }

        bool TLB_reserved(std::uint32_t address) {
            if (TLB_A_Entries.count(address) == 1) {
                TLB_A_Entries.erase(address);
                return true;
            } else {
                return false;
            }
        }

        void TLB_reserve_64(std::uint64_t address) {
            TLB_A_Entries_64.insert(address);
        }

        bool TLB_reserved_64(std::uint64_t address) {
            if (TLB_A_Entries_64.count(address) == 1) {
                TLB_A_Entries_64.erase(address);
                return true;
            } else {
                return false;
            }
        }

        bool exec_instruction(Instruction &inst, op_A_Codes code) {
            bool PC_not_affected = true;

            this->setInstr(inst.getInstr());

            switch (code) {
                case OP_A_LR:
                    Exec_A_LR();
                    break;
                case OP_A_SC:
                    Exec_A_SC();
                    break;
                case OP_A_AMOSWAP:
                    Exec_A_AMOSWAP();
                    break;
                case OP_A_AMOADD:
                    Exec_A_AMOADD();
                    break;
                case OP_A_AMOXOR:
                    Exec_A_AMOXOR();
                    break;
                case OP_A_AMOAND:
                    Exec_A_AMOAND();
                    break;
                case OP_A_AMOOR:
                    Exec_A_AMOOR();
                    break;
                case OP_A_AMOMIN:
                    Exec_A_AMOMIN();
                    break;
                case OP_A_AMOMAX:
                    Exec_A_AMOMAX();
                    break;
                case OP_A_AMOMINU:
                    Exec_A_AMOMINU();
                    break;
                case OP_A_AMOMAXU:
                    Exec_A_AMOMAXU();
                    break;
                case OP_A_LR_D:
                    Exec_LR_D();
                    break;
                case OP_A_SC_D:
                    Exec_SC_D();
                    break;
                case OP_A_AMOSWAP_D:
                case OP_A_AMOADD_D:
                case OP_A_AMOXOR_D:
                case OP_A_AMOAND_D:
                case OP_A_AMOOR_D:
                case OP_A_AMOMIN_D:
                case OP_A_AMOMAX_D:
                case OP_A_AMOMINU_D:
                case OP_A_AMOMAXU_D:
                    Exec_AMO_D(code);
                    break;
                    default:
                    std::cout << "A instruction not implemented yet" << std::endl;
                    inst.dump();
                    this->NOP();
                    break;
            }

            return PC_not_affected;
        }

    private:
        std::unordered_set<std::uint32_t> TLB_A_Entries;
        std::unordered_set<std::uint64_t> TLB_A_Entries_64;
    };
}

#endif
