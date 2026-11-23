#include "PPCInterpreterInternal.h"
#include "PPCInterpreterHelper.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "Cafe/HW/Espresso/Debugger/Debugger.h"
#include "Cafe/HW/MMU/MMU.h"
#include <atomic>
#include <bit>
#include <mutex>
#include <memory>

thread_local PPCInterpreter_t* ppcInterpreterCurrentInstance;

// main thread instruction counter and timing
uint64 ppcMainThreadDECCycleValue = 0; // value that was set to dec register
uint64 ppcMainThreadDECCycleStart = 0; // at which cycle the dec register was set, if == 0 -> dec is 0
uint64 ppcCyclesSince2000 = 0;
uint64 ppcCyclesSince2000TimerClock = 0;
uint64 ppcCyclesSince2000_UTC = 0;

PPCInterpreter_t* PPCInterpreter_createInstance(unsigned int Entrypoint)
{
	PPCInterpreter_t* pData;
	// create instance
	uint32 prefixAreaSize = 0x6000; // we need to allocate some bytes before the interpreter struct because the recompiler will use it as stack area (specifically when the exception handler is called)
	pData = (PPCInterpreter_t*)((uint8*)malloc(sizeof(PPCInterpreter_t)+prefixAreaSize)+prefixAreaSize);
	memset((void*)pData, 0x00, sizeof(PPCInterpreter_t));
	// set instruction pointer to entrypoint
	pData->instructionPointer = (uint32)Entrypoint;
	// set initial register values
	pData->gpr[GPR_SP] = 0x00000000;
	pData->spr.LR = 0;
	// return instance
	return pData;
}

TLS_WORKAROUND_NOINLINE PPCInterpreter_t* PPCInterpreter_getCurrentInstance()
{
	return ppcInterpreterCurrentInstance;
}

TLS_WORKAROUND_NOINLINE void PPCInterpreter_setCurrentInstance(PPCInterpreter_t* hCPU)
{
	ppcInterpreterCurrentInstance = hCPU;
}

uint64 PPCInterpreter_getMainCoreCycleCounter()
{
	return PPCTimer_getFromRDTSC();
}

void PPCInterpreter_nextInstruction(PPCInterpreter_t* cpuInterpreter)
{
	cpuInterpreter->instructionPointer += 4;
}

void PPCInterpreter_jumpToInstruction(PPCInterpreter_t* cpuInterpreter, uint32 newIP)
{
	cpuInterpreter->instructionPointer = (uint32)newIP;
}

void PPCInterpreter_setDEC(PPCInterpreter_t* hCPU, uint32 newValue)
{
	hCPU->sprExtended.DEC = newValue;
	ppcMainThreadDECCycleStart = PPCInterpreter_getMainCoreCycleCounter();
	ppcMainThreadDECCycleValue = newValue;
}

uint32 PPCInterpreter_getXER(PPCInterpreter_t* hCPU)
{
	uint32 xerValue = hCPU->spr.XER;
	xerValue &= ~(1 << XER_BIT_CA);
	xerValue &= ~(1 << XER_BIT_SO);
	xerValue &= ~(1 << XER_BIT_OV);
	if (hCPU->xer_ca)
		xerValue |= (1 << XER_BIT_CA);
	if (hCPU->xer_so)
		xerValue |= (1 << XER_BIT_SO);
	if (hCPU->xer_ov)
		xerValue |= (1 << XER_BIT_OV);
	return xerValue;
}

void PPCInterpreter_setXER(PPCInterpreter_t* hCPU, uint32 v)
{
	const uint32 XER_MASK = 0xE0FFFFFF; // some bits are masked out. Figure out which ones exactly
	hCPU->spr.XER = v & XER_MASK;
	hCPU->xer_ca = (v >> XER_BIT_CA) & 1;
	hCPU->xer_so = (v >> XER_BIT_SO) & 1;
	hCPU->xer_ov = (v >> XER_BIT_OV) & 1;
}

uint32 PPCInterpreter_getCoreIndex(PPCInterpreter_t* hCPU)
{
	return hCPU->spr.UPIR;
};

uint32 PPCInterpreter_getCurrentCoreIndex()
{
	return PPCInterpreter_getCurrentInstance()->spr.UPIR;
};

