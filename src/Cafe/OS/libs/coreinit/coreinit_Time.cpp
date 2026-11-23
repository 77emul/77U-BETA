#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"

namespace coreinit
{
	namespace
	{
		constexpr uint64 CAFE_TIME_DIAGNOSTIC_EMPTY_MIN = std::numeric_limits<uint64>::max();
		constexpr uint64 CAFE_TIME_DIAGNOSTIC_WINDOW_DIVISOR = 4;
		constexpr uint32 CAFE_TIME_DIAGNOSTIC_SAMPLE_INTERVAL = 4096;
		static_assert(std::has_single_bit(CAFE_TIME_DIAGNOSTIC_SAMPLE_INTERVAL));

		struct CafeTimeConsumerAPIStats
		{
			std::atomic<uint64> calls{};
			std::atomic<uint64> lastGuestTime{};
			std::atomic<HRTick> lastHostTime{};
			std::atomic<uint64> deltaSamples{};
			std::atomic<uint64> guestDeltaSum{};
			std::atomic<uint64> hostDeltaSum{};
			std::atomic<uint64> minGuestDelta{CAFE_TIME_DIAGNOSTIC_EMPTY_MIN};
			std::atomic<uint64> maxGuestDelta{};
		};

		CafeTimeConsumerAPIStats s_osGetSystemTimeStats;
		CafeTimeConsumerAPIStats s_osGetSystemTickStats;
		CafeTimeConsumerAPIStats s_mftbStats;
		CafeTimeConsumerAPIStats s_alarmTimeStats;
		std::atomic<uint64> s_osTimeToSystemTimeConversions{};
		std::atomic<uint64> s_osTicksToCalendarTimeConversions{};
		std::atomic_flag s_timeDiagnosticWindowLock = ATOMIC_FLAG_INIT;
		HRTick s_timeDiagnosticWindowStart{};

		std::atomic<double> s_timeDiagnosticWindowSeconds{};
		std::atomic<double> s_osGetSystemTimeCallsPerSecond{};
		std::atomic<double> s_osGetSystemTickCallsPerSecond{};
		std::atomic<double> s_mftbReadsPerSecond{};
		std::atomic<double> s_alarmTimeReadsPerSecond{};
		std::atomic<double> s_osTimeToSystemTimeConversionsPerSecond{};
		std::atomic<double> s_osTicksToCalendarTimeConversionsPerSecond{};
		std::atomic<uint64> s_osGetSystemTimeMinDelta{};
		std::atomic<uint64> s_osGetSystemTimeMaxDelta{};
		std::atomic<uint64> s_osGetSystemTickMinDelta{};
		std::atomic<uint64> s_osGetSystemTickMaxDelta{};
		std::atomic<uint64> s_mftbMinDelta{};
		std::atomic<uint64> s_mftbMaxDelta{};
		std::atomic<uint64> s_alarmTimeMinDelta{};
		std::atomic<uint64> s_alarmTimeMaxDelta{};
		std::atomic<uint64> s_minGuestTimeDelta{};
		std::atomic<uint64> s_maxGuestTimeDelta{};
		std::atomic<double> s_averageGuestTimeDelta{};
		std::atomic<double> s_guestToHostDeltaRatio{};

		void UpdateMinimum(std::atomic<uint64>& target, uint64 value)
		{
			uint64 current = target.load(std::memory_order_relaxed);
			while (value < current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed))
			{
			}
		}

