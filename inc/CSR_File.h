// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifndef CSR_FILE_H
#define CSR_FILE_H

#include <cstdint>
#include <array>

namespace riscv_tlm {

// Privilege levels (RISC-V Privilege Spec 1.10)
enum class PrivMode : uint8_t { U = 0, S = 1, M = 3 };

// CSR addresses (Priv Spec 1.10 — subset needed for M+S+U Linux)
namespace CSR {
    // --- Machine Information ---
    static constexpr uint16_t MVENDORID  = 0xF11;
    static constexpr uint16_t MARCHID    = 0xF12;
    static constexpr uint16_t MIMPID     = 0xF13;
    static constexpr uint16_t MHARTID    = 0xF14;
    // --- Machine Trap Setup ---
    static constexpr uint16_t MSTATUS    = 0x300;
    static constexpr uint16_t MISA       = 0x301;
    static constexpr uint16_t MEDELEG    = 0x302;
    static constexpr uint16_t MIDELEG    = 0x303;
    static constexpr uint16_t MIE        = 0x304;
    static constexpr uint16_t MTVEC      = 0x305;
    static constexpr uint16_t MCOUNTEREN = 0x306;
    // --- Machine Trap Handling ---
    static constexpr uint16_t MSCRATCH   = 0x340;
    static constexpr uint16_t MEPC       = 0x341;
    static constexpr uint16_t MCAUSE     = 0x342;
    static constexpr uint16_t MTVAL      = 0x343;
    static constexpr uint16_t MIP        = 0x344;
    // --- Machine Counters ---
    static constexpr uint16_t MCYCLE     = 0xB00;
    static constexpr uint16_t MINSTRET   = 0xB02;
    static constexpr uint16_t MCYCLEH    = 0xB80;  // RV32 upper half
    static constexpr uint16_t MINSTRETH  = 0xB82;
    // --- Supervisor Trap Setup ---
    static constexpr uint16_t SSTATUS    = 0x100;
    static constexpr uint16_t SIE        = 0x104;
    static constexpr uint16_t STVEC      = 0x105;
    static constexpr uint16_t SCOUNTEREN = 0x106;
    // --- Supervisor Trap Handling ---
    static constexpr uint16_t SSCRATCH   = 0x140;
    static constexpr uint16_t SEPC       = 0x141;
    static constexpr uint16_t SCAUSE     = 0x142;
    static constexpr uint16_t STVAL      = 0x143;
    static constexpr uint16_t SIP        = 0x144;
    // --- Supervisor Address Translation ---
    static constexpr uint16_t SATP       = 0x180;
    // --- User Counters (read-only mirrors) ---
    static constexpr uint16_t CYCLE      = 0xC00;
    static constexpr uint16_t TIME       = 0xC01;
    static constexpr uint16_t INSTRET    = 0xC02;
}

// mstatus bit positions
namespace MSTATUS {
    static constexpr uint64_t UIE  = (1ULL << 0);
    static constexpr uint64_t SIE  = (1ULL << 1);
    static constexpr uint64_t MIE  = (1ULL << 3);
    static constexpr uint64_t UPIE = (1ULL << 4);
    static constexpr uint64_t SPIE = (1ULL << 5);
    static constexpr uint64_t MPIE = (1ULL << 7);
    static constexpr uint64_t SPP  = (1ULL << 8);   // 1-bit S previous privilege
    static constexpr uint64_t MPP  = (3ULL << 11);  // 2-bit M previous privilege
    static constexpr uint64_t FS   = (3ULL << 13);
    static constexpr uint64_t XS   = (3ULL << 15);
    static constexpr uint64_t MPRV = (1ULL << 17);
    static constexpr uint64_t SUM  = (1ULL << 18);
    static constexpr uint64_t MXR  = (1ULL << 19);
    static constexpr uint64_t TVM  = (1ULL << 20);
    static constexpr uint64_t TW   = (1ULL << 21);
    static constexpr uint64_t TSR  = (1ULL << 22);
    static constexpr uint64_t UXL  = (3ULL << 32); // bits 33:32 — U-mode XLEN (read-only, =2 for RV64)
    static constexpr uint64_t SXL  = (3ULL << 34); // bits 35:34 — S-mode XLEN (read-only, =2 for RV64)
    static constexpr uint64_t SD   = (1ULL << 63);
    // Writable mask for mstatus (RV64) — UXL/SXL are read-only, not included
    static constexpr uint64_t WRITE_MASK =
        SIE | MIE | SPIE | MPIE | SPP | MPP | FS | MPRV | SUM | MXR | TVM | TW | TSR;
}

// Exception cause codes (mcause / scause)
namespace CAUSE {
    static constexpr uint64_t INSTR_MISALIGN   = 0;
    static constexpr uint64_t INSTR_ACCESS     = 1;
    static constexpr uint64_t ILLEGAL_INSTR    = 2;
    static constexpr uint64_t BREAKPOINT       = 3;
    static constexpr uint64_t LOAD_MISALIGN    = 4;
    static constexpr uint64_t LOAD_ACCESS      = 5;
    static constexpr uint64_t STORE_MISALIGN   = 6;
    static constexpr uint64_t STORE_ACCESS     = 7;
    static constexpr uint64_t ECALL_U          = 8;
    static constexpr uint64_t ECALL_S          = 9;
    static constexpr uint64_t ECALL_M          = 11;
    static constexpr uint64_t INSTR_PAGE_FAULT = 12;
    static constexpr uint64_t LOAD_PAGE_FAULT  = 13;
    static constexpr uint64_t STORE_PAGE_FAULT = 15;
    // Interrupt flag
    static constexpr uint64_t INTERRUPT        = (1ULL << 63);
}

// Minimal CSR file for RV64 M+S+U (Linux-capable).
// Implements the CSR read/write semantics from Privileged Spec 1.10.
class CSR_File {
public:
    CSR_File() {
        // misa: RV64 IMACFD (bit 62:61 = 10 for MXL=64; A=0,C=2,D=3,F=5,I=8,M=12 set)
        misa = (2ULL << 62) | (1ULL << 0)   // A
                            | (1ULL << 2)   // C
                            | (1ULL << 3)   // D (double-precision FP)
                            | (1ULL << 5)   // F (single-precision FP)
                            | (1ULL << 8)   // I
                            | (1ULL << 12)  // M
                            | (1ULL << 18)  // S (supervisor mode)
                            | (1ULL << 20); // U (user mode)
    }

