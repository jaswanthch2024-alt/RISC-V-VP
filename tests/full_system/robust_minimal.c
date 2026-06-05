// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal RISC-V test - just basic computation and trace output
// Build: riscv32-unknown-elf-gcc -march=rv32imac -mabi=ilp32 -O2 -nostdlib 
//        -Wl,--entry=main -Wl,--gc-sections robust_minimal.c -o robust_minimal.elf

// Inline types
typedef unsigned char      uint8_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

#define TRACE_ADDR 0x40000000u
#define TRACE (*(volatile uint8_t*)TRACE_ADDR)

static inline void putch(char c) { TRACE = (uint8_t)c; }
static void print(const char* s) { while(*s) putch(*s++); }

static void print_hex32(uint32_t v) { 
    for(int sh=28; sh>=0; sh-=4) { 
        uint8_t n=(v>>sh)&0xF; 
        putch(n<10?'0'+n:'A'+n-10);
    } 
}

int main() {
    print("[MINIMAL] start\n");
    
    // Simple computation
    uint32_t sum = 0;
    for(uint32_t i = 0; i < 100; i++) {
        sum += i * 2;
    }
    
    print("[MINIMAL] sum=");
    print_hex32(sum);
    putch('\n');
    
    print("[MINIMAL] end\n");
    
    __asm__ volatile("ecall");
    return 0;
}
