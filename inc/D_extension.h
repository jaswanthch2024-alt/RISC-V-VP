// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifndef D_EXTENSION_H
#define D_EXTENSION_H

#include <cmath>
#include <cfenv>
#include <cstring>
#include <cstdint>
#include "Registers.h"
#include "MemoryInterface.h"

namespace riscv_tlm {

typedef enum {
    OP_D_FLD, OP_D_FSD,
    OP_D_FMADD_D, OP_D_FMSUB_D, OP_D_FNMSUB_D, OP_D_FNMADD_D,
    OP_D_FADD_D, OP_D_FSUB_D, OP_D_FMUL_D, OP_D_FDIV_D, OP_D_FSQRT_D,
    OP_D_FSGNJ_D, OP_D_FSGNJN_D, OP_D_FSGNJX_D,
    OP_D_FMIN_D, OP_D_FMAX_D,
    OP_D_FCVT_W_D, OP_D_FCVT_WU_D, OP_D_FCVT_L_D, OP_D_FCVT_LU_D,
    OP_D_FCVT_D_W, OP_D_FCVT_D_WU, OP_D_FCVT_D_L, OP_D_FCVT_D_LU,
    OP_D_FCVT_S_D, OP_D_FCVT_D_S,
    OP_D_FMV_X_D, OP_D_FMV_D_X, OP_D_FCLASS_D,
    OP_D_FEQ_D, OP_D_FLT_D, OP_D_FLE_D,
    OP_D_ERROR
} op_D_Codes;

template<typename T>
class D_extension {
public:
    D_extension(uint32_t instr, Registers<T>* regs, MemoryInterface* mem)
        : m_instr(instr), regs(regs), mem_intf(mem) {}

    void setInstr(uint32_t instr) { m_instr = instr; }

    op_D_Codes decode() const {
        uint32_t opcode = m_instr & 0x7F;
        uint32_t funct3 = (m_instr >> 12) & 0x7;
        uint32_t funct7 = (m_instr >> 25) & 0x7F;
        uint32_t rs2    = (m_instr >> 20) & 0x1F;

        if (opcode == 0x07 && funct3 == 0x3) return OP_D_FLD;
        if (opcode == 0x27 && funct3 == 0x3) return OP_D_FSD;
        if (opcode == 0x43 && ((m_instr >> 25) & 0x3) == 0x1) return OP_D_FMADD_D;
        if (opcode == 0x47 && ((m_instr >> 25) & 0x3) == 0x1) return OP_D_FMSUB_D;
        if (opcode == 0x4B && ((m_instr >> 25) & 0x3) == 0x1) return OP_D_FNMSUB_D;
        if (opcode == 0x4F && ((m_instr >> 25) & 0x3) == 0x1) return OP_D_FNMADD_D;
        if (opcode == 0x53) {
            switch (funct7) {
                case 0x01: return OP_D_FADD_D;
                case 0x05: return OP_D_FSUB_D;
                case 0x09: return OP_D_FMUL_D;
                case 0x0D: return OP_D_FDIV_D;
                case 0x2D: return OP_D_FSQRT_D;
                case 0x11:
                    if (funct3 == 0) return OP_D_FSGNJ_D;
                    if (funct3 == 1) return OP_D_FSGNJN_D;
                    if (funct3 == 2) return OP_D_FSGNJX_D;
                    break;
                case 0x15:
                    return (funct3 == 0) ? OP_D_FMIN_D : OP_D_FMAX_D;
                case 0x20:
                    if (rs2 == 1) return OP_D_FCVT_S_D;
                    break;
                case 0x21:
                    if (rs2 == 0) return OP_D_FCVT_D_S;
                    break;
                case 0x61:
                    if (rs2 == 0) return OP_D_FCVT_W_D;
                    if (rs2 == 1) return OP_D_FCVT_WU_D;
                    if (rs2 == 2) return OP_D_FCVT_L_D;
                    if (rs2 == 3) return OP_D_FCVT_LU_D;
                    break;
                case 0x69:
                    if (rs2 == 0) return OP_D_FCVT_D_W;
                    if (rs2 == 1) return OP_D_FCVT_D_WU;
                    if (rs2 == 2) return OP_D_FCVT_D_L;
                    if (rs2 == 3) return OP_D_FCVT_D_LU;
                    break;
                case 0x71:
                    return (funct3 == 0) ? OP_D_FMV_X_D : OP_D_FCLASS_D;
                case 0x79: return OP_D_FMV_D_X;
                case 0x51:
                    if (funct3 == 2) return OP_D_FEQ_D;
                    if (funct3 == 1) return OP_D_FLT_D;
                    if (funct3 == 0) return OP_D_FLE_D;
                    break;
            }
        }
        return OP_D_ERROR;
    }

