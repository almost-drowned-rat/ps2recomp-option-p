/*
 * The EE kernel, as the game calls it: `syscall` with the service number in $v1
 * and arguments in $a0-$a3 ($t0, $t1 beyond). A negative number is the same
 * service called from interrupt context. Results come back in $v0.
 *
 * What a program needs to start - its stack and heap, cache flushes, the SIF
 * registers, the syscall table it may extend. Threads, semaphores and interrupt
 * handlers are threads.cpp's. Every service is noted the first time it is used,
 * so the report shows what the game asked for.
 */

#include <atomic>
#include <cstdio>
#include <map>
#include <string>

#include "ps2host/disc.h"
#include "ps2host/memory.h"
#include "ps2host/run.h"
#include "ps2host/settings.h"

namespace ps2host {

namespace {

// The "language" setting as the console's OSD language code (bits 16-20 of the
// config): ja 0, en 1, fr 2, es 3, de 4, it 5, nl 6, pt 7, or the number itself;
// -1 when unset or unknown (the default config's English stays).
int OsdLanguage(const std::string& value) {
    static const char* const names[] = {"ja", "en", "fr", "es", "de", "it", "nl", "pt"};
    for (int i = 0; i < 8; ++i)
        if (value == names[i]) return i;
    if (value.size() == 1 && value[0] >= '0' && value[0] <= '7') return value[0] - '0';
    return -1;
}

// GsPutIMR's mask (bit 8 SIGNAL, 9 FINISH, 10 HSYNC, 11 VSYNC, 12 EDW: 1 masked), read by the GS's events on the
// GS thread (devices.cpp). All masked until the program says otherwise, as after the console's reset.
std::atomic<u32> g_gs_imr{0xFF00u};

struct Kernel {
    u32 heap_end = 0;
    u32 stack_base = 0;
    u64 gs_imr = 0xFF00u;
    std::map<u32, u32> sif_registers;
    // The console's settings, as Get/SetOsdConfigParam pass them: bit 0 S/PDIF,
    // 1-2 screen (0 4:3), 3 video output, 4 non-Japanese language, 5-12 PS1
    // driver settings, 13-15 version, 16-20 language (1 English), 21-31 time
    // zone offset in minutes. A US console in English, at GMT.
    u32 osd_config = (1u << 4) | (1u << 13) | (1u << 16);
    u8 osd_config2[16] = {};                         // Get/SetOsdConfigParam2's bytes
    u32 vsync_flag = 0, vsync_csr = 0;               // SetVSyncFlag's, until the next blank
};

// The kernel's syscall table, in guest kernel RAM as on the console: SetSyscall
// writes a handler into it, and a program may find it there and read or patch it
// (Burnout 3's start-up searches kernel memory for the handlers it installed, to
// learn where the table is). An entry of zero is a service the host provides.
constexpr u32 kSyscallTable = 0x80001000u;
constexpr u32 kSyscallEntries = 256;

u32 TableEntry(s32 number) {
    return u32(number) < kSyscallEntries
               ? u32(GuestMemory().Read(kSyscallTable + 4 * u32(number), 4)) : 0;
}

Kernel& K() {
    static Kernel kernel;
    return kernel;
}

u32 Arg(const lifted::Context& c, int i) {
    static const int regs[] = {4, 5, 6, 7, 8, 9};     // $a0-$a3, $t0, $t1
    return u32(c.r[regs[i]]);
}

void Return(lifted::Context& c, u64 value) { c.r[2] = u64(s64(s32(u32(value)))); }

struct Service {
    s32 number;
    const char* name;
};

// The documented EE kernel services (engine/data/ee_syscalls.csv names them for
// the analysis; these are the ones the host knows by name).
const Service kServices[] = {
    {0x02, "SetGsCrt"}, {0x04, "Exit"}, {0x05, "RFU005"}, {0x06, "LoadExecPS2"}, {0x07, "ExecPS2"},
    {0x0D, "SetVTLBRefillHandler"}, {0x0E, "SetVCommonHandler"},
    {0x10, "AddIntcHandler"}, {0x11, "RemoveIntcHandler"},
    {0x12, "AddDmacHandler"}, {0x13, "RemoveDmacHandler"},
    {0x14, "_EnableIntc"}, {0x15, "_DisableIntc"}, {0x16, "_EnableDmac"},
    {0x17, "_DisableDmac"}, {0x20, "CreateThread"}, {0x21, "DeleteThread"},
    {0x22, "StartThread"}, {0x23, "ExitThread"}, {0x24, "ExitDeleteThread"},
    {0x25, "TerminateThread"}, {0x29, "ChangeThreadPriority"},
    {0x2B, "RotateThreadReadyQueue"}, {0x2F, "GetThreadId"},
    {0x30, "ReferThreadStatus"}, {0x32, "SleepThread"}, {0x33, "WakeupThread"}, {0x34, "iWakeupThread"},
    {0x35, "CancelWakeupThread"}, {0x36, "iCancelWakeupThread"},
    {0x37, "SuspendThread"}, {0x39, "ResumeThread"}, {0x3C, "SetupThread"},
    {0x3D, "SetupHeap"}, {0x3E, "EndOfHeap"}, {0x40, "CreateSema"},
    {0x41, "DeleteSema"}, {0x42, "SignalSema"}, {0x43, "iSignalSema"},
    {0x44, "WaitSema"}, {0x45, "PollSema"}, {0x47, "ReferSemaStatus"},
    {0x4A, "SetOsdConfigParam"}, {0x4B, "GetOsdConfigParam"}, {0x5A, "kCopy"},
    {0x64, "FlushCache"},
    {0x70, "GsGetIMR"}, {0x71, "GsPutIMR"}, {0x73, "SetVSyncFlag"},
    {0x74, "SetSyscall"}, {0x76, "SifDmaStat"}, {0x77, "SifSetDma"},
    {0x78, "SifSetDChain"}, {0x79, "SifSetReg"}, {0x7A, "SifGetReg"},
    {0x7C, "Deci2Call"}, {0xFC, "SetAlarm"}, {0xFD, "ReleaseAlarm"},
    {0xFE, "iSetAlarm"}, {0xFF, "iReleaseAlarm"},
};

}  // namespace

void KernelVblank(u64 frame) {
    Kernel& k = K();
    if (k.vsync_flag) GuestMemory().Write(k.vsync_flag, 1, 4);
    if (k.vsync_csr) {                               // FIELD (bit 13) by the frame, VSINT (bit 3) pending
        const u64 csr = (u64(frame & 1) << 13) | (1u << 3);
        GuestMemory().Write(k.vsync_csr, u32(csr), 4);
        GuestMemory().Write(k.vsync_csr + 4, u32(csr >> 32), 4);
    }
    k.vsync_flag = k.vsync_csr = 0;
}

u32 KernelGsImr() { return g_gs_imr.load(std::memory_order_relaxed); }

const char* SyscallName(s32 service) {
    const s32 number = service < 0 ? -service : service;
    for (const Service& s : kServices)
        if (s.number == number) return s.name;
    return "?";
}

void KernelSyscall(lifted::Context& c) {
    const s32 service = s32(u32(c.r[3]));
    const s32 number = service < 0 ? -service : service;
    if (number < 0x100) ++RunStats().syscall_counts[service + 0x100];
    else ++RunStats().syscalls[service];
    // Only the first call of each service is noted (unless every call is): a game
    // makes hundreds of thousands a minute (Resident Evil Outbreak's thread
    // scheduler), and formatting each one's note cost 2-4% of the EE.
    static std::atomic<bool> noted[0x200];
    const bool first = number < 0x100 ? !noted[service + 0x100].exchange(true) : true;
    if (first || Tracing() || NotingAll()) {
        char text[160];
        std::snprintf(text, sizeof text, "kernel %s (0x%02X) a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X",
                      SyscallName(service), unsigned(number), Arg(c, 0), Arg(c, 1), Arg(c, 2),
                      Arg(c, 3));
        if (Tracing()) std::fprintf(stderr, "[%s]\n", text);
        else Note("syscall " + std::to_string(service), text);
    }
    Kernel& k = K();
    // A service the program installed itself (SetSyscall) runs its own handler,
    // in kernel mode on the console - here, as any other guest function.
    if (const u32 handler = TableEntry(number)) {
        Dispatch(c, handler);
        return;
    }
    u64 result = 0;
    if (ThreadSyscall(c, service, result)) {
        Return(c, result);
        return;
    }
    switch (number) {
        case 0x02:                                   // SetGsCrt(interlace, mode, ffmd)
            // The video mode the kernel sets the GS up for: SMODE2 = INT (bit 0),
            // FFMD (bit 1). A game that never writes SMODE2 itself says it here -
            // ZOE: interlaced field mode, a buffer of half the shown lines, each
            // repeated (Gs::ShownLines); unhandled, the picture read the buffer
            // below it as its lower half (2026-11-07). `mode` (NTSC/PAL/...) picks
            // the CRT timing, which the host's own presentation does not take.
            GsPrivileged(0x12000020u, u64(Arg(c, 0) & 1) | (u64(Arg(c, 2) & 1) << 1));
            Return(c, 0);
            return;
        case 0x04:                                   // Exit(status)
            throw GuestExit{s32(Arg(c, 0))};
        case 0x06: case 0x07:                        // LoadExecPS2 / ExecPS2
            throw Unimplemented{std::string("the kernel service ") + SyscallName(service) +
                                ", which starts another program"};
        case 0x64:                                   // FlushCache(which)
            // Invalidating the instruction cache (2): the program wrote code it is
            // about to run - an overlay loaded. 0 (writing back the data cache,
            // before a DMA) leaves code as it was; Resident Evil Outbreak issues it
            // every frame, and each emptied the whole call cache.
            if (Arg(c, 0) == 2) CodeChanged();
            Return(c, 0);
            return;
        case 0x3C: {                                 // SetupThread(gp, stack, size, args, root)
            const u32 stack = Arg(c, 1), size = Arg(c, 2);
            // The program's argument area (crt0's: argc, argv[16], 256 bytes of strings): the console's kernel
            // fills it with the path the program was started from - argc 1, argv[0] "cdrom0:\SCES_542.06;1" for
            // a disc's BOOT2. God of War II (0x001873D8, its start-up) tells disc from a dev kit's host0 by
            // argv[0]: with argc 0 it took itself for one - no IOPRP300 reboot, every module load refused
            // (loadfile's version 2800, not 3000), its sound servers never there.
            if (const u32 args = Arg(c, 3)) {
                const std::string boot = GuestDisc().IsOpen() ? GuestDisc().BootPath() : std::string();
                if (!boot.empty()) {
                    Memory& m = GuestMemory();
                    const std::string path = "cdrom0:" + boot;
                    const u32 strings = args + 4 + 16 * 4;
                    m.Write(args, 1, 4);
                    m.Write(args + 4, strings, 4);
                    for (std::size_t i = 0; i < path.size() && i < 255; ++i) m.Write(strings + u32(i), u8(path[i]), 1);
                    m.Write(strings + u32(path.size() < 255 ? path.size() : 255), 0, 1);
                }
            }
            // A stack of -1 means "at the top of RAM"; the thread's stack pointer
            // is the top of its stack, less the kernel's argument area.
            const u32 base = stack == 0xFFFFFFFFu ? kRdramSize - size : stack;
            k.stack_base = base;
            Return(c, base + size - 0x2A0);
            return;
        }
        case 0x3D: {                                 // SetupHeap(start, size)
            const u32 start = Arg(c, 0), size = Arg(c, 1);
            k.heap_end = size == 0xFFFFFFFFu ? k.stack_base : start + size;
            Return(c, k.heap_end);
            return;
        }
        case 0x3E:                                   // EndOfHeap()
            Return(c, k.heap_end);
            return;
        case 0x70:                                   // GsGetIMR
            Return(c, k.gs_imr);
            return;
        case 0x71:                                   // GsPutIMR(imr)
            k.gs_imr = Arg(c, 0);
            g_gs_imr.store(u32(k.gs_imr), std::memory_order_relaxed);
            Return(c, 0);
            return;
        case 0x74:                                   // SetSyscall(service, handler)
            // The SDK patches the kernel's alarms (SetAlarm, iSetAlarm, ReleaseAlarm,
            // iReleaseAlarm, and their timer 3 handler at 0x12C) with code that
            // drives the kernel's own interrupt dispatch. The host keeps those
            // calls: its alarms are the same API, run by its own scheduler.
            if (Arg(c, 0) >= 0xFC && Arg(c, 0) <= 0xFF) {
                char text[160];
                std::snprintf(text, sizeof text, "kernel: the program's patch of syscall 0x%02X is not "
                              "installed: the host's alarms stand", unsigned(Arg(c, 0)));
                Note("syscall patch " + std::to_string(Arg(c, 0)), text);
            } else if (Arg(c, 0) < kSyscallEntries) {
                GuestMemory().Write(kSyscallTable + 4 * Arg(c, 0), Arg(c, 1), 4);
            }
            Return(c, 0);
            return;
        case 0x5A: {                                 // kCopy(dest, src, size): from kernel memory
            Memory& m = GuestMemory();
            for (u32 i = 0; i + 4 <= Arg(c, 2); i += 4)
                m.Write(Arg(c, 0) + i, m.Read(Arg(c, 1) + i, 4), 4);
            Return(c, 0);
            return;
        }
        case 0x76:                                   // SifDmaStat(id): < 0 when done
            Return(c, u64(-1));
            return;
        case 0x77: {                                 // SifSetDma(transfers*, count)
            if (Tracing()) {
                // struct SifDmaTransfer { void* src; void* dest; int size; int attr; }
                Memory& m = GuestMemory();
                for (u32 i = 0; i < Arg(c, 1); ++i) {
                    const u32 t = Arg(c, 0) + 16 * i;
                    const u32 src = u32(m.Read(t, 4)), dest = u32(m.Read(t + 4, 4));
                    const u32 size = u32(m.Read(t + 8, 4)), attr = u32(m.Read(t + 12, 4));
                    std::fprintf(stderr, "  [sif dma] src=0x%08X dest=0x%08X size=%u attr=0x%X:",
                                 src, dest, size, attr);
                    for (u32 w = 0; w < size && w < 64; w += 4)
                        std::fprintf(stderr, " %08X", u32(m.Read(src + w, 4)));
                    std::fprintf(stderr, "\n");
                }
            }
            SifDma(Arg(c, 0), Arg(c, 1));
            Return(c, 1);
            return;
        }
        case 0x78:                                   // SifSetDChain
            Return(c, 1);
            return;
        case 0x79:                                   // SifSetReg(reg, value)
            k.sif_registers[Arg(c, 0)] = Arg(c, 1);
            Return(c, 1);
            return;
        case 0x7A:                                   // SifGetReg(reg)
            // Every status bit set: the IOP reports itself up and ready, as the
            // verification interpreter does (engine/ps2decomp/emu.py).
            Return(c, 0xFFFFFFFFu);
            return;
        case 0x4A:                                   // SetOsdConfigParam(config*)
            k.osd_config = u32(GuestMemory().Read(Arg(c, 0), 4));
            Return(c, 0);
            return;
        // The console's second settings block (version >= 1 in GetOsdConfigParam - the host's is 1): bytes a
        // program reads or sets from an offset - date / time notation, summer time (God of War II's libscf,
        // 0x002BEB70 / 0x002BEBD8, reads byte 1's bits 4 and 5; unanswered, it read what its stack held).
        // All zero by default: year/month/day, 24-hour clock, no summer time.
        case 0x6E:                                   // SetOsdConfigParam2(buffer*, size, offset)
        case 0x6F: {                                 // GetOsdConfigParam2(buffer*, size, offset)
            Memory& m = GuestMemory();
            for (u32 i = 0; i < Arg(c, 1) && Arg(c, 2) + i < sizeof k.osd_config2; ++i) {
                u8& b = k.osd_config2[Arg(c, 2) + i];
                if (number == 0x6E) b = u8(m.Read(Arg(c, 0) + i, 1));
                else m.Write(Arg(c, 0) + i, b, 1);
            }
            Return(c, 0);
            return;
        }
        case 0x73:                                   // SetVSyncFlag(flag*, csr*)
            // At the next vertical blank start the kernel writes 1 to *flag and the GS's CSR (64 bits) to *csr,
            // once (KernelVblank). God of War II's libgraph (0x002CF7C0) waits on it and takes its field from
            // CSR >> 13; unanswered, that was whatever its stack held.
            k.vsync_flag = Arg(c, 0);
            k.vsync_csr = Arg(c, 1);
            Return(c, 0);
            return;
        case 0x4B: {                                 // GetOsdConfigParam(config*)
            // The "language" setting is the console's system language: PAL games that
            // take theirs from it (ZOE PAL) start in the player's choice, no game patch.
            const int language = OsdLanguage(Setting("language"));
            if (language >= 0) {
                k.osd_config = (k.osd_config & ~(0x1Fu << 16)) | (u32(language) << 16);
                k.osd_config = language ? (k.osd_config | (1u << 4)) : (k.osd_config & ~(1u << 4));
            }
            GuestMemory().Write(Arg(c, 0), k.osd_config, 4);
            Return(c, 0);
            return;
        }
        case 0x7C: {                                 // Deci2Call(function, params*)
            // DECI2 talks to a development tool's debugger. A retail console has
            // none attached, so every request fails - and code that sends its
            // TTY output over DECI2 gives up rather than waiting on a reply that
            // never comes. kputs (0x10) is the kernel's own debug print: the text
            // is the game's, so it goes to the log.
            if (Arg(c, 0) == 0x10) {
                Memory& m = GuestMemory();
                std::string text;
                for (u32 p = u32(m.Read(Arg(c, 1), 4)); text.size() < 1024; ++p) {
                    const char ch = char(m.Read(p, 1));
                    if (!ch) break;
                    text += ch;
                }
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
                std::fprintf(stderr, "[guest] %s\n", text.c_str());
                Return(c, 0);
                return;
            }
            Return(c, 0xFFFFFFFFu);
            return;
        }
        default:
            // The rest - handler enables, cache flushes, thread status, priorities -
            // have no effect the program depends on yet: succeed quietly.
            Return(c, 0);
            return;
    }
}

}  // namespace ps2host