		void UpdateMaximum(std::atomic<uint64>& target, uint64 value)
		{
			uint64 current = target.load(std::memory_order_relaxed);
			while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed))
			{
			}
		}

		void RecordTimeValue(CafeTimeConsumerAPIStats& stats, uint64 guestTime, bool wrapsAt32Bits, uint32 sampleWeight)
		{
			const HRTick hostTime = HighResolutionTimer::now().getTick();
			stats.calls.fetch_add(sampleWeight, std::memory_order_relaxed);
			const uint64 previousGuestTime = stats.lastGuestTime.exchange(guestTime, std::memory_order_relaxed);
			const HRTick previousHostTime = stats.lastHostTime.exchange(hostTime, std::memory_order_relaxed);
			if (previousHostTime == 0 || hostTime <= previousHostTime)
				return;

			uint64 guestDelta;
			if (wrapsAt32Bits)
				guestDelta = static_cast<uint32>(guestTime) - static_cast<uint32>(previousGuestTime);
			else
			{
				if (guestTime < previousGuestTime)
					return;
				guestDelta = guestTime - previousGuestTime;
			}

			guestDelta /= sampleWeight;
			const uint64 hostDelta = (hostTime - previousHostTime) / sampleWeight;
			stats.deltaSamples.fetch_add(1, std::memory_order_relaxed);
			stats.guestDeltaSum.fetch_add(guestDelta, std::memory_order_relaxed);
			stats.hostDeltaSum.fetch_add(hostDelta, std::memory_order_relaxed);
			UpdateMinimum(stats.minGuestDelta, guestDelta);
			UpdateMaximum(stats.maxGuestDelta, guestDelta);
		}

		void ResetAPIStats(CafeTimeConsumerAPIStats& stats)
		{
			stats.calls.store(0, std::memory_order_relaxed);
			stats.lastGuestTime.store(0, std::memory_order_relaxed);
			stats.lastHostTime.store(0, std::memory_order_relaxed);
			stats.deltaSamples.store(0, std::memory_order_relaxed);
			stats.guestDeltaSum.store(0, std::memory_order_relaxed);
			stats.hostDeltaSum.store(0, std::memory_order_relaxed);
			stats.minGuestDelta.store(CAFE_TIME_DIAGNOSTIC_EMPTY_MIN, std::memory_order_relaxed);
			stats.maxGuestDelta.store(0, std::memory_order_relaxed);
		}

		struct CompletedAPIStats
		{
			uint64 calls{};
			uint64 deltaSamples{};
			uint64 guestDeltaSum{};
			uint64 hostDeltaSum{};
			uint64 minGuestDelta{};
			uint64 maxGuestDelta{};
		};

		CompletedAPIStats CompleteAPIStats(CafeTimeConsumerAPIStats& stats)
		{
			CompletedAPIStats completed{};
			completed.calls = stats.calls.exchange(0, std::memory_order_relaxed);
			completed.deltaSamples = stats.deltaSamples.exchange(0, std::memory_order_relaxed);
			completed.guestDeltaSum = stats.guestDeltaSum.exchange(0, std::memory_order_relaxed);
			completed.hostDeltaSum = stats.hostDeltaSum.exchange(0, std::memory_order_relaxed);
			completed.minGuestDelta = stats.minGuestDelta.exchange(CAFE_TIME_DIAGNOSTIC_EMPTY_MIN, std::memory_order_relaxed);
			completed.maxGuestDelta = stats.maxGuestDelta.exchange(0, std::memory_order_relaxed);
			if (completed.minGuestDelta == CAFE_TIME_DIAGNOSTIC_EMPTY_MIN)
				completed.minGuestDelta = 0;
			return completed;
		}
	}

	uint64 coreinit_GetMFTB()
	{
		// bus clock is 1/5th of core clock
		// timer clock is 1/4th of bus clock
		return PPCInterpreter_getMainCoreCycleCounter() / 20ULL;
	}

	uint64 OSGetSystemTime()
	{
		const uint64 guestTime = coreinit_GetMFTB();
		static thread_local uint32 sampleCounter{};
		if ((++sampleCounter & (CAFE_TIME_DIAGNOSTIC_SAMPLE_INTERVAL - 1)) == 0)
			RecordTimeValue(s_osGetSystemTimeStats, guestTime, false, CAFE_TIME_DIAGNOSTIC_SAMPLE_INTERVAL);
		return guestTime;
	}

	uint64 OSGetTime()
	{
		return OSGetSystemTime() + ppcCyclesSince2000TimerClock;
	}

	uint32 OSGetSystemTick()
	{
		const uint32 guestTime = static_cast<uint32>(coreinit_GetMFTB());
		static thread_local uint32 sampleCounter{};
		if ((++sampleCounter & (CAFE_TIME_DIAGNOSTIC_SAMPLE_INTERVAL - 1)) == 0)
			RecordTimeValue(s_osGetSystemTickStats, guestTime, true, CAFE_TIME_DIAGNOSTIC_SAMPLE_INTERVAL);
		return guestTime;
	}

	uint32 OSGetTick()
	{
		uint64 osTime = OSGetTime();
		return static_cast<uint32>(osTime);
	}

	uint32 getLeapDaysUntilYear(uint32 year)
	{
		if (year == 0)
			return 0;
		return (year + 3) / 4 - (year - 1) / 100 + (year - 1) / 400;
	}

	bool IsLeapYear(uint32 year)
	{
		if (((year & 3) == 0) && (year % 100) != 0)
			return true;
		if ((year % 400) == 0)
			return true;
		return false;
	}

	uint32 dayToMonth[12] =
	{
		0,31,59,90,120,151,181,212,243,273,304,334
	};

	uint32 dayToMonthLeapYear[12] =
	{
		0,31,60,91,121,152,182,213,244,274,305,335
	};

	uint32 getDayInYearByYearAndMonth(uint32 year, uint32 month)
	{
		// Project Zero Maiden of Black Water (JPN) gives us an invalid calendar object
		month %= 12; // or return 0 if too big?
		
		if (IsLeapYear(year))
			return dayToMonthLeapYear[month];
		
		return dayToMonth[month];
	}


	inline const uint64 DAY_BIAS_2000 = 0xB2575;

	uint64 OSCalendarTimeToTicks(OSCalendarTime_t *calendar)
	{
		uint32 year = calendar->year;

		uint32 leapDays = getLeapDaysUntilYear(year);
		uint32 startDayOfCurrentMonth = getDayInYearByYearAndMonth(year, calendar->month);

		uint64 dayInYear = (startDayOfCurrentMonth + calendar->dayOfMonth) - 1;
		
		uint64 dayCount = dayInYear + year * 365 + leapDays - DAY_BIAS_2000;

		// convert date to seconds
		uint64 tSeconds = 0;
		tSeconds += (uint64)dayCount * 24 * 60 * 60;
		tSeconds += (uint64)calendar->hour * 60 * 60;
		tSeconds += (uint64)calendar->minute * 60;
		tSeconds += (uint64)calendar->second;

		uint64 tSubSecondTicks = 0;
		tSubSecondTicks += (uint64)calendar->millisecond * ESPRESSO_TIMER_CLOCK / 1000;
		tSubSecondTicks += (uint64)calendar->microsecond * ESPRESSO_TIMER_CLOCK / 1000000;

		return tSeconds * ESPRESSO_TIMER_CLOCK + tSubSecondTicks;
	}

	void OSTicksToCalendarTime(uint64 ticks, OSCalendarTime_t* calenderStruct)
	{
		s_osTicksToCalendarTimeConversions.fetch_add(1, std::memory_order_relaxed);
		uint64 tSubSecondTicks = ticks % ESPRESSO_TIMER_CLOCK;
		uint64 tSeconds = ticks / ESPRESSO_TIMER_CLOCK;

		uint64 microsecond = tSubSecondTicks * 1000000ull / ESPRESSO_TIMER_CLOCK;
		microsecond %= 1000ull;
		calenderStruct->microsecond = (uint32)microsecond;

		uint64 millisecond = tSubSecondTicks * 1000ull / ESPRESSO_TIMER_CLOCK;
		millisecond %= 1000ull;
		calenderStruct->millisecond = (uint32)millisecond;

		uint64 dayOfWeek = (tSeconds/(24ull * 60 * 60) + 6ull) % 7ull;
		uint64 secondOfDay = (tSeconds % (24ull * 60 * 60));

		calenderStruct->dayOfWeek = (sint32)dayOfWeek;

		uint64 daysSince0AD = tSeconds / (24ull * 60 * 60) + DAY_BIAS_2000;
		uint32 year = (uint32)(daysSince0AD / 365ull);
		uint64 yearStartDay = year * 365 + getLeapDaysUntilYear(year);
		while (yearStartDay > daysSince0AD)
		{
			year--;
			if (IsLeapYear(year))
				yearStartDay -= 366;
			else
				yearStartDay -= 365;
		}

		calenderStruct->year = year;

		// calculate time of day
		uint32 tempSecond = (uint32)secondOfDay;
		calenderStruct->second = tempSecond % 60;
		tempSecond /= 60;
		calenderStruct->minute = tempSecond % 60;
		tempSecond /= 60;
		calenderStruct->hour = tempSecond % 24;
		tempSecond /= 24;

		// calculate month and day
		uint32 dayInYear = (uint32)(daysSince0AD - yearStartDay);
		bool isLeapYear = IsLeapYear(year);

		uint32 month = 0; // 0-11
		uint32 dayInMonth = 0;

		if (isLeapYear && dayInYear < (31+29))
		{
			if (dayInYear < 31)
			{
				// January
				month = 0;
				dayInMonth = dayInYear;
			}
			else
			{
				// February
				month = 1;
				dayInMonth = dayInYear-31;
			}
		}
		else
		{
			if (isLeapYear)
				dayInYear--;

			dayInMonth = dayInYear;
			if (dayInYear >= 334)
			{
				// December
				month = 11;
				dayInMonth -= 334;
			}
			else if (dayInYear >= 304)
			{
				// November
				month = 10;
				dayInMonth -= 304;
			}
			else if (dayInYear >= 273)
			{
				// October
				month = 9;
				dayInMonth -= 273;
			}
			else if (dayInYear >= 243)
			{
				// September
				month = 8;
				dayInMonth -= 243;
			}
			else if (dayInYear >= 212)
			{
				// August
				month = 7;
				dayInMonth -= 212;
			}
			else if (dayInYear >= 181)
			{
				// July
				month = 6;
				dayInMonth -= 181;
			}
			else if (dayInYear >= 151)
			{
				// June
				month = 5;
				dayInMonth -= 151;
			}
			else if (dayInYear >= 120)
			{
				// May
				month = 4;
				dayInMonth -= 120;
			}
			else if (dayInYear >= 90)
			{
				// April
				month = 3;
				dayInMonth -= 90;
			}
			else if (dayInYear >= 59)
			{
				// March
				month = 2;
				dayInMonth -= 59;
			}
			else if (dayInYear >= 31)
			{
				// February
				month = 1;
				dayInMonth -= 31;
			}
			else
			{
				// January
				month = 0;
				dayInMonth -= 0;
			}
		}

		calenderStruct->dayOfYear = dayInYear;
		calenderStruct->month = month;
		calenderStruct->dayOfMonth = dayInMonth + 1;
	}

	uint32 getDaysInMonth(uint32 year, uint32 month)
	{
		switch (month)
		{
		case 0: // January
			return 31;
		case 1: // February
			return IsLeapYear(year) ? 29 : 28;
		case 2: // March
			return 31;
		case 3: // April
			return 30;
		case 4: // May
			return 31;
		case 5: // June
			return 30;
		case 6: // July
			return 31;
		case 7: // August
			return 31;
		case 8: // September
			return 30;
		case 9: // October
			return 31;
		case 10: // November
			return 30;
		case 11: // December
			return 31;
		default:
			cemu_assert(false);
		}
		return 31;
	}

	void verifyDateMatch(OSCalendarTime_t* ct1, OSCalendarTime_t* ct2)
	{
		sint64 m1 = ct1->millisecond * 1000 + ct1->microsecond;
		sint64 m2 = ct2->millisecond * 1000 + ct2->microsecond;
		sint64 microDif = std::abs(m1 - m2);

		if (ct1->year != ct2->year ||
			ct1->month != ct2->month ||
			ct1->dayOfMonth != ct2->dayOfMonth ||
			ct1->hour != ct2->hour ||
			ct1->minute != ct2->minute ||
			ct1->second != ct2->second ||
			microDif > 1ull)
		{
			debug_printf("Mismatch\n");
			assert_dbg();
		}
	}

	void timeTest()
	{
		sint32 iterCount = 0;

		OSCalendarTime_t ct{};
		ct.year = 2000;
		ct.month = 1;
		ct.dayOfMonth = 1;
		ct.hour = 1;
		ct.minute = 1;
		ct.second = 1;
		ct.millisecond = 123;
		ct.microsecond = 321;

		while (true)
		{
			iterCount++;


			uint64 ticks = OSCalendarTimeToTicks(&ct);

			// make sure converting it back results in the same date
			OSCalendarTime_t ctTemp;
			OSTicksToCalendarTime(ticks, &ctTemp);
			verifyDateMatch(&ct, &ctTemp);

			// add a day
			ticks += 24ull * 60 * 60 * ESPRESSO_TIMER_CLOCK;

			OSCalendarTime_t ctOutput;
			OSTicksToCalendarTime(ticks, &ctOutput);

			OSCalendarTime_t ctExpected;
			ctExpected = ct;
			// add a day manually
			sint32 daysInMonth = getDaysInMonth(ctExpected.year, ctExpected.month);
			ctExpected.dayOfMonth = ctExpected.dayOfMonth + 1;
			if (ctExpected.dayOfMonth >= daysInMonth+1)
			{
				ctExpected.dayOfMonth = 1;
				ctExpected.month = ctExpected.month + 1;
				if (ctExpected.month > 11)
				{
					ctExpected.month = 0;
					ctExpected.year = ctExpected.year + 1;
				}
			}

			verifyDateMatch(&ctExpected, &ctOutput);

			ct = ctOutput;
		}
	}

	void CafeTimeConsumerDiagnostics_Reset()
	{
		while (s_timeDiagnosticWindowLock.test_and_set(std::memory_order_acquire))
			std::this_thread::yield();

		ResetAPIStats(s_osGetSystemTimeStats);
		ResetAPIStats(s_osGetSystemTickStats);
		ResetAPIStats(s_mftbStats);
		ResetAPIStats(s_alarmTimeStats);
		s_osTimeToSystemTimeConversions.store(0, std::memory_order_relaxed);
		s_osTicksToCalendarTimeConversions.store(0, std::memory_order_relaxed);
		s_timeDiagnosticWindowStart = 0;
		s_timeDiagnosticWindowSeconds.store(0.0, std::memory_order_relaxed);
		s_osGetSystemTimeCallsPerSecond.store(0.0, std::memory_order_relaxed);
		s_osGetSystemTickCallsPerSecond.store(0.0, std::memory_order_relaxed);
		s_mftbReadsPerSecond.store(0.0, std::memory_order_relaxed);
		s_alarmTimeReadsPerSecond.store(0.0, std::memory_order_relaxed);
		s_osTimeToSystemTimeConversionsPerSecond.store(0.0, std::memory_order_relaxed);
		s_osTicksToCalendarTimeConversionsPerSecond.store(0.0, std::memory_order_relaxed);
		s_osGetSystemTimeMinDelta.store(0, std::memory_order_relaxed);
		s_osGetSystemTimeMaxDelta.store(0, std::memory_order_relaxed);
		s_osGetSystemTickMinDelta.store(0, std::memory_order_relaxed);
		s_osGetSystemTickMaxDelta.store(0, std::memory_order_relaxed);
		s_mftbMinDelta.store(0, std::memory_order_relaxed);
		s_mftbMaxDelta.store(0, std::memory_order_relaxed);
		s_alarmTimeMinDelta.store(0, std::memory_order_relaxed);
		s_alarmTimeMaxDelta.store(0, std::memory_order_relaxed);
		s_minGuestTimeDelta.store(0, std::memory_order_relaxed);
		s_maxGuestTimeDelta.store(0, std::memory_order_relaxed);
		s_averageGuestTimeDelta.store(0.0, std::memory_order_relaxed);
		s_guestToHostDeltaRatio.store(0.0, std::memory_order_relaxed);
		s_timeDiagnosticWindowLock.clear(std::memory_order_release);
	}

	void CafeTimeConsumerDiagnostics_RecordMFTB(uint64 guestTime)
	{
		static thread_local uint32 sampleCounter{};
		if ((++sampleCounter & (CAFE_TIME_DIAGNOSTIC_SAMPLE_INTERVAL - 1)) == 0)
			RecordTimeValue(s_mftbStats, guestTime, false, CAFE_TIME_DIAGNOSTIC_SAMPLE_INTERVAL);
	}

	void CafeTimeConsumerDiagnostics_RecordAlarmTimeRead(uint64 guestTime)
	{
		static thread_local uint32 sampleCounter{};
		if ((++sampleCounter & (CAFE_TIME_DIAGNOSTIC_SAMPLE_INTERVAL - 1)) == 0)
			RecordTimeValue(s_alarmTimeStats, guestTime, false, CAFE_TIME_DIAGNOSTIC_SAMPLE_INTERVAL);
	}

	void CafeTimeConsumerDiagnostics_RecordWindowTick()
	{
		if (s_timeDiagnosticWindowLock.test_and_set(std::memory_order_acquire))
			return;

		const HRTick now = HighResolutionTimer::now().getTick();
		const HRTick frequency = HighResolutionTimer::getFrequency();
		if (s_timeDiagnosticWindowStart == 0)
		{
			s_timeDiagnosticWindowStart = now;
			s_timeDiagnosticWindowLock.clear(std::memory_order_release);
			return;
		}

		const HRTick elapsedTicks = now - s_timeDiagnosticWindowStart;
		if (elapsedTicks < frequency / CAFE_TIME_DIAGNOSTIC_WINDOW_DIVISOR)
		{
			s_timeDiagnosticWindowLock.clear(std::memory_order_release);
			return;
		}

		const CompletedAPIStats systemTime = CompleteAPIStats(s_osGetSystemTimeStats);
		const CompletedAPIStats systemTick = CompleteAPIStats(s_osGetSystemTickStats);
		const CompletedAPIStats mftb = CompleteAPIStats(s_mftbStats);
		const CompletedAPIStats alarmTime = CompleteAPIStats(s_alarmTimeStats);
		const uint64 osTimeConversions = s_osTimeToSystemTimeConversions.exchange(0, std::memory_order_relaxed);
		const uint64 calendarConversions = s_osTicksToCalendarTimeConversions.exchange(0, std::memory_order_relaxed);
		const double elapsedSeconds = static_cast<double>(elapsedTicks) / static_cast<double>(frequency);
		const double inverseElapsedSeconds = 1.0 / elapsedSeconds;

		s_timeDiagnosticWindowSeconds.store(elapsedSeconds, std::memory_order_relaxed);
		s_osGetSystemTimeCallsPerSecond.store(static_cast<double>(systemTime.calls) * inverseElapsedSeconds, std::memory_order_relaxed);
		s_osGetSystemTickCallsPerSecond.store(static_cast<double>(systemTick.calls) * inverseElapsedSeconds, std::memory_order_relaxed);
		s_mftbReadsPerSecond.store(static_cast<double>(mftb.calls) * inverseElapsedSeconds, std::memory_order_relaxed);
		s_alarmTimeReadsPerSecond.store(static_cast<double>(alarmTime.calls) * inverseElapsedSeconds, std::memory_order_relaxed);
		s_osTimeToSystemTimeConversionsPerSecond.store(static_cast<double>(osTimeConversions) * inverseElapsedSeconds, std::memory_order_relaxed);
		s_osTicksToCalendarTimeConversionsPerSecond.store(static_cast<double>(calendarConversions) * inverseElapsedSeconds, std::memory_order_relaxed);
		s_osGetSystemTimeMinDelta.store(systemTime.minGuestDelta, std::memory_order_relaxed);
		s_osGetSystemTimeMaxDelta.store(systemTime.maxGuestDelta, std::memory_order_relaxed);
		s_osGetSystemTickMinDelta.store(systemTick.minGuestDelta, std::memory_order_relaxed);
		s_osGetSystemTickMaxDelta.store(systemTick.maxGuestDelta, std::memory_order_relaxed);
		s_mftbMinDelta.store(mftb.minGuestDelta, std::memory_order_relaxed);
		s_mftbMaxDelta.store(mftb.maxGuestDelta, std::memory_order_relaxed);
		s_alarmTimeMinDelta.store(alarmTime.minGuestDelta, std::memory_order_relaxed);
		s_alarmTimeMaxDelta.store(alarmTime.maxGuestDelta, std::memory_order_relaxed);

		uint64 minimumDelta = CAFE_TIME_DIAGNOSTIC_EMPTY_MIN;
		uint64 maximumDelta = 0;
		auto AccumulateRange = [&minimumDelta, &maximumDelta](const CompletedAPIStats& stats)
		{
			if (stats.deltaSamples == 0)
				return;
			minimumDelta = std::min(minimumDelta, stats.minGuestDelta);
			maximumDelta = std::max(maximumDelta, stats.maxGuestDelta);
		};
		AccumulateRange(systemTime);
		AccumulateRange(systemTick);
		AccumulateRange(mftb);
		AccumulateRange(alarmTime);

		const uint64 totalSamples = systemTime.deltaSamples + systemTick.deltaSamples + mftb.deltaSamples + alarmTime.deltaSamples;
		const uint64 totalGuestDelta = systemTime.guestDeltaSum + systemTick.guestDeltaSum + mftb.guestDeltaSum + alarmTime.guestDeltaSum;
		const uint64 totalHostDelta = systemTime.hostDeltaSum + systemTick.hostDeltaSum + mftb.hostDeltaSum + alarmTime.hostDeltaSum;
		s_minGuestTimeDelta.store(minimumDelta == CAFE_TIME_DIAGNOSTIC_EMPTY_MIN ? 0 : minimumDelta, std::memory_order_relaxed);
		s_maxGuestTimeDelta.store(maximumDelta, std::memory_order_relaxed);
		s_averageGuestTimeDelta.store(totalSamples > 0 ? static_cast<double>(totalGuestDelta) / static_cast<double>(totalSamples) : 0.0, std::memory_order_relaxed);
		if (totalHostDelta > 0)
		{
			const double expectedGuestDelta = static_cast<double>(totalHostDelta) * static_cast<double>(EspressoTime::GetTimerClock()) / static_cast<double>(frequency);
			s_guestToHostDeltaRatio.store(expectedGuestDelta > 0.0 ? static_cast<double>(totalGuestDelta) / expectedGuestDelta : 0.0, std::memory_order_relaxed);
		}
		else
			s_guestToHostDeltaRatio.store(0.0, std::memory_order_relaxed);

		s_timeDiagnosticWindowStart = now;
		s_timeDiagnosticWindowLock.clear(std::memory_order_release);
	}

	CafeTimeConsumerDiagnosticSnapshot CafeTimeConsumerDiagnostics_GetSnapshot()
	{
		CafeTimeConsumerDiagnosticSnapshot snapshot{};
		snapshot.windowSeconds = s_timeDiagnosticWindowSeconds.load(std::memory_order_relaxed);
		snapshot.osGetSystemTimeCallsPerSecond = s_osGetSystemTimeCallsPerSecond.load(std::memory_order_relaxed);
		snapshot.osGetSystemTickCallsPerSecond = s_osGetSystemTickCallsPerSecond.load(std::memory_order_relaxed);
		snapshot.mftbReadsPerSecond = s_mftbReadsPerSecond.load(std::memory_order_relaxed);
		snapshot.alarmTimeReadsPerSecond = s_alarmTimeReadsPerSecond.load(std::memory_order_relaxed);
		snapshot.osTimeToSystemTimeConversionsPerSecond = s_osTimeToSystemTimeConversionsPerSecond.load(std::memory_order_relaxed);
		snapshot.osTicksToCalendarTimeConversionsPerSecond = s_osTicksToCalendarTimeConversionsPerSecond.load(std::memory_order_relaxed);
		snapshot.osGetSystemTimeMinDelta = s_osGetSystemTimeMinDelta.load(std::memory_order_relaxed);
		snapshot.osGetSystemTimeMaxDelta = s_osGetSystemTimeMaxDelta.load(std::memory_order_relaxed);
		snapshot.osGetSystemTickMinDelta = s_osGetSystemTickMinDelta.load(std::memory_order_relaxed);
		snapshot.osGetSystemTickMaxDelta = s_osGetSystemTickMaxDelta.load(std::memory_order_relaxed);
		snapshot.mftbMinDelta = s_mftbMinDelta.load(std::memory_order_relaxed);
		snapshot.mftbMaxDelta = s_mftbMaxDelta.load(std::memory_order_relaxed);
		snapshot.alarmTimeMinDelta = s_alarmTimeMinDelta.load(std::memory_order_relaxed);
		snapshot.alarmTimeMaxDelta = s_alarmTimeMaxDelta.load(std::memory_order_relaxed);
		snapshot.minGuestTimeDelta = s_minGuestTimeDelta.load(std::memory_order_relaxed);
		snapshot.maxGuestTimeDelta = s_maxGuestTimeDelta.load(std::memory_order_relaxed);
		snapshot.averageGuestTimeDelta = s_averageGuestTimeDelta.load(std::memory_order_relaxed);
		snapshot.guestToHostDeltaRatio = s_guestToHostDeltaRatio.load(std::memory_order_relaxed);
		return snapshot;
	}

	void InitializeTimeAndCalendar()
	{
		cafeExportRegister("coreinit", OSGetTime, LogType::Placeholder);
		cafeExportRegister("coreinit", OSGetSystemTime, LogType::Placeholder);
		cafeExportRegister("coreinit", OSGetTick, LogType::Placeholder);
		cafeExportRegister("coreinit", OSGetSystemTick, LogType::Placeholder);

		cafeExportRegister("coreinit", OSTicksToCalendarTime, LogType::Placeholder);
		cafeExportRegister("coreinit", OSCalendarTimeToTicks, LogType::Placeholder);

		//timeTest();
	}
};