uint8* PPCInterpreterGetStackPointer()
{
	return memory_getPointerFromVirtualOffset(PPCInterpreter_getCurrentInstance()->gpr[1]);
}

uint8* PPCInterpreter_PushAndReturnStackPointer(sint32 offset)
{
	PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
	uint8* result = memory_getPointerFromVirtualOffset(hCPU->gpr[1] - offset);
	hCPU->gpr[1] -= offset;
	return result;
}

void PPCInterpreterModifyStackPointer(sint32 offset)
{
	PPCInterpreter_getCurrentInstance()->gpr[1] -= offset;
}

uint32 RPLLoader_MakePPCCallable(void(*ppcCallableExport)(PPCInterpreter_t* hCPU));

// deprecated wrapper, use RPLLoader_MakePPCCallable directly
uint32 PPCInterpreter_makeCallableExportDepr(void (*ppcCallableExport)(PPCInterpreter_t* hCPU))
{
	return RPLLoader_MakePPCCallable(ppcCallableExport);
}

namespace
{
enum FastKind : uint8
{
	kNeedDecode = 0,
	kFallback,
	kNop,
	kAddi,
	kAddis,
	kOri,
	kOris,
	kAndi,
	kAndis,
	kRlwinm,
	kLwz,
	kStw,
	kLbz,
	kStb,
	kLhz,
	kSth,
	kLwzu,
	kStwu,
	kCmpi,
	kCmpli,
	kBx,
	kBc,
	kAdd,
	kOr,
	kAnd,
	kXor,
	kLwzx,
	kStwx,
	kCmp,
	kCmpl,
	kExtsb,
	kExtsh,
	kPsAdd,
	kPsSub,
	kPsMul,
	kPsMadd,
	kPsMsub,
	kPsNmadd,
	kPsNmsub,
	kLfs,
	kStfs,
	kBclr,
	kMflr,
	kMfctr,
	kMtlr,
	kMtctr,
	kBcctr,
	kLbzu,
	kStbu,
	kSubf,
	kMulli,
	kAddic,
	kAddicRc,
	kXori,
	kXoris,
	kLfd,
	kStfd,
};

struct FastInst
{
	void (*fn)(PPCInterpreter_t*, uint32){nullptr};
	std::atomic<uint32> epoch{0};
	uint32 opcode{0};
	uint8 kind{kNeedDecode};
	uint8 rD{0};
	uint8 rA{0};
	uint8 rB{0};
	uint8 flags{0};
	uint32 imm{0};
};

struct FastPage
{
	std::atomic<uint32> epoch{1};
	// Each core owns its decode data; invalidation only changes the shared epoch.
	std::unique_ptr<FastInst[]> inst[3];
};

std::atomic<FastPage*> g_fastPages[1u << 20] = {};
std::mutex g_fastPageLock;

uint32 FastLoadOpcode(uint32 pc)
{
	return _swapEndianU32(*reinterpret_cast<uint32*>(memory_base + pc));
}

void FastWriteU32(uint32 address, uint32 value)
{
	*reinterpret_cast<uint32*>(memory_base + address) = _swapEndianU32(value);
}

uint32 FastReadU32(uint32 address)
{
	return _swapEndianU32(*reinterpret_cast<uint32*>(memory_base + address));
}

uint16 FastReadU16(uint32 address)
{
	return _swapEndianU16(*reinterpret_cast<uint16*>(memory_base + address));
}

void FastWriteU16(uint32 address, uint16 value)
{
	*reinterpret_cast<uint16*>(memory_base + address) = _swapEndianU16(value);
}

void FastSetCR(PPCInterpreter_t* hCPU, uint32 cr, uint32 a, uint32 b, bool isSigned)
{
	const uint32 base = cr * 4;
	hCPU->cr[base + 0] = 0;
	hCPU->cr[base + 1] = 0;
	hCPU->cr[base + 2] = 0;
	hCPU->cr[base + 3] = 0;
	if (isSigned)
	{
		const sint32 sa = static_cast<sint32>(a);
		const sint32 sb = static_cast<sint32>(b);
		if (sa < sb)
			hCPU->cr[base + CR_BIT_LT] = 1;
		else if (sa > sb)
			hCPU->cr[base + CR_BIT_GT] = 1;
		else
			hCPU->cr[base + CR_BIT_EQ] = 1;
	}
	else if (a < b)
		hCPU->cr[base + CR_BIT_LT] = 1;
	else if (a > b)
		hCPU->cr[base + CR_BIT_GT] = 1;
	else
		hCPU->cr[base + CR_BIT_EQ] = 1;
	hCPU->cr[base + CR_BIT_SO] = hCPU->xer_so;
}

void FastInvalidatePage(uint32 address)
{
	FastPage* page = g_fastPages[address >> 12].load(std::memory_order_relaxed);
	if (page)
		page->epoch.fetch_add(1, std::memory_order_release);
}

void FastDecode(FastInst& inst, uint32 opcode, uint32 epoch)
{
	const uint32 primary = opcode >> 26;
	uint8 kind = kFallback;
	uint8 rD = static_cast<uint8>((opcode >> 21) & 31);
	uint8 rA = static_cast<uint8>((opcode >> 16) & 31);
	uint8 rB = static_cast<uint8>((opcode >> 11) & 31);
	uint8 flags = static_cast<uint8>(opcode & 1);
	uint32 imm = static_cast<uint32>(static_cast<sint32>(static_cast<sint16>(opcode & 0xffff)));

	switch (primary)
	{
	case 0: kind = kNop; break;
	case 7: kind = kMulli; break;
	case 12: kind = kAddic; break;
	case 13: kind = kAddicRc; break;
	case 26: kind = kXori; imm = opcode & 0xffff; break;
	case 27: kind = kXoris; imm = opcode << 16; break;
	case 35: kind = kLbzu; break;
	case 39: kind = kStbu; break;
	case 50: kind = kLfd; break;
	case 54: kind = kStfd; break;
	case 10: kind = kCmpli; imm = opcode & 0xffff; rD = static_cast<uint8>(rD >> 2); break;
	case 11: kind = kCmpi; rD = static_cast<uint8>(rD >> 2); break;
	case 14: kind = kAddi; break;
	case 15: kind = kAddis; imm = opcode << 16; break;
	case 16:
		kind = kBc;
		imm = static_cast<uint32>(static_cast<sint32>(static_cast<sint16>(opcode & 0xfffc)));
		flags = static_cast<uint8>(((opcode & PPC_OPC_LK) ? 1 : 0));
		break;
	case 18:
	{
		kind = kBx;
		uint32 li = opcode & 0x03fffffc;
		if (li & 0x02000000)
			li |= 0xfc000000;
		imm = li;
		flags = static_cast<uint8>(((opcode & PPC_OPC_LK) ? 1 : 0) | ((opcode & PPC_OPC_AA) ? 2 : 0));
		break;
	}
	case 21:
		kind = kRlwinm;
		imm = ppc_mask((opcode >> 6) & 31, (opcode >> 1) & 31);
		flags = static_cast<uint8>(opcode & 1);
		break;
	case 24: kind = kOri; imm = opcode & 0xffff; break;
	case 25: kind = kOris; imm = opcode << 16; break;
	case 28: kind = kAndi; imm = opcode & 0xffff; break;
	case 29: kind = kAndis; imm = opcode << 16; break;
	case 32: kind = kLwz; break;
	case 33: kind = kLwzu; break;
	case 34: kind = kLbz; break;
	case 36: kind = kStw; break;
	case 37: kind = kStwu; break;
	case 38: kind = kStb; break;
	case 40: kind = kLhz; break;
	case 44: kind = kSth; break;
	case 4:
	{
		const uint32 xo = (opcode >> 1) & 31;
		const uint8 frC = static_cast<uint8>((opcode >> 6) & 31);
		if (xo == 21) kind = kPsAdd;
		else if (xo == 20) kind = kPsSub;
		else if (xo == 25) { kind = kPsMul; flags = frC; }
		else if (xo == 29) { kind = kPsMadd; flags = frC; }
		else if (xo == 28) { kind = kPsMsub; flags = frC; }
		else if (xo == 31) { kind = kPsNmadd; flags = frC; }
		else if (xo == 30) { kind = kPsNmsub; flags = frC; }
		break;
	}
	case 19:
		if (((opcode >> 1) & 0x3ff) == 16)
			kind = kBclr;
		else if (((opcode >> 1) & 0x3ff) == 528)
			kind = kBcctr;
		break;
	case 48: kind = kLfs; break;
	case 52: kind = kStfs; break;
	case 31:
	{
		const uint32 xo = (opcode >> 1) & 0x3ff;
		if (xo == 0) { kind = kCmp; rD = static_cast<uint8>(rD >> 2); }
		else if (xo == 32) { kind = kCmpl; rD = static_cast<uint8>(rD >> 2); }
		else if (xo == 23) kind = kLwzx;
		else if (xo == 151) kind = kStwx;
		else if (xo == 266) kind = kAdd;
		else if (xo == 444) kind = kOr;
		else if (xo == 28) kind = kAnd;
		else if (xo == 316) kind = kXor;
		else if (xo == 954) kind = kExtsb;
		else if (xo == 922) kind = kExtsh;
		else if (xo == 40) kind = kSubf;
		else if (xo == 339 || xo == 467)
		{
			// only LR (8) and CTR (9); every other SPR keeps the full handler
			const uint32 spr = rA | (static_cast<uint32>(rB) << 5);
			if (spr == 8) kind = xo == 339 ? kMflr : kMtlr;
			else if (spr == 9) kind = xo == 339 ? kMfctr : kMtctr;
		}
		break;
	}
	default:
		break;
	}

	inst.rD = rD;
	inst.rA = rA;
	inst.rB = rB;
	inst.flags = flags;
	inst.imm = imm;
	inst.opcode = opcode;
	inst.fn = kind == kFallback ? PPCInterpreterSlim_handlerFor(opcode) : nullptr;
	inst.kind = kind;
	inst.epoch.store(epoch, std::memory_order_release);
}

FastInst* FastCoreSlots(FastPage& page, uint32 core)
{
	cemu_assert_debug(core < 3);
	auto& slots = page.inst[core];
	if (!slots)
		slots = std::make_unique<FastInst[]>(1024);
	return slots.get();
}
}