    PrivMode priv{PrivMode::M}; // Current privilege mode

    // Read a CSR — returns value and sets access_ok=false on illegal access.
    uint64_t read(uint16_t addr, bool& access_ok) {
        access_ok = true;
        // Privilege check: CSR addr[9:8] = minimum privilege required
        uint8_t req_priv = (addr >> 8) & 0x3;
        if (static_cast<uint8_t>(priv) < req_priv) {
            access_ok = false;
            return 0;
        }
        switch (addr) {
            // Machine info (read-only)
            case CSR::MVENDORID:  return 0;
            case CSR::MARCHID:    return 0;
            case CSR::MIMPID:     return 0;
            case CSR::MHARTID:    return 0;
            // Machine trap setup
            case CSR::MSTATUS:    return mstatus;
            case CSR::MISA:       return misa;
            case CSR::MEDELEG:    return medeleg;
            case CSR::MIDELEG:    return mideleg;
            case CSR::MIE:        return mie;
            case CSR::MTVEC:      return mtvec;
            case CSR::MCOUNTEREN: return mcounteren;
            case 0x30A: return menvcfg;           // menvcfg
            case 0x320: return 0;                 // mcountinhibit (read-as-zero)
            // mhpmevent3-31 (0x323-0x33F)
            case 0x323: case 0x324: case 0x325: case 0x326: case 0x327:
            case 0x328: case 0x329: case 0x32A: case 0x32B: case 0x32C:
            case 0x32D: case 0x32E: case 0x32F: case 0x330: case 0x331:
            case 0x332: case 0x333: case 0x334: case 0x335: case 0x336:
            case 0x337: case 0x338: case 0x339: case 0x33A: case 0x33B:
            case 0x33C: case 0x33D: case 0x33E: case 0x33F: return 0;
            case 0x747: return 0;                 // mseccfg (read-as-zero)
            // Machine trap handling
            case CSR::MSCRATCH:   return mscratch;
            case CSR::MEPC:       return mepc & ~1ULL; // PC always aligned
            case CSR::MCAUSE:     return mcause;
            case CSR::MTVAL:      return mtval;
            case CSR::MIP:        return mip;
            // Machine counters
            case CSR::MCYCLE:     return mcycle;
            case CSR::MINSTRET:   return minstret;
            // mhpmcounter3-31 (0xB03-0xB1F) — read-as-zero
            case 0xB03: case 0xB04: case 0xB05: case 0xB06: case 0xB07:
            case 0xB08: case 0xB09: case 0xB0A: case 0xB0B: case 0xB0C:
            case 0xB0D: case 0xB0E: case 0xB0F: case 0xB10: case 0xB11:
            case 0xB12: case 0xB13: case 0xB14: case 0xB15: case 0xB16:
            case 0xB17: case 0xB18: case 0xB19: case 0xB1A: case 0xB1B:
            case 0xB1C: case 0xB1D: case 0xB1E: case 0xB1F: return 0;
            // Supervisor trap setup (restricted mstatus view)
            case CSR::SSTATUS:    return mstatus & SSTATUS_MASK;
            case CSR::SIE:        return mie  & mideleg;
            case CSR::STVEC:      return stvec;
            case CSR::SCOUNTEREN: return scounteren;
            // Supervisor trap handling
            case CSR::SSCRATCH:   return sscratch;
            case CSR::SEPC:       return sepc & ~1ULL;
            case CSR::SCAUSE:     return scause;
            case CSR::STVAL:      return stval;
            case CSR::SIP:        return mip & mideleg;
            // Supervisor address translation
            case CSR::SATP:       return satp;
            // User counters (read-only mirrors)
            case CSR::CYCLE:
            case CSR::TIME:       return mcycle;
            case CSR::INSTRET:    return minstret;
            // PMP CSRs
            case 0x3A0: return pmpcfg[0];
            case 0x3A1: return pmpcfg[1];
            case 0x3A2: return pmpcfg[2];
            case 0x3A3: return pmpcfg[3];
            case 0x3B0: case 0x3B1: case 0x3B2: case 0x3B3:
            case 0x3B4: case 0x3B5: case 0x3B6: case 0x3B7:
            case 0x3B8: case 0x3B9: case 0x3BA: case 0x3BB:
            case 0x3BC: case 0x3BD: case 0x3BE: case 0x3BF:
                return pmpaddr[addr - 0x3B0];
            // Floating-point CSRs
            case 0x001: return fcsr & 0x1F;        // FFLAGS
            case 0x002: return (fcsr >> 5) & 0x7;  // FRM
            case 0x003: return fcsr & 0xFF;        // FCSR
            default:
                // Unknown CSR — illegal instruction
                access_ok = false;
                return 0;
        }
    }

