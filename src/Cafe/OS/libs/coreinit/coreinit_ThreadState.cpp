#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_ThreadState.h"
#include "Cafe/OS/libs/coreinit/coreinit_Scheduler.h"
#include "Cafe/OS/libs/coreinit/coreinit.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"

namespace coreinit
{
	bool g_isMulticoreMode = false;

	bool __CemuIsMulticoreMode()
	{
		return g_isMulticoreMode;
	}

	SysAllocator<OSThreadQueue> g_activeThreadQueue; // list of all threads (can include non-detached inactive threads)
	SysAllocator<OSThreadQueue, 3> g_coreRunQueue;

	OSThread_t* __currentCoreThread[3] = {};

	void OSSetCurrentThread(uint32 coreIndex, OSThread_t* thread)
	{
		if (coreIndex < 3)
			__currentCoreThread[coreIndex] = thread;
	}

	OSThread_t* OSGetCurrentThread()
	{
		PPCInterpreter_t* currentInstance = PPCInterpreter_getCurrentInstance();
		if (currentInstance == nullptr)
			return nullptr;
		return __currentCoreThread[currentInstance->spr.UPIR];
	}

	void __OSInitContext(OSContext_t* ctx, MEMPTR<void> initialIP, MEMPTR<void> initialStackPointer)
	{
		ctx->SetContextMagic();
		ctx->gpr[0] = 0; // r0 is left uninitialized on console?
		for (auto& it : ctx->gpr)
			it = 0;
		ctx->gpr[1] = _swapEndianU32(initialStackPointer.GetMPTR());
		ctx->gpr[2] = _swapEndianU32(RPLLoader_GetSDA2Base());
		ctx->gpr[13] = _swapEndianU32(RPLLoader_GetSDA1Base());
		ctx->srr0 = initialIP.GetMPTR();
		ctx->cr = 0;
		ctx->ukn0A8 = 0;
		ctx->ukn0AC = 0;
		ctx->gqr[0] = 0;
		ctx->gqr[1] = 0;
		ctx->gqr[2] = 0;
		ctx->gqr[3] = 0;
		ctx->gqr[4] = 0;
		ctx->gqr[5] = 0;
		ctx->gqr[6] = 0;
		ctx->gqr[7] = 0;
		ctx->dsi_dar = 0;
		ctx->srr1 = 0x9032;
		ctx->xer = 0;
		ctx->dsi_dsisr = 0;
		ctx->upir = 0;
		ctx->boostCount = 0;
		ctx->state = 0;
		for (auto& it : ctx->coretime)
			it = 0;
		ctx->starttime = 0;
		ctx->ghs_errno = 0;
		ctx->upmc1 = 0;
		ctx->upmc2 = 0;
		ctx->upmc3 = 0;
		ctx->upmc4 = 0;
		ctx->ummcr0 = 0;
		ctx->ummcr1 = 0;
	}

