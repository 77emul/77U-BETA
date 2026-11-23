#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/TCL/TCL.h"

#include "HW/Latte/Core/LattePM4.h"

namespace TCL
{
	SysAllocator<coreinit::OSEvent> s_updateRetirementEvent;
	uint64 s_currentRetireMarker = 0;

	struct TCLStatePPC // mapped into PPC space
	{
		uint64be gpuRetireMarker; // written by GPU
	};

	SysAllocator<TCLStatePPC> s_tclStatePPC;

	// called from GPU for timestamp EOP event
	void TCLGPUNotifyNewRetirementTimestamp()
	{
		// gpuRetireMarker is updated via event eop command
		__OSLockScheduler();
		coreinit::OSSignalEventAllInternal(s_updateRetirementEvent.GetPtr());
		__OSUnlockScheduler();
	}

	int TCLTimestamp(TCLTimestampId id, uint64be* timestampOut)
	{
		if (id == TCLTimestampId::TIMESTAMP_LAST_BUFFER_RETIRED)
		{
			MEMPTR<uint32> b;
			// this is the timestamp of the last buffer that was retired by the GPU
			stdx::atomic_ref<uint64be> retireTimestamp(s_tclStatePPC->gpuRetireMarker);
			*timestampOut = retireTimestamp.load();
			return 0;
		}
		else
		{
			cemuLog_log(LogType::Force, "TCLTimestamp(): Unsupported timestamp ID {}", (uint32)id);
			*timestampOut = 0;
			return 0;
		}
	}

	int TCLWaitTimestamp(TCLTimestampId id, uint64 waitTs, uint64 timeout)
	{
		if (id == TCLTimestampId::TIMESTAMP_LAST_BUFFER_RETIRED)
		{
			while ( true )
			{
				stdx::atomic_ref<uint64be> retireTimestamp(s_tclStatePPC->gpuRetireMarker);
				uint64 currentTimestamp = retireTimestamp.load();
				if (currentTimestamp >= waitTs)
					return 0;
				coreinit::OSWaitEvent(s_updateRetirementEvent.GetPtr());
			}
		}
		else
		{
			cemuLog_log(LogType::Force, "TCLWaitTimestamp(): Unsupported timestamp ID {}", (uint32)id);
		}
		return 0;
	}

	static constexpr uint32 TCL_RING_BUFFER_SIZE = 4096; // in U32s

	std::atomic<uint32> tclRingBufferA[TCL_RING_BUFFER_SIZE];
	std::atomic<uint32> tclRingBufferA_readIndex{0};
	std::atomic<uint32> tclRingBufferA_writeIndex{0};
#if BOOST_OS_IOS
	static std::mutex s_ringDataMutex;
	static std::condition_variable s_ringDataCondition;
	static std::atomic<bool> s_hasWaiter{false};
	static std::atomic<bool> s_idleWaitEnabled{false};
#endif

	// GPU code calls this to grab the next command word
	bool TCLGPUReadRBWord(uint32& cmdWord)
	{
		uint32 readIndex = tclRingBufferA_readIndex.load(std::memory_order::relaxed);
		uint32 writeIndex = tclRingBufferA_writeIndex.load(std::memory_order::acquire);
		if (readIndex == writeIndex)
			return false;
		cmdWord = tclRingBufferA[readIndex].load(std::memory_order::relaxed);
		tclRingBufferA_readIndex.store((readIndex + 1) & (TCL_RING_BUFFER_SIZE - 1), std::memory_order::release);
		return true;
	}

	uint32 TCLGPUReadRBWords(uint32* dst, uint32 count)
	{
		uint32 readIndex = tclRingBufferA_readIndex.load(std::memory_order::relaxed);
		uint32 writeIndex = tclRingBufferA_writeIndex.load(std::memory_order::acquire);
		if (readIndex == writeIndex || count == 0)
			return 0;
		uint32 available = (writeIndex + TCL_RING_BUFFER_SIZE - readIndex) & (TCL_RING_BUFFER_SIZE - 1);
		uint32 toRead = std::min(count, available);
		for (uint32 i = 0; i < toRead; ++i)
		{
			dst[i] = tclRingBufferA[readIndex].load(std::memory_order::relaxed);
			readIndex = (readIndex + 1) & (TCL_RING_BUFFER_SIZE - 1);
		}
		tclRingBufferA_readIndex.store(readIndex, std::memory_order::release);
		return toRead;
	}

	void TCLGPUSetIdleWaitEnabled(bool enabled)
	{
#if BOOST_OS_IOS
		std::lock_guard lock(s_ringDataMutex);
		s_idleWaitEnabled.store(enabled, std::memory_order_release);
		s_ringDataCondition.notify_one();
#endif
	}

	void TCLGPUWaitForRBData()
	{
#if BOOST_OS_IOS
		if (!s_idleWaitEnabled.load(std::memory_order_acquire))
		{
			std::this_thread::yield();
			return;
		}
		// The Latte thread used to spin for the entire time between Wii U command
		// submissions. That occupied a CPU core even when the emulated GPU had no
		// work and competed with the PPC cores during asset/JIT bursts.
		std::unique_lock lock(s_ringDataMutex);
		s_hasWaiter.store(true, std::memory_order_release);
		s_ringDataCondition.wait_for(lock, std::chrono::microseconds(250), [] {
			return !s_idleWaitEnabled.load(std::memory_order_acquire) ||
				tclRingBufferA_readIndex.load(std::memory_order::relaxed) !=
				tclRingBufferA_writeIndex.load(std::memory_order::acquire);
		});
		s_hasWaiter.store(false, std::memory_order_relaxed);
#else
		std::this_thread::yield();
#endif
	}