    // Write a CSR. Returns false on illegal access.
    bool write(uint16_t addr, uint64_t val) {
        // Privilege check
        uint8_t req_priv = (addr >> 8) & 0x3;
        if (static_cast<uint8_t>(priv) < req_priv) return false;
        // Read-only CSRs: addr[11:10] == 11
        if ((addr >> 10) == 0x3) return false;

        switch (addr) {
            case CSR::MSTATUS:    mstatus = (mstatus & ~MSTATUS::WRITE_MASK) | (val & MSTATUS::WRITE_MASK); break;
            case CSR::MISA:       break; // Writes to misa ignored (extensions fixed)
            case CSR::MEDELEG:    medeleg = val & MEDELEG_MASK; break;
            case CSR::MIDELEG:    mideleg = val & MIDELEG_MASK; break;
            case CSR::MIE:        mie     = val & MIE_MASK;     break;
            case CSR::MTVEC:      mtvec   = val & ~2ULL;        break; // mode bits [1:0]: 0=direct,1=vectored
            case CSR::MCOUNTEREN: mcounteren = val & 0xFFFFFFFF; break;
            case 0x30A: menvcfg = val; break;     // menvcfg
            case 0x320: break;                    // mcountinhibit — silently ignore
            // mhpmevent3-31
            case 0x323: case 0x324: case 0x325: case 0x326: case 0x327:
            case 0x328: case 0x329: case 0x32A: case 0x32B: case 0x32C:
            case 0x32D: case 0x32E: case 0x32F: case 0x330: case 0x331:
            case 0x332: case 0x333: case 0x334: case 0x335: case 0x336:
            case 0x337: case 0x338: case 0x339: case 0x33A: case 0x33B:
            case 0x33C: case 0x33D: case 0x33E: case 0x33F: break;
            // mhpmcounter3-31 — silently ignore
            case 0xB03: case 0xB04: case 0xB05: case 0xB06: case 0xB07:
            case 0xB08: case 0xB09: case 0xB0A: case 0xB0B: case 0xB0C:
            case 0xB0D: case 0xB0E: case 0xB0F: case 0xB10: case 0xB11:
            case 0xB12: case 0xB13: case 0xB14: case 0xB15: case 0xB16:
            case 0xB17: case 0xB18: case 0xB19: case 0xB1A: case 0xB1B:
            case 0xB1C: case 0xB1D: case 0xB1E: case 0xB1F: break;
            case 0x747: break;                    // mseccfg — silently ignore
            case CSR::MSCRATCH:   mscratch = val; break;
            case CSR::MEPC:       mepc  = val & ~1ULL; break;
            case CSR::MCAUSE:     mcause = val; break;
            case CSR::MTVAL:      mtval  = val; break;
            case CSR::MIP: {
                uint64_t old_mip = mip;
                mip = (mip & ~MIP_SW_MASK) | (val & MIP_SW_MASK);
                (void)old_mip;
                break;
            }
            case CSR::SSTATUS:    mstatus = (mstatus & ~SSTATUS_MASK) | (val & SSTATUS_MASK); break;
            case CSR::SIE:        mie     = (mie & ~mideleg) | (val & mideleg); break;
            case CSR::STVEC:      stvec   = val & ~2ULL; break;
            case CSR::SCOUNTEREN: scounteren = val & 0x7; break;
            case CSR::SSCRATCH:   sscratch = val; break;
            case CSR::SEPC:       sepc   = val & ~1ULL; break;
            case CSR::SCAUSE:     scause = val; break;
            case CSR::STVAL:      stval  = val; break;
            case CSR::SIP:        mip    = (mip & ~(mideleg & MIP_SW_MASK)) | (val & mideleg & MIP_SW_MASK); break;
            case CSR::SATP: {
                // Only accept mode=0 (Bare) and mode=8 (Sv39).
                // Per RISC-V spec: unsupported mode writes are silently discarded.
                uint64_t mode = val >> 60;
                if (mode == 0 || mode == 8) satp = val;
                // else: discard (kernel probe sees old value → falls back to supported mode)
                break;
            }
            case CSR::MCYCLE:     mcycle   = val; break;
            case CSR::MINSTRET:   minstret = val; break;
            // PMP CSRs
            case 0x3A0: pmpcfg[0] = val; break;
            case 0x3A1: pmpcfg[1] = val; break;
            case 0x3A2: pmpcfg[2] = val; break;
            case 0x3A3: pmpcfg[3] = val; break;
            case 0x3B0: case 0x3B1: case 0x3B2: case 0x3B3:
            case 0x3B4: case 0x3B5: case 0x3B6: case 0x3B7:
            case 0x3B8: case 0x3B9: case 0x3BA: case 0x3BB:
            case 0x3BC: case 0x3BD: case 0x3BE: case 0x3BF:
                pmpaddr[addr - 0x3B0] = val; break;
            // Floating-point CSRs
            case 0x001: fcsr = (fcsr & ~0x1FULL) | (val & 0x1F); break; // FFLAGS
            case 0x002: fcsr = (fcsr & ~0xE0ULL) | ((val & 0x7) << 5); break; // FRM
            case 0x003: fcsr = val & 0xFF; break; // FCSR
            default: return false;
        }
        return true;
    }