	void __OSThreadInit(OSThread_t* thread, MEMPTR<void> entrypoint, uint32 argInt, MEMPTR<void> argPtr, MEMPTR<void> stackTop, uint32 stackSize, sint32 priority, uint32 upirCoreIndex, OSThread_t::THREAD_TYPE threadType)
	{
		thread->effectivePriority = priority;
		thread->type = threadType;
		thread->basePriority = priority;
		thread->SetThreadMagic();
		thread->id = 0x8000;
		thread->waitAlarm = nullptr;
		thread->entrypoint = entrypoint;
		thread->quantumTicks = 0;
		if (entrypoint)
		{
			thread->state = OSThread_t::THREAD_STATE::STATE_READY;
			thread->suspendCounter = 1;
		}
		else
		{
			thread->state = OSThread_t::THREAD_STATE::STATE_NONE;
			thread->suspendCounter = 0;
		}
		thread->exitValue = (uint32)-1;
		thread->requestFlags = OSThread_t::REQUEST_FLAG_BIT::REQUEST_FLAG_NONE;
		thread->pendingSuspend = 0;
		thread->suspendResult = 0xFFFFFFFF;
		thread->coretimeSumQuantumStart = 0;
		thread->deallocatorFunc = nullptr;
		thread->cleanupCallback = nullptr;
		thread->waitingForFastMutex = nullptr;
		thread->stateFlags = 0;
		thread->waitingForMutex = nullptr;
		memset(&thread->crt, 0, sizeof(thread->crt));
		static_assert(sizeof(thread->crt) == 0x1D8);
		thread->tlsBlocksMPTR = 0;
		thread->numAllocatedTLSBlocks = 0;
		thread->tlsStatus = 0;
		OSInitThreadQueueEx(&thread->joinQueue, thread);
		OSInitThreadQueueEx(&thread->suspendQueue, thread);
		thread->mutexQueue.ukn08 = thread;
		thread->mutexQueue.ukn0C = 0;
		thread->mutexQueue.tail = nullptr;
		thread->mutexQueue.head = nullptr;
		thread->ownedFastMutex.next = nullptr;
		thread->ownedFastMutex.prev = nullptr;
		thread->contendedFastMutex.next = nullptr;
		thread->contendedFastMutex.prev = nullptr;

		MEMPTR<void> alignedStackTop{MEMPTR<void>(stackTop).GetMPTR() & 0xFFFFFFF8};
		MEMPTR<uint32be> alignedStackTop32{alignedStackTop};
		alignedStackTop32[-1] = 0;
		alignedStackTop32[-2] = 0;

		__OSInitContext(&thread->context, entrypoint, (void*)(alignedStackTop32.GetPtr() - 2));
		thread->stackBase = stackTop; // without alignment
		thread->stackEnd = ((uint8*)stackTop.GetPtr() - stackSize);
		thread->context.upir = upirCoreIndex;
		thread->context.lr = entrypoint ? _swapEndianU32(PPCInterpreter_makeCallableExportDepr(coreinitExport_OSExitThreadDepr)) : 0;
		thread->context.gpr[3] = _swapEndianU32(argInt);
		thread->context.gpr[4] = _swapEndianU32(argPtr.GetMPTR());

		*(uint32be*)((uint8*)stackTop.GetPtr() - stackSize) = 0xDEADBABE;
		thread->alarmRelatedUkn = 0;
		for (auto& it : thread->specificArray)
			it = nullptr;
		thread->context.fpscr.fpscr = 4;
		for (sint32 i = 0; i < 32; i++)
		{
			thread->context.fp_ps0[i] = 0.0;
			thread->context.fp_ps1[i] = 0.0;
		}
		thread->context.gqr[2] = 0x40004;
		thread->context.gqr[3] = 0x50005;
		thread->context.gqr[4] = 0x60006;
		thread->context.gqr[5] = 0x70007;

		for (sint32 i = 0; i < Espresso::CORE_COUNT; i++)
			thread->context.coretime[i] = 0;

		for (sint32 i = 0; i < Espresso::CORE_COUNT; i++)
			thread->currentRunQueue[i] = nullptr;
		thread->waitQueueLink.prev = nullptr;
		thread->waitQueueLink.next = nullptr;

		thread->wakeTimeRelatedUkn2 = 0;
		thread->wakeUpCount = 0;
		thread->wakeUpTime = 0;
		thread->wakeTimeRelatedUkn1 = 0x7FFFFFFFFFFFFFFF;
		thread->quantumTicks = 0;
		thread->coretimeSumQuantumStart = 0;
		thread->totalCycles = 0;

		for (auto& it : thread->padding68C)
			it = 0;
	}

	void SetThreadAffinityToCore(OSThread_t* thread, uint32 coreIndex)
	{
		cemu_assert_debug(coreIndex < 3);
		thread->attr &= ~(OSThread_t::ATTR_BIT::ATTR_AFFINITY_CORE0 | OSThread_t::ATTR_BIT::ATTR_AFFINITY_CORE1 | OSThread_t::ATTR_BIT::ATTR_AFFINITY_CORE2 | OSThread_t::ATTR_BIT::ATTR_UKN_010);
		thread->context.affinity &= 0xFFFFFFF8;
		if (coreIndex == 0)
		{
			thread->attr |= OSThread_t::ATTR_BIT::ATTR_AFFINITY_CORE0;
			thread->context.affinity |= (1 << 0);
		}
		else if (coreIndex == 1)
		{
			thread->attr |= OSThread_t::ATTR_BIT::ATTR_AFFINITY_CORE1;
			thread->context.affinity |= (1 << 1);
		}
		else // if (coreIndex == 2)
		{
			thread->attr |= OSThread_t::ATTR_BIT::ATTR_AFFINITY_CORE2;
			thread->context.affinity |= (1 << 2);
		}
	}

