/*
* robust_stress.c - Unified Robust Stress Test for RV32 and RV64
*
* This benchmark is designed to stress pipeline hazard detection and forwarding.
* It contains tight loops with heavy RAW (Read-After-Write) dependencies
* and control hazards (branches).
*
* It avoids FPU instructions to ensure compatibility with integer-only cores.
*/

volatile int data_sink;

// Use 'long' to adapt to native word size (32-bit on RV32, 64-bit on RV64)
typedef long word_t;

volatile int sink;
typedef long word_t;
typedef unsigned int uint32_t;

// Forward declaration
uint32_t lcg_rand(uint32_t *state);

#define ARR_SIZE 1024
volatile word_t ptr_array[ARR_SIZE];

// The entry point must be the first function in the file
int main() {
    // Initialize stack pointer for bare-metal execution
    asm volatile ("li sp, 0x100000");

    word_t i, j;
    uint32_t rng = 0xCAFEBABE;
    
    // 1. Setup Pointer Chase Array (Circular Linked List)
    for (i = 0; i < ARR_SIZE - 1; i++) {
        ptr_array[i] = i + 1;
    }
    ptr_array[ARR_SIZE - 1] = 0; // Loop back
    
    word_t ptr = 0;
    word_t acc = 0;

    // Outer loop for duration
    for (j = 0; j < 2000; j++) {
        
        // Inner Loop: The Gauntlet
        for (i = 0; i < 500; i++) {
            
            // A. POINTER CHASING (Memory Latency Stress)
            // The address of the next load depends on the result of the previous load.
            // This serialization defeats all parallel execution and forwarding.
            // If memory has ANY latency, this triggers stalls.
            ptr = ptr_array[ptr];
            
            // B. RANDOM BRANCHING (Control Hazard Stress)
            // Predictor should fail ~50% of the time, causing Pipeline Flushes/Stalls
            if (lcg_rand(&rng) & 0x100) {
                 acc += ptr;
                 acc ^= 0xDEADBEEF;
            } else {
                 acc -= ptr;
                 acc ^= 0xCAFEBABE;
            }
            
            // Dependencies for RAW hazard checking
            acc = acc + (ptr << 1);
        }
    }
    
    sink = (int)acc;
    
    // Exit simulator gracefully
    asm volatile ("li a7, 93; ecall");
    return 0;
}

// Simple LCG specific to 32-bit to ensure consistent behavior across arches
uint32_t lcg_rand(uint32_t *state) {
    *state = *state * 1103515245 + 12345;
    return (uint32_t)(*state / 65536) % 32768;
}