    // Tick machine counters — called once per cycle.
    void tick_counters(uint32_t num_retired) {
        mcycle++;
        minstret += num_retired;
    }

    // Take a synchronous exception (trap to M or S mode depending on delegation).
    // Returns the trap vector address.
    uint64_t take_exception(uint64_t cause, uint64_t epc, uint64_t tval) {
        bool delegated = (cause < 64) && ((medeleg >> cause) & 1);
        if (delegated && priv != PrivMode::M) {
            // Trap to S-mode
            scause = cause;
            sepc   = epc;
            stval  = tval;
            uint64_t spp_bit = (priv == PrivMode::S) ? MSTATUS::SPP : 0;
            // Save SIE → SPIE, clear SIE, set SPP
            if (mstatus & MSTATUS::SIE) mstatus |=  MSTATUS::SPIE; else mstatus &= ~MSTATUS::SPIE;
            mstatus &= ~MSTATUS::SIE;
            mstatus  = (mstatus & ~MSTATUS::SPP) | spp_bit;
            priv = PrivMode::S;
            return trap_vector(stvec, cause);
        } else {
            // Trap to M-mode
            mcause = cause;
            mepc   = epc;
            mtval  = tval;
            uint64_t mpp = static_cast<uint64_t>(priv) << 11;
            // Save MIE → MPIE, clear MIE, set MPP
            if (mstatus & MSTATUS::MIE) mstatus |=  MSTATUS::MPIE; else mstatus &= ~MSTATUS::MPIE;
            mstatus &= ~MSTATUS::MIE;
            mstatus  = (mstatus & ~MSTATUS::MPP) | mpp;
            priv = PrivMode::M;
            return trap_vector(mtvec, cause);
        }
    }

