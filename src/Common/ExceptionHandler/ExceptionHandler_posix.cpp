#include <signal.h>
#include <execinfo.h>
#include <string.h>
#include <string>
#include "config/CemuConfig.h"
#include "util/helpers/StringHelpers.h"
#include "ExceptionHandler.h"

#include "Cafe/HW/Espresso/Debugger/GDBStub.h"
#include "Cafe/HW/Espresso/Debugger/GDBBreakpoints.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <sys/ucontext.h>
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
extern "C" bool JIT26_IsAddressInArenaAsyncSafe(const void* ptr);
#endif
#endif

#if BOOST_OS_LINUX
#include "ELFSymbolTable.h"
#endif

#if BOOST_OS_LINUX
void DemangleAndPrintBacktrace(char** backtrace, size_t size)
{
	ELFSymbolTable symTable;
	for (char** i = backtrace; i < backtrace + size; i++)
	{
		std::string_view traceLine{*i};

		// basic check to see if the backtrace line matches expected format
		size_t parenthesesOpen = traceLine.find_last_of('(');
		size_t parenthesesClose = traceLine.find_last_of(')');
		size_t offsetPlus = traceLine.find_last_of('+');
		if (!parenthesesOpen || !parenthesesClose || !offsetPlus ||
			 offsetPlus < parenthesesOpen || offsetPlus > parenthesesClose)
		{
			// fall back to default string
            CrashLog_WriteLine(traceLine);
			continue;
		}

		// attempt to resolve symbol from regular symbol table if missing from dynamic symbol table
		uint64 newOffset = -1;
		std::string_view symbolName = traceLine.substr(parenthesesOpen+1, offsetPlus-parenthesesOpen-1);
		if (symbolName.empty())
		{
			uint64 symbolOffset = StringHelpers::ToInt64(traceLine.substr(offsetPlus+1,offsetPlus+1-parenthesesClose-1));
			symbolName = symTable.OffsetToSymbol(symbolOffset, newOffset);
		}

        CrashLog_WriteLine(traceLine.substr(0, parenthesesOpen+1), false);

        CrashLog_WriteLine(boost::core::demangle(symbolName.empty() ? "" : symbolName.data()), false);

		// print relative or existing symbol offset.
        CrashLog_WriteLine("+", false);
		if (newOffset != -1)
		{
            CrashLog_WriteLine(fmt::format("0x{:x}", newOffset), false);
            CrashLog_WriteLine(traceLine.substr(parenthesesClose));
		}
		else
		{
            CrashLog_WriteLine(traceLine.substr(offsetPlus+1));
		}
	}
}
#endif

