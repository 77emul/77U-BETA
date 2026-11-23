#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/Espresso/Debugger/GDBStub.h"
#include "ExceptionHandler.h"

#include <atomic>
#include <csignal>
#include <cstring>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <execinfo.h>
#if defined(__aarch64__)
#include <sys/ucontext.h>
#endif
#endif

#if defined(__APPLE__)
namespace
{
	std::atomic<int> s_persistentCrashLogFd{-1};
	std::atomic<int> s_persistentCrashLogFd2{-1};
	volatile sig_atomic_t s_persistentCrashLogActive = 0;
	char s_persistentCrashLogPath1[1024] = {0};
	char s_persistentCrashLogPath2[1024] = {0};

	void PersistentCrashLog_OpenDescriptorsIfNeeded()
	{
		if (s_persistentCrashLogFd.load(std::memory_order_relaxed) < 0 && s_persistentCrashLogPath1[0] != '\0')
		{
			const int fd = open(s_persistentCrashLogPath1, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0666);
			if (fd >= 0)
				s_persistentCrashLogFd.store(fd, std::memory_order_release);
		}
		if (s_persistentCrashLogFd2.load(std::memory_order_relaxed) < 0 && s_persistentCrashLogPath2[0] != '\0')
		{
			const int fd2 = open(s_persistentCrashLogPath2, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0666);
			if (fd2 >= 0)
				s_persistentCrashLogFd2.store(fd2, std::memory_order_release);
		}
	}

	void PersistentCrashLog_Write(const char* data, size_t size)
	{
		if (!s_persistentCrashLogActive || !data || size == 0)
			return;
		const int fds[2] = {
			s_persistentCrashLogFd.load(std::memory_order_relaxed),
			s_persistentCrashLogFd2.load(std::memory_order_relaxed)
		};
		for (int fd : fds)
		{
			if (fd < 0) continue;
			const char* ptr = data;
			size_t rem = size;
			while (rem > 0)
			{
				const ssize_t written = write(fd, ptr, rem);
				if (written > 0)
				{
					ptr += written;
					rem -= static_cast<size_t>(written);
					continue;
				}
				if (written < 0 && errno == EINTR)
					continue;
				break;
			}
		}
	}

	size_t PersistentCrashLog_AppendUnsigned(char* output, size_t offset, uint64 value, uint32 base)
	{
		char reversed[32];
		size_t count = 0;
		do
		{
			const uint32 digit = static_cast<uint32>(value % base);
			reversed[count++] = digit < 10 ? static_cast<char>('0' + digit) : static_cast<char>('a' + digit - 10);
			value /= base;
		} while (value != 0 && count < sizeof(reversed));
		while (count > 0)
			output[offset++] = reversed[--count];
		return offset;
	}
}

void ExceptionHandler_InstallAltStack()
{
#if defined(__APPLE__) || defined(__unix__)
	stack_t currentStack{};
	if (sigaltstack(nullptr, &currentStack) == 0)
	{
		if ((currentStack.ss_flags & SS_DISABLE) != 0 || currentStack.ss_sp == nullptr)
		{
			constexpr size_t altStackSize = 128 * 1024; // 128 KB dedicated signal stack
			void* altStackMem = malloc(altStackSize);
			if (altStackMem)
			{
				stack_t newStack{};
				newStack.ss_sp = altStackMem;
				newStack.ss_size = altStackSize;
				newStack.ss_flags = 0;
				if (sigaltstack(&newStack, nullptr) != 0)
				{
					free(altStackMem);
				}
			}
		}
	}
#endif
}

void ExceptionHandler_SetPersistentCrashLogPaths(const char* path1, const char* path2)
{
	if (path1 && path1[0] != '\0')
	{
		strncpy(s_persistentCrashLogPath1, path1, sizeof(s_persistentCrashLogPath1) - 1);
		s_persistentCrashLogPath1[sizeof(s_persistentCrashLogPath1) - 1] = '\0';
	}
	if (path2 && path2[0] != '\0')
	{
		strncpy(s_persistentCrashLogPath2, path2, sizeof(s_persistentCrashLogPath2) - 1);
		s_persistentCrashLogPath2[sizeof(s_persistentCrashLogPath2) - 1] = '\0';
	}
}