    // Take an asynchronous interrupt. cause is the interrupt code (0-15, no bit 63 set).
    // Returns the trap vector address to redirect PC to.
    uint64_t take_interrupt(uint64_t cause, uint64_t epc) {
        const uint64_t int_cause = cause | (1ULL << 63);
        bool delegated = ((mideleg >> cause) & 1) && (priv != PrivMode::M);
        if (delegated) {
            scause = int_cause;
            sepc   = epc;
            stval  = 0;
            if (mstatus & MSTATUS::SIE) mstatus |=  MSTATUS::SPIE; else mstatus &= ~MSTATUS::SPIE;
            mstatus &= ~MSTATUS::SIE;
            mstatus  = (mstatus & ~MSTATUS::SPP) | ((priv == PrivMode::S) ? MSTATUS::SPP : 0ULL);
            priv = PrivMode::S;
            return trap_vector(stvec, int_cause);
        } else {
            mcause = int_cause;
            mepc   = epc;
            mtval  = 0;
            uint64_t mpp = static_cast<uint64_t>(priv) << 11;
            if (mstatus & MSTATUS::MIE) mstatus |=  MSTATUS::MPIE; else mstatus &= ~MSTATUS::MPIE;
            mstatus &= ~MSTATUS::MIE;
            mstatus  = (mstatus & ~MSTATUS::MPP) | mpp;
            priv = PrivMode::M;
            return trap_vector(mtvec, int_cause);
        }
    }

