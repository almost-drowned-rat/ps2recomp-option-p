/*
 * A VIF's command stream (EE User's Manual, chapter 6): VIFcodes and their
 * data, delivered to the VU behind the VIF and to the GS.
 *
 * MPG loads microprograms into the VU's instruction memory, UNPACK expands
 * packed vectors into its data memory (formats S/V2/V3/V4 of 32, 16 or 8 bits
 * and V4-5, with STMASK/STROW/STCOL masking, STMOD's offset and difference
 * modes, and STCYCL's skipping and filling writes), and MSCAL, MSCALF and MSCNT
 * run the microprogram - to its end, at once: the EE sees it done by the time
 * it could look. VIF1's double buffering (BASE, OFFSET, TOPS and the FLG bit of
 * UNPACK) and ITOP feed XTOP and XITOP. DIRECT and DIRECTHL carry GIF data on
 * PATH2; MSKPATH3 masks PATH3, which code that hands PATH3 over waits on.
 */

#ifndef PS2HOST_VIF_H
#define PS2HOST_VIF_H

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ps2host {

class Vu;

class VifStream {
public:
    /* The VU behind this VIF (VU1 for VIF1); none drops VU traffic. */
    Vu* vu = nullptr;
    bool double_buffered = false;   /* VIF1's BASE/OFFSET/TOPS */
    /* Called when MSKPATH3 masks (true) or unmasks (false) PATH3. */
    std::function<void(bool masked)> on_mask_path3;
    /* Each word of DIRECT/DIRECTHL data: GIF data on PATH2. */
    std::function<void(std::uint32_t word)> on_direct;
    /* DIRECT's data a run at a time, from guest address `from` (Words); when
       unset, a word at a time through on_direct. */
    std::function<void(const std::uint32_t* words, std::uint32_t count, std::uint32_t from)> on_direct_block;
    /* Commands worth seeing in a trace (FLUSHA), when set. */
    std::function<void(const char* what)> on_trace;
    /* A VIFcode's interrupt bit (bit 31) took effect: the VIF's INTC cause raised (VIF0 13, VIF1 14). */
    std::function<void()> on_interrupt;

    /* The next 32-bit word of the stream, from the FIFO or a DMA transfer. */
    void Word(std::uint32_t word);
    /* `count` words from guest address `from`, as Word each (keeping
       g_dma_source), DIRECT's data handed on a run at a time. */
    void Words(const std::uint32_t* words, std::uint32_t count, std::uint32_t from);

    std::uint64_t commands() const { return commands_; }
    std::uint64_t data_words() const { return data_words_; }
    /* For the report: how often each command came, by name. */
    std::string Summary() const;

    // Registers the EE can read (VIF1_TOP, VIF1_TOPS, ITOP, ...).
    std::uint32_t top = 0, tops = 0, base = 0, offset = 0, itop = 0, itops = 0, dbf = 0;
    std::uint32_t mask = 0, mode = 0, row[4] = {}, col[4] = {};
    /* Where this VIF's registers are (VIF0 0x10003800, VIF1 0x10003C00): Mirror writes the state there - STAT's
       VPS (waiting for data), CYCLE, MODE, NUM, MASK, CODE, ITOPS, BASE, OFST, TOPS, ITOP, TOP, R0-R3, C0-C3 - as
       the EE reads them (ps2autotests dma/vif). 0: none. */
    std::uint32_t register_base = 0;
    /* After each DMA run and each FIFO quadword (Words does it itself). */
    void Mirror();
    /* FBRST.RST: a command waiting for its data is dropped (STAT.VPS 0), a stall and what waits behind it too;
       the registers STCYCL, STMASK, STROW ... set stay. */
    void Reset();
    /* FBRST.STC: a stall ended (STAT's VIS and INT cleared), the words held behind it decoded now. */
    void Continue();
    /* VIFn_MARK written by the EE: STAT.MRK cleared. */
    void MarkWritten() { stat_flags_ &= ~kStatMrk; Mirror(); }
    /* Interrupt stalls so far (the report). */
    std::uint64_t stalls() const { return stalls_; }

    /* STAT's bits this model keeps: MRK (a MARK came), VIS (stalled by an interrupt bit), INT (an interrupt bit). */
    static constexpr std::uint32_t kStatMrk = 0x40, kStatVis = 0x400, kStatInt = 0x800;

private:
    enum class Data { None, Direct, Mpg, Unpack, Mask, Row, Col };
    std::uint32_t pending_ = 0;     /* data words still owed to the last command */
    Data data_ = Data::None;
    std::uint32_t cycle_length_ = 0, write_length_ = 0;     /* STCYCL's CL and WL, as written (CYCLE) */
    /* As UNPACK takes them (ps2autotests dma/vif/stcycl): WL 0 is 256; CL 0 stays 0 - every write a filling one. */
    std::uint32_t Cl() const { return cycle_length_; }
    std::uint32_t Wl() const { return write_length_ ? write_length_ : 256; }
    std::uint32_t data_index_ = 0;  /* words of the current command's data so far */
    std::uint32_t mpg_address_ = 0;
    std::uint32_t end_pc_ = 0;      /* where MSCNT continues */
    std::uint32_t code_ = 0;        /* the last VIFcode (VIFn_CODE) */
    struct Unpack {
        std::uint32_t address = 0, format = 0, num = 0, written = 0;
        bool masked = false, unsigned_ = false;
        std::vector<std::uint8_t> bytes;
        // V3: w takes the stream's next component (the hardware reads on); when that is not here yet (a DMA
        // chunk ended), the quadword waits for it (written unmasked, STMOD 0 only - the usual case).
        bool w_pending = false;
        std::uint32_t w_address = 0;
        std::uint32_t last_address = 0;     // the quadword WriteElement wrote last
    } unpack_;
    std::uint64_t commands_ = 0, data_words_ = 0;
    std::array<std::uint64_t, 128> by_command_{};
    /* The interrupt bit (ps2autotests dma/vif/intstall, as the console): a VIFcode with bit 31 set - MARK
       excepted, which only raises INT - stalls the VIF once it and its data are done: STAT.VIS and INT set,
       VPS 2, the interrupt raised; words that come meanwhile wait here (the console's FIFO; the DMA transfer
       still ends, as the test's three quadwords do) until FBRST.STC goes on with them or RST drops them.
       PS2HOST_VIF_NO_STALL=1: the bit ignored, as before 2026-11-17. */
    bool stalled_ = false, interrupt_after_ = false;
    std::vector<std::uint32_t> held_;
    std::uint32_t stat_flags_ = 0;
    std::uint64_t stalls_ = 0;
    void Stall();

    /* Words of data an UNPACK of `num` elements in format `format` reads. */
    std::uint32_t UnpackWords(std::uint32_t format, std::uint32_t num) const;
    void UnpackData(std::uint32_t word);
    /* A run of UNPACK's data words: whole elements straight from them while
       none is left part-read, the rest a word at a time. */
    void UnpackRun(const std::uint32_t* words, std::uint32_t count);
    /* One element's components from its bytes, as UNPACK expands them; `avail` bytes from b are there (a V3's w
       is the component after it, 0 and *w_missing when it is not there yet). */
    void Element(const std::uint8_t* b, std::uint32_t c[4], std::size_t avail, bool* w_missing = nullptr) const;
    /* A V3 element written: its quadword's w waits for the next component if w_missing. */
    void V3Pending(std::uint32_t address, bool w_missing);
    /* The next element's first component arrived: a waiting V3 w gets it. */
    void V3Resolve(std::uint32_t first);
    void UnpackFill();
    void WriteElement(const std::uint32_t value[4], bool from_data);
    void Start(std::uint32_t pc);
};

/* VIF1's command stream followed on the EE's thread without doing any of it: where each command and its data end,
   so that the MSKPATH3s of what is queued for VIF1's worker are known before the worker gets to them
   (devices.cpp's GIF_STAT guess, ps2autotests dma/vif/mskpath3 - it guessed one transfer behind). It must see
   every word the worker sees, in the same order (DMA transfers and FIFO stores). O(commands): data skipped. */
class VifWalker {
public:
    /* The words in turn; each MSKPATH3 among them appended to *masks (true: masked). */
    void Words(const std::uint32_t* words, std::uint32_t count, std::vector<bool>* masks);
    /* FBRST.RST: a command waiting for its data dropped, as VifStream::Reset. */
    void Reset() { pending_ = 0; }

private:
    std::uint32_t pending_ = 0, cl_ = 0, wl_ = 0;
};

/* The VIF1 stream fed by stores to its FIFO, 0x10005000. */
VifStream& Vif1();
/* VIF0's, at 0x10004000; it feeds VU0. */
VifStream& Vif0();

/* PS2HOST_VIF_STATS=1: UNPACK's words by format, mask, STMOD, CL/WL - printed at the run's end (main.cpp). */
void PrintVifStats();

}  // namespace ps2host

#endif  /* PS2HOST_VIF_H */