void ExceptionHandler_SetPersistentCrashLogPath(const char* path)
{
	ExceptionHandler_SetPersistentCrashLogPaths(path, nullptr);
}
#else
void ExceptionHandler_InstallAltStack() {}
void ExceptionHandler_SetPersistentCrashLogPaths(const char*, const char*) {}
void ExceptionHandler_SetPersistentCrashLogPath(const char*) {}
#endif

#if defined(__APPLE__) && defined(__aarch64__)
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
extern "C" bool JIT26_IsAddressInArenaAsyncSafe(const void* ptr);
#endif
#endif

void ExceptionHandler_AppendPersistentBacktrace(void* const* backtraceEntries, size_t backtraceEntryCount)
{
#if defined(__APPLE__)
	if (!backtraceEntries || backtraceEntryCount == 0)
		return;
	char line[128];
	size_t offset = 0;
	constexpr char stackHeader[] = "Host backtrace addresses:\n";
	PersistentCrashLog_Write(stackHeader, sizeof(stackHeader) - 1);
	for (size_t i = 0; i < backtraceEntryCount; ++i)
	{
		offset = 0;
		line[offset++] = '#';
		offset = PersistentCrashLog_AppendUnsigned(line, offset, i, 10);
		line[offset++] = ' ';
		line[offset++] = '0';
		line[offset++] = 'x';
		offset = PersistentCrashLog_AppendUnsigned(line, offset, reinterpret_cast<uintptr_t>(backtraceEntries[i]), 16);
		line[offset++] = '\n';
		PersistentCrashLog_Write(line, offset);
	}
	ExceptionHandler_FlushPersistentCrashLog();
#else
	(void)backtraceEntries;
	(void)backtraceEntryCount;
#endif
}