    // Check if any interrupt is pending and enabled for the current privilege level.
    // Returns interrupt cause code (0-15) if one is pending, or -1 if none.
    // Priority: MEI(11) > MSI(3) > MTI(7) > SEI(9) > SSI(1) > STI(5)
    int pending_interrupt() const {
        uint64_t pending = mip & mie;
        if (pending == 0) return -1;

        bool m_enabled = (priv != PrivMode::M) || ((mstatus & MSTATUS::MIE) != 0);
        // S-mode interrupts only fire in U-mode, or in S-mode with SIE=1.
        // Per RISC-V spec: higher-privilege code (M-mode) is NEVER interrupted by S-mode signals.
        bool s_enabled = (priv == PrivMode::U) ||
                         (priv == PrivMode::S && (mstatus & MSTATUS::SIE) != 0);

        // M-level interrupt: enabled if m_enabled AND not delegated to S
        auto check_m = [&](int bit) -> bool {
            return m_enabled && ((pending >> bit) & 1) && !((mideleg >> bit) & 1);
        };
        // S-level interrupt: enabled if s_enabled AND delegated to S
        auto check_s = [&](int bit) -> bool {
            return s_enabled && ((pending >> bit) & 1) && ((mideleg >> bit) & 1);
        };

        if (check_m(11)) return 11; // MEIP
        if (check_m(3))  return 3;  // MSIP
        if (check_m(7))  return 7;  // MTIP
        if (check_s(9))  return 9;  // SEIP
        if (check_s(1))  return 1;  // SSIP
        if (check_s(5))  return 5;  // STIP
        return -1;
    }

    void set_fflags(uint32_t flags) { fcsr |= (flags & 0x1F); }

    bool pmp_check(uint64_t paddr, int access_size, bool is_write, bool is_exec) const {
        bool any_active = false;
        for (int i = 0; i < 16; i++) {
            uint8_t cfg = (i < 8) ? ((pmpcfg[0] >> (i * 8)) & 0xFF)
                                  : ((pmpcfg[2] >> ((i - 8) * 8)) & 0xFF);
            uint8_t a = (cfg >> 3) & 0x3;
            if (a != 0) {
                any_active = true;
            }
        }
        if (!any_active) {
            return true; // If no PMP entries are active, permit all accesses
        }

        for (int i = 0; i < 16; i++) {
            uint8_t cfg = (i < 8) ? ((pmpcfg[0] >> (i * 8)) & 0xFF)
                                  : ((pmpcfg[2] >> ((i - 8) * 8)) & 0xFF);
            uint8_t a = (cfg >> 3) & 0x3;
            if (a == 0) continue; // Off

            uint64_t base = 0;
            uint64_t top = 0;

            if (a == 1) { // TOR
                base = (i > 0) ? (pmpaddr[i-1] << 2) : 0;
                top  = pmpaddr[i] << 2;
            } else if (a == 2) { // NA4
                base = pmpaddr[i] << 2;
                top  = base + 4;
            } else if (a == 3) { // NAPOT
                uint64_t addr_val = pmpaddr[i];
                int log2sz = 3;
                while ((addr_val & 1) == 1 && log2sz < 64) {
                    log2sz++;
                    addr_val >>= 1;
                }
                if (log2sz >= 64) {
                    // All-ones pmpaddr = cover entire address space
                    base = 0;
                    top  = UINT64_MAX;
                } else {
                    uint64_t size = 1ULL << log2sz;
                    uint64_t mask = ~(size - 1);
                    base = (pmpaddr[i] << 2) & mask;
                    top  = base + size;
                }
            }

            // Check if there is any overlap (guard against top=UINT64_MAX wrap)
            if (paddr < top && (top == UINT64_MAX || (paddr + access_size) > base) && paddr >= base) {
                bool locked = (cfg >> 7) & 1;
                if (priv == PrivMode::M && !locked) {
                    return true; // M-mode bypass when not locked
                }
                bool r = (cfg >> 0) & 1;
                bool w = (cfg >> 1) & 1;
                bool x = (cfg >> 2) & 1;

                if (is_exec)  return x;
                if (is_write) return w;
                return r;
            }
        }

        // Default: M-mode succeeds, S/U mode fails when PMP is active
        return (priv == PrivMode::M);
    }

