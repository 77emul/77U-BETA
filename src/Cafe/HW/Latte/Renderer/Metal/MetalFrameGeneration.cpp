#include "MetalFrameGeneration.h"
#include "FrameGenerationSchedule.h"
#include "FrameGenerationShaderSource.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <cstdio>
#include <cstdlib>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE || TARGET_OS_IOS
#include <pthread/qos.h>
#include <os/proc.h>
#endif

extern std::atomic<uint64_t> g_metalPresentCount;

namespace
{
template<class T> using MetalPtr = std::shared_ptr<T>;
template<class T> MetalPtr<T> Own(T* value)
{
    return MetalPtr<T>(value, [](T* p) { if (p) p->release(); });
}
template<class T> MetalPtr<T> Retain(T* value) { return Own(value ? value->retain() : nullptr); }

double Now()
{
    static const double scale = [] {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        return double(info.numer) / double(info.denom) / 1.0e9;
    }();
    return mach_absolute_time() * scale;
}

struct FramePool;
struct Image
{
    std::shared_ptr<FramePool> pool;
    MetalPtr<MTL::CommandBuffer> ready;
    unsigned slot = 0;
    uint64_t id = 0;
    double time = 0;
    ~Image();
    MTL::Texture* Texture() const;
};

struct FramePool : std::enable_shared_from_this<FramePool>
{
    // Four history images, one producer and bounded in-flight readers.
    // If readers lag, only FG applies backpressure; a live slot is never reused.
    std::array<MetalPtr<MTL::Texture>, 6> textures;
    std::array<bool, 6> busy{};
    std::mutex mutex;
    std::condition_variable available;
    unsigned width = 0, height = 0;
    MTL::PixelFormat format = MTL::PixelFormatInvalid;

    std::shared_ptr<Image> Acquire()
    {
        auto image = std::make_shared<Image>();
        std::unique_lock lock(mutex);
        available.wait(lock, [&] { return std::find(busy.begin(), busy.end(), false) != busy.end(); });
        image->slot = unsigned(std::find(busy.begin(), busy.end(), false) - busy.begin());
        image->pool = shared_from_this();
        busy[image->slot] = true;
        return image;
    }
};

Image::~Image()
{
    if (pool)
    {
        { std::lock_guard lock(pool->mutex); pool->busy[slot] = false; }
        pool->available.notify_one();
    }
}
MTL::Texture* Image::Texture() const { return pool->textures[slot].get(); }

struct PresentationState
{
    std::mutex mutex;
    std::condition_variable changed;
    bool stop = false, enabled = false;
    uint64_t tick = 0, generation = 0;
    double deadline = 0, lastOutputTime = 0;
    int target = 60;
    std::atomic<bool> fast{false}; // cheaper flow search (option); read by the presentation thread
    FrameGenerationSchedule schedule;
    MetalPtr<CA::MetalLayer> layer;
    std::deque<std::shared_ptr<Image>> frames;
    std::atomic<unsigned> inFlight{0};
    std::atomic<bool> failed{false};
    std::mutex statsMutex;
    FrameGenerationPresentHistory displayed;
    uint64_t generated = 0, repeated = 0, original = 0, dropped = 0;
    uint64_t starved = 0;
    uint64_t displayTicks = 0, acceptedTicks = 0;
    double displayPeriod = 0;
    uint64_t submitted = 0, skippedByDisplay = 0, noDrawable = 0;
    uint64_t lastSource = 0;
    double gpuMs = 0;
    std::array<uint32_t, 8> audit{};
    double auditPosition = 0, auditTime = 0;
};

std::atomic<bool> s_displayActive{false};
std::atomic<bool> s_outputSuspended{false};
std::mutex s_registryMutex;
std::weak_ptr<PresentationState> s_displayState;

void Register(const std::shared_ptr<PresentationState>& state)
{
    std::lock_guard lock(s_registryMutex);
    s_displayState = state;
    s_displayActive.store(true, std::memory_order_release);
}
void Unregister(const std::shared_ptr<PresentationState>& state)
{
    std::lock_guard lock(s_registryMutex);
    if (s_displayState.lock() == state)
    {
        s_displayActive.store(false, std::memory_order_release);
        s_displayState.reset();
    }
}

MetalPtr<MTL::Texture> Texture(MTL::Device* device, unsigned w, unsigned h, MTL::PixelFormat format, MTL::TextureUsage usage)
{
    auto desc = Own(MTL::TextureDescriptor::alloc()->init());
    desc->setTextureType(MTL::TextureType2D);
    desc->setWidth(w); desc->setHeight(h);
    desc->setPixelFormat(format);
    desc->setStorageMode(MTL::StorageModePrivate);
    desc->setUsage(usage);
    return Own(device->newTexture(desc.get()));
}

struct FlowImages
{
    unsigned width = 0, height = 0, gridWidth = 0, gridHeight = 0;
    bool fast = false; // dense field at 8 instead of 4 matching pixels
    std::array<MetalPtr<MTL::Texture>, 2> luma, raw, rawInitial, field, dominant, models, modelSeeds, proposals, initial, resolved;
    std::array<MetalPtr<MTL::Buffer>, 2> projected;
    uint64_t previous = 0, current = 0;
};

struct AuditSample
{
    MetalPtr<MTL::Buffer> buffer;
    std::atomic<bool> busy{false};
};
}