	void __OSCreateThreadOnActiveThreadWorkaround(OSThread_t* thread)
	{
		// Workaround guard for re-initialization
	}

	bool __OSCreateThreadInternal2(OSThread_t* thread, MEMPTR<void> entrypoint, uint32 argInt, MEMPTR<void> argPtr, MEMPTR<void> stackBase, uint32 stackSize, sint32 priority, uint32 attrBits, OSThread_t::THREAD_TYPE threadType)
	{
		__OSCreateThreadOnActiveThreadWorkaround(thread);
		OSThread_t* currentThread = OSGetCurrentThread();
		if (priority < 0 || priority >= 32)
		{
			cemuLog_log(LogType::APIErrors, "OSCreateThreadInternal: Thread priority must be in range 0-31");
			return false;
		}
		if (threadType == OSThread_t::THREAD_TYPE::TYPE_IO)
		{
			priority = priority + 0x20;
		}
		else if (threadType == OSThread_t::THREAD_TYPE::TYPE_APP)
		{
			priority = priority + 0x40;
		}
		if (attrBits >= 0x20 || stackBase == nullptr || stackSize == 0)
		{
			cemuLog_logDebug(LogType::APIErrors, "OSCreateThreadInternal: Invalid attributes, stack base or size");
			return false;
		}
		uint32 im = OSDisableInterrupts();
		__OSLockScheduler(thread);

		uint32 coreIndex = PPCInterpreter_getCurrentInstance() ? OSGetCoreId() : 1;
		__OSThreadInit(thread, entrypoint, argInt, argPtr, stackBase, stackSize, priority, coreIndex, threadType);
		thread->threadName = nullptr;
		thread->context.affinity = attrBits & 7;
		thread->attr = attrBits;
		if ((attrBits & 7) == 0) // if no explicit affinity is given, use the current core
			SetThreadAffinityToCore(thread, PPCInterpreter_getCurrentInstance() ? OSGetCoreId() : 1);
		if (currentThread)
		{
			for (sint32 i = 0; i < Espresso::CORE_COUNT; i++)
			{
				thread->dsiCallback[i] = currentThread->dsiCallback[i];
				thread->isiCallback[i] = currentThread->isiCallback[i];
				thread->programCallback[i] = currentThread->programCallback[i];
				thread->perfMonCallback[i] = currentThread->perfMonCallback[i];
				thread->alignmentExceptionCallback[i] = currentThread->alignmentExceptionCallback[i];
			}
			thread->context.srr1 = thread->context.srr1 | (currentThread->context.srr1 & 0x900);
			thread->context.fpscr.fpscr = thread->context.fpscr.fpscr | (currentThread->context.fpscr.fpscr & 0xF8);
		}
		else
		{
			for (sint32 i = 0; i < Espresso::CORE_COUNT; i++)
			{
				thread->dsiCallback[i] = 0;
				thread->isiCallback[i] = 0;
				thread->programCallback[i] = 0;
				thread->perfMonCallback[i] = 0;
				thread->alignmentExceptionCallback[i] = nullptr;
			}
		}
		if (entrypoint)
		{
			thread->id = 0x8000;
		}
		__OSUnlockScheduler(thread);
		OSRestoreInterrupts(im);
		return true;
	}

	bool OSCreateThreadType(OSThread_t* thread, MPTR entryPoint, sint32 numParam, void* ptrParam, void* stackTop, sint32 stackSize, sint32 priority, uint32 attr, OSThread_t::THREAD_TYPE threadType)
	{
		if (threadType != OSThread_t::THREAD_TYPE::TYPE_APP && threadType != OSThread_t::THREAD_TYPE::TYPE_IO)
		{
			cemuLog_logDebug(LogType::APIErrors, "OSCreateThreadType: Invalid thread type");
			cemu_assert_suspicious();
			return false;
		}
		return __OSCreateThreadInternal2(thread, MEMPTR<void>(entryPoint), numParam, ptrParam, stackTop, stackSize, priority, attr, threadType);
	}

