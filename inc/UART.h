// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "systemc"
#include "tlm.h"
#include "tlm_utils/simple_target_socket.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <deque>
#include <thread>

#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#endif

namespace riscv_tlm { namespace peripherals {

// 16550-compatible UART with bidirectional I/O.
//
// TX: writes go to std::cout.
// RX: a background thread reads raw bytes from stdin and pushes them into a
//     software FIFO.  An SC_THREAD (rx_monitor_process) fires the RDA interrupt
//     into the PLIC every 50 µs of simulation time when unread data is waiting.
//
// 4-byte stride offsets (reg-shift=2):
//   0x00 RBR/THR/DLL  0x04 IER/DLM  0x08 IIR/FCR  0x0C LCR
//   0x10 MCR          0x14 LSR       0x18 MSR       0x1C SCR
// 1-byte stride offsets: 0x0–0x7 map directly.

class UART : public sc_core::sc_module {
public:
    tlm_utils::simple_target_socket<UART> socket;

    // Wire to PLIC: called with true to assert interrupt, false to deassert.
    std::function<void(bool)> set_uart_irq;

    // TX callback to observe characters output to standard output
    std::function<void(char)> tx_callback;
    void register_tx_callback(std::function<void(char)> cb) {
        tx_callback = cb;
    }

    SC_HAS_PROCESS(UART);
    explicit UART(sc_core::sc_module_name const& name)
        : sc_module(name), socket("socket"),
          scr(0), dlab(false), m_ier(0), m_thre_ip(true)
    {
        socket.register_b_transport(this, &UART::b_transport);
        SC_THREAD(rx_monitor_process);
        m_stdin_thread = std::thread(&UART::stdin_reader, this);
        m_stdin_thread.detach();
    }

    ~UART() {
        m_stop.store(true);
        restore_terminal();
    }

private:
    uint8_t scr;
    bool    dlab;
    uint8_t m_ier;
    bool    m_thre_ip;

    // RX FIFO — written by stdin thread, read by SystemC thread via b_transport.
    std::mutex          rx_mutex;
    std::deque<uint8_t> rx_fifo;

    std::atomic<bool> m_stop{false};
    std::thread       m_stdin_thread;

#ifndef _WIN32
    struct termios m_old_termios{};
    bool           m_raw_mode{false};
#endif

    // -------------------------------------------------------------------------
    // Interrupt logic (called from SystemC thread only)
    // -------------------------------------------------------------------------

    void update_interrupts() {
        bool thre_active = (m_ier & 0x02) && m_thre_ip;
        bool rda_active  = (m_ier & 0x01) && !rx_fifo_empty_locked();
        if (set_uart_irq) set_uart_irq(thre_active || rda_active);
    }

    // Peek at FIFO size under lock — used only inside SystemC thread.
    bool rx_fifo_empty_locked() {
        std::lock_guard<std::mutex> lk(rx_mutex);
        return rx_fifo.empty();
    }

    // Pop one byte under lock — used only inside SystemC thread.
    uint8_t rx_pop() {
        std::lock_guard<std::mutex> lk(rx_mutex);
        uint8_t b = rx_fifo.front();
        rx_fifo.pop_front();
        return b;
    }

    // -------------------------------------------------------------------------
    // SC_THREAD: periodically assert RDA interrupt when unread bytes are waiting.
    // Runs every 50 µs of simulation time (~5000 cycles at 100 MHz).
    // -------------------------------------------------------------------------

    void rx_monitor_process() {
        while (true) {
            wait(sc_core::sc_time(50, sc_core::SC_US));
            if ((m_ier & 0x01) && !rx_fifo_empty_locked()) {
                update_interrupts();
            }
        }
    }

    // -------------------------------------------------------------------------
    // Host terminal raw-mode helpers (non-SystemC thread)
    // -------------------------------------------------------------------------

#ifndef _WIN32
    void set_raw_mode() {
        if (!isatty(STDIN_FILENO)) return;
        tcgetattr(STDIN_FILENO, &m_old_termios);
        struct termios raw = m_old_termios;
        cfmakeraw(&raw);
        raw.c_cc[VMIN]  = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        m_raw_mode = true;
    }

    void restore_terminal() {
        if (m_raw_mode && isatty(STDIN_FILENO)) {
            tcsetattr(STDIN_FILENO, TCSANOW, &m_old_termios);
            m_raw_mode = false;
        }
    }
#else
    void set_raw_mode()    {}
    void restore_terminal() {}
#endif