void CemuMetal_SetFrameGenerationSuspended(bool suspended)
{
    s_outputSuspended.store(suspended, std::memory_order_release);
}

extern "C" void CemuMetal_FrameGenerationDisplayTick(void* layer, double targetSeconds, double displayPeriod)
{
    if (!s_displayActive.load(std::memory_order_acquire) || !std::isfinite(targetSeconds))
        return;
    std::shared_ptr<PresentationState> state;
    { std::lock_guard lock(s_registryMutex); state = s_displayState.lock(); }
    if (!state)
        return;
    {
        std::lock_guard lock(state->mutex);
        if (!state->enabled || state->layer.get() != layer)
            return;
        state->schedule.SetDisplayPeriod(displayPeriod);
        ++state->displayTicks; state->displayPeriod = displayPeriod;
        state->deadline = targetSeconds;
        ++state->tick;
    }
    state->changed.notify_one();
}

extern "C" void CemuMetal_GetFrameGenerationRates(double* source, double* displayed)
{
    *source = *displayed = 0;
    if (!s_displayActive.load(std::memory_order_acquire)) return;
    std::shared_ptr<PresentationState> state;
    { std::lock_guard lock(s_registryMutex); state = s_displayState.lock(); }
    if (!state) return;
    std::lock_guard stateLock(state->mutex);
    if (!state->enabled) return;
    *source = 1.0 / state->schedule.Interval();
    std::lock_guard statsLock(state->statsMutex);
    *displayed = state->displayed.FPS(Now());
}

struct MetalFrameGeneration::Impl
{
    MetalPtr<MTL::Device> device;
    MetalPtr<MTL::CommandQueue> queue;
    MetalPtr<MTL::ComputePipelineState> lumaPipeline, flowPipeline, flowFastPipeline, cameraSearchPipeline, cameraSearchFastPipeline, modelPipeline, modelSeedPipeline, consensusPipeline, regularizePipeline, regularizeFastPipeline, widePipeline, propagatePipeline, projectPipeline, upsamplePipeline, auditPipeline, decidePipeline, staticSeedPipeline;
    // Per-game-pixel warp decisions for the synthesized frame (drawable-derived size).
    MetalPtr<MTL::Texture> decisionOffsets, decisionModes;
    MetalPtr<MTL::Buffer> overlayMask; // one byte per decision pixel
    unsigned overlayWidth = 0, overlayHeight = 0;
    std::array<MetalPtr<MTL::RenderPipelineState>, 2> decidedPipeline; // fragment for scale > 1
    std::array<MetalPtr<MTL::RenderPipelineState>, 2> copyPipeline, interpolatePipeline;
    std::shared_ptr<PresentationState> state = std::make_shared<PresentationState>();
    std::shared_ptr<FramePool> pool;
    std::shared_ptr<Image> writing;
    std::shared_ptr<FlowImages> flowImages;
    std::shared_ptr<AuditSample> audit;
    double lastAudit = 0;
    std::thread thread;
    uint64_t nextImage = 0;
    std::string error;

    explicit Impl(MTL::Device* d) : device(Retain(d)) {}
    ~Impl()
    {
        Unregister(state);
        {
            std::lock_guard lock(state->mutex);
            state->stop = true;
            state->enabled = false;
            state->frames.clear();
        }
        state->changed.notify_all();
        if (thread.joinable()) thread.join();
        // GPU callbacks capture image/flow/state owners, never this Impl.
    }

