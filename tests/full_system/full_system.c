#include <stdint.h>
#include <stddef.h>

#define TRACE (*(volatile unsigned char*)0x40000000)
#define MTIME_LO   (*(volatile uint32_t*)0x40004000)
#define MTIME_HI   (*(volatile uint32_t*)0x40004004)
#define MTIMECMP_LO (*(volatile uint32_t*)0x40004008)
#define MTIMECMP_HI (*(volatile uint32_t*)0x4000400C)

static inline void putch(char c){ TRACE = (unsigned char)c; }
static void print(const char* s){ while(*s) putch(*s++); }

static inline void write_mstatus(uint32_t v){ asm volatile("csrw mstatus,%0"::"r"(v)); }
static inline uint32_t read_mstatus(){ uint32_t v; asm volatile("csrr %0,mstatus":"=r"(v)); return v; }
static inline void write_mtvec(uint32_t v){ asm volatile("csrw mtvec,%0"::"r"(v)); }
static inline void write_mie(uint32_t v){ asm volatile("csrw mie,%0"::"r"(v)); }
static inline uint32_t read_mcycle(){ uint32_t v; asm volatile("csrr %0,mcycle":"=r"(v)); return v; }
static inline uint32_t read_minstret(){ uint32_t v; asm volatile("csrr %0,minstret":"=r"(v)); return v; }

volatile uint32_t irq_count = 0;

void trap_handler(void){
    // Simple handler: acknowledge timer and bump counter
    irq_count++;
    // Re-arm timer (periodic every ~5000 ticks of mtime)
    uint64_t now = ((uint64_t)MTIME_HI << 32) | MTIME_LO;
    uint64_t next = now + 5000ULL;
    MTIMECMP_LO = (uint32_t)(next & 0xFFFFFFFF);
    MTIMECMP_HI = (uint32_t)(next >> 32);
}

// Naked handler stub in assembly to jump to C trap_handler
__attribute__((naked)) void _start_trap(){
    asm volatile(
        "addi sp, sp, -16\n"        // minimal stack frame
        "sw ra, 12(sp)\n"
        "call trap_handler\n"
        "lw ra, 12(sp)\n"
        "addi sp, sp, 16\n"
        "mret\n"
    );
}

static void setup_timer_irq(){
    // mtvec points to our handler base (direct mode)
    write_mtvec((uint32_t)(uintptr_t)_start_trap);
    // Enable machine timer interrupt (MTIE) in mie and global MIE in mstatus
    write_mie(1 << 7); // MTIE bit
    write_mstatus(read_mstatus() | 0x8); // MIE bit
    // Program initial mtimecmp a little ahead
    uint64_t now = ((uint64_t)MTIME_HI << 32) | MTIME_LO;
    uint64_t next = now + 5000ULL;
    MTIMECMP_LO = (uint32_t)(next & 0xFFFFFFFF);
    MTIMECMP_HI = (uint32_t)(next >> 32);
}

int main(){
    print("[FS] RISC-V full system test start\n");

    setup_timer_irq();

    // Workload: simple memory operations + arithmetic
    enum { N = 1024 }; static uint32_t buf[N];
    for(size_t i=0;i<N;i++){ buf[i] = (uint32_t)i * 3u + 1u; }
    uint64_t sum = 0;
    for(size_t i=0;i<N;i++){ sum += buf[i]; }

    // Busy loop to accumulate timer interrupts
    for(volatile int outer=0; outer<2000; ++outer){
        asm volatile("nop;nop;nop;nop;nop");
    }

    uint32_t ccycle = read_mcycle();
    uint32_t cinstr = read_minstret();

    print("[FS] sum=");
    // crude hex print
    for(int sh=28; sh>=0; sh-=4){
        uint8_t nib = (sum >> sh) & 0xF;
        putch(nib < 10 ? ('0'+nib) : ('A'+nib-10));
    }
    putch('\n');

    print("[FS] interrupts=");
    { uint32_t v=irq_count; for(int sh=28; sh>=0; sh-=4){ uint8_t nib=(v>>sh)&0xF; putch(nib<10?('0'+nib):('A'+nib-10)); } putch('\n'); }

    print("[FS] mcycle=");
    { uint32_t v=ccycle; for(int sh=28; sh>=0; sh-=4){ uint8_t nib=(v>>sh)&0xF; putch(nib<10?('0'+nib):('A'+nib-10)); } putch('\n'); }

    print("[FS] minstret=");
    { uint32_t v=cinstr; for(int sh=28; sh>=0; sh-=4){ uint8_t nib=(v>>sh)&0xF; putch(nib<10?('0'+nib):('A'+nib-10)); } putch('\n'); }

    print("[FS] Done\n");

    asm volatile("ecall"); // signal end
    return 0;
}