void ExceptionHandler_BeginPersistentCrashLog(int signalNumber, const void* faultAddress,
	void* const* backtraceEntries, size_t backtraceEntryCount, void* context)
{
#if defined(__APPLE__)
	s_persistentCrashLogActive = 1;
	PersistentCrashLog_OpenDescriptorsIfNeeded();
	char line[128];
	size_t offset = 0;
	constexpr char header[] = "=== Cemu iPadOS fatal crash ===\nsignal=";
	memcpy(line + offset, header, sizeof(header) - 1);
	offset += sizeof(header) - 1;
	offset = PersistentCrashLog_AppendUnsigned(line, offset, static_cast<uint64>(signalNumber), 10);
	constexpr char faultLabel[] = " fault=0x";
	memcpy(line + offset, faultLabel, sizeof(faultLabel) - 1);
	offset += sizeof(faultLabel) - 1;
	offset = PersistentCrashLog_AppendUnsigned(line, offset, reinterpret_cast<uintptr_t>(faultAddress), 16);
	line[offset++] = '\n';
	PersistentCrashLog_Write(line, offset);

	const intptr_t vmSlide = _dyld_get_image_vmaddr_slide(0);
	const struct mach_header* machHeader = _dyld_get_image_header(0);
	offset = 0;
	constexpr char slideLabel[] = "aslr_slide=0x";
	memcpy(line + offset, slideLabel, sizeof(slideLabel) - 1);
	offset += sizeof(slideLabel) - 1;
	offset = PersistentCrashLog_AppendUnsigned(line, offset, static_cast<uint64>(vmSlide), 16);
	constexpr char headerLabel[] = " image_header=0x";
	memcpy(line + offset, headerLabel, sizeof(headerLabel) - 1);
	offset += sizeof(headerLabel) - 1;
	offset = PersistentCrashLog_AppendUnsigned(line, offset, reinterpret_cast<uintptr_t>(machHeader), 16);
	line[offset++] = '\n';
	PersistentCrashLog_Write(line, offset);

	char threadName[64] = {0};
	if (pthread_getname_np(pthread_self(), threadName, sizeof(threadName)) == 0 && threadName[0] != '\0')
	{
		offset = 0;
		constexpr char tLabel[] = "Faulting Thread: ";
		memcpy(line + offset, tLabel, sizeof(tLabel) - 1);
		offset += sizeof(tLabel) - 1;
		size_t tLen = strlen(threadName);
		memcpy(line + offset, threadName, tLen);
		offset += tLen;
		line[offset++] = '\n';
		PersistentCrashLog_Write(line, offset);
	}

#if defined(__aarch64__)
	if (context)
	{
		ucontext_t* uc = reinterpret_cast<ucontext_t*>(context);
		if (uc && uc->uc_mcontext)
		{
			auto& ss = uc->uc_mcontext->__ss;
			constexpr char regHeader[] = "ARM64 Registers:\n";
			PersistentCrashLog_Write(regHeader, sizeof(regHeader) - 1);
			for (int i = 0; i < 29; ++i)
			{
				offset = 0;
				line[offset++] = 'x';
				offset = PersistentCrashLog_AppendUnsigned(line, offset, i, 10);
				line[offset++] = '=';
				line[offset++] = '0';
				line[offset++] = 'x';
				offset = PersistentCrashLog_AppendUnsigned(line, offset, ss.__x[i], 16);
				line[offset++] = (i % 2 == 1 || i == 28) ? '\n' : '\t';
				PersistentCrashLog_Write(line, offset);
			}
			offset = 0;
			constexpr char specialRegs[] = "fp=0x";
			memcpy(line + offset, specialRegs, sizeof(specialRegs) - 1);
			offset += sizeof(specialRegs) - 1;
			offset = PersistentCrashLog_AppendUnsigned(line, offset, ss.__fp, 16);
			constexpr char lrLabel[] = "\tlr=0x";
			memcpy(line + offset, lrLabel, sizeof(lrLabel) - 1);
			offset += sizeof(lrLabel) - 1;
			offset = PersistentCrashLog_AppendUnsigned(line, offset, ss.__lr, 16);
			line[offset++] = '\n';
			PersistentCrashLog_Write(line, offset);

			offset = 0;
			constexpr char spLabel[] = "sp=0x";
			memcpy(line + offset, spLabel, sizeof(spLabel) - 1);
			offset += sizeof(spLabel) - 1;
			offset = PersistentCrashLog_AppendUnsigned(line, offset, ss.__sp, 16);
			constexpr char pcLabel[] = "\tpc=0x";
			memcpy(line + offset, pcLabel, sizeof(pcLabel) - 1);
			offset += sizeof(pcLabel) - 1;
			offset = PersistentCrashLog_AppendUnsigned(line, offset, ss.__pc, 16);
			line[offset++] = '\n';
			PersistentCrashLog_Write(line, offset);

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
			if (JIT26_IsAddressInArenaAsyncSafe(reinterpret_cast<const void*>(ss.__pc)))
			{
				constexpr char jitMsg[] = "[JIT] Fault PC is inside JIT recompiler executable code cache!\n";
				PersistentCrashLog_Write(jitMsg, sizeof(jitMsg) - 1);
			}
#endif

			// Probe guest PPCInterpreter state from fp (HCPU_REG_ID = 29 in recompiler)
			if (ss.__fp != 0)
			{
				PPCInterpreter_t probeHCPU;
				vm_size_t outSize = 0;
				if (vm_read_overwrite(mach_task_self(), static_cast<vm_address_t>(ss.__fp),
				                      sizeof(probeHCPU), reinterpret_cast<vm_address_t>(&probeHCPU),
				                      &outSize) == KERN_SUCCESS && outSize >= sizeof(probeHCPU))
				{
					offset = 0;
					constexpr char ppcHeader[] = "Guest PPC State: IP=0x";
					memcpy(line + offset, ppcHeader, sizeof(ppcHeader) - 1);
					offset += sizeof(ppcHeader) - 1;
					offset = PersistentCrashLog_AppendUnsigned(line, offset, probeHCPU.instructionPointer, 16);
					constexpr char lrPart[] = " LR=0x";
					memcpy(line + offset, lrPart, sizeof(lrPart) - 1);
					offset += sizeof(lrPart) - 1;
					offset = PersistentCrashLog_AppendUnsigned(line, offset, probeHCPU.spr.LR, 16);
					constexpr char spPart[] = " SP=0x";
					memcpy(line + offset, spPart, sizeof(spPart) - 1);
					offset += sizeof(spPart) - 1;
					offset = PersistentCrashLog_AppendUnsigned(line, offset, probeHCPU.gpr[1], 16);
					line[offset++] = '\n';
					PersistentCrashLog_Write(line, offset);
				}
			}
		}
	}
#endif

	// Immediately flush registers and state to disk so the crash report is never empty!
	ExceptionHandler_FlushPersistentCrashLog();

	if (backtraceEntries && backtraceEntryCount > 0)
	{
		ExceptionHandler_AppendPersistentBacktrace(backtraceEntries, backtraceEntryCount);
	}
#else
	(void)signalNumber;
	(void)faultAddress;
	(void)backtraceEntries;
	(void)backtraceEntryCount;
#endif
}