    bool Initialize()
    {
        if (queue) return true;
        NS::Error* failure = nullptr;
        auto library = Own(device->newLibrary(NS::String::string(frameGenerationShaderSource, NS::UTF8StringEncoding), nullptr, &failure));
        if (!library) { error = failure ? failure->localizedDescription()->utf8String() : "Frame-generation shader compilation failed"; return false; }
        auto function = [&](const char* name) { return Own(library->newFunction(NS::String::string(name, NS::UTF8StringEncoding))); };
        auto luma = function("fgLuma"), flow = function("fgFlow"), consensus = function("fgConsensus"), regularize = function("fgRegularize");
        lumaPipeline = Own(device->newComputePipelineState(luma.get(), &failure));
        flowPipeline = Own(device->newComputePipelineState(flow.get(), &failure));
        auto flowFast = function("fgFlowFast");
        flowFastPipeline = Own(device->newComputePipelineState(flowFast.get(), &failure));
        auto cameraSearch = function("fgCameraSearch");
        cameraSearchPipeline = Own(device->newComputePipelineState(cameraSearch.get(), &failure));
        auto cameraSearchFast = function("fgCameraSearchFast");
        cameraSearchFastPipeline = Own(device->newComputePipelineState(cameraSearchFast.get(), &failure));
        auto modelSeed = function("fgModelSeeds");
        modelSeedPipeline = Own(device->newComputePipelineState(modelSeed.get(), &failure));
        auto model = function("fgModelCandidates");
        modelPipeline = Own(device->newComputePipelineState(model.get(), &failure));
        consensusPipeline = Own(device->newComputePipelineState(consensus.get(), &failure));
        regularizePipeline = Own(device->newComputePipelineState(regularize.get(), &failure));
        auto regularizeFast = function("fgRegularizeFast");
        regularizeFastPipeline = Own(device->newComputePipelineState(regularizeFast.get(), &failure));
        auto wide = function("fgWideLocal");
        widePipeline = Own(device->newComputePipelineState(wide.get(), &failure));
        auto propagate = function("fgPropagate");
        propagatePipeline = Own(device->newComputePipelineState(propagate.get(), &failure));
        auto upsample = function("fgUpsample");
        upsamplePipeline = Own(device->newComputePipelineState(upsample.get(), &failure));
        auto project = function("fgProject");
        projectPipeline = Own(device->newComputePipelineState(project.get(), &failure));
        auto staticSeed = function("fgStaticSeed");
        staticSeedPipeline = Own(device->newComputePipelineState(staticSeed.get(), &failure));
        auto decide = function("fgDecide");
        decidePipeline = Own(device->newComputePipelineState(decide.get(), &failure));
        auto vertex = function("fgVertex"), copy = function("fgCopy"), interpolate = function("fgInterpolate"), decided = function("fgInterpolateDecided");
        auto desc = Own(MTL::RenderPipelineDescriptor::alloc()->init());
        desc->setVertexFunction(vertex.get());
        for (unsigned i = 0; i < 2; ++i)
        {
            desc->colorAttachments()->object(0)->setPixelFormat(i ? MTL::PixelFormatBGRA8Unorm_sRGB : MTL::PixelFormatBGRA8Unorm);
            desc->setFragmentFunction(copy.get());
            copyPipeline[i] = Own(device->newRenderPipelineState(desc.get(), &failure));
            desc->setFragmentFunction(interpolate.get());
            interpolatePipeline[i] = Own(device->newRenderPipelineState(desc.get(), &failure));
            desc->setFragmentFunction(decided.get());
            decidedPipeline[i] = Own(device->newRenderPipelineState(desc.get(), &failure));
        }
        if (!lumaPipeline || !flowPipeline || !flowFastPipeline || !cameraSearchPipeline || !cameraSearchFastPipeline || !modelPipeline || !modelSeedPipeline || !consensusPipeline || !regularizePipeline || !regularizeFastPipeline || !widePipeline || !propagatePipeline || !projectPipeline || !upsamplePipeline || !decidePipeline || !staticSeedPipeline || !copyPipeline[0] || !copyPipeline[1] || !interpolatePipeline[0] || !interpolatePipeline[1] || !decidedPipeline[0] || !decidedPipeline[1])
        { error = failure ? failure->localizedDescription()->utf8String() : "Frame-generation pipeline creation failed"; return false; }
        // Diagnostics are optional and exist only while FG is enabled. No
        // full image readback or CPU/GPU wait is needed for these 576 samples.
        auto auditFunction = function("fgAudit");
        if (auditFunction)
            auditPipeline = Own(device->newComputePipelineState(auditFunction.get(), &failure));
        if (auditPipeline)
        {
            audit = std::make_shared<AuditSample>();
            audit->buffer = Own(device->newBuffer(sizeof(uint32_t) * 8, MTL::ResourceStorageModeShared));
            if (!audit->buffer) audit.reset();
        }
        queue = Own(device->newCommandQueue());
        if (!queue) { error = "Frame-generation queue allocation failed"; return false; }
        thread = std::thread([this] {
            try { Run(); }
            catch (...) { state->failed.store(true, std::memory_order_release); Unregister(state); }
        });
        return true;
    }

    bool EnsureFlow(unsigned width, unsigned height, bool fast)
    {
        const double scale = std::min(1.0, 320.0 / std::max(width, height));
        unsigned w = std::max(1u, unsigned(std::ceil(width * scale)));
        unsigned h = std::max(1u, unsigned(std::ceil(height * scale)));
        if (flowImages && flowImages->width == w && flowImages->height == h && flowImages->fast == fast) return true;
        auto images = std::make_shared<FlowImages>();
        images->width = w; images->height = h; images->fast = fast;
        const unsigned cell = fast ? 8 : 4; // dense field cell; wide proposals stay at 16
        images->gridWidth = (w + 7) / 8; images->gridHeight = (h + 7) / 8;
        for (unsigned i = 0; i < 2; ++i)
        {
            const auto usage = MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite;
            images->luma[i] = Texture(device.get(), w, h, MTL::PixelFormatR16Float, usage);
            images->rawInitial[i] = Texture(device.get(), images->gridWidth, images->gridHeight, MTL::PixelFormatRGBA32Float, usage);
            images->raw[i] = Texture(device.get(), images->gridWidth, images->gridHeight, MTL::PixelFormatRGBA16Float, usage);
            images->field[i] = Texture(device.get(), (w + cell - 1) / cell, (h + cell - 1) / cell, MTL::PixelFormatRGBA16Float, usage);
            images->dominant[i] = Texture(device.get(), 3, 1, MTL::PixelFormatRGBA16Float, usage);
            images->initial[i] = Texture(device.get(), (w + cell - 1) / cell, (h + cell - 1) / cell, MTL::PixelFormatRGBA16Float, usage);
            images->proposals[i] = Texture(device.get(), (w + 15) / 16, (h + 15) / 16, MTL::PixelFormatRGBA16Float, usage);
            images->modelSeeds[i] = Texture(device.get(), 3, 384, MTL::PixelFormatRGBA32Float, usage);
            images->models[i] = Texture(device.get(), 3, 384, MTL::PixelFormatRGBA16Float, usage);
            images->resolved[i] = Texture(device.get(), w, h, MTL::PixelFormatRGBA16Float, usage);
            images->projected[i] = Own(device->newBuffer(w * h * sizeof(uint32_t), MTL::ResourceStorageModePrivate));
            if (!images->rawInitial[i] || !images->modelSeeds[i] || !images->resolved[i] || !images->projected[i] || !images->luma[i] || !images->raw[i] || !images->field[i] || !images->dominant[i] || !images->models[i] || !images->proposals[i] || !images->initial[i]) return false;
        }
        flowImages = images;
        return true;
    }

