#!/usr/bin/env python3
"""Three host threads exercise the production no-JIT decode cache (FastInst/FastDecode from PPCInterpreterMain.cpp),
as the three interpreter cores do. Every slot a reader accepts as current must hold exactly the decode of its opcode,
also while pages are invalidated and re-decoded under the readers.
usage: InterpreterFastSlotPublicationTests.py [repo root]   (default: this checkout)"""
from pathlib import Path
import subprocess, sys, tempfile

repo = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[3]
src = (repo / 'src/Cafe/HW/Espresso/Interpreter/PPCInterpreterMain.cpp').read_text()

def block(signature, tail=''):
    start = src.index(signature)
    end, depth = src.index('{', start) + 1, 1
    while depth:
        depth += (src[end] == '{') - (src[end] == '}')
        end += 1
    return src[start:end] + tail

parts = [block('enum FastKind', ';'), block('struct FastInst', ';'), block('struct FastPage', ';'), block('void FastInvalidatePage')]
current = 'FastSlotCurrent(inst, epoch)'
if 'inline bool FastSlotCurrent' in src:
    parts.append(block('inline bool FastSlotCurrent'))
else:  # reader check of the code before the fix, kept so this test can show the old race
    current = '!(inst.kind == kNeedDecode || inst.epoch.load(std::memory_order_relaxed) != epoch)'
parts.append(block('void FastDecode'))
if 'FastInst* FastCoreSlots' in src:
    parts.append(block('FastInst* FastCoreSlots'))

fixture = r'''
#include <algorithm>
#include <cassert>
#include <memory>
#include <atomic>
#define cemu_assert_debug assert
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>
using uint8 = uint8_t; using uint32 = uint32_t; using sint16 = int16_t; using sint32 = int32_t;
struct PPCInterpreter_t;
#define PPC_OPC_LK 1
#define PPC_OPC_AA (1<<1)
static inline uint32 ppc_mask(int MB, int ME)
{
	uint32 maskMB = 0xFFFFFFFF >> MB, maskME = 0xFFFFFFFF << (31 - ME);
	return (MB <= ME) ? maskMB & maskME : maskMB | maskME;
}
static void handlerA(PPCInterpreter_t*, uint32) {}
static void handlerB(PPCInterpreter_t*, uint32) {}
void (*PPCInterpreterSlim_handlerFor(uint32 opcode))(PPCInterpreter_t*, uint32) { return (opcode & 0x100) ? &handlerA : ((opcode & 0x200) ? &handlerB : nullptr); }
std::atomic<struct FastPage*> g_fastPages[16] = {};
'''
fixture += '\n'.join(parts) + '\n'
fixture += r'''
struct Ref { uint8 kind, rD, rA, rB, flags; uint32 imm, opcode; void (*fn)(PPCInterpreter_t*, uint32); };
int main()
{
	// mix of fast kinds (addi, rlwinm, lwz, stw, bc, cmpi, lfs, ps_madd, x-form) and fallback opcodes
	const uint32 seeds[] = {0x38630010, 0x5463103a, 0x80830004, 0x90830008, 0x4182fff0, 0x2c030000, 0xc0230000,
	                        0x1022185c, 0x7c632214, 0x7c0802a6, 0xfc20f890, 0x7c6319d6, 0x48000001, 0x4e800020};
	std::vector<uint32> code(1024);
	std::mt19937 rng(77);
	for (auto& c : code) c = seeds[rng() % (sizeof(seeds) / 4)] ^ ((rng() & 0x3) << 21) ^ (rng() & 0x300);
	std::vector<Ref> ref(1024);
	for (uint32 i = 0; i < 1024; i++)
	{
		FastInst t;
		FastDecode(t, code[i], 1);
		ref[i] = {t.kind, t.rD, t.rA, t.rB, t.flags, t.imm, t.opcode, t.fn};
	}
	constexpr int kThreads = 3, kRounds = 6000;
	std::atomic<int> round{-1}, done{0};
	std::atomic<uint64_t> torn{0}, accepted{0}, decodes{0};
	auto core = [&](int id)
	{
		std::mt19937 r(id);
		std::vector<uint32> order(1024);
		for (uint32 i = 0; i < 1024; i++) order[i] = i;
		for (int n = 0; n < kRounds; n++)
		{
			while (round.load(std::memory_order_acquire) < n) {}
			std::shuffle(order.begin(), order.end(), r);
			FastPage* page = g_fastPages[0].load(std::memory_order_acquire);
			uint32 epoch = page->epoch.load(std::memory_order_acquire);
			for (uint32 k = 0; k < 1024; k++)
			{
				if (id == 0 && n % 3 == 0 && k == 512)
				{
					FastInvalidatePage(0); // unchanged code, as after a store into a code page
					epoch = page->epoch.load(std::memory_order_acquire);
				}
				const uint32 index = order[k];
				FastInst& inst = SLOT_ACCESS;
				if (!(CURRENT_CHECK))
				{
					FastDecode(inst, code[index], epoch);
					decodes.fetch_add(1, std::memory_order_relaxed);
				}
				// same field order as PPCInterpreterFast_executeSlice
				const uint8 rD = inst.rD, rA = inst.rA, rB = inst.rB; const uint32 imm = inst.imm;
				const uint8 kind = inst.kind, flags = inst.flags; const uint32 opcode = inst.opcode; auto fn = inst.fn;
				const Ref& e = ref[index];
				if (kind != e.kind || rD != e.rD || rA != e.rA || rB != e.rB || imm != e.imm || flags != e.flags || opcode != e.opcode || fn != e.fn)
					torn.fetch_add(1, std::memory_order_relaxed);
				accepted.fetch_add(1, std::memory_order_relaxed);
			}
			done.fetch_add(1, std::memory_order_acq_rel);
		}
	};
	std::vector<std::thread> threads;
	for (int i = 0; i < kThreads; i++) threads.emplace_back(core, i);
	std::vector<FastPage*> retired;
	for (int n = 0; n < kRounds; n++)
	{
		retired.push_back(g_fastPages[0].exchange(new FastPage(), std::memory_order_acq_rel)); // fresh page: all slots need a decode
		round.store(n, std::memory_order_release);
		while (done.load(std::memory_order_acquire) < (n + 1) * kThreads) {}
	}
	for (auto& t : threads) t.join();
	for (auto* p : retired) delete p;
	std::printf("%s: %d threads x %d fresh pages x 1024 slots, %llu accepted reads, %llu decodes, %llu torn slots\n",
	            torn.load() ? "FAIL" : "PASS", kThreads, kRounds, (unsigned long long)accepted.load(), (unsigned long long)decodes.load(), (unsigned long long)torn.load());
	return torn.load() ? 1 : 0;
}
'''
fixture = fixture.replace('CURRENT_CHECK', current)
fixture = fixture.replace('SLOT_ACCESS', 'FastCoreSlots(*page, id)[index]' if 'FastInst* FastCoreSlots' in src else 'page->inst[index]')
with tempfile.TemporaryDirectory() as d:
    p = Path(d)
    (p / 'test.cpp').write_text(fixture)
    subprocess.run(['xcrun', 'clang++', '-std=c++20', '-O2', *(['-fsanitize=thread', '-g'] if '--tsan' in sys.argv else []), '-pthread', str(p / 'test.cpp'), '-o', str(p / 'test')], check=True)
    sys.exit(subprocess.run([str(p / 'test')]).returncode)