    // MRET: return from M-mode trap.
    uint64_t mret() {
        uint64_t ret_pc = mepc;
        // Restore MIE from MPIE, set MPIE, restore priv from MPP
        if (mstatus & MSTATUS::MPIE) mstatus |= MSTATUS::MIE; else mstatus &= ~MSTATUS::MIE;
        mstatus |= MSTATUS::MPIE;
        PrivMode new_priv = static_cast<PrivMode>((mstatus & MSTATUS::MPP) >> 11);
        mstatus &= ~MSTATUS::MPP; // Set MPP to U
        priv = new_priv;
        return ret_pc;
    }

    // SRET: return from S-mode trap.
    uint64_t sret() {
        uint64_t ret_pc = sepc;
        if (mstatus & MSTATUS::SPIE) mstatus |= MSTATUS::SIE; else mstatus &= ~MSTATUS::SIE;
        mstatus |= MSTATUS::SPIE;
        priv = (mstatus & MSTATUS::SPP) ? PrivMode::S : PrivMode::U;
        mstatus &= ~MSTATUS::SPP;
        return ret_pc;
    }

    // Public register state (accessed directly by pipeline for performance).
    // UXL=2 (RV64 U-mode), SXL=2 (RV64 S-mode) hardwired at reset per Privileged Spec §3.1.6
    uint64_t mstatus{(2ULL << 32) | (2ULL << 34)};
    uint64_t misa{0};
    uint64_t medeleg{0};
    uint64_t mideleg{0};
    uint64_t mie{0};
    uint64_t mip{0};
    uint64_t mtvec{0};
    uint64_t mcounteren{0};
    uint64_t menvcfg{0};
    uint64_t mscratch{0};
    uint64_t mepc{0};
    uint64_t mcause{0};
    uint64_t mtval{0};

    uint64_t stvec{0};
    uint64_t scounteren{0};
    uint64_t sscratch{0};
    uint64_t sepc{0};
    uint64_t scause{0};
    uint64_t stval{0};
    uint64_t satp{0};

    uint64_t mcycle{0};
    uint64_t minstret{0};
    uint64_t fcsr{0};


    std::array<uint64_t, 4> pmpcfg{};   // 0x3A0-0x3A3
    std::array<uint64_t, 16> pmpaddr{}; // 0x3B0-0x3BF

private:
    // sstatus is a restricted view of mstatus
    static constexpr uint64_t SSTATUS_MASK =
        MSTATUS::UIE | MSTATUS::SIE | MSTATUS::UPIE | MSTATUS::SPIE |
        MSTATUS::SPP | MSTATUS::FS  | MSTATUS::XS   | MSTATUS::SUM  |
        MSTATUS::MXR | MSTATUS::SD  | MSTATUS::UXL;  // UXL visible in sstatus per Privileged Spec §4.1.1

    static constexpr uint64_t MEDELEG_MASK = 0xB3FF; // delegatable exception bits
    static constexpr uint64_t MIDELEG_MASK = 0x0222; // delegatable interrupt bits (SSIP,STIP,SEIP)
    static constexpr uint64_t MIE_MASK     = 0x0AAA; // writable interrupt enable bits
    static constexpr uint64_t MIP_SW_MASK  = 0x0222; // software-writable MIP bits (SSIP, STIP, SEIP)

    static uint64_t trap_vector(uint64_t tvec, uint64_t cause) {
        uint64_t mode = tvec & 0x3;
        uint64_t base = tvec & ~0x3ULL;
        // Vectored mode (mode=1): base + 4*cause for interrupts, base for exceptions
        if (mode == 1 && (cause >> 63))
            return base + 4 * (cause & 0x7FF);
        return base;
    }
};

} // namespace riscv_tlm

#endif // CSR_FILE_H