    static constexpr unsigned kSearchThreads = 256; // FG_SEARCH_THREADS (camera search) in the shader
    void Compute(MTL::ComputeCommandEncoder* encoder, MTL::ComputePipelineState* pipeline,
                 MTL::Texture* a, MTL::Texture* b, MTL::Texture* out, unsigned width, unsigned height,
                 MTL::Texture* dominant = nullptr, MTL::Texture* otherLuma = nullptr,
                 MTL::Texture* sourceA = nullptr, MTL::Texture* sourceB = nullptr, bool grouped = false, unsigned groupThreads = 64)
    {
        encoder->setComputePipelineState(pipeline);
        encoder->setTexture(a, 0); encoder->setTexture(b, 1);
        encoder->setTexture(out, 2);
        encoder->setTexture(dominant, 3);
        encoder->setTexture(otherLuma, 4);
        encoder->setTexture(sourceA, 5);
        encoder->setTexture(sourceB, 6);
        if (grouped) encoder->dispatchThreadgroups(MTL::Size(width, height, 1), MTL::Size(groupThreads, 1, 1));
        else encoder->dispatchThreads(MTL::Size(width, height, 1), height == 1 ? MTL::Size(64, 1, 1) : MTL::Size(8, 8, 1));
    }

    void Present(const std::shared_ptr<Image>& previous, const std::shared_ptr<Image>& current,
                 const MetalPtr<CA::MetalLayer>& layer, double position, double deadline, uint64_t generation)
    {
        { std::lock_guard lock(state->mutex); if (!state->enabled || state->generation != generation) return; }
        if (s_outputSuspended.load(std::memory_order_acquire)) return;
        if (state->inFlight.load(std::memory_order_relaxed) >= 2)
        { std::lock_guard lock(state->statsMutex); ++state->dropped; return; }
        auto autorelease = Own(NS::AutoreleasePool::alloc()->init());
        auto drawable = Retain(layer->nextDrawable());
        if (!drawable) { std::lock_guard lock(state->statsMutex); ++state->noDrawable; return; }
        // Waiting for a drawable can spend the whole display interval. A ready
        // original can still be shown at the next refresh.
        if (deadline < Now()) position = 1.0;
        { std::lock_guard lock(state->mutex); if (!state->enabled || state->generation != generation) return; }
        if (s_outputSuspended.load(std::memory_order_acquire)) return;
        const auto format = drawable->texture()->pixelFormat();
        if (format != MTL::PixelFormatBGRA8Unorm && format != MTL::PixelFormatBGRA8Unorm_sRGB)
            return;
        auto cb = queue->commandBuffer();
        if (!cb) return;
        unsigned decideScale = 1; // output pixels per warp decision side
        bool generated = previous != current && position > 0.001 && position < 0.999 &&
                         EnsureFlow(unsigned(current->Texture()->width()), unsigned(current->Texture()->height()), state->fast.load(std::memory_order_relaxed));
        auto work = flowImages;
        if (generated && (work->previous != previous->id || work->current != current->id))
        {
            MTL::ComputeCommandEncoder* flowEncoder = nullptr;
            bool holdBarrier = false;
            auto stage = [&](MTL::ComputePipelineState* pipeline, auto... args) {
                // Concurrent pass: explicit barriers order producers/consumers;
                // the independent forward/backward dispatches of a pair overlap.
                if (!flowEncoder) flowEncoder = cb->computeCommandEncoder(MTL::DispatchTypeConcurrent);
                Compute(flowEncoder, pipeline, args...);
                if (!holdBarrier) flowEncoder->memoryBarrier(MTL::BarrierScopeTextures);
            };
            // Runs one stage for both directions; they share no texture.
            auto both = [&](auto&& dispatch) { holdBarrier = true; dispatch(0u); holdBarrier = false; dispatch(1u); };
            both([&](unsigned i) { stage(lumaPipeline.get(), (i ? current : previous)->Texture(), work->luma[i].get(), nullptr, work->width, work->height); });
            both([&](unsigned i) {
                stage(work->fast ? flowFastPipeline.get() : flowPipeline.get(), work->luma[i].get(), work->luma[1-i].get(), work->rawInitial[i].get(), (work->gridWidth + 1) / 2, work->gridHeight,
                        nullptr, nullptr, (i ? current : previous)->Texture(), (i ? previous : current)->Texture(), true); }); // two cells per group
            both([&](unsigned i) {
                stage(work->fast ? cameraSearchFastPipeline.get() : cameraSearchPipeline.get(), work->rawInitial[i].get(), work->luma[i].get(), work->raw[i].get(), (work->gridWidth + 3) / 4, (work->gridHeight + 3) / 4,
                        nullptr, nullptr, (i ? current : previous)->Texture(), (i ? previous : current)->Texture(), true, kSearchThreads); }); // one group per 4x4 block
            both([&](unsigned i) { stage(modelSeedPipeline.get(), work->raw[i].get(), work->modelSeeds[i].get(), nullptr, 384, 1); });
            both([&](unsigned i) {
                stage(modelPipeline.get(), work->modelSeeds[i].get(), work->models[i].get(), work->raw[i].get(), 384, 1,
                        work->luma[i].get(), nullptr, (i ? current : previous)->Texture(), (i ? previous : current)->Texture(), true); });
            stage(consensusPipeline.get(), work->models[0].get(), work->models[1].get(), work->dominant[0].get(), 1, 1,
                    work->dominant[1].get(), work->luma[0].get());
            auto* regularizeStage = work->fast ? regularizeFastPipeline.get() : regularizePipeline.get();
            both([&](unsigned i) { stage(regularizeStage, work->raw[i].get(), work->luma[i].get(), work->initial[i].get(), unsigned(work->field[i]->width() + 1) / 2, unsigned(work->field[i]->height()),
                        work->dominant[i].get(), work->luma[1-i].get(), (i ? current : previous)->Texture(), (i ? previous : current)->Texture(), true); }); // two cells per group
            both([&](unsigned i) { stage(widePipeline.get(), work->initial[i].get(), work->luma[i].get(), work->proposals[i].get(), unsigned(work->proposals[i]->width()), unsigned(work->proposals[i]->height()),
                        work->dominant[i].get(), nullptr, (i ? current : previous)->Texture(), (i ? previous : current)->Texture(), true); });
            both([&](unsigned i) { stage(propagatePipeline.get(), work->proposals[i].get(), work->luma[i].get(), work->field[i].get(), unsigned(work->field[i]->width() + 1) / 2, unsigned(work->field[i]->height()),
                        work->dominant[i].get(), work->initial[i].get(), (i ? current : previous)->Texture(), (i ? previous : current)->Texture(), true); }); // two cells per group
            both([&](unsigned i) { stage(upsamplePipeline.get(), work->field[i].get(), work->luma[i].get(), work->resolved[i].get(), work->width, work->height,
                        nullptr, nullptr, (i ? current : previous)->Texture(), (i ? previous : current)->Texture()); });
            if (flowEncoder) flowEncoder->endEncoding();
            work->previous = previous->id; work->current = current->id;
        }
        if (generated)
        {
            // Reproject for this display position, even when the flow pair is
            // cached. Queue order protects reuse; work is leased by completion.
            auto clear = cb->blitCommandEncoder();
            for (auto& map : work->projected)
                clear->fillBuffer(map.get(), NS::Range(0, work->width * work->height * sizeof(uint32_t)), 0);
            clear->endEncoding();
            for (unsigned i = 0; i < 2; ++i)
            {
                auto projection = cb->computeCommandEncoder();
                projection->setComputePipelineState(projectPipeline.get());
                projection->setTexture((i ? current : previous)->Texture(), 0);
                projection->setTexture((i ? previous : current)->Texture(), 1);
                projection->setTexture(work->resolved[i].get(), 2);
                projection->setTexture(work->resolved[1-i].get(), 3);
                projection->setTexture(work->dominant[i].get(), 4);
                struct { float inverseSize[2]; float t, unused; } params{{1.f/work->width,1.f/work->height},float(i ? 1.0-position : position),0};
                projection->setBytes(&params, sizeof(params), 0);
                projection->setBuffer(work->projected[i].get(), 0, 1);
                projection->dispatchThreads(MTL::Size(work->width, work->height, 1), MTL::Size(8, 8, 1));
                projection->endEncoding();
            }
            // One decision per game pixel: Wii U output is at most 1280x720
            // here, so a 2x Retina drawable needs 1/4 of the warp searches.
            // shortcut: assumes <=1280x720 game output, pass the source size for resolution packs.
            const unsigned outW = unsigned(drawable->texture()->width()), outH = unsigned(drawable->texture()->height());
            decideScale = std::max(1u, std::min(outW / 1280, outH / 720));
            const unsigned scale = decideScale;
            const unsigned decideW = (outW + scale - 1) / scale, decideH = (outH + scale - 1) / scale;
            const auto usage = MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite;
            if (!overlayMask || overlayWidth != decideW || overlayHeight != decideH)
            {
                overlayMask = Own(device->newBuffer(NS::UInteger(decideW) * decideH, MTL::ResourceStorageModePrivate));
                overlayWidth = decideW; overlayHeight = decideH;
            }
            // 1x drawable: the per-pixel fragment is cheaper than an extra decision pass.
            if (scale > 1 && (!decisionOffsets || decisionOffsets->width() != decideW || decisionOffsets->height() != decideH))
            {
                decisionOffsets = Texture(device.get(), decideW, decideH, MTL::PixelFormatRGBA32Float, usage);
                decisionModes = Texture(device.get(), decideW, decideH, MTL::PixelFormatR8Uint, usage);
            }
            if (!overlayMask || (scale > 1 && (!decisionOffsets || !decisionModes))) generated = false;
            else
            {
                auto clearMask = cb->blitCommandEncoder();
                clearMask->fillBuffer(overlayMask.get(), NS::Range(0, overlayMask->length()), 0);
                clearMask->endEncoding();
                const struct { uint32_t width, height; } maskSize{decideW, decideH};
                auto decision = cb->computeCommandEncoder();
                decision->setComputePipelineState(staticSeedPipeline.get());
                decision->setTexture(previous->Texture(), 0); decision->setTexture(current->Texture(), 1);
                decision->setBuffer(overlayMask.get(), 0, 0);
                decision->setBytes(&maskSize, sizeof(maskSize), 1);
                decision->dispatchThreads(MTL::Size(decideW, decideH, 1), MTL::Size(8, 8, 1));
                if (scale > 1)
                {
                    decision->memoryBarrier(MTL::BarrierScopeBuffers);
                    decision->setComputePipelineState(decidePipeline.get());
                    decision->setTexture(previous->Texture(), 0); decision->setTexture(current->Texture(), 1);
                    decision->setTexture(work->resolved[0].get(), 2); decision->setTexture(work->resolved[1].get(), 3);
                    decision->setTexture(decisionOffsets.get(), 4); decision->setTexture(decisionModes.get(), 5);
                    decision->setBuffer(overlayMask.get(), 0, 3);
                    struct { float inverseSize[2]; float t, unused; } params{{1.f/work->width,1.f/work->height},float(position),0};
                    decision->setBytes(&params, sizeof(params), 0);
                    decision->setBuffer(work->projected[0].get(), 0, 1);
                    decision->setBuffer(work->projected[1].get(), 0, 2);
                    decision->dispatchThreads(MTL::Size(decideW, decideH, 1), MTL::Size(8, 8, 1));
                }
                decision->endEncoding();
            }
        }
        auto desc = Own(MTL::RenderPassDescriptor::alloc()->init());
        auto color = desc->colorAttachments()->object(0);
        color->setTexture(drawable->texture());
        color->setLoadAction(MTL::LoadActionDontCare); color->setStoreAction(MTL::StoreActionStore);
        auto encoder = cb->renderCommandEncoder(desc.get());
        const unsigned index = format == MTL::PixelFormatBGRA8Unorm_sRGB;
        const bool decided = generated && decideScale > 1;
        encoder->setRenderPipelineState(!generated ? copyPipeline[index].get() : decided ? decidedPipeline[index].get() : interpolatePipeline[index].get());
        if (generated)
        {
            encoder->setFragmentTexture(previous->Texture(), 0);
            encoder->setFragmentTexture(current->Texture(), 1);
            encoder->setFragmentTexture(decided ? decisionOffsets.get() : work->resolved[0].get(), 2);
            encoder->setFragmentTexture(decided ? decisionModes.get() : work->resolved[1].get(), 3);
            const struct { uint32_t width, height; } maskSize{overlayWidth, overlayHeight};
            encoder->setFragmentBuffer(overlayMask.get(), 0, 3);
            encoder->setFragmentBytes(&maskSize, sizeof(maskSize), 4);
            struct { float inverseSize[2]; float t; float unused; } params{{1.f/work->width,1.f/work->height},float(position),0};
            encoder->setFragmentBytes(&params, sizeof(params), 0);
            encoder->setFragmentBuffer(work->projected[0].get(), 0, 1);
            encoder->setFragmentBuffer(work->projected[1].get(), 0, 2);
        }
        else encoder->setFragmentTexture((position < 0.5 ? previous : current)->Texture(), 0);
        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        encoder->endEncoding();
        std::shared_ptr<AuditSample> probe;
        if (generated && audit && position > .05 && position < .95 && Now() - lastAudit >= 1.0 &&
            !audit->busy.exchange(true, std::memory_order_acq_rel))
        {
            probe = audit;
            lastAudit = Now();
            std::fill_n(static_cast<uint32_t*>(probe->buffer->contents()), 8, 0u);
            auto sample = cb->computeCommandEncoder();
            sample->setComputePipelineState(auditPipeline.get());
            sample->setTexture(previous->Texture(), 0); sample->setTexture(current->Texture(), 1);
            sample->setTexture(work->resolved[0].get(), 2); sample->setTexture(work->resolved[1].get(), 3);
            struct { float inverseSize[2]; float t, unused; } params{{1.f/work->width,1.f/work->height},float(position),0};
            sample->setBytes(&params, sizeof(params), 0);
            sample->setBuffer(probe->buffer.get(), 0, 1);
            sample->setBuffer(work->projected[0].get(), 0, 2);
            sample->setBuffer(work->projected[1].get(), 0, 3);
            sample->dispatchThreads(MTL::Size(32, 18, 1), MTL::Size(8, 8, 1));
            sample->endEncoding();
        }
        auto owner = state;
        owner->inFlight.fetch_add(1, std::memory_order_relaxed);
        // Keep leases until the actual GPU read has finished, including errors.
        cb->addCompletedHandler(MTL::HandlerFunction([owner, previous, current, work, probe, generation, position](MTL::CommandBuffer* completed) {
            const double elapsed = (completed->GPUEndTime() - completed->GPUStartTime()) * 1000.0;
            if (elapsed >= 0 && elapsed < 1000)
            { std::lock_guard lock(owner->statsMutex); owner->gpuMs = elapsed; }
            if (probe)
            {
                if (completed->status() == MTL::CommandBufferStatusCompleted)
                {
                    std::lock_guard stateLock(owner->mutex);
                    if (owner->enabled && owner->generation == generation)
                    {
                        std::lock_guard statsLock(owner->statsMutex);
                        std::copy_n(static_cast<const uint32_t*>(probe->buffer->contents()), 8, owner->audit.begin());
                        owner->auditPosition = position;
                        owner->auditTime = Now();
                    }
                }
                probe->busy.store(false, std::memory_order_release);
            }
            owner->inFlight.fetch_sub(1, std::memory_order_relaxed);
        }));
        const uint64_t sourceId = current->id;
        drawable->addPresentedHandler(MTL::DrawablePresentedHandlerFunction([owner, generated, sourceId, generation](MTL::Drawable* shown) {
            const double time = shown->presentedTime();
            if (time <= 0) { std::lock_guard lock(owner->statsMutex); ++owner->skippedByDisplay; return; } // never on screen: not displayed FPS
            std::lock_guard stateLock(owner->mutex);
            if (!owner->enabled || owner->generation != generation) return;
            std::lock_guard lock(owner->statsMutex);
            if (!owner->displayed.Add(time)) return;
            owner->lastOutputTime = std::max(owner->lastOutputTime, time);
            if (generated) ++owner->generated;
            else if (owner->lastSource == sourceId) ++owner->repeated;
            else ++owner->original;
            owner->lastSource = sourceId;
            g_metalPresentCount.fetch_add(1, std::memory_order_relaxed);
        }));
        { std::lock_guard lock(state->statsMutex); ++state->submitted; }
        cb->presentDrawableAtTime(drawable.get(), std::max(deadline, Now()));
        cb->commit();
    }