	bool OSCreateThread(OSThread_t* thread, MPTR entryPoint, sint32 numParam, void* ptrParam, void* stackTop, sint32 stackSize, sint32 priority, uint32 attr)
	{
		return __OSCreateThreadInternal2(thread, MEMPTR<void>(entryPoint), numParam, ptrParam, stackTop, stackSize, priority, attr, OSThread_t::THREAD_TYPE::TYPE_APP);
	}

	bool __OSCreateThreadType(OSThread_t* thread, MPTR entryPoint, sint32 numParam, void* ptrParam, void* stackTop, sint32 stackSize, sint32 priority, uint32 attr, OSThread_t::THREAD_TYPE threadType)
	{
		return __OSCreateThreadInternal2(thread, MEMPTR<void>(entryPoint), numParam, ptrParam, stackTop, stackSize, priority, attr, threadType);
	}

	void OSSetThreadName(OSThread_t* thread, const char* name)
	{
		thread->threadName = name;
	}

	const char* OSGetThreadName(OSThread_t* thread)
	{
		return thread->threadName.GetPtr();
	}

	void __OSSetThreadBasePriority(OSThread_t* thread, sint32 newPriority)
	{
		cemu_assert_debug(newPriority >= 0 && newPriority < 32);
		newPriority += OSThread_t::GetTypePriorityBase(thread->type);
		thread->basePriority = newPriority;
	}

	void __OSUpdateThreadEffectivePriority(OSThread_t* thread)
	{
		if (thread->context.boostCount != 0)
		{
			// temporarily boosted threads have their priority set to 0 (maximum)
			thread->effectivePriority = 0;
			return;
		}
		thread->effectivePriority = thread->basePriority;
	}

	SysAllocator<OSThread_t, PPC_CORE_COUNT> s_defaultThreads;
	SysAllocator<uint8, PPC_CORE_COUNT * 1024 * 1024> s_stack;
	SysAllocator<char, 64> _defaultThreadName[PPC_CORE_COUNT];

	OSThread_t* OSGetDefaultThread(sint32 coreIndex)
	{
		if (coreIndex < 0 || coreIndex >= PPC_CORE_COUNT)
			return nullptr;
		return s_defaultThreads.GetPtr() + coreIndex;
	}

	void* OSGetDefaultThreadStack(sint32 coreIndex, uint32& size)
	{
		if (coreIndex < 0 || coreIndex >= PPC_CORE_COUNT)
			return nullptr;
		size = 1024 * 1024;
		return s_stack.GetPtr() + coreIndex * size;
	}

	void OSInitThreadQueue(OSThreadQueue* threadQueue)
	{
		threadQueue->head = nullptr;
		threadQueue->tail = nullptr;
		threadQueue->userData = nullptr;
		threadQueue->ukn0C = 0;
	}

	void OSInitThreadQueueEx(OSThreadQueue* threadQueue, void* userData)
	{
		threadQueue->head = nullptr;
		threadQueue->tail = nullptr;
		threadQueue->userData = userData;
		threadQueue->ukn0C = 0;
	}

	void __OSInitDefaultThreads()
	{
		for (sint32 i = 0; i < PPC_CORE_COUNT; i++)
		{
			snprintf(_defaultThreadName[i].GetPtr(), 64, "Default Core %d", i);
			OSThread_t* coreDefaultThread = coreinit::OSGetDefaultThread(i);
			uint32 stackSize;
			void* stackBase = coreinit::OSGetDefaultThreadStack(i, stackSize);
			coreinit::OSCreateThreadType(coreDefaultThread, MPTR_NULL, 0, nullptr, (uint8*)stackBase + stackSize, stackSize, 16, 1 << i, OSThread_t::THREAD_TYPE::TYPE_APP);
			coreinit::OSSetThreadName(coreDefaultThread, _defaultThreadName[i].GetPtr());
		}
	}

	void MapThreadStateExports()
	{
		static bool s_mapped = false;
		if (s_mapped)
			return;
		s_mapped = true;

		cafeExportRegister("coreinit", OSGetCurrentThread, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSGetDefaultThread, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSSetThreadName, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSGetThreadName, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSInitThreadQueue, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSInitThreadQueueEx, LogType::CoreinitThread);
	}
}