void PPCInterpreterFast_invalidateRange(uint32 startAddress, uint32 endAddress)
{
	if (endAddress < startAddress)
		return;
	uint32 page = startAddress & ~0xFFFu;
	const uint32 last = endAddress & ~0xFFFu;
	while (true)
	{
		FastInvalidatePage(page);
		if (page == last || page > 0xFFFFF000u - 0x1000u)
			break;
		page += 0x1000u;
	}
}

void PPCInterpreterFast_executeSlice(PPCInterpreter_t* hCPU)
{
	static std::once_flag s_logged;
	std::call_once(s_logged, [] { cemuLog_log(LogType::Force, "No-JIT cached interpreter active"); });

	while (hCPU->remainingCycles > 0)
	{
		uint32 pc = hCPU->instructionPointer;
		FastPage* page = g_fastPages[pc >> 12].load(std::memory_order_acquire);
		if (!page)
		{
			std::lock_guard<std::mutex> lock(g_fastPageLock);
			page = g_fastPages[pc >> 12].load(std::memory_order_relaxed);
			if (!page)
			{
				page = new FastPage();
				g_fastPages[pc >> 12].store(page, std::memory_order_release);
			}
		}
		const uint32 core = PPCInterpreter_getCoreIndex(hCPU);
		FastInst* slots = FastCoreSlots(*page, core);
		uint32 epoch = page->epoch.load(std::memory_order_acquire);
		uint32 index = (pc >> 2) & 1023;
		while (index < 1024 && (--hCPU->remainingCycles) >= 0)
		{
		FastInst& inst = slots[index];
		if (inst.kind == kNeedDecode || inst.epoch.load(std::memory_order_relaxed) != epoch)
			FastDecode(inst, FastLoadOpcode(pc), epoch);
		hCPU->instructionPointer = pc;

		const uint8 rD = inst.rD;
		const uint8 rA = inst.rA;
		const uint8 rB = inst.rB;
		const uint32 imm = inst.imm;
		const bool fallback = inst.kind == kFallback;
		switch (inst.kind)
		{
		case kNop:
			hCPU->instructionPointer = pc + 4;
			break;
		case kAddi:
			hCPU->gpr[rD] = (rA ? hCPU->gpr[rA] : 0) + imm;
			hCPU->instructionPointer = pc + 4;
			break;
		case kAddis:
			hCPU->gpr[rD] = (rA ? hCPU->gpr[rA] : 0) + imm;
			hCPU->instructionPointer = pc + 4;
			break;
		case kOri:
		case kOris:
			hCPU->gpr[rA] = hCPU->gpr[rD] | imm;
			hCPU->instructionPointer = pc + 4;
			break;
		case kAndi:
		case kAndis:
			hCPU->gpr[rA] = hCPU->gpr[rD] & imm;
			ppc_update_cr0(hCPU, hCPU->gpr[rA]);
			hCPU->instructionPointer = pc + 4;
			break;
		case kRlwinm:
		{
			const uint32 rotated = std::rotl<uint32>(hCPU->gpr[rD], rB);
			hCPU->gpr[rA] = rotated & imm;
			if (inst.flags)
				ppc_update_cr0(hCPU, hCPU->gpr[rA]);
			hCPU->instructionPointer = pc + 4;
			break;
		}
		case kLwz:
			hCPU->gpr[rD] = FastReadU32((rA ? hCPU->gpr[rA] : 0) + imm);
			hCPU->instructionPointer = pc + 4;
			break;
		case kLwzu:
			hCPU->gpr[rA] = hCPU->gpr[rA] + imm;
			hCPU->gpr[rD] = FastReadU32(hCPU->gpr[rA]);
			hCPU->instructionPointer = pc + 4;
			break;
		case kStw:
			if (rA)
			{
				const uint32 ea = hCPU->gpr[rA] + imm;
				FastWriteU32(ea, hCPU->gpr[rD]);
				FastInvalidatePage(ea);
			}
			hCPU->instructionPointer = pc + 4;
			break;
		case kStwu:
		{
			const uint32 ea = hCPU->gpr[rA] + imm;
			FastWriteU32(ea, hCPU->gpr[rD]);
			FastInvalidatePage(ea);
			hCPU->gpr[rA] = ea;
			hCPU->instructionPointer = pc + 4;
			break;
		}
		case kLbz:
			hCPU->gpr[rD] = memory_base[(rA ? hCPU->gpr[rA] : 0) + imm];
			hCPU->instructionPointer = pc + 4;
			break;
		case kStb:
		{
			const uint32 ea = (rA ? hCPU->gpr[rA] : 0) + imm;
			memory_base[ea] = static_cast<uint8>(hCPU->gpr[rD]);
			FastInvalidatePage(ea);
			hCPU->instructionPointer = pc + 4;
			break;
		}
		case kLhz:
			hCPU->gpr[rD] = FastReadU16((rA ? hCPU->gpr[rA] : 0) + imm);
			hCPU->instructionPointer = pc + 4;
			break;
		case kSth:
		{
			const uint32 ea = (rA ? hCPU->gpr[rA] : 0) + imm;
			FastWriteU16(ea, static_cast<uint16>(hCPU->gpr[rD]));
			FastInvalidatePage(ea);
			hCPU->instructionPointer = pc + 4;
			break;
		}
		case kCmpi:
			FastSetCR(hCPU, rD, hCPU->gpr[rA], imm, true);
			hCPU->instructionPointer = pc + 4;
			break;
		case kCmpli:
			FastSetCR(hCPU, rD, hCPU->gpr[rA], imm, false);
			hCPU->instructionPointer = pc + 4;
			break;
		case kBx:
		{
			uint32 target = imm;
			if ((inst.flags & 2) == 0)
				target += pc;
			if (inst.flags & 1)
				hCPU->spr.LR = pc + 4;
			hCPU->instructionPointer = target;
			break;
		}
		case kBc:
		{
			const uint32 BO = rD;
			if ((BO & 4) == 0)
				hCPU->spr.CTR--;
			const bool bo2 = (BO & 2) != 0;
			const bool bo8 = (BO & 8) != 0;
			const bool cr = hCPU->cr[rA] != 0;
			if (((BO & 4) || ((hCPU->spr.CTR != 0) ^ bo2)) && ((BO & 16) || (!(cr ^ bo8))))
			{
				if (inst.flags & 1)
					hCPU->spr.LR = pc + 4;
				hCPU->instructionPointer = pc + imm;
			}
			else
				hCPU->instructionPointer = pc + 4;
			break;
		}
		case kAdd:
			hCPU->gpr[rD] = hCPU->gpr[rA] + hCPU->gpr[rB];
			if (inst.flags)
				ppc_update_cr0(hCPU, hCPU->gpr[rD]);
			hCPU->instructionPointer = pc + 4;
			break;
		case kOr:
			hCPU->gpr[rA] = hCPU->gpr[rD] | hCPU->gpr[rB];
			if (inst.flags)
				ppc_update_cr0(hCPU, hCPU->gpr[rA]);
			hCPU->instructionPointer = pc + 4;
			break;
		case kAnd:
			hCPU->gpr[rA] = hCPU->gpr[rD] & hCPU->gpr[rB];
			if (inst.flags)
				ppc_update_cr0(hCPU, hCPU->gpr[rA]);
			hCPU->instructionPointer = pc + 4;
			break;
		case kXor:
			hCPU->gpr[rA] = hCPU->gpr[rD] ^ hCPU->gpr[rB];
			if (inst.flags)
				ppc_update_cr0(hCPU, hCPU->gpr[rA]);
			hCPU->instructionPointer = pc + 4;
			break;
		case kLwzx:
			hCPU->gpr[rD] = FastReadU32((rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB]);
			hCPU->instructionPointer = pc + 4;
			break;
		case kStwx:
		{
			const uint32 ea = (rA ? hCPU->gpr[rA] : 0) + hCPU->gpr[rB];
			FastWriteU32(ea, hCPU->gpr[rD]);
			FastInvalidatePage(ea);
			hCPU->instructionPointer = pc + 4;
			break;
		}
		case kCmp:
			FastSetCR(hCPU, rD, hCPU->gpr[rA], hCPU->gpr[rB], true);
			hCPU->instructionPointer = pc + 4;
			break;
		case kCmpl:
			FastSetCR(hCPU, rD, hCPU->gpr[rA], hCPU->gpr[rB], false);
			hCPU->instructionPointer = pc + 4;
			break;
		case kExtsb:
			hCPU->gpr[rA] = static_cast<uint32>(static_cast<sint32>(static_cast<sint8>(hCPU->gpr[rD])));
			if (inst.flags)
				ppc_update_cr0(hCPU, hCPU->gpr[rA]);
			hCPU->instructionPointer = pc + 4;
			break;
		case kExtsh:
			hCPU->gpr[rA] = static_cast<uint32>(static_cast<sint32>(static_cast<sint16>(hCPU->gpr[rD])));
			if (inst.flags)
				ppc_update_cr0(hCPU, hCPU->gpr[rA]);
			hCPU->instructionPointer = pc + 4;
			break;
		case kPsAdd:
			hCPU->fpr[rD].fp0 = static_cast<float>(hCPU->fpr[rA].fp0 + hCPU->fpr[rB].fp0);
			hCPU->fpr[rD].fp1 = static_cast<float>(hCPU->fpr[rA].fp1 + hCPU->fpr[rB].fp1);
			hCPU->instructionPointer = pc + 4;
			break;
		case kPsSub:
			hCPU->fpr[rD].fp0 = static_cast<float>(hCPU->fpr[rA].fp0 - hCPU->fpr[rB].fp0);
			hCPU->fpr[rD].fp1 = static_cast<float>(hCPU->fpr[rA].fp1 - hCPU->fpr[rB].fp1);
			hCPU->instructionPointer = pc + 4;
			break;
		case kPsMul:
			hCPU->fpr[rD].fp0 = flushDenormalToZero(static_cast<float>(hCPU->fpr[rA].fp0 * roundTo25BitAccuracy(hCPU->fpr[inst.flags].fp0)));
			hCPU->fpr[rD].fp1 = flushDenormalToZero(static_cast<float>(hCPU->fpr[rA].fp1 * roundTo25BitAccuracy(hCPU->fpr[inst.flags].fp1)));
			hCPU->instructionPointer = pc + 4;
			break;
		case kPsMadd:
		{
			const float s0 = static_cast<float>(static_cast<float>(hCPU->fpr[rA].fp0 * roundTo25BitAccuracy(hCPU->fpr[inst.flags].fp0)) + hCPU->fpr[rB].fp0);
			const float s1 = static_cast<float>(static_cast<float>(hCPU->fpr[rA].fp1 * roundTo25BitAccuracy(hCPU->fpr[inst.flags].fp1)) + hCPU->fpr[rB].fp1);
			hCPU->fpr[rD].fp0 = flushDenormalToZero(s0);
			hCPU->fpr[rD].fp1 = flushDenormalToZero(s1);
			hCPU->instructionPointer = pc + 4;
			break;
		}
		case kPsMsub:
		{
			const float s0 = static_cast<float>(static_cast<float>(hCPU->fpr[rA].fp0 * roundTo25BitAccuracy(hCPU->fpr[inst.flags].fp0)) - hCPU->fpr[rB].fp0);
			const float s1 = static_cast<float>(static_cast<float>(hCPU->fpr[rA].fp1 * roundTo25BitAccuracy(hCPU->fpr[inst.flags].fp1)) - hCPU->fpr[rB].fp1);
			hCPU->fpr[rD].fp0 = flushDenormalToZero(s0);
			hCPU->fpr[rD].fp1 = flushDenormalToZero(s1);
			hCPU->instructionPointer = pc + 4;
			break;
		}
		case kPsNmadd:
		{
			const float s0 = static_cast<float>(-(static_cast<float>(hCPU->fpr[rA].fp0 * roundTo25BitAccuracy(hCPU->fpr[inst.flags].fp0)) + hCPU->fpr[rB].fp0));
			const float s1 = static_cast<float>(-(static_cast<float>(hCPU->fpr[rA].fp1 * roundTo25BitAccuracy(hCPU->fpr[inst.flags].fp1)) + hCPU->fpr[rB].fp1));
			hCPU->fpr[rD].fp0 = flushDenormalToZero(s0);
			hCPU->fpr[rD].fp1 = flushDenormalToZero(s1);
			hCPU->instructionPointer = pc + 4;
			break;
		}
		case kPsNmsub:
		{
			const float s0 = static_cast<float>(-(static_cast<float>(hCPU->fpr[rA].fp0 * roundTo25BitAccuracy(hCPU->fpr[inst.flags].fp0)) - hCPU->fpr[rB].fp0));
			const float s1 = static_cast<float>(-(static_cast<float>(hCPU->fpr[rA].fp1 * roundTo25BitAccuracy(hCPU->fpr[inst.flags].fp1)) - hCPU->fpr[rB].fp1));
			hCPU->fpr[rD].fp0 = flushDenormalToZero(s0);
			hCPU->fpr[rD].fp1 = flushDenormalToZero(s1);
			hCPU->instructionPointer = pc + 4;
			break;
		}
		case kLfs:
		{
			const uint64 val = ConvertToDoubleNoFTZ(FastReadU32((rA ? hCPU->gpr[rA] : 0) + imm));
			hCPU->fpr[rD].fp0int = val;
			hCPU->fpr[rD].fp1int = val;
			hCPU->instructionPointer = pc + 4;
			break;
		}
		case kStfs:
		{
			const uint32 ea = (rA ? hCPU->gpr[rA] : 0) + imm;
			*reinterpret_cast<uint32*>(memory_base + ea) = _swapEndianU32(ConvertToSingleNoFTZ(hCPU->fpr[rD].fp0int));
			FastInvalidatePage(ea);
			hCPU->instructionPointer = pc + 4;
			break;
		}
		case kBclr:
		{
			const uint32 BO = rD;
			if ((BO & 4) == 0)
				hCPU->spr.CTR--;
			const bool bo2 = (BO & 2) != 0;
			const bool bo8 = (BO & 8) != 0;
			const bool cr = hCPU->cr[rA] != 0;
			if (((BO & 4) || ((hCPU->spr.CTR != 0) ^ bo2)) && ((BO & 16) || (!(cr ^ bo8))))
			{
				const uint32 target = hCPU->spr.LR & 0xfffffffc;
				if (inst.flags & 1)
					hCPU->spr.LR = pc + 4;
				hCPU->instructionPointer = target;
			}
			else
				hCPU->instructionPointer = pc + 4;
			break;
		}
		case kMflr:
			hCPU->gpr[rD] = hCPU->spr.LR;
			hCPU->instructionPointer = pc + 4;
			break;
		case kMfctr:
			hCPU->gpr[rD] = hCPU->spr.CTR;
			hCPU->instructionPointer = pc + 4;
			break;
		case kMtlr:
			hCPU->spr.LR = hCPU->gpr[rD];
			hCPU->instructionPointer = pc + 4;
			break;
		case kMtctr:
			hCPU->spr.CTR = hCPU->gpr[rD];
			hCPU->instructionPointer = pc + 4;
			break;
		case kBcctr:
		{
			const uint32 BO = rD;
			const bool bo8 = (BO & 8) != 0;
			const bool cr = hCPU->cr[rA] != 0;
			if ((BO & 16) || (!(cr ^ bo8)))
			{
				const uint32 target = hCPU->spr.CTR & 0xfffffffc;
				if (inst.flags & 1)
					hCPU->spr.LR = pc + 4;
				hCPU->instructionPointer = target;
			}
			else
				hCPU->instructionPointer = pc + 4;
			break;
		}
		case kLbzu:
		{
			const uint32 ea = hCPU->gpr[rA] + imm;
			hCPU->gpr[rA] = ea;
			hCPU->gpr[rD] = memory_base[ea];
			hCPU->instructionPointer = pc + 4;
			break;
		}
		case kStbu:
		{
			const uint32 ea = hCPU->gpr[rA] + imm;
			memory_base[ea] = static_cast<uint8>(hCPU->gpr[rD]);
			FastInvalidatePage(ea);
			hCPU->gpr[rA] = ea;
			hCPU->instructionPointer = pc + 4;
			break;
		}
		case kSubf:
			hCPU->gpr[rD] = ~hCPU->gpr[rA] + hCPU->gpr[rB] + 1;
			if (inst.flags)
				ppc_update_cr0(hCPU, hCPU->gpr[rD]);
			hCPU->instructionPointer = pc + 4;
			break;
		case kMulli:
			hCPU->gpr[rD] = hCPU->gpr[rA] * imm;
			hCPU->instructionPointer = pc + 4;
			break;
		case kAddic:
		case kAddicRc:
		{
			const uint32 a = hCPU->gpr[rA];
			const uint32 r = a + imm;
			hCPU->gpr[rD] = r;
			hCPU->xer_ca = r < a ? 1 : 0;
			if (inst.kind == kAddicRc)
				ppc_update_cr0(hCPU, r);
			hCPU->instructionPointer = pc + 4;
			break;
		}
		case kXori:
		case kXoris:
			hCPU->gpr[rA] = hCPU->gpr[rD] ^ imm;
			hCPU->instructionPointer = pc + 4;
			break;
		case kLfd:
		{
			// guest addresses wrap at 32 bits: form the address before adding memory_base
			const uint32 ea = (rA ? hCPU->gpr[rA] : 0) + imm;
			hCPU->fpr[rD].fp0int = _swapEndianU64(*reinterpret_cast<uint64*>(memory_base + ea));
			hCPU->instructionPointer = pc + 4;
			break;
		}
		case kStfd:
		{
			const uint32 ea = (rA ? hCPU->gpr[rA] : 0) + imm;
			*reinterpret_cast<uint64*>(memory_base + ea) = _swapEndianU64(hCPU->fpr[rD].fp0int);
			FastInvalidatePage(ea);
			hCPU->instructionPointer = pc + 4;
			break;
		}
		default:
			if (inst.fn)
				inst.fn(hCPU, inst.opcode);
			else
				PPCInterpreterSlim_executeInstruction(hCPU);
			break;
		}
		// A fallback may yield and resume on another core; reacquire that core's slots.
		if (fallback && PPCInterpreter_getCoreIndex(hCPU) != core)
			break;
		if (page->epoch.load(std::memory_order_relaxed) != epoch)
			break;
		const uint32 next = hCPU->instructionPointer;
		if (next == pc + 4)
		{
			pc = next;
			index++;
		}
		else if ((next >> 12) == (pc >> 12))
		{
			// taken branch inside this page: keep page, slots and epoch
			pc = next;
			index = (pc >> 2) & 1023;
		}
		else
			break;
		}
	}
}