    void Run()
    {
#if TARGET_OS_IPHONE || TARGET_OS_IOS
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
        uint64_t lastTick = 0;
        while (true)
        {
            std::vector<std::shared_ptr<Image>> ready;
            std::vector<double> readyTimes;
            MetalPtr<CA::MetalLayer> layer;
            double deadline = 0, playback = 0;
            uint64_t generation = 0;
            bool interpolate = false;
            unsigned phaseSteps = 0;
            {
                std::unique_lock lock(state->mutex);
#if TARGET_OS_IPHONE || TARGET_OS_IOS
                state->changed.wait(lock, [&] { return state->stop || state->tick != lastTick; });
#else
                if (__builtin_available(macOS 14.0, *))
                    state->changed.wait(lock, [&] { return state->stop || state->tick != lastTick; });
                else {
                state->changed.wait_for(lock, std::chrono::duration<double>(1.0 / state->target), [&] { return state->stop || state->tick != lastTick; });
                if (state->enabled && state->tick == lastTick) { state->deadline = Now() + 1.0/state->target; ++state->tick; }
                }
#endif
                if (state->stop) return;
                lastTick = state->tick;
                if (!state->enabled || !state->layer || !state->schedule.Tick(state->deadline, state->target)) continue;
                ++state->acceptedTicks;
                deadline = state->deadline;
                generation = state->generation;
                interpolate = state->schedule.NeedsInterpolation(state->target);
                phaseSteps = state->schedule.PhaseSteps(state->target);
                layer = state->layer;
                for (auto& image : state->frames)
                    if (image->ready && image->ready->status() == MTL::CommandBufferStatusCompleted)
                    {
                        state->schedule.Completed(image->time, image->ready->GPUEndTime());
                        ready.push_back(image);
                        readyTimes.push_back(image->time);
                    }
                playback = state->schedule.PlaybackTime(deadline, state->target);
            }
            if (ready.empty()) continue;
            auto pair = FrameGenerationSchedule::Select(readyTimes, playback, interpolate);
            // Exact phases: at 60 -> 120 the original frames are shown as-is and
            // only the midpoint is generated (half the interpolation GPU work).
            const double locked = FrameGenerationSchedule::LockPhase(pair.position, phaseSteps);
            if (phaseSteps && pair.previous != pair.current)
            {
                std::lock_guard lock(state->mutex);
                state->schedule.NudgePhase((locked - pair.position) * (readyTimes[pair.current] - readyTimes[pair.previous]));
            }
            pair.position = locked;
            if (pair.starved) { std::lock_guard lock(state->statsMutex); ++state->starved; }
            Present(ready[pair.previous], ready[pair.current], layer, pair.position, deadline, generation);
        }
    }
};