void ExceptionHandler_FlushPersistentCrashLog()
{
#if defined(__APPLE__)
	const int fd = s_persistentCrashLogFd.load(std::memory_order_relaxed);
	if (fd >= 0)
		fsync(fd);
	const int fd2 = s_persistentCrashLogFd2.load(std::memory_order_relaxed);
	if (fd2 >= 0)
		fsync(fd2);
#endif
}

void ExceptionHandler_ActivatePersistentCrashLog()
{
#if defined(__APPLE__)
	s_persistentCrashLogActive = 1;
#endif
}

void ExceptionHandler_RecordFatalError(const char* title, const char* details)
{
#if defined(__APPLE__)
	s_persistentCrashLogActive = 1;
	PersistentCrashLog_OpenDescriptorsIfNeeded();
	char line[256];
	size_t offset = 0;
	constexpr char header[] = "=== Cemu iPadOS fatal error ===\n";
	PersistentCrashLog_Write(header, sizeof(header) - 1);

	if (title && title[0] != '\0')
	{
		offset = 0;
		constexpr char tLabel[] = "Error: ";
		memcpy(line + offset, tLabel, sizeof(tLabel) - 1);
		offset += sizeof(tLabel) - 1;
		PersistentCrashLog_Write(line, offset);
		PersistentCrashLog_Write(title, strlen(title));
		PersistentCrashLog_Write("\n", 1);
	}

	const intptr_t vmSlide = _dyld_get_image_vmaddr_slide(0);
	const struct mach_header* machHeader = _dyld_get_image_header(0);
	offset = 0;
	constexpr char slideLabel[] = "aslr_slide=0x";
	memcpy(line + offset, slideLabel, sizeof(slideLabel) - 1);
	offset += sizeof(slideLabel) - 1;
	offset = PersistentCrashLog_AppendUnsigned(line, offset, static_cast<uint64>(vmSlide), 16);
	constexpr char headerLabel[] = " image_header=0x";
	memcpy(line + offset, headerLabel, sizeof(headerLabel) - 1);
	offset += sizeof(headerLabel) - 1;
	offset = PersistentCrashLog_AppendUnsigned(line, offset, reinterpret_cast<uintptr_t>(machHeader), 16);
	line[offset++] = '\n';
	PersistentCrashLog_Write(line, offset);

	char threadName[64] = {0};
	if (pthread_getname_np(pthread_self(), threadName, sizeof(threadName)) == 0 && threadName[0] != '\0')
	{
		offset = 0;
		constexpr char tLabel[] = "Faulting Thread: ";
		memcpy(line + offset, tLabel, sizeof(tLabel) - 1);
		offset += sizeof(tLabel) - 1;
		size_t tLen = strlen(threadName);
		memcpy(line + offset, threadName, tLen);
		offset += tLen;
		line[offset++] = '\n';
		PersistentCrashLog_Write(line, offset);
	}

	if (details && details[0] != '\0')
	{
		PersistentCrashLog_Write(details, strlen(details));
		PersistentCrashLog_Write("\n", 1);
	}

	// Always flush immediately before unwinding backtrace
	ExceptionHandler_FlushPersistentCrashLog();

	void* backtraceArray[64];
	int count = backtrace(backtraceArray, 64);
	if (count > 0)
	{
		ExceptionHandler_AppendPersistentBacktrace(backtraceArray, static_cast<size_t>(count));
	}

	ExceptionHandler_FlushPersistentCrashLog();
#else
	(void)title;
	(void)details;
#endif
}