    // Execute returns the FPU result cycle latency (for pipeline stall)
    int execute(op_D_Codes op, uint64_t rs1_val, uint64_t rs2_val, uint64_t rs3_val,
                int rd, uint64_t mem_addr, uint32_t& fcsr_flags) {
        std::feclearexcept(FE_ALL_EXCEPT);

        // Rounding mode mapping
        uint32_t frm = (regs->getCSR(0x003) >> 5) & 0x7;
        switch (frm) {
            case 0: std::fesetround(FE_TONEAREST); break;
            case 1: std::fesetround(FE_TOWARDZERO); break;
            case 2: std::fesetround(FE_DOWNWARD); break;
            case 3: std::fesetround(FE_UPWARD); break;
            default: std::fesetround(FE_TONEAREST); break;
        }

        int latency = 2; // Default CVA6 FP latency

        switch (op) {
            case OP_D_FLD: {
                uint64_t raw = mem_intf->readDataMem64(mem_addr, 8);
                regs->setFPValue(rd, raw);
                latency = 2;
                break;
            }
            case OP_D_FSD: {
                uint64_t raw = rs2_val;
                mem_intf->writeDataMem64(mem_addr, raw, 8);
                latency = 1;
                break;
            }
            case OP_D_FADD_D: { regs->setF64(rd, regs->getF64(get_rs1()) + regs->getF64(get_rs2())); latency = 4; break; }
            case OP_D_FSUB_D: { regs->setF64(rd, regs->getF64(get_rs1()) - regs->getF64(get_rs2())); latency = 4; break; }
            case OP_D_FMUL_D: { regs->setF64(rd, regs->getF64(get_rs1()) * regs->getF64(get_rs2())); latency = 4; break; }
            case OP_D_FDIV_D: { regs->setF64(rd, regs->getF64(get_rs1()) / regs->getF64(get_rs2())); latency = 20; break; }
            case OP_D_FSQRT_D: { regs->setF64(rd, std::sqrt(regs->getF64(get_rs1()))); latency = 22; break; }
            case OP_D_FMADD_D: { regs->setF64(rd, std::fma(regs->getF64(get_rs1()), regs->getF64(get_rs2()), regs->getF64(get_rs3()))); latency = 5; break; }
            case OP_D_FMSUB_D: { regs->setF64(rd, std::fma(regs->getF64(get_rs1()), regs->getF64(get_rs2()), -regs->getF64(get_rs3()))); latency = 5; break; }
            case OP_D_FNMSUB_D:{ regs->setF64(rd, -std::fma(regs->getF64(get_rs1()), regs->getF64(get_rs2()), -regs->getF64(get_rs3()))); latency = 5; break; }
            case OP_D_FNMADD_D:{ regs->setF64(rd, -std::fma(regs->getF64(get_rs1()), regs->getF64(get_rs2()), regs->getF64(get_rs3()))); latency = 5; break; }
            case OP_D_FSGNJ_D: { regs->setF64(rd, std::copysign(std::fabs(regs->getF64(get_rs1())),  regs->getF64(get_rs2()))); latency = 2; break; }
            case OP_D_FSGNJN_D:{ regs->setF64(rd, std::copysign(std::fabs(regs->getF64(get_rs1())), -regs->getF64(get_rs2()))); latency = 2; break; }
            case OP_D_FSGNJX_D:{ double a=regs->getF64(get_rs1()),b=regs->getF64(get_rs2()); regs->setF64(rd,std::copysign(a,a*b)); latency = 2; break; }
            case OP_D_FMIN_D:  { regs->setF64(rd, std::fmin(regs->getF64(get_rs1()), regs->getF64(get_rs2()))); latency = 2; break; }
            case OP_D_FMAX_D:  { regs->setF64(rd, std::fmax(regs->getF64(get_rs1()), regs->getF64(get_rs2()))); latency = 2; break; }
            case OP_D_FEQ_D: { regs->setValue(rd, regs->getF64(get_rs1()) == regs->getF64(get_rs2() ) ? 1 : 0); latency = 2; break; }
            case OP_D_FLT_D: { regs->setValue(rd, regs->getF64(get_rs1()) <  regs->getF64(get_rs2()) ? 1 : 0); latency = 2; break; }
            case OP_D_FLE_D: { regs->setValue(rd, regs->getF64(get_rs1()) <= regs->getF64(get_rs2()) ? 1 : 0); latency = 2; break; }
            case OP_D_FCVT_W_D: { regs->setValue(rd, static_cast<int64_t>(static_cast<int32_t>(regs->getF64(get_rs1())))); latency = 2; break; }
            case OP_D_FCVT_WU_D:{ regs->setValue(rd, static_cast<int64_t>(static_cast<uint32_t>(regs->getF64(get_rs1())))); latency = 2; break; }
            case OP_D_FCVT_L_D: { regs->setValue(rd, static_cast<int64_t>(regs->getF64(get_rs1()))); latency = 2; break; }
            case OP_D_FCVT_LU_D:{ regs->setValue(rd, static_cast<int64_t>(static_cast<uint64_t>(regs->getF64(get_rs1())))); latency = 2; break; }
            case OP_D_FCVT_D_W: { regs->setF64(rd, static_cast<double>(static_cast<int32_t>(rs1_val))); latency = 2; break; }
            case OP_D_FCVT_D_WU:{ regs->setF64(rd, static_cast<double>(static_cast<uint32_t>(rs1_val))); latency = 2; break; }
            case OP_D_FCVT_D_L: { regs->setF64(rd, static_cast<double>(static_cast<int64_t>(rs1_val))); latency = 2; break; }
            case OP_D_FCVT_D_LU:{ regs->setF64(rd, static_cast<double>(static_cast<uint64_t>(rs1_val))); latency = 2; break; }
            case OP_D_FCVT_S_D: { regs->setF32(rd, static_cast<float>(regs->getF64(get_rs1()))); latency = 2; break; }
            case OP_D_FCVT_D_S: { regs->setF64(rd, static_cast<double>(regs->getF32(get_rs1()))); latency = 2; break; }
            case OP_D_FMV_X_D: { regs->setValue(rd, static_cast<int64_t>(regs->getFPValue(get_rs1()))); latency = 1; break; }
            case OP_D_FMV_D_X: { regs->setFPValue(rd, rs1_val); latency = 1; break; }
            case OP_D_FCLASS_D: { regs->setValue(rd, fclass_d(regs->getF64(get_rs1()))); latency = 1; break; }
            default: latency = 0; break;
        }

        // Propagate exceptions
        int fe = std::fetestexcept(FE_ALL_EXCEPT);
        fcsr_flags = 0;
        if (fe & FE_INEXACT)   fcsr_flags |= 0x01;
        if (fe & FE_UNDERFLOW) fcsr_flags |= 0x02;
        if (fe & FE_OVERFLOW)  fcsr_flags |= 0x04;
        if (fe & FE_DIVBYZERO) fcsr_flags |= 0x08;
        if (fe & FE_INVALID)   fcsr_flags |= 0x10;
        return latency;
    }

private:
    uint32_t       m_instr;
    Registers<T>*  regs;
    MemoryInterface* mem_intf;

    uint32_t get_rd()  const { return (m_instr >> 7)  & 0x1F; }
    uint32_t get_rs1() const { return (m_instr >> 15) & 0x1F; }
    uint32_t get_rs2() const { return (m_instr >> 20) & 0x1F; }
    uint32_t get_rs3() const { return (m_instr >> 27) & 0x1F; }

    uint32_t fclass_d(double d) const {
        uint64_t bits; std::memcpy(&bits, &d, 8);
        bool sign = bits >> 63;
        uint32_t exp = (bits >> 52) & 0x7FF;
        uint64_t mant = bits & 0xFFFFFFFFFFFFFULL;
        if (exp == 0x7FF && mant)    return sign ? (1<<8) : (1<<9); // sNaN/qNaN
        if (exp == 0x7FF && !mant)   return sign ? (1<<0) : (1<<7); // -inf/+inf
        if (exp == 0 && mant == 0)  return sign ? (1<<3) : (1<<4); // -0/+0
        if (exp == 0)               return sign ? (1<<2) : (1<<5); // subnormal
        return sign ? (1<<1) : (1<<6); // normal
    }
};

} // namespace riscv_tlm
#endif