MetalFrameGeneration::MetalFrameGeneration(MTL::Device* device) : m_impl(std::make_unique<Impl>(device)) {}
MetalFrameGeneration::~MetalFrameGeneration() = default;

MTL::Texture* MetalFrameGeneration::Begin(CA::MetalLayer* layer, unsigned width, unsigned height, MTL::PixelFormat format)
{
    auto& impl = *m_impl;
    if (!layer || !width || !height || !impl.Initialize()) return nullptr;
    if (!impl.pool || impl.pool->width != width || impl.pool->height != height || impl.pool->format != format)
    {
        Reset();
#if TARGET_OS_IPHONE || TARGET_OS_IOS
        const uint64_t needed = uint64_t(width) * height * 4 * 6;
        if (width > 16384 || height > 16384 || os_proc_available_memory() < needed + 64 * 1024 * 1024)
        { impl.error = "Insufficient headroom for safe frame-generation image storage"; return nullptr; }
#endif
        auto pool = std::make_shared<FramePool>();
        pool->width = width; pool->height = height; pool->format = format;
        for (auto& texture : pool->textures)
        {
            texture = Texture(impl.device.get(), width, height, format, MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);
            if (!texture) { impl.error = "Frame-generation image allocation failed"; return nullptr; }
        }
        impl.pool = pool;
    }
    {
        std::lock_guard lock(impl.state->mutex);
        if (impl.state->layer.get() != layer)
        {
            ++impl.state->generation;
            impl.state->frames.clear(); impl.state->schedule.Reset();
            impl.state->layer = Retain(layer);
            impl.state->lastOutputTime = 0;
        }
    }
    if (!impl.writing) impl.writing = impl.pool->Acquire();
    return impl.writing->Texture();
}