    // -------------------------------------------------------------------------
    // Background stdin reader (runs in a detached std::thread)
    // -------------------------------------------------------------------------

    void stdin_reader() {
        set_raw_mode();
        while (!m_stop.load()) {
            uint8_t ch;
#ifndef _WIN32
            int n = ::read(STDIN_FILENO, &ch, 1);
#else
            int c = _getch();
            if (c < 0) break;
            ch = static_cast<uint8_t>(c);
            int n = 1;
#endif
            if (n <= 0) break;
            {
                std::lock_guard<std::mutex> lk(rx_mutex);
                if (rx_fifo.size() < 64)
                    rx_fifo.push_back(ch);
            }
        }
        restore_terminal();
    }

    // -------------------------------------------------------------------------
    // Register address decode
    // -------------------------------------------------------------------------

    static unsigned reg_from_offset(uint64_t off) {
        if ((off & 3) == 0 && off <= 0x1C)
            return static_cast<unsigned>(off >> 2);
        if (off <= 7) return static_cast<unsigned>(off);
        return 0xFF;
    }

    // -------------------------------------------------------------------------
    // TLM b_transport (runs in SystemC thread)
    // -------------------------------------------------------------------------

    void b_transport(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay) {
        (void)delay;
        unsigned char* ptr = trans.get_data_ptr();
        unsigned int   len = trans.get_data_length();
        unsigned       reg = reg_from_offset(trans.get_address());

        if (trans.get_command() == tlm::TLM_WRITE_COMMAND && len > 0) {
            uint8_t val = ptr[0];
            switch (reg) {
                case 0: // THR (DLAB=0) or DLL (DLAB=1)
                    if (!dlab) {
                        std::cout << static_cast<char>(val) << std::flush;
                        if (tx_callback) {
                            tx_callback(static_cast<char>(val));
                        }
                        m_thre_ip = false;
                        m_thre_ip = true;
                        update_interrupts();
                    }
                    break;
                case 1: // IER (DLAB=0) or DLM (DLAB=1)
                    if (!dlab) {
                        bool old_thri = (m_ier & 0x02) != 0;
                        bool new_thri = (val & 0x02) != 0;
                        m_ier = val;
                        if (new_thri && !old_thri) {
                            m_thre_ip = true;
                        } else if (!new_thri) {
                            m_thre_ip = false;
                        }
                        update_interrupts();
                    }
                    break;
                case 2: break; // FCR (write-only) — ignore
                case 3: dlab = (val >> 7) & 1; break; // LCR
                case 7: scr = val; break;              // SCR
                default: break;
            }
        } else if (trans.get_command() == tlm::TLM_READ_COMMAND && len > 0) {
            uint32_t val = 0;
            switch (reg) {
                case 0: // RBR
                    if (!dlab && !rx_fifo_empty_locked()) {
                        val = rx_pop();
                        update_interrupts(); // clear RDA if FIFO now empty
                    }
                    break;
                case 1: val = m_ier; break; // IER
                case 2: // IIR — report highest-priority pending interrupt
                    {
                        bool rda  = (m_ier & 0x01) && !rx_fifo_empty_locked();
                        bool thre = (m_ier & 0x02) && m_thre_ip;
                        if (rda) {
                            val = 0x04; // Received Data Available (priority 2)
                        } else if (thre) {
                            val = 0x02; // THRE (priority 3)
                            m_thre_ip = false;
                            update_interrupts();
                        } else {
                            val = 0x01; // no interrupt pending
                        }
                    }
                    break;
                case 3: val = 0;    break; // LCR
                case 4: val = 0;    break; // MCR
                case 5: // LSR
                    {
                        uint8_t lsr = 0x60; // THRE(5)=1, TEMT(6)=1 — TX always ready
                        if (!rx_fifo_empty_locked()) lsr |= 0x01; // DR — data ready
                        val = lsr;
                    }
                    break;
                case 6: val = 0;   break; // MSR
                case 7: val = scr; break; // SCR
                default: val = 0;  break;
            }
            for (unsigned i = 0; i < len && i < 4; ++i)
                ptr[i] = static_cast<uint8_t>((val >> (8 * i)) & 0xFF);
        }

        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
};

}} // namespace
