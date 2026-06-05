// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifndef F_EXTENSION_H
#define F_EXTENSION_H

#include <cmath>
#include <cfenv>
#include <cstring>
#include <cstdint>
#include "Registers.h"
#include "MemoryInterface.h"

namespace riscv_tlm {

typedef enum {
    OP_F_FLW, OP_F_FSW,
    OP_F_FMADD_S, OP_F_FMSUB_S, OP_F_FNMSUB_S, OP_F_FNMADD_S,
    OP_F_FADD_S, OP_F_FSUB_S, OP_F_FMUL_S, OP_F_FDIV_S, OP_F_FSQRT_S,
    OP_F_FSGNJ_S, OP_F_FSGNJN_S, OP_F_FSGNJX_S,
    OP_F_FMIN_S, OP_F_FMAX_S,
    OP_F_FCVT_W_S, OP_F_FCVT_WU_S, OP_F_FCVT_L_S, OP_F_FCVT_LU_S,
    OP_F_FCVT_S_W, OP_F_FCVT_S_WU, OP_F_FCVT_S_L, OP_F_FCVT_S_LU,
    OP_F_FMV_X_W, OP_F_FMV_W_X, OP_F_FCLASS_S,
    OP_F_FEQ_S, OP_F_FLT_S, OP_F_FLE_S,
    OP_F_ERROR
} op_F_Codes;

template<typename T>
class F_extension {
public:
    F_extension(uint32_t instr, Registers<T>* regs, MemoryInterface* mem)
        : m_instr(instr), regs(regs), mem_intf(mem) {}

    void setInstr(uint32_t instr) { m_instr = instr; }

    op_F_Codes decode() const {
        uint32_t opcode = m_instr & 0x7F;
        uint32_t funct3 = (m_instr >> 12) & 0x7;
        uint32_t funct7 = (m_instr >> 25) & 0x7F;
        uint32_t rs2    = (m_instr >> 20) & 0x1F;

        if (opcode == 0x07 && funct3 == 0x2) return OP_F_FLW;
        if (opcode == 0x27 && funct3 == 0x2) return OP_F_FSW;
        if (opcode == 0x43 && ((m_instr >> 25) & 0x3) == 0x0) return OP_F_FMADD_S;
        if (opcode == 0x47 && ((m_instr >> 25) & 0x3) == 0x0) return OP_F_FMSUB_S;
        if (opcode == 0x4B && ((m_instr >> 25) & 0x3) == 0x0) return OP_F_FNMSUB_S;
        if (opcode == 0x4F && ((m_instr >> 25) & 0x3) == 0x0) return OP_F_FNMADD_S;
        if (opcode == 0x53) {
            switch (funct7) {
                case 0x00: return OP_F_FADD_S;
                case 0x04: return OP_F_FSUB_S;
                case 0x08: return OP_F_FMUL_S;
                case 0x0C: return OP_F_FDIV_S;
                case 0x2C: return OP_F_FSQRT_S;
                case 0x10:
                    if (funct3 == 0) return OP_F_FSGNJ_S;
                    if (funct3 == 1) return OP_F_FSGNJN_S;
                    if (funct3 == 2) return OP_F_FSGNJX_S;
                    break;
                case 0x14:
                    return (funct3 == 0) ? OP_F_FMIN_S : OP_F_FMAX_S;
                case 0x60:
                    if (rs2 == 0) return OP_F_FCVT_W_S;
                    if (rs2 == 1) return OP_F_FCVT_WU_S;
                    if (rs2 == 2) return OP_F_FCVT_L_S;
                    if (rs2 == 3) return OP_F_FCVT_LU_S;
                    break;
                case 0x68:
                    if (rs2 == 0) return OP_F_FCVT_S_W;
                    if (rs2 == 1) return OP_F_FCVT_S_WU;
                    if (rs2 == 2) return OP_F_FCVT_S_L;
                    if (rs2 == 3) return OP_F_FCVT_S_LU;
                    break;
                case 0x70:
                    return (funct3 == 0) ? OP_F_FMV_X_W : OP_F_FCLASS_S;
                case 0x78: return OP_F_FMV_W_X;
                case 0x50:
                    if (funct3 == 2) return OP_F_FEQ_S;
                    if (funct3 == 1) return OP_F_FLT_S;
                    if (funct3 == 0) return OP_F_FLE_S;
                    break;
            }
        }
        return OP_F_ERROR;
    }