	void TCLWaitForRBSpace(uint32be numU32s)
	{
		uint32 writeIndex = tclRingBufferA_writeIndex.load(std::memory_order::relaxed);
		while (true)
		{
			uint32 readIndex = tclRingBufferA_readIndex.load(std::memory_order::acquire);
			uint32 distance = (readIndex + TCL_RING_BUFFER_SIZE - writeIndex) & (TCL_RING_BUFFER_SIZE - 1);
			if (writeIndex == readIndex) // buffer completely empty
				distance = TCL_RING_BUFFER_SIZE;
			if (distance >= numU32s + 1) // assume distance minus one, because we are never allowed to completely wrap around
				break;
			_mm_pause();
		}
	}

	// this function assumes that TCLWaitForRBSpace was called and that there is enough space
	void TCLWriteCmd(uint32be* cmd, uint32 cmdLen)
	{
		uint32 writeIndex = tclRingBufferA_writeIndex.load(std::memory_order::relaxed);

		while (cmdLen > 0)
		{
			tclRingBufferA[writeIndex].store(*cmd, std::memory_order::relaxed);
			writeIndex++;
			writeIndex &= (TCL_RING_BUFFER_SIZE - 1);
			cmd++;
			cmdLen--;
		}

		tclRingBufferA_writeIndex.store(writeIndex, std::memory_order::release);

#if BOOST_OS_IOS
		if (s_hasWaiter.load(std::memory_order_acquire))
		{
			std::lock_guard lock(s_ringDataMutex);
			s_ringDataCondition.notify_one();
		}
#endif
	}

	#define EVENT_TYPE_TS		5

	void TCLSubmitRetireMarker(bool triggerEventInterrupt)
	{
		s_currentRetireMarker++;
		uint32be cmd[6];
		cmd[0] = pm4HeaderType3(IT_EVENT_WRITE_EOP, 5);
		cmd[1] = (4 | (EVENT_TYPE_TS << 8)); // event type (bits 8-15) and event index (bits 0-7).
		cmd[2] = MEMPTR<void>(&s_tclStatePPC->gpuRetireMarker).GetMPTR(); // address lower 32bits + data sel bits
		cmd[3] = 0x40000000; // select 64bit write, lower 16 bits are the upper bits of the address
		if (triggerEventInterrupt)
			cmd[3] |= 0x2000000; // trigger interrupt after value has been written
		cmd[4] = (uint32)s_currentRetireMarker; // data lower 32 bits
		cmd[5] = (uint32)(s_currentRetireMarker>>32); // data higher 32 bits
		TCLWriteCmd(cmd, 6);
	}

	int TCLSubmitToRing(uint32be* cmd, uint32 cmdLen, betype<TCLSubmissionFlag>* controlFlags, uint64be* timestampValueOut)
	{
		TCLSubmissionFlag flags = *controlFlags;
		cemu_assert_debug(timestampValueOut); // handle case where this is null

		// make sure there is enough space to submit all commands at one
		uint32 totalCommandLength = cmdLen;
		totalCommandLength += 6; // space needed for TCLSubmitRetireMarker

		TCLWaitForRBSpace(totalCommandLength);

		// submit command buffer
		TCLWriteCmd(cmd, cmdLen);

		// create new marker timestamp and tell GPU to write it to our variable after its done processing the command
		if ((HAS_FLAG(flags, TCLSubmissionFlag::USE_RETIRED_MARKER)))
		{
			TCLSubmitRetireMarker(!HAS_FLAG(flags, TCLSubmissionFlag::NO_MARKER_INTERRUPT));
			*timestampValueOut = s_currentRetireMarker; // incremented before each submit
		}
		else
		{
			cemu_assert_unimplemented();
		}
		return 0;
	}

	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "tcl";
		}

		void RPLMapped() override
		{
			cafeExportRegister("TCL", TCLSubmitToRing, LogType::Placeholder);
			cafeExportRegister("TCL", TCLTimestamp, LogType::Placeholder);
			cafeExportRegister("TCL", TCLWaitTimestamp, LogType::Placeholder);
		};

		void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override
		{
			if (reason == coreinit::RplEntryReason::Loaded)
			{
				s_currentRetireMarker = 0;
				s_tclStatePPC->gpuRetireMarker = 0;
				tclRingBufferA_readIndex.store(0, std::memory_order_relaxed);
				tclRingBufferA_writeIndex.store(0, std::memory_order_relaxed);
				coreinit::OSInitEvent(s_updateRetirementEvent.GetPtr(), coreinit::OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, coreinit::OSEvent::EVENT_MODE::MODE_AUTO);
			}
			else if (reason == coreinit::RplEntryReason::Unloaded)
			{
				s_currentRetireMarker = 0;
				s_tclStatePPC->gpuRetireMarker = 0;
				tclRingBufferA_readIndex.store(0, std::memory_order_relaxed);
				tclRingBufferA_writeIndex.store(0, std::memory_order_relaxed);
			}
		}
	}s_COStclModule;

	COSModule* GetModule()
	{
		return &s_COStclModule;
	}
}
