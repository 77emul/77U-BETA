#include "Cafe/HW/Latte/Renderer/Metal/FrameGenerationSchedule.h"
#include <cassert>
#include <cstdio>
#include <deque>
#include <vector>

int main()
{
    for(int rate : {30,60,120})
    {
        FrameGenerationPresentHistory history;
        assert(!history.Add(0) && !history.Add(NAN));
        for(int i=0;i<1000;++i) assert(history.Add(1+double(i)/rate));
        assert(std::abs(history.FPS(1+999.0/rate)-rate)<1e-6);
        const auto intervals=history.IntervalRangeMs();
        assert(std::abs(intervals[0]-1000.0/rate)<1e-6);
        assert(std::abs(intervals[1]-1000.0/rate)<1e-6);
        assert(!history.Add(1));
        assert(history.FPS(3+999.0/rate)==0); // background/hidden output isn't FPS
        history.Reset();assert(history.FPS(2)==0);
        assert(history.IntervalRangeMs()[1]==0);
    }
    FrameGenerationPresentHistory uneven;
    uneven.Add(1);uneven.Add(1.008);uneven.Add(1.033);
    assert(std::abs(uneven.IntervalRangeMs()[0]-8)<1e-6);
    assert(std::abs(uneven.IntervalRangeMs()[1]-25)<1e-6);
    FrameGenerationMode mode;
    assert(mode.ForFrame(0)==0);
    assert(mode.ForFrame(60)==0); // UI cannot redirect half of this frame
    mode.FinishFrame();
    assert(mode.ForFrame(60)==60);
    assert(mode.ForFrame(0)==60);
    mode.Fail();assert(mode.ForFrame(60)==0);
    mode.FinishFrame();assert(mode.ForFrame(60)==0); // no repeated allocation failures
    mode.FinishFrame();assert(mode.ForFrame(0)==0);
    mode.FinishFrame();assert(mode.ForFrame(120)==120); // explicit retry after off/on
    for (int source : {30, 40, 60}) for (int target : {30, 60, 120})
    {
        FrameGenerationSchedule schedule;
        for (int i = 0; i < 20; ++i) schedule.Source(1.0 + double(i)/source);
        assert(schedule.NeedsInterpolation(target) == (target > source));
        assert(std::abs(schedule.Interval() - 1.0/source) < 1e-9);
        unsigned outputs = 0;
        for (unsigned i = 0; i < 1200; ++i)
            outputs += schedule.Tick(2.0 + double(i)/120, target);
        assert(outputs == unsigned(target * 10));
        assert(!schedule.Tick(11.99, target)); // no duplicate/backward deadlines
    }
    // 30 -> 120 needs three genuinely different intermediate positions.
    for (double t : {.25, .5, .75})
        assert(std::abs(FrameGenerationSchedule::Position(2+t/30,2,2+1.0/30)-t) < 1e-9);
    assert(FrameGenerationSchedule::Position(1,2,3) == 0);
    assert(FrameGenerationSchedule::Position(4,2,3) == 1);
    assert(FrameGenerationSchedule::Position(2,2,2) == 1);
    FrameGenerationSchedule paused;
    for (int i=0;i<10;++i) paused.Source(1+i/30.0);
    assert(paused.NeedsInterpolation(60));
    assert(paused.Source(20)); // app pause/teleport must abandon stale history
    assert(!paused.NeedsInterpolation(60));
    assert(!paused.Source(NAN));
    assert(!paused.Tick(NAN,60));
    assert(!paused.Tick(21,45));
    for (int rate : {30,60})
    {
        FrameGenerationSchedule native;
        for(int i=0;i<10;++i) native.Source(1+i/double(rate));
        assert(native.PlaybackTime(5,rate)==5); // no extra native-rate delay
    }
    unsigned oldStarved = 0, correctedStarved = 0, compared = 0;
    // Drive the production selector with real display callbacks one refresh
    // before presentation, source submission, and delayed GPU completion.
    // A constant-rate test with instantly ready images missed this failure.
    for (int source : {30,40,60}) for (int target : {60,120})
    {
        if (target <= source) continue;
        for (int displayHz : {60,120})
        {
        if (displayHz < target) continue;
        for (int phase=0;phase<24;++phase) for (double delay : {.1,.45,.8})
        {
            FrameGenerationSchedule clock;
            std::deque<double> submitted;
            unsigned nextSource = 0;
            double lastPlayback = 0;
            for (int tick=0;tick<displayHz*10;++tick)
            {
                const double deadline=1+double(tick)/displayHz+phase/(displayHz*24.0);
                const double callback=deadline-1.0/displayHz;
                while (1+nextSource/double(source)<=callback)
                {
                    double time=1+nextSource++/double(source);
                    clock.Source(time);submitted.push_back(time);
                    if(submitted.size()>4)submitted.pop_front();
                }
                if(!clock.Tick(deadline,target))continue;
                std::vector<double> ready, oldReady;
                for(size_t i=0;i<submitted.size();++i)
                    if(submitted[i]+delay/source<=callback)
                    {
                        clock.Completed(submitted[i],submitted[i]+delay/source);
                        ready.push_back(submitted[i]);
                        if(i+3>=submitted.size())oldReady.push_back(submitted[i]);
                    }
                if(ready.empty())continue;
                const double playback=clock.PlaybackTime(deadline,target);
                assert(playback>=lastPlayback);lastPlayback=playback;
                if(deadline<3)continue;
                auto corrected=FrameGenerationSchedule::Select(ready,playback,true);
                auto before=FrameGenerationSchedule::Select(oldReady,deadline-clock.Interval()-1.0/target-.002,true);
                oldStarved+=before.starved;
                correctedStarved+=corrected.starved;
                ++compared;
            }
        }
        }
    }
    printf("GPU-delayed source selection: %u/%u unavailable before, %u after\n",oldStarved,compared,correctedStarved);
    assert(oldStarved>0 && correctedStarved==0);
    FrameGenerationSchedule latency;
    for(int i=0;i<60;++i)latency.Source(1+i/30.0);
    latency.Completed(2.9,2.915);
    latency.Completed(2.8,3); // late observation cannot replace the newer sample
    assert(std::abs(latency.ReadyDelay()-.015)<1e-9);
    auto beforeStall=latency.PlaybackTime(3.1,60);
    latency.Completed(3,3.14);
    assert(latency.PlaybackTime(3.11,60)>=beforeStall); // no backwards camera movement
    assert(latency.PlaybackTime(4,60)>3.90); // bounded history cannot retain a 140 ms stall
    assert(FrameGenerationSchedule::Select({},3,true).position==1);
    puts("PASS: 30/40/60 sources -> 30/60/120 targets; display-rate divisors; arbitrary intermediate positions; pause/reset; invalid time rejection; native-rate bypass");
}