// handle signals that would dump core, print stacktrace and then dump depending on config
void handlerDumpingSignal(int sig, siginfo_t *info, void *context)
{
#if defined(ARCH_X86_64) && BOOST_OS_LINUX
	// Check for hardware breakpoints
	if (info->si_signo == SIGTRAP && info->si_code == TRAP_HWBKPT)
	{
		uint64 dr6 = _ReadDR6();
		g_gdbstub->HandleAccessException(dr6);
		return;
	}
#endif

#if defined(__APPLE__) && defined(__aarch64__)
	ucontext_t *uc = (ucontext_t *)context;
	uintptr_t crashPC = (uc && uc->uc_mcontext) ? uc->uc_mcontext->__ss.__pc : 0;
	uintptr_t crashFP = (uc && uc->uc_mcontext) ? uc->uc_mcontext->__ss.__fp : 0;
	uintptr_t crashSP = (uc && uc->uc_mcontext) ? uc->uc_mcontext->__ss.__sp : 0;

	// Check for StikJIT brk #0xf00d breakpoint without debugger attached
	if (sig == SIGTRAP && crashPC != 0)
	{
		uint32_t insn = 0;
		vm_size_t outSize = 0;
		if (vm_read_overwrite(mach_task_self(), (vm_address_t)crashPC, sizeof(insn), (vm_address_t)&insn, &outSize) == KERN_SUCCESS && outSize == sizeof(insn))
		{
			if (insn == 0xd43e01a0) // brk #0xf00d
			{
				uc->uc_mcontext->__ss.__pc += 4;
				return;
			}
		}
	}
#endif

	// Begin persistent crash log immediately so register dump, thread info,
	// and guest PPC state are guaranteed written and fsynced to disk BEFORE
	// any unwinding or backtrace is attempted!
	ExceptionHandler_BeginPersistentCrashLog(sig, info ? info->si_addr : nullptr, nullptr, 0, context);

	char* sigName = strsignal(sig);
	if (sigName)
	{
		printf("%s!\n", sigName);
	}
	else
	{
		printf("Unknown core dumping signal!\n");
	}

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// The regular logger is asynchronous and may already hold its mutex on the
	// faulting thread. Keep the fatal path independent and write only through
	// the pre-opened persistent crash descriptor.
	CrashLog_SetOutputChannels(false, false);
#else
	if(!CrashLog_Create())
	{
		ExceptionHandler_FlushPersistentCrashLog();
		return; // give up if crashlog was already created
	}
#endif

	CrashLog_WriteLine(fmt::format("Error: signal {}:", sig));

	void* backtraceArray[128];
	size_t size = 0;

	bool canSafelyBacktrace = true;
#if defined(__APPLE__) && defined(__aarch64__)
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	if (crashPC != 0 && JIT26_IsAddressInArenaAsyncSafe((const void*)crashPC))
	{
		canSafelyBacktrace = false;
	}
#endif
	if (crashFP == 0 || (crashFP & 0xf) != 0 || crashFP < crashSP || crashFP > crashSP + 16 * 1024 * 1024)
	{
		canSafelyBacktrace = false;
	}
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	if (crashFP != 0 && JIT26_IsAddressInArenaAsyncSafe((const void*)crashFP))
	{
		canSafelyBacktrace = false;
	}
#endif
#endif

	if (canSafelyBacktrace)
	{
		size = backtrace(backtraceArray, 128);
#if defined(ARCH_X86_64) && BOOST_OS_LINUX > 0
		ucontext_t *uc = (ucontext_t *)context;
		if (uc)
			backtraceArray[0] = (void *)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__APPLE__) && defined(__aarch64__)
		if (crashPC != 0 && size > 0)
		{
			backtraceArray[0] = (void *)crashPC;
		}
#endif
		ExceptionHandler_AppendPersistentBacktrace(backtraceArray, size);

#if BOOST_OS_LINUX
		char** symbol_trace = backtrace_symbols(backtraceArray, size);
		if (symbol_trace)
		{
			DemangleAndPrintBacktrace(symbol_trace, size);
			free(symbol_trace);
		}
		else
		{
			CrashLog_WriteLine("Failed to read backtrace");
		}
#else
		backtrace_symbols_fd(backtraceArray, (int)size, STDERR_FILENO);
#endif
	}
	else
	{
		CrashLog_WriteLine("[CrashHandler] Stack backtrace omitted: execution inside JIT or invalid frame pointer (x29 holds PPCInterpreter pointer)");
		ExceptionHandler_FlushPersistentCrashLog();
	}

	std::cerr << fmt::format("\nStacktrace and additional info written to:") << std::endl;
	std::cerr << cemuLog_GetLogFilePath().generic_string() << std::endl;

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	ExceptionHandler_LogGeneralInfo();
#else
	CrashLog_SetOutputChannels(false, true);
	ExceptionHandler_LogGeneralInfo();
	CrashLog_SetOutputChannels(true, true);
#endif
	ExceptionHandler_FlushPersistentCrashLog();

	if (GetConfig().crash_dump == CrashDump::Enabled)
	{
		// reset signal handler to default and re-raise signal to dump core
		signal(sig, SIG_DFL);
		raise(sig);
		return;
	}
	// exit process ignoring all issues
	_Exit(1);
}

void handler_SIGINT(int sig)
{
	/*
	 * Received when pressing CTRL + C in a console
	 * Ideally should be exiting cleanly after saving settings but currently
	 * there's no clean exit pathway (at least on linux) and exiting the app
	 * by any mean ends up with a SIGABRT from the standard library destroying
	 * threads.
	 */
	_Exit(0);
}

void ExceptionHandler_Init()
{
	ExceptionHandler_InstallAltStack();

	struct sigaction action;
	action.sa_flags = 0;
	sigfillset(&action.sa_mask); // don't allow signals to be interrupted

	action.sa_handler = handler_SIGINT;
	sigaction(SIGINT, &action, nullptr);
	// SIGTERM is how iOS/Xcode normally stops the app. Treating it as a crash
	// overwrites the last actionable fault report with a harmless termination.
	sigaction(SIGTERM, &action, nullptr);

    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    action.sa_handler = nullptr;
	action.sa_sigaction = handlerDumpingSignal;
	sigaction(SIGABRT, &action, nullptr);
	sigaction(SIGBUS, &action, nullptr);
	sigaction(SIGFPE, &action, nullptr);
	sigaction(SIGILL, &action, nullptr);
	sigaction(SIGIOT, &action, nullptr);
	sigaction(SIGQUIT, &action, nullptr);
	sigaction(SIGSEGV, &action, nullptr);
	sigaction(SIGSYS, &action, nullptr);
	sigaction(SIGTRAP, &action, nullptr);
}