    // Execute returns the FPU result cycle latency (for pipeline stall)
    int execute(op_F_Codes op, uint64_t rs1_val, uint64_t rs2_val, uint64_t rs3_val,
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
            case OP_F_FLW: {
                uint32_t raw = mem_intf->readDataMem(mem_addr, 4);
                regs->setFPValue(rd, 0xFFFFFFFF00000000ULL | raw);
                latency = 2;
                break;
            }
            case OP_F_FSW: {
                uint32_t raw = rs2_val & 0xFFFFFFFF;
                mem_intf->writeDataMem(mem_addr, raw, 4);
                latency = 1;
                break;
            }
            case OP_F_FADD_S: { regs->setF32(rd, regs->getF32(get_rs1()) + regs->getF32(get_rs2())); latency = 3; break; }
            case OP_F_FSUB_S: { regs->setF32(rd, regs->getF32(get_rs1()) - regs->getF32(get_rs2())); latency = 3; break; }
            case OP_F_FMUL_S: { regs->setF32(rd, regs->getF32(get_rs1()) * regs->getF32(get_rs2())); latency = 3; break; }
            case OP_F_FDIV_S: { regs->setF32(rd, regs->getF32(get_rs1()) / regs->getF32(get_rs2())); latency = 12; break; }
            case OP_F_FSQRT_S: { regs->setF32(rd, std::sqrt(regs->getF32(get_rs1()))); latency = 14; break; }
            case OP_F_FMADD_S: { regs->setF32(rd, std::fma(regs->getF32(get_rs1()), regs->getF32(get_rs2()), regs->getF32(get_rs3()))); latency = 5; break; }
            case OP_F_FMSUB_S: { regs->setF32(rd, std::fma(regs->getF32(get_rs1()), regs->getF32(get_rs2()), -regs->getF32(get_rs3()))); latency = 5; break; }
            case OP_F_FNMSUB_S:{ regs->setF32(rd, -std::fma(regs->getF32(get_rs1()), regs->getF32(get_rs2()), -regs->getF32(get_rs3()))); latency = 5; break; }
            case OP_F_FNMADD_S:{ regs->setF32(rd, -std::fma(regs->getF32(get_rs1()), regs->getF32(get_rs2()), regs->getF32(get_rs3()))); latency = 5; break; }
            case OP_F_FSGNJ_S: { regs->setF32(rd, std::copysign(std::fabs(regs->getF32(get_rs1())),  regs->getF32(get_rs2()))); latency = 2; break; }
            case OP_F_FSGNJN_S:{ regs->setF32(rd, std::copysign(std::fabs(regs->getF32(get_rs1())), -regs->getF32(get_rs2()))); latency = 2; break; }
            case OP_F_FSGNJX_S:{ float a=regs->getF32(get_rs1()),b=regs->getF32(get_rs2()); regs->setF32(rd,std::copysign(a,a*b)); latency = 2; break; }
            case OP_F_FMIN_S:  { regs->setF32(rd, std::fmin(regs->getF32(get_rs1()), regs->getF32(get_rs2()))); latency = 2; break; }
            case OP_F_FMAX_S:  { regs->setF32(rd, std::fmax(regs->getF32(get_rs1()), regs->getF32(get_rs2()))); latency = 2; break; }
            case OP_F_FEQ_S: { regs->setValue(rd, regs->getF32(get_rs1()) == regs->getF32(get_rs2() ) ? 1 : 0); latency = 2; break; }
            case OP_F_FLT_S: { regs->setValue(rd, regs->getF32(get_rs1()) <  regs->getF32(get_rs2()) ? 1 : 0); latency = 2; break; }
            case OP_F_FLE_S: { regs->setValue(rd, regs->getF32(get_rs1()) <= regs->getF32(get_rs2()) ? 1 : 0); latency = 2; break; }
            case OP_F_FCVT_W_S: { regs->setValue(rd, static_cast<int64_t>(static_cast<int32_t>(regs->getF32(get_rs1())))); latency = 2; break; }
            case OP_F_FCVT_WU_S:{ regs->setValue(rd, static_cast<int64_t>(static_cast<uint32_t>(regs->getF32(get_rs1())))); latency = 2; break; }
            case OP_F_FCVT_L_S: { regs->setValue(rd, static_cast<int64_t>(regs->getF32(get_rs1()))); latency = 2; break; }
            case OP_F_FCVT_LU_S:{ regs->setValue(rd, static_cast<int64_t>(static_cast<uint64_t>(regs->getF32(get_rs1())))); latency = 2; break; }
            case OP_F_FCVT_S_W: { regs->setF32(rd, static_cast<float>(static_cast<int32_t>(rs1_val))); latency = 2; break; }
            case OP_F_FCVT_S_WU:{ regs->setF32(rd, static_cast<float>(static_cast<uint32_t>(rs1_val))); latency = 2; break; }
            case OP_F_FCVT_S_L: { regs->setF32(rd, static_cast<float>(static_cast<int64_t>(rs1_val))); latency = 2; break; }
            case OP_F_FCVT_S_LU:{ regs->setF32(rd, static_cast<float>(static_cast<uint64_t>(rs1_val))); latency = 2; break; }
            case OP_F_FMV_X_W: { regs->setValue(rd, static_cast<int64_t>(static_cast<int32_t>(regs->getFPValue(get_rs1()) & 0xFFFFFFFF))); latency = 1; break; }
            case OP_F_FMV_W_X: { regs->setFPValue(rd, 0xFFFFFFFF00000000ULL | (rs1_val & 0xFFFFFFFF)); latency = 1; break; }
            case OP_F_FCLASS_S: { regs->setValue(rd, fclass_s(regs->getF32(get_rs1()))); latency = 1; break; }
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

    uint32_t fclass_s(float f) const {
        uint32_t bits; std::memcpy(&bits, &f, 4);
        bool sign = bits >> 31;
        uint32_t exp = (bits >> 23) & 0xFF;
        uint32_t mant = bits & 0x7FFFFF;
        if (exp == 0xFF && mant)    return sign ? (1<<8) : (1<<9); // sNaN/qNaN
        if (exp == 0xFF && !mant)   return sign ? (1<<0) : (1<<7); // -inf/+inf
        if (exp == 0 && mant == 0)  return sign ? (1<<3) : (1<<4); // -0/+0
        if (exp == 0)               return sign ? (1<<2) : (1<<5); // subnormal
        return sign ? (1<<1) : (1<<6); // normal
    }
};

} // namespace riscv_tlm
#endif