static std::atomic<ExceptionHandlerAdditionalLogCallback> s_additionalLogCallback{nullptr};

void ExceptionHandler_SetAdditionalLogCallback(ExceptionHandlerAdditionalLogCallback callback)
{
    s_additionalLogCallback.store(callback, std::memory_order_release);
}

bool crashLogCreated = false;

bool CrashLog_Create()
{
    if (crashLogCreated)
        return false; // only create one crashlog
    crashLogCreated = true;
    cemuLog_createLogFile(true);
    CrashLog_SetOutputChannels(true, true);
    return true;
}

static bool s_writeToStdErr{true};
static bool s_writeToLogTxt{true};

void CrashLog_SetOutputChannels(bool writeToStdErr, bool writeToLogTxt)
{
    s_writeToStdErr = writeToStdErr;
    s_writeToLogTxt = writeToLogTxt;
}

// outputs to both stderr and log.txt
void CrashLog_WriteLine(std::string_view text, bool newLine)
{
#if defined(__APPLE__)
	PersistentCrashLog_Write(text.data(), text.size());
	if (newLine)
		PersistentCrashLog_Write("\n", 1);
#endif
    if(s_writeToLogTxt)
        cemuLog_writeLineToLog(text, false, newLine);
    if(s_writeToStdErr)
    {
        fwrite(text.data(), sizeof(char), text.size(), stderr);
        if(newLine)
            fwrite("\n", sizeof(char), 1, stderr);
    }
}

void CrashLog_WriteHeader(const char* header)
{
    CrashLog_WriteLine("-----------------------------------------");
    CrashLog_WriteLine("   ", false);
    CrashLog_WriteLine(header);
    CrashLog_WriteLine("-----------------------------------------");
}

