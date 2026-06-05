/*!
 \file Trace.cpp
 \brief Basic TLM-2 Trace module
 \author Màrius Montón
 \date September 2018
 */
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdio>
#include <iostream>
#include <cstring>

#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <libgen.h>
#else
#define NOMINMAX 1
#include <windows.h>
#include <iostream>
// Provide minimal stubs so that Trace prints to stdout on Windows.
#define SIGKILL 9
using pid_t = int;
static inline pid_t fork() { return -1; }
static inline int grantpt(int) { return 0; }
static inline int unlockpt(int) { return 0; }
static inline char* ptsname(int) { return (char*)""; }
#endif

// Code partially taken from
// https://github.com/embecosm/esp1-systemc-tlm/blob/master/sysc-models/simple-soc/TermSC.h

#include "Trace.h"

namespace riscv_tlm::peripherals {

    void Trace::xtermLaunch(char *slaveName) const {
#ifndef _WIN32
        char *arg;

        // Guard: slaveName must be at least 2 characters to safely index [strlen-2]
        if (slaveName == nullptr || strlen(slaveName) < 2) {
            _exit(1);
        }

        char *fin = &(slaveName[strlen(slaveName) - 2]);

        if (nullptr == strchr(fin, '/')) {
            std::size_t buf_size = 2 + 1 + 1 + 20 + 1;
            arg = new char[buf_size];
            snprintf(arg, buf_size, "-S%c%c%d", fin[0], fin[1], ptMaster);
        } else {
            char *slaveBase = ::basename(slaveName);
            std::size_t buf_size = 2 + strlen(slaveBase) + 1 + 20 + 1;
            arg = new char[buf_size];
            snprintf(arg, buf_size, "-S%s/%d", slaveBase, ptMaster);
        }

        // Use a generic font to avoid missing legacy bitmap font; allow env override
        const char* font = std::getenv("TRACE_XTERM_FONT");
        if (!font) font = "Monospace";
        const char* fsize = std::getenv("TRACE_XTERM_FONTSIZE");
        if (!fsize) fsize = "12";

        // argv: xterm -fa <font> -fs <size> -S<pty>
        char *argv[8];
        argv[0] = (char *) ("xterm");
        argv[1] = (char *) ("-fa");
        argv[2] = (char *) (font);
        argv[3] = (char *) ("-fs");
        argv[4] = (char *) (fsize);
        argv[5] = arg;
        argv[6] = nullptr;

        execvp("xterm", argv);

        // If execvp fails, clean up and fall back gracefully
        perror("xterm execvp failed");
        delete[] arg;
        _exit(1); // Use _exit instead of exit in child process
#else
        (void)slaveName;
#endif
    }

    void Trace::xtermKill() {
#ifndef _WIN32
        if (-1 != ptSlave) {        // Close down the slave
            close(ptSlave);            // Close the FD
            ptSlave = -1;
        }

        if (-1 != ptMaster) {        // Close down the master
            close(ptMaster);
            ptMaster = -1;
        }

        if (xtermPid > 0) {            // Kill the terminal
            kill(xtermPid, SIGKILL);
            waitpid(xtermPid, nullptr, 0);
        }
#else
#endif
    }

    void Trace::xtermSetup() {
#ifndef _WIN32
        ptMaster = open("/dev/ptmx", O_RDWR);

        if (ptMaster != -1) {
            grantpt(ptMaster);

            unlockpt(ptMaster);

            char *ptSlaveName = ptsname(ptMaster);
            ptSlave = open(ptSlaveName, O_RDWR);    // In and out are the same

            struct termios termInfo{};
            tcgetattr(ptSlave, &termInfo);

            termInfo.c_lflag &= ~ECHO;
            termInfo.c_lflag &= ~ICANON;
            tcsetattr(ptSlave, TCSADRAIN, &termInfo);

            xtermPid = fork();

            if (xtermPid == 0) {
                xtermLaunch(ptSlaveName);
            }
        }
#else
        // Windows: no xterm, just stdout fallback handled elsewhere
#endif
    }

    SC_HAS_PROCESS(Trace);

    Trace::Trace(sc_core::sc_module_name const &name) :
            sc_module(name), socket("socket") {

        socket.register_b_transport(this, &Trace::b_transport);

        // Allow forcing stdout and avoid xterm dependency in headless/WSL
        const char* force_stdout = std::getenv("TRACE_STDOUT");
        const char* display = std::getenv("DISPLAY");
        if (force_stdout || !display) {
            ptSlave = -1;
            ptMaster = -1;
            xtermPid = -1;
        } else {
            xtermSetup();
        }
    }

    Trace::~Trace() {
        xtermKill();
    }

    void Trace::b_transport(tlm::tlm_generic_payload &trans,
                            sc_core::sc_time &delay) {

        unsigned char *ptr = trans.get_data_ptr();
        delay = sc_core::SC_ZERO_TIME;


#ifndef _WIN32
        if (ptSlave >= 0) {
            ssize_t a = write(ptSlave, ptr, 1);
            (void) a;
        } else {
            std::cout << static_cast<char>(*ptr);
            std::cout.flush();
        }
#else
        // Always stdout on Windows
        std::cout << static_cast<char>(*ptr);
        std::cout.flush();
#endif

        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
}