void MetalFrameGeneration::Submit(MTL::CommandBuffer* sourceReady, int targetFps, bool fast)
{
    auto& impl = *m_impl;
    impl.state->fast.store(fast, std::memory_order_relaxed);
    if (!impl.writing || !sourceReady) return;
    auto image = std::move(impl.writing);
    image->id = ++impl.nextImage;
    image->time = Now(); image->ready = Retain(sourceReady);
    {
        std::lock_guard lock(impl.state->mutex);
        if (impl.state->target != targetFps)
        { ++impl.state->generation; impl.state->frames.clear(); impl.state->schedule.Reset(); impl.state->lastOutputTime = 0; }
        impl.state->target = targetFps;
        if (impl.state->schedule.Source(image->time))
        {
            ++impl.state->generation;
            impl.state->frames.clear();
            impl.state->lastOutputTime = 0;
        }
        if (!impl.state->lastOutputTime || s_outputSuspended.load(std::memory_order_acquire))
            impl.state->lastOutputTime = image->time;
        else if (image->time - impl.state->lastOutputTime >= 2.0)
        {
            impl.error = "No frame displayed for two seconds while the game was producing frames";
            impl.state->failed.store(true, std::memory_order_release);
            return;
        }
        impl.state->frames.push_back(image);
        // Keep one more history image: dropping the oldest on CPU submission
        // discarded the required pair while its replacement was still on GPU.
        while (impl.state->frames.size() > 4) impl.state->frames.pop_front();
        impl.state->enabled = true;
    }
    Register(impl.state);
}