void ExceptionHandler_LogGeneralInfo()
{
    char dumpLine[1024];
    // info about game
    CrashLog_WriteLine("");
    CrashLog_WriteHeader("Game info");
    if (CafeSystem::IsTitleRunning())
    {
        CrashLog_WriteLine("Game: ", false);
        CrashLog_WriteLine(CafeSystem::GetForegroundTitleName());
        // title id
        CrashLog_WriteLine(fmt::format("TitleId: {:016x}", CafeSystem::GetForegroundTitleId()));
        // rpx hash
        snprintf(dumpLine, sizeof(dumpLine), "RPXHash: %08x (Update: %08x)", CafeSystem::GetRPXHashBase(), CafeSystem::GetRPXHashUpdated());
        CrashLog_WriteLine(dumpLine);
    }
    else
    {
        CrashLog_WriteLine("Not running");
    }
    // info about active PPC instance:
    CrashLog_WriteLine("");
    CrashLog_WriteHeader("Active PPC instance");
	PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
    if (hCPU)
    {
        OSThread_t* currentThread = coreinit::OSGetCurrentThread();
        uint32 threadPtr = memory_getVirtualOffsetFromPointer(coreinit::OSGetCurrentThread());
        snprintf(dumpLine, sizeof(dumpLine), "IP 0x%08x LR 0x%08x Thread 0x%08x", hCPU->instructionPointer, hCPU->spr.LR, threadPtr);
        CrashLog_WriteLine(dumpLine);

        // GPR info
        CrashLog_WriteLine("");
        auto gprs = hCPU->gpr;
        snprintf(dumpLine, sizeof(dumpLine), "r0 =%08x r1 =%08x r2 =%08x r3 =%08x r4 =%08x r5 =%08x r6 =%08x r7 =%08x", gprs[0], gprs[1], gprs[2], gprs[3], gprs[4], gprs[5], gprs[6], gprs[7]);
        CrashLog_WriteLine(dumpLine);
        snprintf(dumpLine, sizeof(dumpLine), "r8 =%08x r9 =%08x r10=%08x r11=%08x r12=%08x r13=%08x r14=%08x r15=%08x", gprs[8], gprs[9], gprs[10], gprs[11], gprs[12], gprs[13], gprs[14], gprs[15]);
        CrashLog_WriteLine(dumpLine);
        snprintf(dumpLine, sizeof(dumpLine), "r16=%08x r17=%08x r18=%08x r19=%08x r20=%08x r21=%08x r22=%08x r23=%08x", gprs[16], gprs[17], gprs[18], gprs[19], gprs[20], gprs[21], gprs[22], gprs[23]);
        CrashLog_WriteLine(dumpLine);
        snprintf(dumpLine, sizeof(dumpLine), "r24=%08x r25=%08x r26=%08x r27=%08x r28=%08x r29=%08x r30=%08x r31=%08x", gprs[24], gprs[25], gprs[26], gprs[27], gprs[28], gprs[29], gprs[30], gprs[31]);
        CrashLog_WriteLine(dumpLine);

        // stack trace
        MPTR currentStackVAddr = hCPU->gpr[1];
        CrashLog_WriteLine("");
        CrashLog_WriteHeader("PPC stack trace");
        DebugLogStackTrace(currentThread, currentStackVAddr);

        // stack dump
        CrashLog_WriteLine("");
        CrashLog_WriteHeader("PPC stack dump");
        for (uint32 i = 0; i < 16; i++)
        {
            MPTR lineAddr = currentStackVAddr + i * 8 * 4;
            if (memory_isAddressRangeAccessible(lineAddr, 8 * 4))
            {
                snprintf(dumpLine, sizeof(dumpLine), "[0x%08x] %08x %08x %08x %08x - %08x %08x %08x %08x", lineAddr, memory_readU32(lineAddr + 0), memory_readU32(lineAddr + 4), memory_readU32(lineAddr + 8), memory_readU32(lineAddr + 12), memory_readU32(lineAddr + 16), memory_readU32(lineAddr + 20), memory_readU32(lineAddr + 24), memory_readU32(lineAddr + 28));
                CrashLog_WriteLine(dumpLine);
            }
            else
            {
                snprintf(dumpLine, sizeof(dumpLine), "[0x%08x] ?", lineAddr);
                CrashLog_WriteLine(dumpLine);
            }
        }
    }
    else
    {
        CrashLog_WriteLine("Not active");
    }

    // PPC thread log
    CrashLog_WriteLine("");
    CrashLog_WriteHeader("PPC threads");
    if (activeThreadCount == 0)
    {
        CrashLog_WriteLine("None active");
    }
    for (sint32 i = 0; i < activeThreadCount; i++)
    {
        MPTR threadItrMPTR = activeThread[i];
        OSThread_t* threadItrBE = (OSThread_t*)memory_getPointerFromVirtualOffset(threadItrMPTR);

        // get thread state
        OSThread_t::THREAD_STATE threadState = threadItrBE->state;
        const char* threadStateStr = "UNDEFINED";
        if (threadItrBE->suspendCounter != 0)
            threadStateStr = "SUSPENDED";
        else if (threadState == OSThread_t::THREAD_STATE::STATE_NONE)
            threadStateStr = "NONE";
        else if (threadState == OSThread_t::THREAD_STATE::STATE_READY)
            threadStateStr = "READY";
        else if (threadState == OSThread_t::THREAD_STATE::STATE_RUNNING)
            threadStateStr = "RUNNING";
        else if (threadState == OSThread_t::THREAD_STATE::STATE_WAITING)
            threadStateStr = "WAITING";
        else if (threadState == OSThread_t::THREAD_STATE::STATE_MORIBUND)
            threadStateStr = "MORIBUND";
        // generate log line
        uint8 affinity = threadItrBE->attr;
        sint32 effectivePriority = threadItrBE->effectivePriority;
        const char* threadName = "NULL";
        if (!threadItrBE->threadName.IsNull())
            threadName = threadItrBE->threadName.GetPtr();
        snprintf(dumpLine, sizeof(dumpLine), "%08x Ent %08x IP %08x LR %08x %-9s Aff %d%d%d Pri %2d Name %s", threadItrMPTR, threadItrBE->entrypoint.GetMPTR(), threadItrBE->context.srr0, _swapEndianU32(threadItrBE->context.lr), threadStateStr, (affinity >> 0) & 1, (affinity >> 1) & 1, (affinity >> 2) & 1, effectivePriority, threadName);
        // write line to log
        CrashLog_WriteLine(dumpLine);
    }

    if (auto callback = s_additionalLogCallback.load(std::memory_order_acquire))
        callback();
}
