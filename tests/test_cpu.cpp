#include <memory>
#include <random>

#include "test_util.hpp"
#include "z80/assembler.hpp"
#include "z80/cpu.hpp"
#include "z80/snapshot.hpp"

using namespace z80;

namespace {

struct Machine {
    // The 64 KiB live on the heap: MSVC does not overlap the stack slots of sibling scopes, and
    // this file creates dozens of machines in one function.
    std::unique_ptr<Memory> store = std::make_unique<Memory>();
    Memory& memory = *store;
    Cpu cpu{memory};

    /// Assembles `source` into memory and points PC at the first byte.
    void load(const std::string& source) {
        const auto r = Assembler().assemble(source);
        if (!r.ok()) std::cerr << r.formatDiagnostics("test") << source;
        CHECK(r.ok());
        r.applyTo(memory);
        cpu.regs().pc = r.segments.front().start;  // first ORG = program entry
    }
    void steps(int n) {
        for (int i = 0; i < n; ++i) cpu.step();
    }
};

}  // namespace

int main() {
    // ---- reset state ----
    {
        Machine m;
        CHECK_EQ(m.cpu.regs().pc, Word{0});
        CHECK_EQ(m.cpu.regs().sp, Word{0xFFFF});
        CHECK_EQ(m.cpu.regs().af(), Word{0xFFFF});
        CHECK(!m.cpu.regs().iff1);
        CHECK_EQ(m.cpu.cycles(), std::uint64_t{0});
    }

    // ---- arithmetic and flags ----
    {
        Machine m;
        m.load("ORG &100\nLD A,&7F\nADD A,1\n");
        m.steps(2);
        CHECK_EQ(m.cpu.regs().a(), Byte{0x80});
        CHECK_EQ(m.cpu.regs().f(), Byte{flag::S | flag::H | flag::PV});  // 0x94
    }
    {
        Machine m;  // BCD: 15 + 27 = 42
        m.load("ORG &100\nLD A,&15\nADD A,&27\nDAA\n");
        m.steps(3);
        CHECK_EQ(m.cpu.regs().a(), Byte{0x42});
    }
    {
        Machine m;  // SUB sets N and borrows
        m.load("ORG &100\nLD A,5\nSUB 6\n");
        m.steps(2);
        CHECK_EQ(m.cpu.regs().a(), Byte{0xFF});
        CHECK((m.cpu.regs().f() & (flag::C | flag::N | flag::S)) == (flag::C | flag::N | flag::S));
    }
    {
        Machine m;  // 16-bit: ADD HL,DE and SBC HL,BC
        m.load("ORG &100\nLD HL,&1000\nLD DE,&0234\nADD HL,DE\nLD BC,&0001\nOR A\nSBC HL,BC\n");
        m.steps(6);
        CHECK_EQ(m.cpu.regs().hl(), Word{0x1233});
    }

    // ---- timing ----
    {
        Machine m;
        m.load("ORG &100\nNOP\nLD A,5\nLD HL,&1234\nPUSH HL\nCALL &200\nORG &200\nRET\n");
        CHECK_EQ(m.cpu.step(), 4u);
        CHECK_EQ(m.cpu.step(), 7u);
        CHECK_EQ(m.cpu.step(), 10u);
        CHECK_EQ(m.cpu.step(), 11u);
        CHECK_EQ(m.cpu.step(), 17u);
        CHECK_EQ(m.cpu.step(), 10u);
        CHECK_EQ(m.cpu.cycles(), std::uint64_t{4 + 7 + 10 + 11 + 17 + 10});
    }
    {
        Machine m;  // DJNZ: 13 when taken, 8 when not
        m.load("ORG &100\nLD B,2\nloop: DJNZ loop\n");
        m.cpu.step();
        CHECK_EQ(m.cpu.step(), 13u);
        CHECK_EQ(m.cpu.step(), 8u);
    }
    {
        Machine m;  // LDIR: 21 per repeated iteration, 16 for the last
        m.load("ORG &100\nLD HL,&300\nLD DE,&400\nLD BC,3\nLDIR\n");
        m.steps(3);
        const auto before = m.cpu.cycles();
        m.steps(3);
        CHECK_EQ(m.cpu.cycles() - before, std::uint64_t{21 + 21 + 16});
    }
    {
        Machine m;  // indexed addressing costs 19/23 T-states
        m.load("ORG &100\nLD A,(IX+5)\nINC (IX+5)\nLD (IY+1),&7\n");
        CHECK_EQ(m.cpu.step(), 19u);
        CHECK_EQ(m.cpu.step(), 23u);
        CHECK_EQ(m.cpu.step(), 19u);
    }

    // ---- block copy, indexed, undocumented ----
    {
        Machine m;
        m.load("ORG &100\nLD HL,&300\nLD DE,&400\nLD BC,4\nLDIR\nHALT\nORG &300\nDB 1,2,3,4\n");
        const RunResult r = m.cpu.run();
        CHECK(r.reason == StopReason::Halted);
        for (int i = 0; i < 4; ++i) CHECK_EQ(m.memory.read(static_cast<Word>(0x400 + i)), static_cast<Byte>(i + 1));
        CHECK_EQ(m.cpu.regs().bc(), Word{0});
    }
    {
        Machine m;
        m.load("ORG &100\nLD IX,&500\nLD (IX+3),&AB\nLD A,(IX+3)\nLD (IX-1),A\n");
        m.steps(4);
        CHECK_EQ(m.memory.read(0x503), Byte{0xAB});
        CHECK_EQ(m.memory.read(0x4FF), Byte{0xAB});
    }
    {
        Machine m;  // undocumented IXH/IXL: LD IXH,5 / LD IXL,6 / LD A,IXH
        m.load("ORG &100\nDB &DD,&26,5, &DD,&2E,6, &DD,&7C\n");
        m.steps(3);
        CHECK_EQ(m.cpu.regs().ix, Word{0x0506});
        CHECK_EQ(m.cpu.regs().a(), Byte{0x05});
    }
    {
        Machine m;  // undocumented SLL
        m.load("ORG &100\nLD B,&81\nDB &CB,&30\n");
        m.steps(2);
        CHECK_EQ(m.cpu.regs().main[0], Byte{0x03});
        CHECK((m.cpu.regs().f() & flag::C) != 0);
    }

    // ---- calls, returns, running ----
    {
        Machine m;
        m.load("ORG &100\nCALL fn\nHALT\nfn: LD A,42\nRET\n");
        const RunResult r = m.cpu.run();
        CHECK(r.reason == StopReason::Halted);
        CHECK_EQ(m.cpu.regs().a(), Byte{42});
        CHECK_EQ(m.cpu.regs().sp, Word{0xFFFF});
        CHECK_EQ(r.instructions, std::uint64_t{4});
    }
    {
        Machine m;  // run_until_return: from inside a subroutine
        m.load("ORG &100\nCALL fn\nHALT\nfn: LD B,3\nloop: DJNZ loop\nRET\n");
        m.cpu.addBreakpoint(0x104);  // fn
        CHECK(m.cpu.run().reason == StopReason::Breakpoint);
        CHECK_EQ(m.cpu.regs().pc, Word{0x104});
        const RunResult r = m.cpu.runUntilReturn();
        CHECK(r.reason == StopReason::Returned);
        CHECK_EQ(m.cpu.regs().pc, Word{0x103});  // instruction after the CALL
    }
    {
        Machine m;  // nested: returning from an inner call does not end the outer routine
        m.load("ORG &100\nCALL outer\nHALT\nouter: CALL inner\nINC B\nRET\ninner: RET\n");
        m.cpu.step();  // into outer
        const RunResult r = m.cpu.runUntilReturn();
        CHECK(r.reason == StopReason::Returned);
        CHECK_EQ(m.cpu.regs().main[0], Byte{1});  // INC B ran in the outer routine
        CHECK_EQ(m.cpu.regs().pc, Word{0x103});
    }
    {
        Machine m;  // step over
        m.load("ORG &100\nCALL fn\nNOP\nHALT\nfn: LD A,9\nNOP\nRET\n");
        const RunResult r = m.cpu.stepOver();
        CHECK(r.reason == StopReason::Stepped);
        CHECK_EQ(m.cpu.regs().pc, Word{0x103});
        CHECK_EQ(m.cpu.regs().a(), Byte{9});
        CHECK_EQ(r.instructions, std::uint64_t{4});
        // stepping over a non-call is a single step
        const RunResult r2 = m.cpu.stepOver();
        CHECK_EQ(r2.instructions, std::uint64_t{1});
        CHECK_EQ(m.cpu.regs().pc, Word{0x104});
    }
    {
        Machine m;  // a breakpoint inside the callee interrupts step over
        m.load("ORG &100\nCALL fn\nHALT\nfn: NOP\nNOP\nRET\n");
        m.cpu.addBreakpoint(0x105);
        CHECK(m.cpu.stepOver().reason == StopReason::Breakpoint);
        CHECK_EQ(m.cpu.regs().pc, Word{0x105});
    }

    // ---- breakpoints, limits, watchpoints, trace ----
    {
        Machine m;
        m.load("ORG &100\nNOP\nNOP\nNOP\nNOP\nHALT\n");
        m.cpu.addBreakpoint(0x102);
        CHECK(m.cpu.run().reason == StopReason::Breakpoint);
        CHECK_EQ(m.cpu.regs().pc, Word{0x102});
        CHECK(m.cpu.run().reason == StopReason::Halted);  // resumes past the breakpoint it sits on
        m.cpu.regs().pc = 0x100;
        m.cpu.regs().halted = false;
        m.cpu.clearBreakpoints();
        CHECK(m.cpu.runUntil(0x103).reason == StopReason::Address);
        CHECK_EQ(m.cpu.regs().pc, Word{0x103});
    }
    {
        Machine m;
        m.load("ORG &100\nloop: JR loop\n");
        RunLimits limits;
        limits.maxInstructions = 50;
        const RunResult r = m.cpu.run(limits);
        CHECK(r.reason == StopReason::InstructionLimit);
        CHECK_EQ(r.instructions, std::uint64_t{50});
        limits.maxInstructions = 0;
        limits.maxCycles = 100;
        CHECK(m.cpu.run(limits).reason == StopReason::CycleLimit);
    }
    {
        Machine m;
        m.load("ORG &100\nLD A,1\nLD (&800),A\nLD A,(&801)\nHALT\n");
        m.cpu.addWatchpoint(0x800, WatchKind::Write);
        m.cpu.addWatchpoint(0x801, WatchKind::Write);  // reading 0x801 must not trigger a write watch
        const RunResult r = m.cpu.run();
        CHECK(r.reason == StopReason::Watchpoint);
        CHECK(r.watchAddress.has_value() && *r.watchAddress == 0x800 && r.watchWrite);
        CHECK(m.cpu.run().reason == StopReason::Halted);
    }
    {
        Machine m;
        m.load("ORG &100\nNOP\nNOP\nHALT\n");
        int calls = 0;
        m.cpu.setTrace([&](const Cpu&) { ++calls; });
        m.cpu.run();
        CHECK_EQ(calls, 3);
    }

    // ---- interrupts ----
    {
        Machine m;  // IM 1: EI, then the interrupt is taken after the *next* instruction
        m.load("ORG &100\nIM 1\nEI\nNOP\nNOP\nNOP\nORG &38\nINC B\nEI\nRETI\n");
        m.cpu.regs().sp = 0x9000;
        m.steps(2);  // IM 1, EI
        m.cpu.requestInterrupt();
        m.cpu.step();  // NOP (still protected by the EI delay)
        CHECK_EQ(m.cpu.regs().pc, Word{0x104});
        m.cpu.step();  // interrupt acknowledge
        CHECK_EQ(m.cpu.regs().pc, Word{0x38});
        CHECK(!m.cpu.regs().iff1);
        m.steps(3);  // INC B, EI, RETI
        CHECK_EQ(m.cpu.regs().pc, Word{0x104});
        CHECK_EQ(m.cpu.regs().main[0], Byte{1});
        CHECK_EQ(m.cpu.regs().sp, Word{0x9000});
    }
    {
        Machine m;  // IM 2 vector table
        m.load("ORG &100\nLD A,&80\nLD I,A\nIM 2\nEI\nNOP\nNOP\nORG &8010\nDW &0300\nORG &300\nINC C\nRETI\n");
        m.cpu.regs().sp = 0x9000;
        m.steps(5);
        m.cpu.requestInterrupt(0x10);
        m.cpu.step();
        CHECK_EQ(m.cpu.regs().pc, Word{0x300});
    }
    {
        Machine m;  // masked interrupt stays pending
        m.load("ORG &100\nDI\nNOP\nNOP\n");
        m.cpu.requestInterrupt();
        m.steps(3);
        CHECK_EQ(m.cpu.regs().pc, Word{0x103});
        CHECK(m.cpu.state().irqPending);
    }
    {
        Machine m;  // NMI jumps to 0x66 and RETN restores IFF1
        m.load("ORG &100\nEI\nNOP\nNOP\nORG &66\nRETN\n");
        m.cpu.regs().sp = 0x9000;
        m.steps(2);
        m.cpu.requestNmi();
        m.cpu.step();
        CHECK_EQ(m.cpu.regs().pc, Word{0x66});
        CHECK(!m.cpu.regs().iff1);
        CHECK(m.cpu.regs().iff2);
        m.cpu.step();
        CHECK(m.cpu.regs().iff1);
        CHECK_EQ(m.cpu.regs().pc, Word{0x102});
    }
    {
        Machine m;  // HALT: stops the run; an interrupt wakes the CPU
        m.load("ORG &100\nIM 1\nEI\nHALT\nINC B\nHALT\nORG &38\nRETI\n");
        m.cpu.regs().sp = 0x9000;
        CHECK(m.cpu.run().reason == StopReason::Halted);
        m.cpu.requestInterrupt();
        m.cpu.step();  // accepted: CPU leaves HALT
        CHECK(!m.cpu.regs().halted);
        CHECK(m.cpu.run().reason == StopReason::Halted);  // woke up, ran INC B, halted again
        CHECK_EQ(m.cpu.regs().main[0], Byte{1});
    }

    // ---- I/O ports ----
    {
        Machine m;
        std::vector<std::pair<Word, Byte>> out;
        m.cpu.setPortHandlers([](Word port) -> Byte { return static_cast<Byte>(port & 0xFF) + 1; },
                              [&](Word port, Byte v) { out.emplace_back(port, v); });
        m.load("ORG &100\nLD A,&12\nOUT (&FE),A\nIN A,(&10)\nLD BC,&1234\nOUT (C),A\n");
        m.steps(5);
        CHECK_EQ(out.size(), std::size_t{2});
        CHECK(out[0] == std::make_pair(Word{0x12FE}, Byte{0x12}));  // high byte of the port is A
        CHECK_EQ(m.cpu.regs().a(), Byte{0x11});
        CHECK(out[1] == std::make_pair(Word{0x1234}, Byte{0x11}));
    }

    // ---- register access by name ----
    {
        Registers r;
        CHECK(setRegister(r, "hl", 0x1234));
        CHECK_EQ(r.hl(), Word{0x1234});
        CHECK_EQ(*getRegister(r, "H"), 0x12u);
        CHECK(setRegister(r, "AF'", 0xABCD));
        CHECK_EQ(r.af2(), Word{0xABCD});
        CHECK(setRegister(r, "ixl", 0x7F));
        CHECK_EQ(r.ix, Word{0x007F});
        CHECK(!setRegister(r, "A", 256));
        CHECK(!setRegister(r, "nonsense", 1));
        CHECK(!getRegister(r, "nonsense").has_value());
        CHECK(!setRegister(r, "IM", 3));
        CHECK_EQ(flagsToString(0x00), std::string("........"));
        CHECK_EQ(flagsToString(0xFF), std::string("SZYHXPNC"));
        CHECK_EQ(flagsToString(flag::Z | flag::C), std::string(".Z.....C"));
    }

    // ---- snapshots and state serialisation ----
    {
        Machine m;
        m.load("ORG &100\nLD HL,&1234\nEXX\nLD A,&55\nEX AF,AF'\nEI\nPUSH HL\nHALT\n");
        m.cpu.run();
        m.memory.write(0xC000, 0x99);

        const Snapshot snap = captureSnapshot(m.cpu);
        const auto bytes = serializeSnapshot(snap);
        CHECK_EQ(bytes.size(), std::size_t{8 + 16 + 10 + 3 + 1 + 1 + 16 + kAddressSpace});
        const Snapshot back = deserializeSnapshot(bytes);
        CHECK(back == snap);

        // restoring really restores: change everything, then roll back
        m.cpu.regs().setHl(0);
        m.memory.write(0xC000, 0);
        m.cpu.step();
        restoreSnapshot(m.cpu, back);
        CHECK(m.cpu.state() == snap.state);
        CHECK_EQ(m.memory.read(0xC000), Byte{0x99});

        // execution continues identically from a restored state
        Machine a, b;
        a.load("ORG &100\nLD B,10\nloop: INC A\nDJNZ loop\nHALT\n");
        b.memory = a.memory;
        a.cpu.regs() = Registers{};
        a.cpu.regs().pc = 0x100;
        a.steps(7);
        const Snapshot mid = captureSnapshot(a.cpu);
        a.cpu.run();
        restoreSnapshot(b.cpu, mid);
        b.cpu.run();
        CHECK(a.cpu.state() == b.cpu.state());
        CHECK(captureSnapshot(a.cpu) == captureSnapshot(b.cpu));

        // damaged input is rejected, not trusted
        auto bad = bytes;
        bad[0] = 'X';
        bool threw = false;
        try { deserializeSnapshot(bad); } catch (const std::exception&) { threw = true; }
        CHECK(threw);
        threw = false;
        try { deserializeSnapshot(std::vector<Byte>(bytes.begin(), bytes.end() - 1)); } catch (const std::exception&) { threw = true; }
        CHECK(threw);
        auto badMode = bytes;
        badMode[8 + 16 + 10 + 1 + 1] = 7;  // interrupt mode > 2
        threw = false;
        try { deserializeSnapshot(badMode); } catch (const std::exception&) { threw = true; }
        CHECK(threw);
    }
    {
        Machine m;
        m.load("ORG &100\nLD HL,&BEEF\nEXX\nLD IX,&1234\nEI\nNOP\n");
        m.steps(5);
        m.cpu.requestInterrupt(0x20);
        const CpuState st = m.cpu.state();
        const CpuState back = stateFromJson(Json::parse(stateToJson(st).dump()));
        CHECK(back == st);
        // numbers as strings are accepted, unknown members ignored, bad values rejected
        const CpuState parsed = stateFromJson(Json::parse(R"({"PC":"0x4000","SP":"&8000","A":7,"foo":1,"IM":2})"));
        CHECK_EQ(parsed.regs.pc, Word{0x4000});
        CHECK_EQ(parsed.regs.sp, Word{0x8000});
        CHECK_EQ(parsed.regs.a(), Byte{7});
        CHECK_EQ(parsed.regs.im, Byte{2});
        bool threw = false;
        try { stateFromJson(Json::parse(R"({"PC":70000})")); } catch (const JsonError&) { threw = true; }
        CHECK(threw);
    }

    // ---- random code: never crashes, and a restored snapshot replays identically ----
    {
        std::mt19937 rng(0xBEEF);
        for (int iteration = 0; iteration < 40; ++iteration) {
            Machine m;
            for (std::size_t a = 0; a < kAddressSpace; ++a) m.memory.write(static_cast<Word>(a), static_cast<Byte>(rng()));
            Registers& r = m.cpu.regs();
            for (Byte& b : r.main) b = static_cast<Byte>(rng());
            r.ix = static_cast<Word>(rng());
            r.iy = static_cast<Word>(rng());
            r.sp = static_cast<Word>(rng());
            r.pc = static_cast<Word>(rng());
            r.im = static_cast<Byte>(rng() % 3);
            r.iff1 = r.iff2 = rng() % 2;
            if (iteration % 4 == 0) m.cpu.requestInterrupt(static_cast<Byte>(rng()));
            const Snapshot start = captureSnapshot(m.cpu);

            RunLimits limits;
            limits.maxInstructions = 20000;
            m.cpu.run(limits);
            const Snapshot first = captureSnapshot(m.cpu);

            restoreSnapshot(m.cpu, start);
            m.cpu.run(limits);
            ++g_checks;
            if (!(captureSnapshot(m.cpu) == first)) {
                ++g_failures;
                std::cerr << "iteration " << iteration << ": replay from snapshot diverged\n";
            }
        }
    }

    return finish("cpu");
}