void MetalFrameGeneration::Reset()
{
    auto& impl = *m_impl;
    Unregister(impl.state);
    {
        std::lock_guard lock(impl.state->mutex);
        impl.state->enabled = false;
        impl.state->lastOutputTime = 0;
        ++impl.state->generation;
        impl.state->frames.clear(); impl.state->schedule.Reset();
    }
    impl.writing.reset(); impl.pool.reset();
    {
        std::lock_guard lock(impl.state->statsMutex);
        impl.state->displayed.Reset(); impl.state->lastSource = 0;
        impl.state->audit.fill(0); impl.state->auditTime = 0;
    }
}

double MetalFrameGeneration::PresentedFPS() const
{
    return GetStatistics().displayedFPS;
}
MetalFrameGeneration::Statistics MetalFrameGeneration::GetStatistics() const
{
    auto state = m_impl->state;
    Statistics result;
    {
        std::lock_guard lock(state->mutex);
        result.sourceFPS = 1.0 / state->schedule.Interval();
        result.sourceReadyMs = state->schedule.ReadyDelay() * 1000;
        result.displayTicks = state->displayTicks; result.acceptedTicks = state->acceptedTicks;
        result.displayPeriodMs = state->displayPeriod * 1000;
    }
    std::lock_guard lock(state->statsMutex);
    result.displayedFPS = state->displayed.FPS(Now());
    result.original = state->original; result.generated = state->generated;
    result.repeated = state->repeated; result.dropped = state->dropped; result.gpuMs = state->gpuMs;
    result.starved = state->starved;
    result.submitted = state->submitted; result.skippedByDisplay = state->skippedByDisplay; result.noDrawable = state->noDrawable;
    result.audit = state->audit;
    result.auditPosition = state->auditPosition;
    result.auditAgeSeconds = state->auditTime ? Now() - state->auditTime : 0;
    const auto intervals = state->displayed.IntervalRangeMs();
    result.shortestPresentMs = intervals[0]; result.longestPresentMs = intervals[1];
    return result;
}
const std::string& MetalFrameGeneration::Error() const { return m_impl->error; }
bool MetalFrameGeneration::Failed() const { return m_impl->state->failed.load(std::memory_order_acquire); }
