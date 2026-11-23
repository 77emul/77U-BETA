#include <cmath>
#include "DocumentsLayout.h"
#import "CemuBridge.h"
#include "Cafe/HW/Latte/Renderer/Metal/RuntimePerformance.h"
#include "Common/MemoryBudget.h"
#include "SaveManagerIOS.h"

#import <Metal/Metal.h>
#import <GameController/GameController.h>
#import <AVFoundation/AVFoundation.h>
#import <os/log.h>

#include "Common/platform.h"
#include "Common/version.h"

#include "util/MemMapper/MemMapper.h"

#include <mach/mach.h>
#include <mach/task_info.h>
#include <mach/vm_region.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif
#import <Security/Security.h>
#import <dlfcn.h>
#include <cctype>
#include <limits>
#include <sys/sysctl.h>
#include <unistd.h>
#include <zip.h>
#include <deque>
#include "Cemu/ncrypto/ncrypto.h"


static os_log_t s_log = OS_LOG_DEFAULT;
static id<MTLDevice> s_metalDevice = nil;
static std::atomic_uint32_t s_memoryWarningCount{0};
static std::atomic_uint64_t s_runningTitleId{0};
static std::atomic_bool s_diagnosticLoggingEnabled{true};

static uint64_t ProcessFootprint()
{
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    return task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS
        ? info.phys_footprint : 0;
}

// Keep Apple unified-log formatting out of normal release gameplay as well.
// os_log_error remains available for actionable failures.
#define CEMU_DIAGNOSTIC_LOG(log, format, ...) \
    do { \
        if (s_diagnosticLoggingEnabled.load(std::memory_order_relaxed)) \
            os_log_with_type((log), OS_LOG_TYPE_DEFAULT, (format), ##__VA_ARGS__); \
    } while (0)
#undef os_log
#define os_log(log, format, ...) CEMU_DIAGNOSTIC_LOG((log), (format), ##__VA_ARGS__)

#include "Common/precompiled.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/HW/MMU/MMU.h"
#include <sys/mman.h>
#include <map>
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/TitleList/TitleInfo.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cafe/Filesystem/fsc.h"
#include "Common/FileStream.h"
#include "audio/IAudioAPI.h"
#include "audio/IAudioInputAPI.h"
#include "Cafe/OS/libs/mic/mic.h"
#include "audio/CubebAPI.h"
#include "config/CemuConfig.h"
#include "config/NetworkSettings.h"
#include "config/LaunchSettings.h"
#include "Cafe/Account/Account.h"
#include "Cafe/IOSU/legacy/iosu_crypto.h"
#include "util/crypto/aes128.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/snd_core/ax.h"
#include "Cafe/OS/libs/swkbd/swkbd.h"
#include "input/InputManager.h"
#include "input/api/GCController/GCController.h"
#include "Common/ExceptionHandler/ExceptionHandler.h"
#include "Cemu/Logging/CemuLogging.h"
#include "Cafe/HW/Latte/Core/LatteTiming.h"

// Set by the Latte thread once GPU initialization has finished (main.cpp on desktop).
std::atomic_bool g_isGPUInitFinished = false;

#include <thread>

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/RPL/rpl_symbol_storage.h"
#include "Cafe/OS/libs/coreinit/coreinit.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM.h"
#include "Cafe/OS/libs/coreinit/coreinit_Memory.h"
#include "Cafe/OS/libs/coreinit/coreinit_FG.h"
#include "Cafe/OS/libs/coreinit/coreinit_FS.h"
#include "Cafe/OS/libs/coreinit/coreinit_FSInit.h"
#include "Cafe/OS/libs/coreinit/coreinit_DynLoad.h"
#include "Cafe/OS/libs/coreinit/coreinit_IPC.h"
#include "Cafe/OS/libs/coreinit/coreinit_IPCBuf.h"
#include "Cafe/OS/libs/coreinit/coreinit_MessageQueue.h"
#include "Cafe/OS/libs/coreinit/coreinit_OSScreen.h"
#include "Cafe/OS/libs/coreinit/coreinit_Misc.h"
#include "Cafe/OS/libs/proc_ui/proc_ui.h"
#include "Cafe/OS/libs/vpad/vpad.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "Cafe/OS/libs/gx2/GX2_Event.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/OS/libs/TCL/TCL.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/IOSU/kernel/iosu_kernel.h"
#include "Cafe/IOSU/fsa/iosu_fsa.h"
#include "Cafe/Filesystem/fsc.h"

#include "Common/SysAllocator.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "config/ActiveSettings.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalFrameGeneration.h"
#include "gui/interface/WindowSystem.h"

// Weak (non-retaining) reference to the CAMetalLayer.
// The layer is owned by EmulatorMetalView; the bridge must
// never extend its lifetime beyond the view's.
__weak static CAMetalLayer* s_metalLayer = nil;
__weak static CAMetalLayer* s_drcMetalLayer = nil;
static std::mutex s_drcPreviewTextureMutex;
__strong static id<MTLTexture> s_retainedDRCPreviewTexture = nil;
__strong static id<MTLCommandQueue> s_retainedDRCPreviewCommandQueue = nil;
static uint64_t s_retainedDRCPreviewGeneration = 0;
static std::atomic<bool> s_drcPreviewMainDispatchPending{false};
__strong static void (^s_drcPreviewPublicationHandler)(void) = nil;
static std::string s_mainLogPath;

static bool PrepareDefaultMLCDirectories()
{
    const fs::path mlc = ActiveSettings::GetMlcPath();
    const fs::path directories[] = {
        mlc,
        mlc / "sys",
        mlc / "usr",
        mlc / "usr/title/00050000",
        mlc / "usr/title/0005000c",
        mlc / "usr/title/0005000e",
        mlc / "usr/save/00050010/1004a000/user/common/db",
        mlc / "usr/save/00050010/1004a100/user/common/db",
        mlc / "usr/save/00050010/1004a200/user/common/db",
        mlc / "sys/title/0005001b/1005c000/content",
    };

    for (const auto& directory : directories)
    {
        std::error_code ec;
        fs::create_directories(directory, ec);
        if (ec)
        {
            os_log_error(s_log, "CemuBridge_Initialize: failed to prepare MLC directory %{public}s: %{public}s",
                         _pathToUtf8(directory).c_str(), ec.message().c_str());
            return false;
        }
    }

    try
    {
        const auto langDir = mlc / "sys/title/0005001b/1005c000/content";
        const auto langFile = langDir / "language.txt";
        std::error_code ec;
        if (!fs::exists(langFile, ec))
        {
            std::ofstream file(langFile);
            if (file.is_open())
            {
                const char* langStrings[] = { "ja","en","fr","de","it","es","zh","ko","nl","pt","ru","zh" };
                for (const char* lang : langStrings)
                    file << fmt::format(R"("{}",)", lang) << std::endl;
                file.flush();
                file.close();
            }
        }

        const auto countryFile = langDir / "country.txt";
        if (!fs::exists(countryFile, ec))
        {
            std::ofstream file(countryFile);
            if (file.is_open())
            {
                for (sint32 i = 0; i < (sint32)NCrypto::GetCountryCount(); i++)
                {
                    const char* countryCode = NCrypto::GetCountryAsString(i);
                    if (countryCode && boost::iequals(countryCode, "NN"))
                        file << "NULL," << std::endl;
                    else
                        file << fmt::format(R"("{}",)", countryCode ? countryCode : "US") << std::endl;
                }
                file.flush();
                file.close();
            }
        }
    }
    catch (...)
    {
    }

    return true;
}
static std::string s_previousCrashReportPath;
static std::string s_previousLogDirectory;

static float s_drawableWidth = 0.0f;
static float s_drawableHeight = 0.0f;

static constexpr int32_t kDRCDrawableWidth = 854;
static constexpr int32_t kDRCDrawableHeight = 480;

static void CemuBridge_DRCPreviewPublished(void)
{
    if (s_drcPreviewMainDispatchPending.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    dispatch_async(dispatch_get_main_queue(), ^{
        s_drcPreviewMainDispatchPending.store(false, std::memory_order_release);
        void (^handler)(void) = nil;
        {
            std::lock_guard<std::mutex> lock(s_drcPreviewTextureMutex);
            handler = s_drcPreviewPublicationHandler;
        }
        if (handler) {
            handler();
        }
    });
}

static void CemuBridge_RestoreDRCPreviewPublishCallback(void)
{
    bool hasPreviewConsumer = false;
    {
        std::lock_guard<std::mutex> lock(s_drcPreviewTextureMutex);
        hasPreviewConsumer = s_drcPreviewPublicationHandler != nil;
    }
    MetalRenderer_SetDRCPreviewPublishCallback(
        hasPreviewConsumer ? CemuBridge_DRCPreviewPublished : nullptr);
}

// Device name stored as a static string so GetInfo can return
// a pointer that outlives the call.
static char s_deviceName[256] = {0};

static bool s_audioSessionConfigured = false;
static bool s_audioSessionActive = false;

static bool CemuBridge_IsPlayStationControllerAudioPort(AVAudioSessionPortDescription* port)
{
    NSString* name = port.portName.lowercaseString;
    if (!name)
        return false;
    return [name containsString:@"dualsense"] ||
           [name containsString:@"dualshock"] ||
           [name containsString:@"playstation"] ||
           [name containsString:@"wireless controller"];
}

static void CemuBridge_KeepControllerFromCapturingAudio(AVAudioSession* session)
{
    bool controllerOwnsOutput = false;
    for (AVAudioSessionPortDescription* output in session.currentRoute.outputs) {
        if (CemuBridge_IsPlayStationControllerAudioPort(output)) {
            controllerOwnsOutput = true;
            break;
        }
    }
    if (!controllerOwnsOutput)
        return;

    // DualShock/DualSense expose an audio device. iPadOS can silently route the
    // whole game into that device when it connects, making the iPad appear
    // muted. Override only that controller route; AirPods and wired headphones
    // remain untouched.
    NSError* routeError = nil;
    if (![session overrideOutputAudioPort:AVAudioSessionPortOverrideSpeaker error:&routeError]) {
        os_log_error(s_log, "CemuBridge: failed to restore speaker after controller audio route: %{public}@", routeError);
    }
}

static void CemuBridge_ConfigureAudioSession(void)
{
    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSError* error = nil;
    s_audioSessionConfigured = [session setCategory:AVAudioSessionCategoryPlayAndRecord
                                              mode:AVAudioSessionModeDefault
                                           options:(AVAudioSessionCategoryOptionDefaultToSpeaker |
                                                    AVAudioSessionCategoryOptionAllowBluetoothHFP |
                                                    AVAudioSessionCategoryOptionAllowAirPlay |
                                                    AVAudioSessionCategoryOptionAllowBluetoothA2DP)
                                             error:&error];
    if (!s_audioSessionConfigured) {
        os_log_error(s_log, "CemuBridge: AVAudioSession category failed: %{public}@", error);
        return;
    }

    error = nil;
    s_audioSessionActive = [session setActive:YES error:&error];
    if (!s_audioSessionActive) {
        os_log_error(s_log, "CemuBridge: AVAudioSession activation failed: %{public}@", error);
    } else {
        CemuBridge_KeepControllerFromCapturingAudio(session);
    }
}

int32_t CemuBridge_GetMicrophonePermissionStatus(void);

extern "C" bool CemuBridge_IsMicrophoneCaptureReady(void)
{
    AVAudioSession* session = [AVAudioSession sharedInstance];
    return CemuBridge_GetMicrophonePermissionStatus() == 2 &&
           session.isInputAvailable && session.sampleRate > 0.0;
}

static void CemuBridge_UpdateMicrophoneInputSelection(void)
{
    AVAudioSession* session = [AVAudioSession sharedInstance];
    const bool captureReady = CemuBridge_IsMicrophoneCaptureReady();
    GetConfigHandle().data().input_device = captureReady ? L"default" : L"";
    mic_requestInputDeviceRefresh();
    os_log(s_log,
           "CemuBridge: GamePad microphone %{public}s (permission=%d input=%{public}s rate=%.1f)",
           captureReady ? "enabled" : "disabled",
           CemuBridge_GetMicrophonePermissionStatus(),
           session.isInputAvailable ? "yes" : "no",
           session.sampleRate);
}

const char* CemuBridge_GetCemuVersion(void)
{
    return BUILD_VERSION_WITH_NAME_STRING;
}

static std::atomic<double> s_currentFPS{0.0};
static std::atomic<double> s_currentCPUWaitMs{0.0};
static std::atomic<double> s_currentGPUMs{0.0};
static std::atomic<uint32_t> s_currentDrawCalls{0};
static std::atomic<uint32_t> s_currentRenderPasses{0};
static std::atomic<double> s_cachedHostDisplayHz{60.0};
static std::atomic<double> s_guestClockSpeedPercent{100.0};
static std::atomic_bool s_titleEverLaunched{false};
static std::atomic_bool s_titleWasObservedRunning{false};
// 0 = idle, 1 = save operation, 2 = title launch/running.
static std::atomic<int> s_titleSaveLifecycleState{0};
static int s_cpuDiagnosticMode = 2; // 0 = SinglecoreRecompiler (JIT), 1 = SinglecoreInterpreter, 2 = MulticoreRecompiler (JIT)
std::atomic<int> g_rtStoreDiagMode{0}; // 0 = Normal, 1 = Force Store Color Attachments
std::atomic<int> g_strictAliasSyncDiagMode{0}; // 0 = Normal, 1 = Strict GPU Alias Sync

extern "C" double CemuBridge_GetCurrentFPS(void)
{
    return s_currentFPS.load(std::memory_order_relaxed);
}

extern "C" CemuPerfStatsSnapshot CemuBridge_GetPerfStatsSnapshot(void)
{
    CemuPerfStatsSnapshot snapshot;
    snapshot.fps = s_currentFPS.load(std::memory_order_relaxed);
    snapshot.cpuWaitMs = s_currentCPUWaitMs.load(std::memory_order_relaxed);
    snapshot.gpuMs = s_currentGPUMs.load(std::memory_order_relaxed);
    snapshot.drawCalls = s_currentDrawCalls.load(std::memory_order_relaxed);
    snapshot.renderPasses = s_currentRenderPasses.load(std::memory_order_relaxed);
    return snapshot;
}

extern "C" void CemuBridge_UpdatePerfStats(double fps, double cpuWaitMs, double gpuMs, uint32_t drawCalls, uint32_t renderPasses)
{
    s_currentFPS.store(fps, std::memory_order_relaxed);
    s_currentCPUWaitMs.store(cpuWaitMs, std::memory_order_relaxed);
    s_currentGPUMs.store(gpuMs, std::memory_order_relaxed);
    s_currentDrawCalls.store(drawCalls, std::memory_order_relaxed);
    s_currentRenderPasses.store(renderPasses, std::memory_order_relaxed);
}

extern "C" void CemuBridge_SetCurrentFPS(double fps)
{
    s_currentFPS.store(fps, std::memory_order_relaxed);
}

extern "C" void CemuBridge_SetGuestClockSpeedPercent(double pct)
{
    s_guestClockSpeedPercent.store(pct, std::memory_order_relaxed);
}

extern "C" double CemuBridge_GetHostDisplayHz(void)
{
    return s_cachedHostDisplayHz.load(std::memory_order_relaxed);
}

extern "C" int CemuBridge_GetCPUDiagnosticMode(void)
{
    uint64 titleId = CafeSystem::GetForegroundTitleId();
    bool isColorSplash = (CafeSystem::GetRPXHashUpdated() == 0x3034f732)
        || titleId == 0x000500001f600b00ULL
        || titleId == 0x000500001f600a00ULL
        || titleId == 0x000500001f600900ULL;
    if (isColorSplash) {
        return 0;
    }
    return s_cpuDiagnosticMode;
}

extern "C" void CemuBridge_SetCPUDiagnosticMode(int mode)
{
    if (s_titleEverLaunched) {
        return; // Do not switch CPU mode while a title is running
    }
    s_cpuDiagnosticMode = std::clamp(mode, 0, 2);
    [[NSUserDefaults standardUserDefaults] setInteger:s_cpuDiagnosticMode forKey:@"CemuIOS.CPUDiagnosticMode"];
}

extern "C" int CemuBridge_GetTextureCacheDiagMode(void)
{
    return g_textureCacheDiagMode.load(std::memory_order_relaxed);
}

extern "C" void CemuBridge_SetTextureCacheDiagMode(int mode)
{
    g_textureCacheDiagMode.store(std::clamp(mode, 0, 3), std::memory_order_relaxed);
    [[NSUserDefaults standardUserDefaults] setInteger:std::clamp(mode, 0, 3) forKey:@"CemuIOS.TextureCacheDiagnosticMode"];
}

extern "C" int CemuBridge_GetRTStoreDiagMode(void)
{
    return g_rtStoreDiagMode.load(std::memory_order_relaxed);
}

extern "C" void CemuBridge_SetRTStoreDiagMode(int mode)
{
    g_rtStoreDiagMode.store(std::clamp(mode, 0, 1), std::memory_order_relaxed);
    [[NSUserDefaults standardUserDefaults] setInteger:std::clamp(mode, 0, 1) forKey:@"CemuIOS.RTStoreDiagnosticMode"];
}

static std::atomic<double> s_emuSpeedPercent{100.0};

extern "C" void CemuBridge_SetEmuSpeedPercent(double pct)
{
    s_emuSpeedPercent.store(pct, std::memory_order_relaxed);
}

class CemuIOSSystemImplementation : public CafeSystem::SystemImplementation
{
public:
    void CafeRecreateCanvas() override
    {
        os_log(s_log, "CemuIOSSystemImplementation::CafeRecreateCanvas: Recreating canvas and renderer for title transition...");
        CemuBridge_LogJITMilestone("[CANVAS] Recreating canvas and renderer for title transition...");

        auto recreate = ^{
            // Give the window system the current layer pointers
            auto& winInfo = WindowSystem::GetWindowInfo();
            if (s_metalLayer) {
                winInfo.window_main.surface = (__bridge void*)s_metalLayer;
                winInfo.window_main.backend = WindowSystem::WindowHandleInfo::Backend::Cocoa;
                winInfo.phys_width = (int32_t)s_metalLayer.drawableSize.width;
                winInfo.phys_height = (int32_t)s_metalLayer.drawableSize.height;
            }

            if (s_drcMetalLayer) {
                winInfo.window_pad.surface = (__bridge void*)s_drcMetalLayer;
                winInfo.window_pad.backend = WindowSystem::WindowHandleInfo::Backend::Cocoa;
                winInfo.phys_pad_width = kDRCDrawableWidth;
                winInfo.phys_pad_height = kDRCDrawableHeight;
            }

            // Instantiate MetalRenderer if it was shut down
            if (!g_renderer) {
                g_renderer = std::make_unique<MetalRenderer>();
            }

            auto metal_renderer = MetalRenderer::GetInstance();
            if (metal_renderer && s_metalLayer) {
                Vector2i layerSize(winInfo.phys_width, winInfo.phys_height);
                metal_renderer->InitializeLayer(layerSize, true);
                CemuBridge_LogJITMilestone("[CANVAS] Primary Metal layer reinitialized");
            }

            if (metal_renderer && s_drcMetalLayer && !metal_renderer->IsPadWindowActive()) {
                metal_renderer->InitializeLayer({kDRCDrawableWidth, kDRCDrawableHeight}, false);
                CemuBridge_LogJITMilestone("[CANVAS] DRC Pad Metal layer reinitialized");
            }
            // SwiftUI's preview view normally installs this callback. A title
            // transition can recreate the renderer between SwiftUI updates, so
            // restore the bridge-owned consumer explicitly here as well.
            CemuBridge_RestoreDRCPreviewPublishCallback();
        };

        if ([NSThread isMainThread]) {
            recreate();
        } else {
            dispatch_sync(dispatch_get_main_queue(), recreate);
        }
    }

    void CafePPCProcessExit() override
    {
        os_log(s_log, "CemuIOSSystemImplementation::CafePPCProcessExit called");
        CemuBridge_LogJITMilestone("[CANVAS] CafePPCProcessExit called");
    }
};

static void CemuIOS_UncaughtExceptionHandler(NSException* exception)
{
    std::string details = "NSException: ";
    if (exception.name) details += [exception.name UTF8String];
    details += "\nReason: ";
    if (exception.reason) details += [exception.reason UTF8String];
    details += "\nCall stack symbols:\n";
    for (NSString* sym in exception.callStackSymbols)
    {
        details += "  ";
        details += [sym UTF8String];
        details += "\n";
    }
    ExceptionHandler_RecordFatalError("Uncaught NSException", details.c_str());
}

static void CemuIOS_TerminateHandler()
{
    std::string details = "std::terminate() called\n";
    try
    {
        auto ex = std::current_exception();
        if (ex)
            std::rethrow_exception(ex);
    }
    catch (const std::exception& e)
    {
        details += "std::exception what(): ";
        details += e.what();
        details += "\n";
    }
    catch (...)
    {
        details += "Unknown exception type thrown\n";
    }
    ExceptionHandler_RecordFatalError("C++ std::terminate", details.c_str());
    _Exit(1);
}

static CemuIOSSystemImplementation s_iosSystemImplementation;

bool CemuBridge_Initialize(void)
{
    @autoreleasepool {
        NSString* documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
        if (documents) {
            const fs::path userPath(documents.UTF8String);
            s_previousLogDirectory = CemuIOSDocuments::prepare(userPath).string();
            const fs::path pendingCrashPath = userPath / "Logs/crash_report.pending";
            const fs::path previousCrashPath = userPath / "Logs/last_crash_report.txt";
            std::error_code crashFileError;
            if (fs::exists(pendingCrashPath, crashFileError) &&
                fs::file_size(pendingCrashPath, crashFileError) > 0 &&
                (!fs::exists(previousCrashPath, crashFileError) || fs::file_size(previousCrashPath, crashFileError) == 0)) {
                fs::copy_file(pendingCrashPath, previousCrashPath,
                    fs::copy_options::overwrite_existing, crashFileError);
            }
            s_mainLogPath = (userPath / "Logs/log.txt").string();
            s_previousCrashReportPath = previousCrashPath.string();
            // Register before installing handlers or initializing Metal/config:
            // an early initialization fault needs the same persistent report.
            ExceptionHandler_SetPersistentCrashLogPaths(previousCrashPath.string().c_str(), pendingCrashPath.string().c_str());
        }
    }

	static std::once_flag exceptionHandlerInitOnce;
	std::call_once(exceptionHandlerInitOnce, [] {
		ExceptionHandler_Init();
		NSSetUncaughtExceptionHandler(CemuIOS_UncaughtExceptionHandler);
		std::set_terminate(CemuIOS_TerminateHandler);
	});
	static std::once_flag aesInitOnce;
	std::call_once(aesInitOnce, AES128_init);

    s_log = os_log_create("app.sevenseven.emulator", "Bridge");
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    const BOOL diagnosticLogging = [defaults objectForKey:@"CemuIOS.DiagnosticLoggingEnabled"] == nil
        || [defaults boolForKey:@"CemuIOS.DiagnosticLoggingEnabled"];
    s_diagnosticLoggingEnabled.store(diagnosticLogging, std::memory_order_relaxed);
    MemoryBudget::footprintProvider = ProcessFootprint;
    cemuLog_setOutputEnabled(diagnosticLogging);

    os_log(s_log, "CemuBridge_Initialize: starting...");

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    s_cachedHostDisplayHz.store((double)[UIScreen mainScreen].maximumFramesPerSecond, std::memory_order_relaxed);
#pragma clang diagnostic pop
#else
    s_cachedHostDisplayHz.store(60.0, std::memory_order_relaxed);
#endif

    s_metalDevice = MTLCreateSystemDefaultDevice();
    if (!s_metalDevice) {
        os_log_error(s_log, "CemuBridge_Initialize: MTLCreateSystemDefaultDevice() returned nil");
        return false;
    }

    const char* name = [[s_metalDevice name] UTF8String];
    if (name) {
        strlcpy(s_deviceName, name, sizeof(s_deviceName));
    }

    cemuLog_log(LogType::Force, "[METAL INSTRUMENTATION] captureSupported={} deviceClass={}",
                CemuBridge_IsGPUFrameCaptureSupported(), NSStringFromClass([s_metalDevice class]).UTF8String);

    os_log(s_log, "CemuBridge_Initialize: Metal device = %{public}s, hostDisplayHz = %.1f", s_deviceName, s_cachedHostDisplayHz.load(std::memory_order_relaxed));
    os_log(s_log, "CemuBridge_Initialize: Cemu core = %{public}s", CemuBridge_GetCemuVersion());


    // Host paths and virtual filesystem, as in upstream CafeSystem::Initialize
    @autoreleasepool {
        NSArray* paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
        NSString* docsDir = [paths firstObject];
        if (docsDir) {
            fs::path userPath = [docsDir UTF8String];
            fs::path execPath = [[[NSBundle mainBundle] bundlePath] UTF8String];
            fs::path configDir = fs::exists(userPath / "config") ? userPath / "config" : userPath / "System/config";
            fs::path cacheDir = fs::exists(userPath / "cache") ? userPath / "cache" : userPath / "System/cache";
            // Runtime resources are immutable files inside the application
            // bundle. Point Cemu's data path at CemuData inside the bundle so
            // shared fonts and profiles can be discovered through GetDataPath()
            // without creating a root-level 'resources' folder that breaks codesign.
            fs::path dataDir = execPath / "CemuData";
            if (!fs::exists(dataDir)) {
                dataDir = execPath;
            }
            std::error_code ec;
            fs::create_directories(configDir, ec);
            fs::create_directories(cacheDir, ec);
            std::set<fs::path> failedWrites;
            ActiveSettings::SetPaths(false, execPath, userPath, configDir, cacheDir, dataDir, failedWrites);
            cemuLog_setOutputEnabled(true);
            cemuLog_createLogFile(false);

            GetConfigHandle().SetFilename(ActiveSettings::GetConfigPath("settings.xml").generic_wstring());
            GetConfigHandle().Load();
            NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
            if ([defaults objectForKey:@"CemuIOS.CPUDiagnosticMode"] != nil) {
                s_cpuDiagnosticMode = std::clamp((int)[defaults integerForKey:@"CemuIOS.CPUDiagnosticMode"], 0, 2);
            } else {
                s_cpuDiagnosticMode = 2;
            }
            g_textureCacheDiagMode.store(std::clamp((int)[defaults integerForKey:@"CemuIOS.TextureCacheDiagnosticMode"], 0, 3));
            g_rtStoreDiagMode.store(std::clamp((int)[defaults integerForKey:@"CemuIOS.RTStoreDiagnosticMode"], 0, 1));
            g_strictAliasSyncDiagMode.store(std::clamp((int)[defaults integerForKey:@"CemuIOS.StrictAliasSyncDiagnosticMode"], 0, 1));

            auto& configInit = GetConfigHandle().data();
            configInit.console_language = CafeConsoleLanguage::EN;
            if (![defaults boolForKey:@"CemuIOS.StartupSoundDefaultV1"]) {
                configInit.play_boot_sound = true;
                [defaults setBool:YES forKey:@"CemuIOS.StartupSoundDefaultV1"];
            }
            GetConfigHandle().Save();
            if (![defaults boolForKey:@"CemuIOS.BestDefaultsAppliedV2"]) {
                configInit.async_compile = true;
                configInit.vsync = 1;
                configInit.gx2drawdone_sync = false;
                configInit.upscale_filter = 0;
                configInit.downscale_filter = 0;
                configInit.fullscreen_scaling = 0;
                configInit.audio_delay = 2;
                configInit.tv_volume = 100;
                // TV and GamePad both use the iPad's default speaker. Playing both
                // at full volume repeats shared game audio with a small delay.
                configInit.pad_volume = 0;
                GetConfigHandle().Save();
                [defaults setBool:YES forKey:@"CemuIOS.BestDefaultsAppliedV2"];
                [defaults setInteger:2 forKey:@"CemuIOS.CPUDiagnosticMode"];
                s_cpuDiagnosticMode = 2;
            }

            if (WindowSystem::IsPhone())
            {
                bool configChanged = false;
                for (auto& entry : configInit.graphic_pack_entries)
                {
                    const std::string entryPath = _pathToUtf8(entry.first);
                    if (entryPath.find("BreathOfTheWild/Graphics") != std::string::npos ||
                        entryPath.find("Breath of the Wild/Graphics") != std::string::npos)
                    {
                        auto itRes = entry.second.find("Resolution");
                        if (itRes != entry.second.end() && itRes->second != "1280x720 (HD, Default)")
                        {
                            itRes->second = "1280x720 (HD, Default)";
                            configChanged = true;
                        }
                        auto itShad = entry.second.find("Shadows");
                        if (itShad != entry.second.end() && itShad->second.find("Low") == std::string::npos)
                        {
                            itShad->second = "Low (50%)";
                            configChanged = true;
                        }
                    }
                }
                if (configChanged)
                {
                    GetConfigHandle().Save();
                }
            }

            // The desktop startup prepares this base MLC layout before any
            // title runs. FFL relies on the Mii Maker database directories
            // already existing when it creates FFL_HDB.dat.
            if (!PrepareDefaultMLCDirectories()) {
                return false;
            }

            Account::RefreshAccounts();
            const auto& discoveredAccounts = Account::GetAccounts();
            const uint32 currentSavedPid = GetConfigHandle().data().account.m_persistent_id.GetValue();
            bool hasMatchingAccount = false;
            for (const auto& acc : discoveredAccounts) {
                if (acc.GetPersistentId() == currentSavedPid) {
                    hasMatchingAccount = true;
                    break;
                }
            }
            if (!hasMatchingAccount && !discoveredAccounts.empty()) {
                GetConfigHandle().data().account.m_persistent_id.SetValue(discoveredAccounts.front().GetPersistentId());
                GetConfigHandle().Save();
            }

			// Export universal.js into user documents directory so LiveContainer can select it as JIT script
			@autoreleasepool {
				NSString* targetPath = [NSString stringWithUTF8String:(userPath / "JIT/universal.js").c_str()];
				NSString* srcPath = [[NSBundle mainBundle] pathForResource:@"universal" ofType:@"js"];
				if (!srcPath) {
					srcPath = [[[NSBundle mainBundle] bundlePath] stringByAppendingPathComponent:@"PlugIns/CemuJITHelper.appex/universal.js"];
				}
				NSData* script = srcPath ? [NSData dataWithContentsOfFile:srcPath] : nil;
				NSData* installedScript = [NSData dataWithContentsOfFile:targetPath];
				if (script.length > 0 && ![script isEqualToData:installedScript]) {
					NSError* writeError = nil;
					if (![script writeToFile:targetPath options:NSDataWritingAtomic error:&writeError]) {
						os_log_error(s_log, "Could not prepare LiveContainer JIT script: %{public}@", writeError.localizedDescription);
					}
				}
			}
        }

        // Graphic packs come from the user directory and the app bundle
        std::vector<fs::path> additionalGfxPaths;
        NSString* resourcePath = [[NSBundle mainBundle] resourcePath];
        if (resourcePath) {
            fs::path bundleResPath = [resourcePath UTF8String];
            additionalGfxPaths.push_back(bundleResPath / "graphicPacks");
        }
        GraphicPack2::LoadAll(additionalGfxPaths);
        os_log(s_log, "CemuBridge_Initialize: GraphicPack2::LoadAll() completed (%zu graphic packs discovered)",
               GraphicPack2::GetGraphicPacks().size());
    }

    // Log JIT session start with PID and timestamp
    {
        char sessionStartMsg[256];
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        struct tm tm_buf;
        localtime_r(&t, &tm_buf);
        char timeStr[64];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &tm_buf);
        snprintf(sessionStartMsg, sizeof(sessionStartMsg), "=== JIT SESSION START pid=%d timestamp=%s ===", getpid(), timeStr);
        CemuBridge_LogJITMilestone(sessionStartMsg);
        CemuBridge_LogJITMilestone("[BUILD] " __DATE__ " " __TIME__);
    }

    if (s_cpuDiagnosticMode == 1) {
        // No-JIT runs the three Wii U cores on three interpreter threads; CemuIOS.NoJITSingleCore=true restores the old one-thread path.
        // SinglecoreInterpreter keeps PPCRecompiler_init off; ForceMultiCoreInterpreter alone makes OSSchedulerBegin start 3 cores.
        const bool noJitSingleCore = [[NSUserDefaults standardUserDefaults] boolForKey:@"CemuIOS.NoJITSingleCore"];
        LaunchSettings::SetForceInterpreter(noJitSingleCore);
        LaunchSettings::SetForceMultiCoreInterpreter(!noJitSingleCore);
        GetConfigHandle().data().cpu_mode.SetValue(CPUMode::SinglecoreInterpreter);
        CemuBridge_LogJITMilestone(noJitSingleCore ? "[CPU MODE] Singlecore interpreter selected (CemuBridge_Initialize)"
                                                   : "[CPU MODE] Triplecore interpreter selected (CemuBridge_Initialize)");
    } else if (s_cpuDiagnosticMode == 2) {
        LaunchSettings::SetForceInterpreter(false);
        LaunchSettings::SetForceMultiCoreInterpreter(false);
        GetConfigHandle().data().cpu_mode.SetValue(CPUMode::MulticoreRecompiler);
        CemuBridge_LogJITMilestone("[CPU MODE] MulticoreRecompiler (Triplecore JIT) selected (CemuBridge_Initialize)");
    } else {
        LaunchSettings::SetForceInterpreter(false);
        LaunchSettings::SetForceMultiCoreInterpreter(false);
        GetConfigHandle().data().cpu_mode.SetValue(CPUMode::SinglecoreRecompiler);
        CemuBridge_LogJITMilestone("[CPU MODE] SinglecoreRecompiler (JIT) selected (CemuBridge_Initialize)");
    }
    InputManager::instance().load();

    auto& config = GetConfigHandle().data();
    config.audio_api = IAudioAPI::Cubeb;
    config.tv_device = L"default";
    // Previous iPad defaults enabled both Wii U outputs on the same speaker.
    // Restore the desktop TV-only default for those auto-selected values; the
    // GamePad volume slider can still enable its separate stream explicitly.
    NSUserDefaults* audioDefaults = NSUserDefaults.standardUserDefaults;
    if (![audioDefaults boolForKey:@"CemuIOS.SingleSpeakerAudioDefaultV1"]) {
        const bool usedOldDefault = [audioDefaults boolForKey:@"CemuIOS.BestDefaultsAppliedV2"] && config.pad_volume == 100;
        const bool usedMigratedDefault = [audioDefaults boolForKey:@"CemuIOS.GamePadAudioDefaultV1"] && config.pad_volume == 30;
        if (usedOldDefault || usedMigratedDefault) {
            config.pad_volume = 0;
            GetConfigHandle().Save();
        }
        [audioDefaults setBool:YES forKey:@"CemuIOS.SingleSpeakerAudioDefaultV1"];
    }
    config.pad_device = L"default";
    if (![audioDefaults boolForKey:@"CemuIOS.HideShaderNotificationsV1"]) {
        config.notification.shader_compiling = false;
        GetConfigHandle().Save();
        [audioDefaults setBool:YES forKey:@"CemuIOS.HideShaderNotificationsV1"];
    }
    CemuBridge_ConfigureAudioSession();
    CemuBridge_UpdateMicrophoneInputSelection();
    IAudioAPI::InitializeStatic();
    IAudioInputAPI::InitializeStatic();

    // Match desktop Cemu's online bootstrap. This validates only the user's
    // existing Wii U account/certificate dump; it creates no credentials.
    ActiveSettings::Init();

    // Apply the selected log channels so per-packet tracing is not left on for a whole online match.
    cemuLog_setActiveLoggingFlags(s_loggingFlagMask);

    CafeSystem::Initialize();
    CafeSystem::SetImplementation(&s_iosSystemImplementation);

    // Match the desktop startup lifecycle for installed titles. Updates and DLC
    // live persistently in mlc01, but CafeSystem resolves them through
    // CafeTitleList. Without rebuilding that list on every process launch, an
    // update installed by the iPadOS UI is visible only until the app restarts.
    CafeTitleList::Initialize(ActiveSettings::GetUserDataPath("System/title_list_cache.xml"));
    fs::path gamesPath = ActiveSettings::GetUserDataPath("Games");
    std::error_code ecGames;
    if (fs::exists(gamesPath, ecGames)) {
        CafeTitleList::AddScanPath(gamesPath);
        os_log(s_log, "CemuBridge_Initialize: added default Games scan path: %{public}s", _pathToUtf8(gamesPath).c_str());
    }
    for (auto& it : GetConfig().game_paths) {
        CafeTitleList::AddScanPath(_utf8ToPath(it));
    }
    CafeTitleList::SetMLCPath(ActiveSettings::GetMlcPath());
    CafeTitleList::Refresh();
    while (CafeTitleList::IsScanning())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    cemuLog_log(LogType::Force, "iPadOS: persistent MLC and Games title scan completed ({} titles known)", CafeTitleList::GetAllTitleIds().size());

    PPCTimer_init();
    os_log(s_log, "CemuBridge_Initialize: PPCTimer_init() completed (ready=%d)", PPCTimer_isReady());

    os_log(s_log, "CemuBridge_Initialize: CafeSystem initialized (memory_base=0x%lx); ready for title launch",
           (unsigned long)memory_base);

    return true;
}

CemuBridgeInfo CemuBridge_GetInfo(void)
{
    CemuBridgeInfo info = {};

    if (CafeSystem::IsTitleRunning() && PPCTimer_isReady())
        PPCTimebaseDiagnostics_SampleGuestClock(coreinit::OSGetSystemTime());
    info.metalDeviceName = s_deviceName[0] != '\0' ? s_deviceName : NULL;
    info.metalAvailable = (s_metalDevice != nil);
    info.metalLayerAttached = (s_metalLayer != nil);
    info.cemuVersion = CemuBridge_GetCemuVersion();
    info.titleBootResult = CemuBridge_GetTitleBootInfo();
    return info;
}

static CemuTitleBootInfo s_titleBootResult = {};

static char s_securityScopeStatus[32] = "Not Required";

extern "C" bool JIT26_PrepareArena(void);
extern "C" void JIT26Detach(void);
static std::atomic<bool> s_jitMemoryReady{false};
extern "C" bool CemuBridge_IsJITMemoryReady(void) { return s_jitMemoryReady.load(std::memory_order_acquire); }
#include <mach-o/dyld.h>

static bool CemuBridge_IsProcessTraced(void) {
    int mib[] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    struct kinfo_proc processInfo = {};
    size_t size = sizeof(processInfo);
    return sysctl(mib, 4, &processInfo, &size, nullptr, 0) == 0 &&
        (processInfo.kp_proc.p_flag & P_TRACED) != 0;
}

extern "C" bool CemuBridge_IsXcodeDebuggerAttached(void) {
    // Xcode also injects these launch-environment markers when "Debug
    // executable" is disabled. They describe how the process was launched,
    // not whether LLDB is currently attached. Requiring P_TRACED prevents a
    // normal Xcode/device launch from blocking the built-in or external JIT.
    if (!CemuBridge_IsProcessTraced())
        return false;
    if (getenv("OS_ACTIVITY_DT_MODE") != nullptr ||
        getenv("XCODE_DBG_XPC_EXCLUSIONS") != nullptr ||
        getenv("__XCODE_BUILT_PRODUCTS_DIR_PATHS") != nullptr ||
        getenv("NSUnbufferedIO") != nullptr) {
        return true;
    }
    const char* dyldInsert = getenv("DYLD_INSERT_LIBRARIES");
    if (dyldInsert != nullptr && (strstr(dyldInsert, "libBacktraceRecording") != nullptr ||
                                  strstr(dyldInsert, "libMainThreadChecker") != nullptr ||
                                  strstr(dyldInsert, "libViewDebuggerSupport") != nullptr)) {
        return true;
    }
    uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i) {
        const char* name = _dyld_get_image_name(i);
        if (name && (strstr(name, "libBacktraceRecording") != nullptr ||
                     strstr(name, "libMainThreadChecker") != nullptr ||
                     strstr(name, "libViewDebuggerSupport") != nullptr)) {
            return true;
        }
    }
    return false;
}

extern "C" bool CemuBridge_HasJITDebugger(void) {
    if (CemuBridge_IsXcodeDebuggerAttached())
        return false;
    return CemuBridge_IsProcessTraced();
}
extern "C" bool CemuBridge_PrepareJITMemory(void) {
    if (CemuBridge_IsJITMemoryReady()) return true;
    if (!CemuBridge_HasJITDebugger()) return false;
    const bool ready = JIT26_PrepareArena();
    if (ready) s_jitMemoryReady.store(true, std::memory_order_release);
    return ready;
}
extern "C" void CemuBridge_DetachJITDebugger(void) {
    if (CemuBridge_IsJITMemoryReady() && CemuBridge_HasJITDebugger()) JIT26Detach();
}
extern "C" bool CemuBridge_IsJITEnabled(void) {
    return CemuBridge_IsJITMemoryReady() || CemuBridge_HasJITDebugger();
}

namespace
{
bool PrepareGraphicPackOperation()
{
    if (CafeSystem::IsTitleRunning())
    {
        s_titleWasObservedRunning.store(true, std::memory_order_release);
        return false;
    }

    // A successful title launch owns lifecycle state 2 until the Cafe title has
    // actually stopped. Once that transition was observed, release the state so
    // graphic-pack choices can be saved before the app restart required for the
    // next title launch. Title relaunch itself remains separately guarded.
    if (s_titleWasObservedRunning.load(std::memory_order_acquire))
    {
        int expected = 2;
        s_titleSaveLifecycleState.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    }
    return true;
}

class SaveOperationScope
{
public:
    SaveOperationScope()
    {
        int expected = 0;
        m_acquired = s_titleSaveLifecycleState.compare_exchange_strong(expected, 1, std::memory_order_acq_rel);
    }

    ~SaveOperationScope()
    {
        if (m_acquired)
            s_titleSaveLifecycleState.store(0, std::memory_order_release);
    }

    explicit operator bool() const { return m_acquired; }

private:
    bool m_acquired{};
};

class TitleLaunchScope
{
public:
    TitleLaunchScope()
    {
        int expected = 0;
        m_acquired = s_titleSaveLifecycleState.compare_exchange_strong(expected, 2, std::memory_order_acq_rel);
    }

    ~TitleLaunchScope()
    {
        if (m_acquired && !m_committed)
            s_titleSaveLifecycleState.store(0, std::memory_order_release);
    }

    explicit operator bool() const { return m_acquired; }
    void Commit() { m_committed = true; }

private:
    bool m_acquired{};
    bool m_committed{};
};

void CopySavePath(char* destination, size_t destinationSize, const std::filesystem::path& path)
{
    const std::string value = path.generic_string();
    strlcpy(destination, value.c_str(), destinationSize);
}

void CopySaveError(char* destination, size_t destinationSize, const std::string& error)
{
    strlcpy(destination, error.c_str(), destinationSize);
}
}

extern "C" bool CemuBridge_CanManageSaves(void)
{
    return !s_titleEverLaunched.load(std::memory_order_acquire) &&
           s_titleSaveLifecycleState.load(std::memory_order_acquire) == 0;
}

extern "C" CemuSaveInfo CemuBridge_GetSaveInfo(uint64_t titleId)
{
    CemuSaveInfo output{};
    if (s_titleEverLaunched.load(std::memory_order_acquire))
    {
        strlcpy(output.errorMessage, "Save management is unavailable while a title is active", sizeof(output.errorMessage));
        return output;
    }

    SaveOperationScope operation;
    if (!operation)
    {
        strlcpy(output.errorMessage, "Another save operation is already running", sizeof(output.errorMessage));
        return output;
    }

    try
    {
        const auto info = SaveManagerIOS::GetSaveInfo(titleId);
        output.success = info.success;
        output.saveExists = info.saveExists;
        output.hasUserSaveData = info.hasUserSaveData;
        output.rollbackExists = info.rollbackExists;
        output.totalBytes = info.totalBytes;
        output.modifiedUnixSeconds = info.modifiedUnixSeconds;
        CopySavePath(output.latestRollbackPath, sizeof(output.latestRollbackPath), info.latestRollbackPath);
        CopySaveError(output.errorMessage, sizeof(output.errorMessage), info.error);
    }
    catch (const std::exception& exception)
    {
        CopySaveError(output.errorMessage, sizeof(output.errorMessage), exception.what());
    }
    catch (...)
    {
        strlcpy(output.errorMessage, "Unexpected error while inspecting the save", sizeof(output.errorMessage));
    }
    return output;
}

extern "C" CemuInstalledContentInfo CemuBridge_GetInstalledContentInfo(uint64_t titleId)
{
    CemuInstalledContentInfo output{};
    TitleIdParser parser(TitleIdParser::MakeBaseTitleId(titleId));
    if (!parser.CanHaveSeparateUpdateTitleId())
        return output;

    TitleInfo updateInfo;
    if (CafeTitleList::GetFirstByTitleId(parser.GetSeparateUpdateTitleId(), updateInfo))
    {
        output.hasUpdate = true;
        output.updateVersion = updateInfo.GetAppTitleVersion();
    }

    const uint64_t dlcTitleId = TitleIdParser::MakeBaseTitleId(titleId) | 0xC00000000ULL;
    TitleInfo dlcInfo;
    if (CafeTitleList::GetFirstByTitleId(dlcTitleId, dlcInfo))
    {
        output.hasDLC = true;
        output.dlcVersion = dlcInfo.GetAppTitleVersion();
    }
    return output;
}

extern "C" CemuGameFileInfo CemuBridge_InspectGameFile(const char* utf8Path)
{
    CemuGameFileInfo output{};
    if (!utf8Path || utf8Path[0] == '\0')
    {
        strlcpy(output.errorMessage, "The game-file path is empty", sizeof(output.errorMessage));
        return output;
    }

    try
    {
        const fs::path path(utf8Path);
        TitleInfo titleInfo(path);
        if (!titleInfo.IsValid())
        {
            switch (titleInfo.GetInvalidReason())
            {
            case TitleInfo::InvalidReason::NO_DISC_KEY:
                strlcpy(output.errorMessage, "The disc key is missing from keys.txt", sizeof(output.errorMessage));
                break;
            case TitleInfo::InvalidReason::BAD_PATH_OR_INACCESSIBLE:
                strlcpy(output.errorMessage, "The selected game file is no longer accessible", sizeof(output.errorMessage));
                break;
            default:
                strlcpy(output.errorMessage, "The selected file is not a valid or readable Wii U archive", sizeof(output.errorMessage));
                break;
            }
            return output;
        }

        const auto format = titleInfo.GetFormat();
        if (format != TitleInfo::TitleDataFormat::WIIU_ARCHIVE &&
            format != TitleInfo::TitleDataFormat::WUD)
        {
            strlcpy(output.errorMessage, "Only WUA, WUD and WUX game files are supported here", sizeof(output.errorMessage));
            return output;
        }

        output.success = true;
        output.titleId = titleInfo.GetAppTitleId();
        output.titleVersion = titleInfo.GetAppTitleVersion();
        output.region = static_cast<uint32_t>(titleInfo.GetMetaRegion());
        const std::string titleName = titleInfo.GetMetaTitleName();
        strlcpy(output.titleName, titleName.c_str(), sizeof(output.titleName));
        strlcpy(output.format,
                format == TitleInfo::TitleDataFormat::WIIU_ARCHIVE ? "WUA" : "WUD/WUX",
                sizeof(output.format));
    }
    catch (const std::exception& exception)
    {
        strlcpy(output.errorMessage, exception.what(), sizeof(output.errorMessage));
    }
    catch (...)
    {
        strlcpy(output.errorMessage, "Unexpected error while inspecting the game file", sizeof(output.errorMessage));
    }
    return output;
}

extern "C" bool CemuBridge_ExtractGameFileIcon(const char* utf8Path, const char* utf8DestinationPath)
{
    if (!utf8Path || !utf8DestinationPath || utf8Path[0] == '\0' || utf8DestinationPath[0] == '\0')
        return false;

    try
    {
        TitleInfo titleInfo{fs::path(utf8Path)};
        if (!titleInfo.IsValid())
            return false;

        const std::string mountPath = TitleInfo::GetUniqueTempMountingPath();
        if (!titleInfo.Mount(mountPath, "", FSC_PRIORITY_BASE))
            return false;

        auto icon = fsc_extractFile(fmt::format("{}meta/iconTex.tga", mountPath).c_str());
        titleInfo.Unmount(mountPath);
        if (!icon || icon->size() < 18)
            return false;

        FileStream* output = FileStream::createFile2(fs::path(utf8DestinationPath));
        if (!output)
            return false;
        const auto iconSize = static_cast<sint32>(icon->size());
        const bool written = output->writeData(icon->data(), iconSize) == iconSize;
        delete output;
        return written;
    }
    catch (...)
    {
        return false;
    }
}

extern "C" bool CemuBridge_ExtractGameBootMedia(const char* utf8Path, const char* utf8DestinationPath, bool sound)
{
    if (!utf8Path || !utf8DestinationPath || !utf8Path[0] || !utf8DestinationPath[0]) return false;
    try
    {
        TitleInfo titleInfo{fs::path(utf8Path)};
        if (!titleInfo.IsValid()) return false;
        const std::string mountPath = TitleInfo::GetUniqueTempMountingPath();
        if (!titleInfo.Mount(mountPath, "", FSC_PRIORITY_BASE)) return false;
        auto unmount = stdx::scope_exit{[&] { titleInfo.Unmount(mountPath); }};
        sint32 status = FSC_STATUS_UNDEFINED;
        auto* file = fsc_open(fmt::format("{}meta/{}", mountPath, sound ? "bootSound.btsnd" : "bootTvTex.tga").c_str(),
                              FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::READ_PERMISSION, &status);
        if (!file) return false;
        auto close = stdx::scope_exit{[&] { fsc_close(file); }};
        const uint32 size = fsc_getFileSize(file);
        if (size < (sound ? 12u : 18u) || size > (sound ? 8u : 16u) * 1024 * 1024) return false;
        std::vector<uint8> bytes(size);
        if (fsc_readFile(file, bytes.data(), size) != size) return false;
        std::unique_ptr<FileStream> output(FileStream::createFile2(fs::path(utf8DestinationPath)));
        return output && output->writeData(bytes.data(), static_cast<sint32>(size)) == static_cast<sint32>(size);
    }
    catch (...) { return false; }
}

extern "C" CemuSaveOperationResult CemuBridge_ExportSave(uint64_t titleId, const char* titleName, const char* outputPath)
{
    CemuSaveOperationResult output{};
    if (s_titleEverLaunched.load(std::memory_order_acquire))
    {
        strlcpy(output.errorMessage, "Save export is unavailable while a title is active", sizeof(output.errorMessage));
        return output;
    }
    if (!titleName || !outputPath || outputPath[0] == '\0')
    {
        strlcpy(output.errorMessage, "The export request is invalid", sizeof(output.errorMessage));
        return output;
    }

    SaveOperationScope operation;
    if (!operation)
    {
        strlcpy(output.errorMessage, "Another save operation is already running", sizeof(output.errorMessage));
        return output;
    }

    try
    {
        const auto result = SaveManagerIOS::ExportSave(titleId, titleName, std::filesystem::path(outputPath));
        output.success = result.success;
        CopySavePath(output.outputPath, sizeof(output.outputPath), result.outputPath);
        CopySavePath(output.rollbackPath, sizeof(output.rollbackPath), result.rollbackPath);
        CopySaveError(output.errorMessage, sizeof(output.errorMessage), result.error);
    }
    catch (const std::exception& exception)
    {
        CopySaveError(output.errorMessage, sizeof(output.errorMessage), exception.what());
    }
    catch (...)
    {
        strlcpy(output.errorMessage, "Unexpected error while exporting the save", sizeof(output.errorMessage));
    }
    return output;
}

extern "C" CemuSaveOperationResult CemuBridge_RestoreSave(uint64_t titleId, const char* archivePath)
{
    CemuSaveOperationResult output{};
    if (s_titleEverLaunched.load(std::memory_order_acquire))
    {
        strlcpy(output.errorMessage, "Save restore is unavailable while a title is active", sizeof(output.errorMessage));
        return output;
    }
    if (!archivePath || archivePath[0] == '\0')
    {
        strlcpy(output.errorMessage, "The restore request is invalid", sizeof(output.errorMessage));
        return output;
    }

    SaveOperationScope operation;
    if (!operation)
    {
        strlcpy(output.errorMessage, "Another save operation is already running", sizeof(output.errorMessage));
        return output;
    }

    try
    {
        const auto result = SaveManagerIOS::RestoreSave(titleId, std::filesystem::path(archivePath));
        output.success = result.success;
        CopySavePath(output.outputPath, sizeof(output.outputPath), result.outputPath);
        CopySavePath(output.rollbackPath, sizeof(output.rollbackPath), result.rollbackPath);
        CopySaveError(output.errorMessage, sizeof(output.errorMessage), result.error);
    }
    catch (const std::exception& exception)
    {
        CopySaveError(output.errorMessage, sizeof(output.errorMessage), exception.what());
    }
    catch (...)
    {
        strlcpy(output.errorMessage, "Unexpected error while restoring the save", sizeof(output.errorMessage));
    }
    return output;
}

extern "C" CemuSaveOperationResult CemuBridge_RestoreRollback(uint64_t titleId)
{
    CemuSaveOperationResult output{};
    if (s_titleEverLaunched.load(std::memory_order_acquire))
    {
        strlcpy(output.errorMessage, "Close and reopen 77U before restoring a rollback", sizeof(output.errorMessage));
        return output;
    }
    SaveOperationScope operation;
    if (!operation)
    {
        strlcpy(output.errorMessage, "Another save operation is already running", sizeof(output.errorMessage));
        return output;
    }
    try
    {
        const auto result = SaveManagerIOS::RestoreRollback(titleId);
        output.success = result.success;
        CopySavePath(output.rollbackPath, sizeof(output.rollbackPath), result.rollbackPath);
        CopySaveError(output.errorMessage, sizeof(output.errorMessage), result.error);
    }
    catch (const std::exception& exception)
    {
        CopySaveError(output.errorMessage, sizeof(output.errorMessage), exception.what());
    }
    catch (...)
    {
        strlcpy(output.errorMessage, "Unexpected error while restoring the rollback", sizeof(output.errorMessage));
    }
    return output;
}

extern "C" CemuSaveOperationResult CemuBridge_DeleteSave(uint64_t titleId)
{
    CemuSaveOperationResult output{};
    if (s_titleEverLaunched.load(std::memory_order_acquire))
    {
        strlcpy(output.errorMessage, "Save deletion is unavailable while a title is active", sizeof(output.errorMessage));
        return output;
    }

    SaveOperationScope operation;
    if (!operation)
    {
        strlcpy(output.errorMessage, "Another save operation is already running", sizeof(output.errorMessage));
        return output;
    }

    try
    {
        const auto result = SaveManagerIOS::DeleteSave(titleId);
        output.success = result.success;
        CopySaveError(output.errorMessage, sizeof(output.errorMessage), result.error);
    }
    catch (const std::exception& exception)
    {
        CopySaveError(output.errorMessage, sizeof(output.errorMessage), exception.what());
    }
    catch (...)
    {
        strlcpy(output.errorMessage, "Unexpected error while deleting the save", sizeof(output.errorMessage));
    }
    return output;
}

extern "C" CemuTitleContentInstallResult CemuBridge_InstallTitleContent(const char* sourcePath)
{
    CemuTitleContentInstallResult output{};
    if (s_titleEverLaunched.load(std::memory_order_acquire))
    {
        strlcpy(output.errorMessage, "Update installation is unavailable while a title is active", sizeof(output.errorMessage));
        return output;
    }
    if (!sourcePath || sourcePath[0] == '\0')
    {
        strlcpy(output.errorMessage, "The selected update or DLC folder is invalid", sizeof(output.errorMessage));
        return output;
    }

    SaveOperationScope operation;
    if (!operation)
    {
        strlcpy(output.errorMessage, "Another file operation is already running", sizeof(output.errorMessage));
        return output;
    }

    fs::path targetPath;
    fs::path stagingPath;
    fs::path backupPath;
    bool movedExistingTarget = false;
    try
    {
        const fs::path source(sourcePath);
        TitleInfo titleInfo(source);
        if (!titleInfo.IsValid() || titleInfo.GetFormat() != TitleInfo::TitleDataFormat::HOST_FS)
            throw std::runtime_error("Select an extracted Wii U title folder containing valid code, content, and meta data");

        const auto titleType = titleInfo.GetTitleType();
        if (titleType == TitleIdParser::TITLE_TYPE::BASE_TITLE_UPDATE)
            output.contentType = 1;
        else if (titleType == TitleIdParser::TITLE_TYPE::AOC)
            output.contentType = 2;
        else
            throw std::runtime_error("The selected folder is not a Wii U update or DLC title");

        output.titleId = titleInfo.GetAppTitleId();
        output.titleVersion = titleInfo.GetAppTitleVersion();

		TitleId baseTitleId = 0;
		if (!CafeTitleList::FindBaseTitleId(output.titleId, baseTitleId))
			throw std::runtime_error("Could not determine which base game this content belongs to");
		TitleInfo baseTitleInfo;
		if (!CafeTitleList::GetFirstByTitleId(baseTitleId, baseTitleInfo))
			throw std::runtime_error(fmt::format(
				"Install the matching base game ({:016x}) before installing this {}",
				baseTitleId, output.contentType == 1 ? "update" : "DLC"));

        targetPath = ActiveSettings::GetMlcPath(titleInfo.GetInstallPath());
        stagingPath = targetPath;
        stagingPath += ".installing";
        backupPath = targetPath;
        backupPath += ".backup";

        std::error_code ec;
        fs::remove_all(stagingPath, ec);
        if (ec)
            throw std::runtime_error(fmt::format("Could not clear the temporary install folder: {}", ec.message()));
        fs::create_directories(stagingPath, ec);
        if (ec)
            throw std::runtime_error(fmt::format("Could not prepare the temporary install directory: {}", ec.message()));

        for (const char* directory : { "code", "content", "meta" })
        {
            const fs::path sourceDirectory = source / directory;
            if (!fs::is_directory(sourceDirectory, ec) || ec)
                throw std::runtime_error(fmt::format("The selected title is missing its {} folder", directory));
            fs::copy(sourceDirectory, stagingPath / directory,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (ec)
                throw std::runtime_error(fmt::format("Could not copy the {} folder: {}", directory, ec.message()));
        }

        TitleInfo stagedTitle(stagingPath);
        if (!stagedTitle.IsValid() || stagedTitle.GetAppTitleId() != output.titleId ||
            stagedTitle.GetAppTitleVersion() != output.titleVersion)
            throw std::runtime_error("The copied update failed validation before installation");

        fs::remove_all(backupPath, ec);
        if (ec)
            throw std::runtime_error(fmt::format("Could not clear the previous installation backup: {}", ec.message()));
        if (fs::exists(targetPath, ec))
        {
            fs::rename(targetPath, backupPath, ec);
            if (ec)
                throw std::runtime_error(fmt::format("Could not preserve the previous installation: {}", ec.message()));
            movedExistingTarget = true;
        }
        else if (ec)
            throw std::runtime_error(fmt::format("Could not inspect the destination: {}", ec.message()));

        fs::rename(stagingPath, targetPath, ec);
        if (ec)
            throw std::runtime_error(fmt::format("Could not activate the installed title: {}", ec.message()));

        CafeTitleList::AddTitleFromPath(targetPath);
        if (movedExistingTarget)
        {
            fs::remove_all(backupPath, ec);
            if (ec)
                cemuLog_log(LogType::Force, "iPadOS: update installed but old backup cleanup failed: {}", ec.message());
        }

        output.success = true;
        CopySavePath(output.installPath, sizeof(output.installPath), targetPath);
        cemuLog_log(LogType::Force, "iPadOS: installed {} {:016x} v{} to {}",
                    output.contentType == 1 ? "update" : "DLC", output.titleId,
                    output.titleVersion, _pathToUtf8(targetPath));
    }
    catch (const std::exception& exception)
    {
        std::error_code ec;
        fs::remove_all(stagingPath, ec);
        if (movedExistingTarget && !fs::exists(targetPath, ec) && fs::exists(backupPath, ec))
            fs::rename(backupPath, targetPath, ec);
        CopySaveError(output.errorMessage, sizeof(output.errorMessage), exception.what());
    }
    catch (...)
    {
        std::error_code ec;
        fs::remove_all(stagingPath, ec);
        if (movedExistingTarget && !fs::exists(targetPath, ec) && fs::exists(backupPath, ec))
            fs::rename(backupPath, targetPath, ec);
        strlcpy(output.errorMessage, "Unexpected error while installing the update", sizeof(output.errorMessage));
    }
    return output;
}

namespace
{
std::mutex s_graphicPackBridgeMutex;
std::unordered_map<std::string, std::shared_ptr<GraphicPack2>> s_graphicPackByIdentifier;
std::unordered_map<uint64_t, std::vector<std::shared_ptr<GraphicPack2>>> s_graphicPacksByTitle;

void InvalidateGraphicPackLookups()
{
    s_graphicPackByIdentifier.clear();
    s_graphicPacksByTitle.clear();
}

void EnsureGraphicPackIdentifierIndex()
{
    const auto& packs = GraphicPack2::GetGraphicPacks();
    if (s_graphicPackByIdentifier.size() == packs.size())
        return;
    s_graphicPackByIdentifier.clear();
    s_graphicPackByIdentifier.reserve(packs.size());
    for (const auto& pack : packs)
        s_graphicPackByIdentifier.try_emplace(pack->GetNormalizedPathString(), pack);
}

std::shared_ptr<GraphicPack2> FindGraphicPack(std::string_view identifier)
{
    EnsureGraphicPackIdentifierIndex();
    const auto it = s_graphicPackByIdentifier.find(std::string(identifier));
    return it == s_graphicPackByIdentifier.end() ? std::shared_ptr<GraphicPack2>{} : it->second;
}

const std::vector<std::shared_ptr<GraphicPack2>>& GraphicPacksForTitle(uint64_t titleId)
{
    if (const auto cached = s_graphicPacksByTitle.find(titleId); cached != s_graphicPacksByTitle.end())
        return cached->second;

    std::vector<std::shared_ptr<GraphicPack2>> result;
    result.reserve(32);
    for (const auto& pack : GraphicPack2::GetGraphicPacks())
    {
        if (pack->IsUniversal() || pack->ContainsTitleId(titleId))
            result.emplace_back(pack);
    }
    std::ranges::sort(result, [](const auto& lhs, const auto& rhs) {
        return lhs->GetVirtualPath() < rhs->GetVirtualPath();
    });
    return s_graphicPacksByTitle.try_emplace(titleId, std::move(result)).first->second;
}

std::string GraphicPackCategory(std::string path)
{
    boost::algorithm::to_lower(path);
    if (path.find("/graphics/") != std::string::npos || path.ends_with("/graphics"))
        return "Graphics";
    if (path.find("/enhancements/") != std::string::npos || path.ends_with("/enhancements"))
        return "Enhancements";
    if (path.find("/mods/") != std::string::npos || path.ends_with("/mods"))
        return "Mods";
    if (path.find("/workarounds/") != std::string::npos || path.ends_with("/workarounds"))
        return "Workarounds";
    return "Other";
}

void SaveGraphicPackState()
{
    auto& entries = GetConfigHandle().data().graphic_pack_entries;
    entries.clear();
    for (const auto& pack : GraphicPack2::GetGraphicPacks())
    {
        const fs::path identifier = _utf8ToPath(pack->GetNormalizedPathString());
        if (pack->IsEnabled())
        {
            auto& savedPresets = entries[identifier];
            for (const auto& preset : pack->GetActivePresets())
                savedPresets.try_emplace(preset->category, preset->name);
        }
        else if (pack->IsDefaultEnabled())
        {
            entries[identifier].try_emplace("_disabled", "false");
        }
    }
    GetConfigHandle().Save();
}

void LoadAllGraphicPacks()
{
    std::lock_guard<std::mutex> lock(s_graphicPackBridgeMutex);
    // A community download can replace a bundled pack with the same virtual
    // identity but a different filesystem identifier. Carry the user's choices
    // across that replacement, then persist the new identifiers.
    struct Selection
    {
        bool enabled;
        std::vector<std::pair<std::string, std::string>> presets;
    };
    std::unordered_map<std::string, Selection> selections;
    for (const auto& pack : GraphicPack2::GetGraphicPacks())
    {
        Selection selection{pack->IsEnabled(), {}};
        for (const auto& preset : pack->GetActivePresets())
            selection.presets.emplace_back(preset->category, preset->name);
        selections.try_emplace(pack->GetVirtualPath(), std::move(selection));
    }
    InvalidateGraphicPackLookups();
    GraphicPack2::ClearGraphicPacks();
    std::vector<fs::path> additionalPaths;
    NSString* resourcePath = [[NSBundle mainBundle] resourcePath];
    if (resourcePath)
        additionalPaths.emplace_back(fs::path([resourcePath UTF8String]) / "graphicPacks");
    GraphicPack2::LoadAll(additionalPaths);
    for (const auto& pack : GraphicPack2::GetGraphicPacks())
    {
        const auto previous = selections.find(pack->GetVirtualPath());
        if (previous == selections.end()) continue;
        pack->SetEnabled(previous->second.enabled);
        for (const auto& [category, name] : previous->second.presets)
            pack->SetActivePreset(category, name, false);
        pack->UpdatePresetVisibility();
        pack->ValidatePresetSelections();
    }
    if (!selections.empty()) SaveGraphicPackState();
    EnsureGraphicPackIdentifierIndex();
}

void DisableSplatoonPlazaFPSPatchForFirstRun(uint64_t titleId)
{
    constexpr uint64_t kSplatoonEU = 0x0005000010176A00ULL;
    constexpr uint64_t kSplatoonUS = 0x0005000010176900ULL;
    constexpr uint64_t kSplatoonJP = 0x0005000010162B00ULL;
    // PrepareForegroundTitle may expose the mounted update title here. Always
    // compare the base title ID or Splatoon's update ID bypasses this guard.
    const uint64_t baseTitleId = TitleIdParser::MakeBaseTitleId(titleId);
    if (baseTitleId != kSplatoonEU && baseTitleId != kSplatoonUS && baseTitleId != kSplatoonJP)
        return;

    const fs::path userSavePath = ActiveSettings::GetMlcPath("usr/save/{:08x}/{:08x}/user",
        static_cast<uint32_t>(baseTitleId >> 32), static_cast<uint32_t>(baseTitleId));
    std::error_code saveError;
    const fs::file_status userSaveStatus = fs::status(userSavePath, saveError);
    const bool userSaveExists = !saveError && fs::is_directory(userSaveStatus);
    // A missing directory is the expected first-run state. libc++ reports
    // ENOENT through the error_code overload of status(), so do not treat that
    // as an I/O failure and accidentally leave first-run-only patches enabled.
    if (saveError && saveError != std::errc::no_such_file_or_directory)
    {
        cemuLog_log(LogType::Force,
            "iPadOS: could not inspect Splatoon first-run save path: {}",
            saveError.message());
        return;
    }
    if (userSaveExists)
        return;

    bool changed = false;
    std::lock_guard<std::mutex> lock(s_graphicPackBridgeMutex);
    for (const auto& pack : GraphicPack2::GetGraphicPacks())
    {
        if (!pack || !pack->IsEnabled())
            continue;

        const std::string& path = pack->GetVirtualPath();
        if (path != "Splatoon/Mods/60FPS In Plaza")
            continue;

        // Splatoon's first-run character flow intentionally changes the game's
        // frame-rate mode. The plaza patch NOPs that call globally and can leave
        // the title waiting forever after character confirmation. Keep the pack
        // installed, but defer it until Splatoon has created its first save.
        pack->SetEnabled(false);
        changed = true;
        cemuLog_log(LogType::Force,
            "iPadOS: disabled {} for Splatoon first-run character setup", path);
    }
    if (changed)
        SaveGraphicPackState();
}

static int ZeldaBOTWInitialFPSLimit()
{
    char model[128]{};
    size_t size = sizeof(model);
    if (sysctlbyname("hw.machine", model, &size, nullptr, 0) != 0)
        return 60;
    unsigned major = 0, minor = 0;
    char trailing;
    if (sscanf(model, "iPhone%u,%u%c", &major, &minor, &trailing) == 2)
        return (major > 0 && (major < 15 || (major == 15 && minor <= 3))) ? 30 : 60;
    if (sscanf(model, "iPad%u,%u%c", &major, &minor, &trailing) == 2)
    {
        // Newer model numbers interleave A-series iPads with M-series models.
        const bool olderAChip = (major > 0 && major <= 12) ||
            (major == 13 && (minor == 1 || minor == 2 || minor == 18 || minor == 19)) ||
            (major == 14 && (minor == 1 || minor == 2)) ||
            (major == 15 && (minor == 7 || minor == 8));
        return olderAChip ? 30 : 60;
    }
    return 60;
}

void EnsureZeldaBOTWDefaultFPSPackEnabled(uint64_t titleId)
{
    constexpr uint64_t kBotWEU = 0x00050000101C9500ULL;
    constexpr uint64_t kBotWUS = 0x00050000101C9400ULL;
    constexpr uint64_t kBotWJP = 0x00050000101C9300ULL;
    const uint64_t baseTitleId = TitleIdParser::MakeBaseTitleId(titleId);
    if (baseTitleId != kBotWEU && baseTitleId != kBotWUS && baseTitleId != kBotWJP)
        return;

    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    const bool defaultsApplied = [defaults boolForKey:@"CemuIOS.BotW60FPSDefaultApplied_v3"];
    const bool repairStartupFence = WindowSystem::IsPhone() && defaultsApplied &&
        ![defaults boolForKey:@"CemuIOS.BotWStartupFenceRepair_v1"];
    if (defaultsApplied && !repairStartupFence)
        return;

    bool changed = false;
    std::lock_guard<std::mutex> lock(s_graphicPackBridgeMutex);
    for (const auto& pack : GraphicPack2::GetGraphicPacks())
    {
        if (!pack)
            continue;

        const std::string& path = pack->GetVirtualPath();
        if (path.find("FPS++") == std::string::npos &&
            path.find("BreathOfTheWild/Mods/FPS++") == std::string::npos)
            continue;

        if (repairStartupFence)
        {
            // The old automatic phone default skipped fences during initialization.
            if (pack->GetActivePreset("Fence Type") == "Fence Skip")
            {
                pack->SetActivePreset("Fence Type", "Accurate Fence", false);
                pack->UpdatePresetVisibility();
                pack->ValidatePresetSelections();
                changed = true;
                cemuLog_log(LogType::Force, "BOTW: repaired automatic Fence Skip startup setting to Accurate Fence");
            }
            [defaults setBool:YES forKey:@"CemuIOS.BotWStartupFenceRepair_v1"];
            continue;
        }
        const int initialFPS = ZeldaBOTWInitialFPSLimit();
        pack->SetEnabled(true);
        pack->SetActivePreset("Mode", "Advanced Settings", false);
        pack->SetActivePreset("Framerate Limit", initialFPS == 30 ? "30FPS (ideal for 240/120/60Hz displays)" : "60FPS (ideal for 240/120/60Hz displays)", false);
        pack->SetActivePreset("Cutscene FPS Limit", "Automatically Limit In Few Cutscenes (Recommended)", false);
        pack->SetActivePreset("FPS Limit", initialFPS == 30 ? "30FPS Limit" : "60FPS Limit (Default)", false);
        pack->SetActivePreset("Static Mode", "Disabled (Default, dynamically adjust game speed)", false);
        pack->SetActivePreset("Fence Type", "Accurate Fence", false);
        pack->SetActivePreset("Frame Average", "8 Frames Averaged (Default)", false);
        pack->UpdatePresetVisibility();
        pack->ValidatePresetSelections();
        changed = true;
        cemuLog_log(LogType::Force, "iPadOS: initialized FPS++ at {} FPS ({}) with Accurate Fence for Zelda BOTW",
            initialFPS, path);
    }

    if (changed)
    {
        SaveGraphicPackState();
        [defaults setBool:YES forKey:@"CemuIOS.BotW60FPSDefaultApplied_v3"];
        if (WindowSystem::IsPhone()) [defaults setBool:YES forKey:@"CemuIOS.BotWStartupFenceRepair_v1"];
    }
}

void EnsureZeldaBOTWIPhoneThermalSettings(uint64_t titleId)
{
    if (!WindowSystem::IsPhone())
        return;

    constexpr uint64_t kBotWEU = 0x00050000101C9500ULL;
    constexpr uint64_t kBotWUS = 0x00050000101C9400ULL;
    constexpr uint64_t kBotWJP = 0x00050000101C9300ULL;
    const uint64_t baseTitleId = TitleIdParser::MakeBaseTitleId(titleId);
    if (baseTitleId != kBotWEU && baseTitleId != kBotWUS && baseTitleId != kBotWJP)
        return;

    bool changed = false;
    std::lock_guard<std::mutex> lock(s_graphicPackBridgeMutex);
    for (const auto& pack : GraphicPack2::GetGraphicPacks())
    {
        if (!pack)
            continue;

        const std::string& path = pack->GetVirtualPath();
        if (path.find("Breath of the Wild/Graphics") != std::string::npos ||
            path.find("BreathOfTheWild/Graphics") != std::string::npos)
        {
            std::string res = pack->GetActivePreset("Resolution");
            if (!res.empty() && res != "1280x720 (HD, Default)")
            {
                pack->SetActivePreset("Resolution", "1280x720 (HD, Default)", false);
                changed = true;
                cemuLog_log(LogType::Force, "iPhone Thermal Guard: Capped BOTW Graphics resolution from '{}' to '1280x720 (HD, Default)' to prevent thermal throttling", res);
            }
            std::string shadows = pack->GetActivePreset("Shadows");
            std::string targetShadow = "Medium (100%, Default)";
            for (const auto& pr : pack->GetPresets())
            {
                if (pr && pr->category == "Shadows" && pr->name.find("Medium") != std::string::npos)
                {
                    targetShadow = pr->name;
                    break;
                }
            }
            if (shadows.empty())
            {
                pack->SetActivePreset("Shadows", targetShadow, false);
                changed = true;
                cemuLog_log(LogType::Force, "iPhone Settings: Defaulted BOTW Shadows to '{}'", targetShadow);
            }
            std::string aa = pack->GetActivePreset("Anti-Aliasing");
            std::string targetAA;
            for (const auto& pr : pack->GetPresets())
            {
                if (pr && pr->category == "Anti-Aliasing" && pr->name.find("FXAA") != std::string::npos)
                {
                    targetAA = pr->name;
                    break;
                }
            }
            if (!targetAA.empty() && aa.empty())
            {
                pack->SetActivePreset("Anti-Aliasing", targetAA, false);
                changed = true;
                cemuLog_log(LogType::Force, "iPhone Settings: Defaulted BOTW Anti-Aliasing to '{}'", targetAA);
            }
            std::string shadowDist = pack->GetActivePreset("Shadow Draw Distance");
            std::string targetDist;
            for (const auto& pr : pack->GetPresets())
            {
                if (pr && pr->category == "Shadow Draw Distance" && (pr->name.find("Medium") != std::string::npos || pr->name.find("Normal") != std::string::npos))
                {
                    targetDist = pr->name;
                    break;
                }
            }
            if (targetDist.empty())
            {
                for (const auto& pr : pack->GetPresets())
                {
                    if (pr && pr->category == "Shadow Draw Distance" && pr->name.find("Default") != std::string::npos)
                    {
                        targetDist = pr->name;
                        break;
                    }
                }
            }
            if (!targetDist.empty() && shadowDist.empty())
            {
                pack->SetActivePreset("Shadow Draw Distance", targetDist, false);
                changed = true;
                cemuLog_log(LogType::Force, "iPhone Settings: Defaulted BOTW Shadow Draw Distance to '{}'", targetDist);
            }
            if (changed)
            {
                pack->UpdatePresetVisibility();
                pack->ValidatePresetSelections();
            }
        }
        else if (path.find("FPS++") != std::string::npos ||
                 path.find("BreathOfTheWild/Mods/FPS++") != std::string::npos)
        {
            std::string fence = pack->GetActivePreset("Fence Type");
            std::string targetFence = "Performance Fence (Default)";
            for (const auto& pr : pack->GetPresets())
            {
                if (pr && pr->category == "Fence Type" && pr->name.find("Performance Fence") != std::string::npos)
                {
                    targetFence = pr->name;
                    break;
                }
            }
            if (fence.empty())
            {
                pack->SetActivePreset("Fence Type", targetFence, false);
                changed = true;
                cemuLog_log(LogType::Force, "iPhone Settings: Defaulted BOTW Fence Type to '{}'", targetFence);
                pack->UpdatePresetVisibility();
                pack->ValidatePresetSelections();
            }
        }
    }

    if (changed)
    {
        SaveGraphicPackState();
    }
}

CemuGraphicPackOperationResult GraphicPackFailure(std::string_view message)
{
    CemuGraphicPackOperationResult result{};
    strlcpy(result.message, std::string(message).c_str(), sizeof(result.message));
    return result;
}

bool IsSafeGraphicPackArchivePath(std::string name, fs::path& relativePath)
{
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.empty() || name.front() == '/')
        return false;
    relativePath = _utf8ToPath(name).lexically_normal();
    if (relativePath.empty() || relativePath.is_absolute() || relativePath.has_root_name())
        return false;
    for (const auto& component : relativePath)
    {
        if (component == "..")
            return false;
    }
    return true;
}
}

extern "C" int CemuBridge_GetRunningGameFrameGenerationTarget(void)
{
	const CPUMode cpuMode = ActiveSettings::GetCPUMode();
	const bool jitOn = cpuMode == CPUMode::SinglecoreRecompiler || cpuMode == CPUMode::DualcoreRecompiler || cpuMode == CPUMode::MulticoreRecompiler;
	if (!jitOn)
		return 0;
	return CemuBridge_GetFrameGenerationTargetForTitle(CafeSystem::GetForegroundTitleId());
}

extern "C" int CemuBridge_GetFrameGenerationTargetForTitle(uint64_t titleId)
{
	NSInteger target = GetConfig().frame_generation_target.GetValue();
	if (titleId)
	{
		NSString* key = [NSString stringWithFormat:@"CemuIOS.FrameGenerationTarget.%llu", (unsigned long long)titleId];
		NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
		NSNumber* stored = [defaults objectForKey:key];
		if ([stored isKindOfClass:NSNumber.class])
		{
			const NSInteger value = stored.integerValue;
			if (value == 0 || value == 30 || value == 60 || value == 120)
				target = value;
		}
	}
	return target == 30 || target == 60 || target == 120 ? static_cast<int>(target) : 0;
}

extern "C" bool CemuBridge_GetRunningGameFrameGenerationFast(void)
{
	const uint64_t titleId = CafeSystem::GetForegroundTitleId();
	NSString* key = [NSString stringWithFormat:@"CemuIOS.FrameGenerationFast.%llu", (unsigned long long)titleId];
	return [[NSUserDefaults standardUserDefaults] boolForKey:key];
}

extern "C" CemuFrameGenerationRates CemuBridge_GetFrameGenerationRates(void)
{
    CemuFrameGenerationRates rates{};
    CemuMetal_GetFrameGenerationRates(&rates.sourceFPS, &rates.displayedFPS);
    return rates;
}

extern "C" size_t CemuBridge_GetGraphicPackCount(uint64_t titleId)
{
    std::lock_guard<std::mutex> lock(s_graphicPackBridgeMutex);
    return GraphicPacksForTitle(titleId).size();
}

extern "C" bool CemuBridge_GetGraphicPackInfo(uint64_t titleId, size_t index,
                                                CemuGraphicPackInfo* output)
{
    std::lock_guard<std::mutex> lock(s_graphicPackBridgeMutex);
    if (!output)
        return false;
    *output = {};
    const auto& packs = GraphicPacksForTitle(titleId);
    if (index >= packs.size())
        return false;

    const auto& pack = packs[index];
    output->enabled = pack->IsEnabled();
    output->compatible = pack->IsCompatibleWith(RendererAPI::Metal, GfxVendor::Apple);
    output->universal = pack->IsUniversal();
    output->defaultEnabled = pack->IsDefaultEnabled();
    output->hasPresets = std::ranges::any_of(pack->GetPresets(), [](const auto& preset) {
        return preset->visible;
    });
    strlcpy(output->identifier, pack->GetNormalizedPathString().c_str(), sizeof(output->identifier));
    strlcpy(output->virtualPath, pack->GetVirtualPath().c_str(), sizeof(output->virtualPath));
    strlcpy(output->name, pack->GetName().c_str(), sizeof(output->name));
    strlcpy(output->description, pack->GetDescription().c_str(), sizeof(output->description));
    const std::string category = GraphicPackCategory(pack->GetVirtualPath());
    strlcpy(output->category, category.c_str(), sizeof(output->category));
    return true;
}

extern "C" size_t CemuBridge_GetGraphicPackPresetCount(const char* identifier)
{
    std::lock_guard<std::mutex> lock(s_graphicPackBridgeMutex);
    if (!identifier)
        return 0;
    const auto pack = FindGraphicPack(identifier);
    if (!pack)
        return 0;
    return std::ranges::count_if(pack->GetPresets(), [](const auto& preset) {
        return preset->visible;
    });
}

extern "C" bool CemuBridge_GetGraphicPackPresetInfo(const char* identifier, size_t index,
                                                      CemuGraphicPackPresetInfo* output)
{
    std::lock_guard<std::mutex> lock(s_graphicPackBridgeMutex);
    if (!identifier || !output)
        return false;
    *output = {};
    const auto pack = FindGraphicPack(identifier);
    if (!pack)
        return false;

    size_t visibleIndex = 0;
    for (const auto& preset : pack->GetPresets())
    {
        if (!preset->visible)
            continue;
        if (visibleIndex++ != index)
            continue;
        output->active = preset->active;
        output->visible = preset->visible;
        output->isDefault = preset->is_default;
        strlcpy(output->category, preset->category.c_str(), sizeof(output->category));
        strlcpy(output->name, preset->name.c_str(), sizeof(output->name));
        return true;
    }
    return false;
}

extern "C" CemuGraphicPackOperationResult CemuBridge_SetGraphicPackEnabled(const char* identifier,
                                                                            bool enabled)
{
    if (!PrepareGraphicPackOperation())
        return GraphicPackFailure("Graphic packs cannot be changed while a game is active");
    SaveOperationScope operation;
    if (!operation) return GraphicPackFailure("Wait for the current library operation to finish");
    std::lock_guard<std::mutex> lock(s_graphicPackBridgeMutex);
    if (!identifier)
        return GraphicPackFailure("The graphic pack identifier is invalid");
    const auto pack = FindGraphicPack(identifier);
    if (!pack)
        return GraphicPackFailure("The selected graphic pack is no longer available");
    if (!pack->IsCompatibleWith(RendererAPI::Metal, GfxVendor::Apple))
        return GraphicPackFailure("This graphic pack targets a different renderer or GPU");

    CemuGraphicPackOperationResult result{};
    result.requiresRestart = pack->RequiresRestart(true, false);
    pack->SetEnabled(enabled);

    SaveGraphicPackState();
    result.success = true;
    result.discoveredPackCount = static_cast<uint32_t>(GraphicPack2::GetGraphicPacks().size());
    strlcpy(result.message, enabled ? "Graphic pack enabled" : "Graphic pack disabled", sizeof(result.message));
    return result;
}

extern "C" CemuGraphicPackOperationResult CemuBridge_SetGraphicPackPreset(const char* identifier,
                                                                           const char* category,
                                                                           const char* presetName)
{
    if (!PrepareGraphicPackOperation())
        return GraphicPackFailure("Graphic pack presets cannot be changed while a game is active");
    SaveOperationScope operation;
    if (!operation) return GraphicPackFailure("Wait for the current library operation to finish");
    std::lock_guard<std::mutex> lock(s_graphicPackBridgeMutex);
    if (!identifier || !category || !presetName)
        return GraphicPackFailure("The graphic pack preset request is invalid");
    const auto pack = FindGraphicPack(identifier);
    if (!pack)
        return GraphicPackFailure("The selected graphic pack is no longer available");
    if (!pack->SetActivePreset(category, presetName))
        return GraphicPackFailure("The selected preset is not available");

    // SaveGraphicPackState drops presets of disabled packs. Selecting a preset
    // is an explicit request to use the pack, so enable it.
    pack->SetEnabled(true);

    SaveGraphicPackState();
    cemuLog_log(LogType::Force, "iPadOS: graphic pack preset applied: {} [{} = {}]",
                pack->GetNormalizedPathString(), category, presetName);
    CemuGraphicPackOperationResult result{};
    result.success = true;
    result.requiresRestart = pack->RequiresRestart(false, true);
    result.discoveredPackCount = static_cast<uint32_t>(GraphicPack2::GetGraphicPacks().size());
    strlcpy(result.message, "Graphic pack preset saved", sizeof(result.message));
    return result;
}

extern "C" CemuGraphicPackOperationResult CemuBridge_InstallGraphicPackArchive(const char* archivePath,
                                                                                const char* releaseName)
{
    if (!PrepareGraphicPackOperation())
        return GraphicPackFailure("Community packs cannot be installed while a game is active");
    if (!archivePath || archivePath[0] == '\0')
        return GraphicPackFailure("The downloaded graphic-pack archive is invalid");

    SaveOperationScope operation;
    if (!operation)
        return GraphicPackFailure("Another library operation is already running");

    const fs::path target = ActiveSettings::GetUserDataPath("graphicPacks/downloadedGraphicPacks");
    fs::path staging = target;
    staging += ".installing";
    fs::path backup = target;
    backup += ".backup";
    bool movedExistingTarget = false;

    try
    {
        int zipError = 0;
        std::unique_ptr<zip_t, decltype(&zip_close)> archive(zip_open(archivePath, ZIP_RDONLY, &zipError), &zip_close);
        if (!archive)
            throw std::runtime_error(fmt::format("Could not open the downloaded ZIP (error {})", zipError));

        std::error_code ec;
        fs::remove_all(staging, ec);
        ec.clear();
        fs::create_directories(staging, ec);
        if (ec)
            throw std::runtime_error(fmt::format("Could not prepare graphic-pack staging: {}", ec.message()));

        constexpr uint64_t maxEntryBytes = 128ULL * 1024ULL * 1024ULL;
        constexpr uint64_t maxArchiveBytes = 1024ULL * 1024ULL * 1024ULL;
        uint64_t extractedBytes = 0;
        bool foundRules = false;
        const zip_int64_t entryCount = zip_get_num_entries(archive.get(), 0);
        if (entryCount <= 0)
            throw std::runtime_error("The downloaded graphic-pack ZIP is empty");

        // libdispatch worker threads have a much smaller stack than the main
        // thread on iOS. Keeping the extraction buffer on the stack could
        // overflow it (especially in interpreter/no-JIT launches).
        std::vector<char> buffer(128 * 1024);
        cemuLog_log(LogType::Force, "iPadOS: extracting {} graphic-pack archive entries", entryCount);
        for (zip_uint64_t index = 0; index < static_cast<zip_uint64_t>(entryCount); ++index)
        {
            zip_stat_t stat{};
            if (zip_stat_index(archive.get(), index, 0, &stat) != 0 || !stat.name)
                throw std::runtime_error("The graphic-pack ZIP contains an unreadable entry");

            fs::path relativePath;
            if (!IsSafeGraphicPackArchivePath(stat.name, relativePath))
                throw std::runtime_error("The graphic-pack ZIP contains an unsafe path");
            if (stat.size > maxEntryBytes || extractedBytes > maxArchiveBytes - stat.size)
                throw std::runtime_error("The graphic-pack ZIP exceeds the safe extraction limit");
            extractedBytes += stat.size;

            const std::string entryName(stat.name);
            const fs::path destination = staging / relativePath;
            if (entryName.ends_with('/'))
            {
                fs::create_directories(destination, ec);
                if (ec)
                    throw std::runtime_error(fmt::format("Could not create a graphic-pack folder: {}", ec.message()));
                continue;
            }

            fs::create_directories(destination.parent_path(), ec);
            if (ec)
                throw std::runtime_error(fmt::format("Could not prepare a graphic-pack folder: {}", ec.message()));
            std::unique_ptr<zip_file_t, decltype(&zip_fclose)> input(zip_fopen_index(archive.get(), index, 0), &zip_fclose);
            if (!input)
                throw std::runtime_error("Could not read a file from the graphic-pack ZIP");
            std::ofstream output(destination, std::ios::binary | std::ios::trunc);
            if (!output)
                throw std::runtime_error("Could not create an extracted graphic-pack file");

            uint64_t remaining = stat.size;
            while (remaining > 0)
            {
                const zip_uint64_t request = std::min<uint64_t>(remaining, buffer.size());
                const zip_int64_t bytesRead = zip_fread(input.get(), buffer.data(), request);
                if (bytesRead <= 0)
                    throw std::runtime_error("The graphic-pack ZIP ended unexpectedly");
                output.write(buffer.data(), bytesRead);
                if (!output)
                    throw std::runtime_error("Could not write an extracted graphic-pack file");
                remaining -= static_cast<uint64_t>(bytesRead);
            }
            if (boost::iequals(destination.filename().string(), "rules.txt"))
                foundRules = true;
        }

        if (!foundRules)
            throw std::runtime_error("The archive does not contain any Cemu rules.txt graphic packs");
        if (releaseName && releaseName[0] != '\0')
        {
            std::ofstream versionFile(staging / "version.txt", std::ios::binary | std::ios::trunc);
            versionFile << releaseName;
        }

        fs::remove_all(backup, ec);
        ec.clear();
        if (fs::exists(target, ec))
        {
            fs::rename(target, backup, ec);
            if (ec)
                throw std::runtime_error(fmt::format("Could not preserve existing community packs: {}", ec.message()));
            movedExistingTarget = true;
        }
        ec.clear();
        fs::rename(staging, target, ec);
        if (ec)
            throw std::runtime_error(fmt::format("Could not activate downloaded community packs: {}", ec.message()));

        cemuLog_log(LogType::Force, "iPadOS: graphic-pack extraction complete ({} MiB), scanning packs",
                    extractedBytes / (1024 * 1024));
        LoadAllGraphicPacks();
        if (movedExistingTarget)
        {
            fs::remove_all(backup, ec);
            if (ec)
                cemuLog_log(LogType::Force, "iPadOS: old graphic-pack cleanup failed: {}", ec.message());
        }

        CemuGraphicPackOperationResult result{};
        result.success = true;
        result.discoveredPackCount = static_cast<uint32_t>(GraphicPack2::GetGraphicPacks().size());
        cemuLog_log(LogType::Force, "iPadOS: community graphic-pack install complete ({} packs)",
                    result.discoveredPackCount);
        strlcpy(result.message, "Community graphic packs installed", sizeof(result.message));
        return result;
    }
    catch (const std::exception& exception)
    {
        std::error_code ec;
        fs::remove_all(staging, ec);
        if (movedExistingTarget && !fs::exists(target, ec) && fs::exists(backup, ec))
            fs::rename(backup, target, ec);
        return GraphicPackFailure(exception.what());
    }
}

namespace
{
struct PretendoDumpSources
{
    fs::path otpPath;
    fs::path seepromPath;
    fs::path mlcPath;
};

PretendoDumpSources FindPretendoDumpSources(const fs::path& selectedPath)
{
    PretendoDumpSources sources{};
    std::error_code ec;

    std::vector<fs::path> candidateRoots;
    candidateRoots.push_back(selectedPath);
    if (selectedPath.has_parent_path())
        candidateRoots.push_back(selectedPath.parent_path());
    if (selectedPath.has_parent_path() && selectedPath.parent_path().has_parent_path())
        candidateRoots.push_back(selectedPath.parent_path().parent_path());
    candidateRoots.push_back(selectedPath / "Online Files");
    candidateRoots.push_back(selectedPath / "dumpling" / "Online Files");

    if (fs::is_directory(selectedPath, ec) && !ec)
    {
        for (fs::directory_iterator it(selectedPath, ec), end; it != end; it.increment(ec))
        {
            if (it->is_directory(ec) && !ec)
            {
                candidateRoots.push_back(it->path());
                candidateRoots.push_back(it->path() / "Online Files");
            }
            ec.clear();
        }
    }

    for (const auto& root : candidateRoots)
    {
        if (root.empty()) continue;
        const auto p = root / "otp.bin";
        if (fs::is_regular_file(p, ec) && !ec)
        {
            sources.otpPath = p;
            break;
        }
        ec.clear();
    }

    for (const auto& root : candidateRoots)
    {
        if (root.empty()) continue;
        const auto p = root / "seeprom.bin";
        if (fs::is_regular_file(p, ec) && !ec)
        {
            sources.seepromPath = p;
            break;
        }
        ec.clear();
    }

    for (const auto& root : candidateRoots)
    {
        if (root.empty()) continue;
        if (root.filename() == "mlc01" && fs::is_directory(root, ec) && !ec)
        {
            sources.mlcPath = root;
            break;
        }
        ec.clear();
        const auto p = root / "mlc01";
        if (fs::is_directory(p, ec) && !ec)
        {
            sources.mlcPath = p;
            break;
        }
        ec.clear();
    }

    if (sources.otpPath.empty() || sources.seepromPath.empty() || sources.mlcPath.empty())
    {
        std::string missing;
        if (sources.otpPath.empty()) missing += "otp.bin ";
        if (sources.seepromPath.empty()) missing += "seeprom.bin ";
        if (sources.mlcPath.empty()) missing += "mlc01 ";
        throw std::runtime_error(fmt::format("Select the Online Files folder containing otp.bin, seeprom.bin, and mlc01 (missing: {})", missing));
    }

    return sources;
}

void RequireRegularFile(const fs::path& path, uintmax_t expectedSize, std::string_view displayName)
{
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec)
        throw std::runtime_error(fmt::format("{} is missing from the selected dump", displayName));
    const auto size = fs::file_size(path, ec);
    if (ec || size != expectedSize)
        throw std::runtime_error(fmt::format("{} has an invalid size", displayName));
}

void CopyPretendoFile(const fs::path& source, const fs::path& destination, std::string_view displayName)
{
    std::error_code ec;
    if (!fs::is_regular_file(source, ec) || ec)
        throw std::runtime_error(fmt::format("The selected dump is missing {}", displayName));
    fs::create_directories(destination.parent_path(), ec);
    if (ec)
        throw std::runtime_error(fmt::format("Could not prepare the import path for {}: {}", displayName, ec.message()));
    fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
    if (ec)
        throw std::runtime_error(fmt::format("Could not import {}: {}", displayName, ec.message()));
}
}

extern "C" CemuPretendoImportResult CemuBridge_ImportPretendoDump(const char* sourcePath)
{
    CemuPretendoImportResult output{};
    if (s_titleEverLaunched.load(std::memory_order_acquire))
    {
        strlcpy(output.errorMessage, "Pretendo import is unavailable after a title has started; restart 77U first", sizeof(output.errorMessage));
        return output;
    }
    if (!sourcePath || sourcePath[0] == '\0')
    {
        strlcpy(output.errorMessage, "The selected Pretendo dump folder is invalid", sizeof(output.errorMessage));
        return output;
    }

    SaveOperationScope operation;
    if (!operation)
    {
        strlcpy(output.errorMessage, "Another file operation is already running", sizeof(output.errorMessage));
        return output;
    }

    try
    {
        const auto sources = FindPretendoDumpSources(fs::path(sourcePath));
        RequireRegularFile(sources.otpPath, 1024, "otp.bin");
        RequireRegularFile(sources.seepromPath, 512, "seeprom.bin");

        const fs::path sourceMlc = sources.mlcPath;
        const fs::path sourceCertificatePath = sourceMlc / "sys/title/0005001b/10054000/content";
        std::error_code ec;
        for (const auto* fileName : iosuCrypt_getCertificateKeys())
        {
            if (!fs::is_regular_file(sourceCertificatePath / fileName, ec) || ec)
                throw std::runtime_error("The dump is missing required Wii U online certificate keys");
            ec.clear();
        }
        for (const auto* fileName : iosuCrypt_getCertificateNames())
        {
            if (!fs::is_regular_file(sourceCertificatePath / fileName, ec) || ec)
                throw std::runtime_error("The dump is missing required Wii U online certificates");
            ec.clear();
        }

        const fs::path sourceAccountRoot = sourceMlc / "usr/save/system/act";
        if (!fs::is_directory(sourceAccountRoot, ec) || ec)
            throw std::runtime_error("The dump does not contain Wii U account data");

        struct ImportedAccount
        {
            uint32 persistentId;
            std::string name;
            fs::path sourceAccountDir;
            fs::path sourceAccountFile;
        };
        std::vector<ImportedAccount> importedAccounts;
        for (fs::directory_iterator it(sourceAccountRoot, ec), end; it != end; it.increment(ec))
        {
            if (ec)
                throw std::runtime_error(fmt::format("Could not read Wii U account data: {}", ec.message()));
            if (!it->is_directory(ec) || ec)
            {
                ec.clear();
                continue;
            }
            const std::string directoryName = it->path().filename().string();
            if (directoryName.size() != 8 || !std::all_of(directoryName.begin(), directoryName.end(), [](unsigned char value) {
                    return std::isxdigit(value) != 0;
                }))
                continue;

            const fs::path accountPath = it->path() / "account.dat";
            if (!fs::is_regular_file(accountPath, ec) || ec)
            {
                ec.clear();
                continue;
            }
            Account account(accountPath.wstring());
            uint32 persistentId = account.GetPersistentId();
            if (persistentId < Account::kMinPersistendId)
            {
                uint32 dirId = 0;
                auto [ptr, ecConv] = std::from_chars(directoryName.data(), directoryName.data() + directoryName.size(), dirId, 16);
                if (ecConv == std::errc{} && dirId >= Account::kMinPersistendId)
                    persistentId = dirId;
            }
            if (persistentId < Account::kMinPersistendId || !account.IsValidOnlineAccount())
                continue;
            const std::string name = account.GetAccountId().empty()
                ? boost::nowide::narrow(account.GetMiiName())
                : std::string(account.GetAccountId());
            importedAccounts.push_back({ persistentId, name, it->path(), accountPath });
        }
        if (ec)
            throw std::runtime_error(fmt::format("Could not finish reading Wii U account data: {}", ec.message()));
        if (importedAccounts.empty())
            throw std::runtime_error("No valid online Wii U account was found in the selected dump");

        const uint32 currentPersistentId = GetConfigHandle().data().account.m_persistent_id.GetValue();
        const auto selectedAccount = std::find_if(importedAccounts.begin(), importedAccounts.end(),
            [currentPersistentId](const auto& account) { return account.persistentId == currentPersistentId; });
        const ImportedAccount& accountToSelect = selectedAccount != importedAccounts.end()
            ? *selectedAccount : importedAccounts.front();

        const fs::path userDataPath = ActiveSettings::GetUserDataPath();
        const fs::path destinationMlc = ActiveSettings::GetMlcPath();
        fs::create_directories(userDataPath, ec);
        if (ec)
            throw std::runtime_error(fmt::format("Could not prepare the 77U user folder: {}", ec.message()));
        CopyPretendoFile(sources.otpPath, userDataPath / "otp.bin", "otp.bin");
        CopyPretendoFile(sources.seepromPath, userDataPath / "seeprom.bin", "seeprom.bin");

        const fs::path destinationCertificatePath =
            destinationMlc / "sys/title/0005001b/10054000/content";
        for (const auto* fileName : iosuCrypt_getCertificateKeys())
            CopyPretendoFile(sourceCertificatePath / fileName,
                destinationCertificatePath / fileName, _pathToUtf8(fs::path(fileName)));
        for (const auto* fileName : iosuCrypt_getCertificateNames())
            CopyPretendoFile(sourceCertificatePath / fileName,
                destinationCertificatePath / fileName, _pathToUtf8(fs::path(fileName)));

        // Optional system fonts
        const fs::path sourceFontPath = sourceMlc / "sys/title/0005001b/10056000/content";
        if (fs::is_directory(sourceFontPath, ec) && !ec)
        {
            const fs::path destinationFontPath = destinationMlc / "sys/title/0005001b/10056000/content";
            for (fs::directory_iterator fontIt(sourceFontPath, ec), fontEnd; fontIt != fontEnd; fontIt.increment(ec))
            {
                if (ec || !fontIt->is_regular_file(ec))
                {
                    ec.clear();
                    continue;
                }
                const auto fileName = fontIt->path().filename();
                CopyPretendoFile(fontIt->path(), destinationFontPath / fileName, fileName.string());
            }
        }
        ec.clear();

        // Optional ec_account_info.exi
        const fs::path sourceNimPath = sourceMlc / "usr/save/system/nim/ec/ec_account_info.exi";
        if (fs::is_regular_file(sourceNimPath, ec) && !ec)
        {
            const fs::path destinationNimPath = destinationMlc / "usr/save/system/nim/ec/ec_account_info.exi";
            CopyPretendoFile(sourceNimPath, destinationNimPath, "ec_account_info.exi");
        }
        ec.clear();

        const fs::path destinationAccountRoot = destinationMlc / "usr/save/system/act";
        for (const auto& account : importedAccounts)
        {
            const auto accountDirectory = fmt::format("{:08x}", account.persistentId);
            const auto destDir = destinationAccountRoot / accountDirectory;
            fs::create_directories(destDir, ec);
            for (fs::directory_iterator actIt(account.sourceAccountDir, ec), actEnd; actIt != actEnd; actIt.increment(ec))
            {
                if (ec || !actIt->is_regular_file(ec))
                {
                    ec.clear();
                    continue;
                }
                const auto fileName = actIt->path().filename();
                CopyPretendoFile(actIt->path(), destDir / fileName, fmt::format("{}/{}", accountDirectory, fileName.string()));
            }
        }

        Account::RefreshAccounts();
        const Account& importedAccount = Account::GetAccount(accountToSelect.persistentId);
        if (importedAccount.GetPersistentId() != accountToSelect.persistentId || !importedAccount.ValidateOnlineFiles().IsValid())
            throw std::runtime_error("The imported Wii U online files failed Cemu validation");

        auto& config = GetConfigHandle().data();
        config.account.m_persistent_id.SetValue(accountToSelect.persistentId);
        config.SetAccountSelectedService(accountToSelect.persistentId, NetworkService::Pretendo);
        GetConfigHandle().Save();

        output.success = true;
        output.persistentId = accountToSelect.persistentId;
        output.importedAccountCount = static_cast<uint32_t>(importedAccounts.size());
        strlcpy(output.accountName, accountToSelect.name.c_str(), sizeof(output.accountName));
        cemuLog_log(LogType::Force, "iPadOS: imported Pretendo online files and selected account {} ({:08x})",
                    accountToSelect.name, accountToSelect.persistentId);
    }
    catch (const std::exception& exception)
    {
        strlcpy(output.errorMessage, exception.what(), sizeof(output.errorMessage));
    }
    catch (...)
    {
        strlcpy(output.errorMessage, "Unexpected error while importing the Pretendo dump", sizeof(output.errorMessage));
    }
    return output;
}

void CemuBridge_SetSecurityScopeStatus(const char* status)
{
    if (status) {
        strlcpy(s_securityScopeStatus, status, sizeof(s_securityScopeStatus));
        strlcpy(s_titleBootResult.securityScopeStatus, status, sizeof(s_titleBootResult.securityScopeStatus));
    }
}

void CemuBridge_AddGameScanPath(const char* utf8Path)
{
    if (!utf8Path || utf8Path[0] == '\0')
        return;
    fs::path p(utf8Path);
    std::error_code ec;
    if (fs::exists(p, ec)) {
        CafeTitleList::AddScanPath(p);
        os_log(s_log, "CemuBridge_AddGameScanPath: added %{public}s", utf8Path);
    }
}

void CemuBridge_RescanTitles(void)
{
    CafeTitleList::Refresh();
    while (CafeTitleList::IsScanning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    cemuLog_log(LogType::Force, "iPadOS: Title list refresh completed ({} titles known)", CafeTitleList::GetAllTitleIds().size());
}

CemuTitleBootInfo CemuBridge_LaunchTitle(const char* utf8Path)
{
    char entryMsg[256];
    snprintf(entryMsg, sizeof(entryMsg), "[JIT] CemuBridge_LaunchTitle entered (path=%s, pid=%d)", utf8Path ? utf8Path : "null", getpid());
    CemuBridge_LogJITMilestone(entryMsg);

    bool isColorSplash = (utf8Path && (strstr(utf8Path, "1f600b00") || strstr(utf8Path, "1f600a00") || strstr(utf8Path, "1f600900") ||
                                      strstr(utf8Path, "1F600B00") || strstr(utf8Path, "1F600A00") || strstr(utf8Path, "1F600900") ||
                                      strcasestr(utf8Path, "color splash") || strcasestr(utf8Path, "paper mario")));

    if (isColorSplash) {
        LaunchSettings::SetForceInterpreter(false);
        LaunchSettings::SetForceMultiCoreInterpreter(false);
        GetConfigHandle().data().cpu_mode.SetValue(CPUMode::SinglecoreRecompiler);
        CemuBridge_LogJITMilestone("[CPU MODE] Paper Mario: Color Splash detected -> enforcing SinglecoreRecompiler");
    } else if (s_cpuDiagnosticMode == 1) {
        // No-JIT runs the three Wii U cores on three interpreter threads; CemuIOS.NoJITSingleCore=true restores the old one-thread path.
        // SinglecoreInterpreter keeps PPCRecompiler_init off; ForceMultiCoreInterpreter alone makes OSSchedulerBegin start 3 cores.
        const bool noJitSingleCore = [[NSUserDefaults standardUserDefaults] boolForKey:@"CemuIOS.NoJITSingleCore"];
        LaunchSettings::SetForceInterpreter(noJitSingleCore);
        LaunchSettings::SetForceMultiCoreInterpreter(!noJitSingleCore);
        GetConfigHandle().data().cpu_mode.SetValue(CPUMode::SinglecoreInterpreter);
        CemuBridge_LogJITMilestone(noJitSingleCore ? "[CPU MODE] Singlecore interpreter selected (CemuBridge_LaunchTitle)"
                                                   : "[CPU MODE] Triplecore interpreter selected (CemuBridge_LaunchTitle)");
    } else if (s_cpuDiagnosticMode == 2) {
        LaunchSettings::SetForceInterpreter(false);
        LaunchSettings::SetForceMultiCoreInterpreter(false);
        GetConfigHandle().data().cpu_mode.SetValue(CPUMode::MulticoreRecompiler);
        CemuBridge_LogJITMilestone("[CPU MODE] MulticoreRecompiler (Triplecore JIT) selected (CemuBridge_LaunchTitle)");
    } else {
        LaunchSettings::SetForceInterpreter(false);
        LaunchSettings::SetForceMultiCoreInterpreter(false);
        GetConfigHandle().data().cpu_mode.SetValue(CPUMode::SinglecoreRecompiler);
        CemuBridge_LogJITMilestone("[CPU MODE] SinglecoreRecompiler (JIT) selected (CemuBridge_LaunchTitle)");
    }
    memset(&s_titleBootResult, 0, sizeof(s_titleBootResult));
    strlcpy(s_titleBootResult.securityScopeStatus, s_securityScopeStatus, sizeof(s_titleBootResult.securityScopeStatus));

    if (s_titleEverLaunched) {
        strlcpy(s_titleBootResult.errorMessage, "A title is already active. Please restart the app to load a different title.", sizeof(s_titleBootResult.errorMessage));
        os_log_error(s_log, "CemuBridge_LaunchTitle: %{public}s", s_titleBootResult.errorMessage);
        return s_titleBootResult;
    }

    TitleLaunchScope titleLaunch;
    if (!titleLaunch) {
        strlcpy(s_titleBootResult.errorMessage, "A library operation is still running. Wait for it to finish before launching.", sizeof(s_titleBootResult.errorMessage));
        os_log_error(s_log, "CemuBridge_LaunchTitle: %{public}s", s_titleBootResult.errorMessage);
        return s_titleBootResult;
    }

    if (!utf8Path || utf8Path[0] == '\0') {
        strlcpy(s_titleBootResult.errorMessage, "Invalid or empty title path", sizeof(s_titleBootResult.errorMessage));
        os_log_error(s_log, "CemuBridge_LaunchTitle: %{public}s", s_titleBootResult.errorMessage);
        return s_titleBootResult;
    }

    NSString* pathStr = [NSString stringWithUTF8String:utf8Path];
    strlcpy(s_titleBootResult.rpxPath, utf8Path, sizeof(s_titleBootResult.rpxPath));
    NSString* fileName = [pathStr lastPathComponent];
    strlcpy(s_titleBootResult.titleName, [fileName UTF8String], sizeof(s_titleBootResult.titleName));

    // Point the window system at the attached CAMetalLayer
    auto& winInfo = WindowSystem::GetWindowInfo();
    auto configureSurface = ^{
        winInfo.window_main.surface = (__bridge void*)s_metalLayer;
        winInfo.window_main.backend = WindowSystem::WindowHandleInfo::Backend::Cocoa;
        winInfo.phys_width = s_metalLayer ? (int32_t)s_metalLayer.drawableSize.width : 1920;
        winInfo.phys_height = s_metalLayer ? (int32_t)s_metalLayer.drawableSize.height : 1080;
        CGFloat scale = s_metalLayer ? (s_metalLayer.contentsScale > 0 ? s_metalLayer.contentsScale : 1.0) : 1.0;
        winInfo.width = (int32_t)(winInfo.phys_width / scale);
        winInfo.height = (int32_t)(winInfo.phys_height / scale);
        winInfo.dpi_scale = (double)scale;
    };
    if ([NSThread isMainThread]) configureSurface();
    else dispatch_sync(dispatch_get_main_queue(), configureSurface);

    fs::path launchFsPath(utf8Path);
    std::error_code ecDir;
    bool isDirectory = fs::is_directory(launchFsPath, ecDir);

    if (ecDir) {
        snprintf(s_titleBootResult.errorMessage, sizeof(s_titleBootResult.errorMessage),
                 "Filesystem error inspecting path '%s': %s (code %d)",
                 [fileName UTF8String], ecDir.message().c_str(), ecDir.value());
        os_log_error(s_log, "CemuBridge_LaunchTitle: %{public}s", s_titleBootResult.errorMessage);
        return s_titleBootResult;
    }

    if (isDirectory) {
        // DIRECTORY PATH: Must be a valid extracted Wii U HOST_FS title.
        // NEVER fall back to Standalone RPX!
        std::error_code ecCode, ecContent, ecMeta;
        bool hasCode = fs::exists(launchFsPath / "code", ecCode);
        bool hasContent = fs::exists(launchFsPath / "content", ecContent);
        bool hasMeta = fs::exists(launchFsPath / "meta", ecMeta);

        if (ecCode || ecContent || ecMeta) {
            std::error_code firstEc = ecCode ? ecCode : (ecContent ? ecContent : ecMeta);
            snprintf(s_titleBootResult.errorMessage, sizeof(s_titleBootResult.errorMessage),
                     "Filesystem access error inspecting title folders in '%s': %s (code %d)",
                     [fileName UTF8String], firstEc.message().c_str(), firstEc.value());
            os_log_error(s_log, "CemuBridge_LaunchTitle: %{public}s", s_titleBootResult.errorMessage);
            return s_titleBootResult;
        }

        if (!hasCode || !hasContent || !hasMeta) {
            snprintf(s_titleBootResult.errorMessage, sizeof(s_titleBootResult.errorMessage),
                     "Invalid extracted Wii U title structure: missing required folder(s)%s%s%s in '%s'",
                     !hasCode ? " [code]" : "",
                     !hasContent ? " [content]" : "",
                     !hasMeta ? " [meta]" : "",
                     [fileName UTF8String]);
            os_log_error(s_log, "CemuBridge_LaunchTitle: %{public}s", s_titleBootResult.errorMessage);
            return s_titleBootResult;
        }

        TitleInfo launchTitle{ launchFsPath };
        if (!launchTitle.IsValid() || launchTitle.GetFormat() != TitleInfo::TitleDataFormat::HOST_FS) {
            snprintf(s_titleBootResult.errorMessage, sizeof(s_titleBootResult.errorMessage),
                     "TitleInfo XML parse failure (missing or corrupted meta.xml/app.xml/cos.xml) for '%s'",
                     [fileName UTF8String]);
            os_log_error(s_log, "CemuBridge_LaunchTitle: %{public}s", s_titleBootResult.errorMessage);
            return s_titleBootResult;
        }

        strlcpy(s_titleBootResult.detectedFormat, "HOST_FS", sizeof(s_titleBootResult.detectedFormat));
        strlcpy(s_titleBootResult.launchMode, "ExtractedTitle (HOST_FS)", sizeof(s_titleBootResult.launchMode));
        s_titleBootResult.hostFsValid = true;
        s_titleBootResult.metaXmlParsed = (launchTitle.GetMetaInfo() != nullptr);
        s_titleBootResult.cosXmlParsed = (launchTitle.GetCosInfo() != nullptr);

        std::string metaName = launchTitle.GetMetaTitleName();
        if (!metaName.empty()) {
            strlcpy(s_titleBootResult.titleName, metaName.c_str(), sizeof(s_titleBootResult.titleName));
        }

        std::string argStr = launchTitle.GetArgStr();
        if (!argStr.empty()) {
            strlcpy(s_titleBootResult.cosArgStr, argStr.c_str(), sizeof(s_titleBootResult.cosArgStr));
        }

        uint64_t appTitleId = launchTitle.GetAppTitleId();
        s_titleBootResult.appTitleId = appTitleId;

        // Register the discovered HOST_FS title in CafeTitleList
        CafeTitleList::AddTitleFromPath(launchFsPath);

        TitleId baseTitleId;
        if (!CafeTitleList::FindBaseTitleId(appTitleId, baseTitleId)) {
            snprintf(s_titleBootResult.errorMessage, sizeof(s_titleBootResult.errorMessage),
                     "FindBaseTitleId failed for title ID 0x%016llX (title: '%s')", (unsigned long long)appTitleId, s_titleBootResult.titleName);
            os_log_error(s_log, "CemuBridge_LaunchTitle: %{public}s", s_titleBootResult.errorMessage);
            return s_titleBootResult;
        }

        s_titleBootResult.baseTitleId = baseTitleId;
        DisableSplatoonPlazaFPSPatchForFirstRun(baseTitleId);
        EnsureZeldaBOTWDefaultFPSPackEnabled(baseTitleId);
        EnsureZeldaBOTWIPhoneThermalSettings(baseTitleId);
        const auto prepareStatus = CafeSystem::PrepareForegroundTitle(baseTitleId);
        if (prepareStatus != CafeSystem::PREPARE_STATUS_CODE::SUCCESS) {
            snprintf(s_titleBootResult.errorMessage, sizeof(s_titleBootResult.errorMessage),
                     "PrepareForegroundTitle failed with code %d for '%s' (TitleId: 0x%016llX)", (int)prepareStatus, s_titleBootResult.titleName, (unsigned long long)baseTitleId);
            os_log_error(s_log, "CemuBridge_LaunchTitle: %{public}s", s_titleBootResult.errorMessage);
            return s_titleBootResult;
        }
    } else {
        // REGULAR FILE PATH: Check whether the file itself is accessible
        std::error_code ecFile;
        bool isReg = fs::is_regular_file(launchFsPath, ecFile);
        if (ecFile) {
            snprintf(s_titleBootResult.errorMessage, sizeof(s_titleBootResult.errorMessage),
                     "Filesystem access error checking file '%s': %s (code %d)",
                     [fileName UTF8String], ecFile.message().c_str(), ecFile.value());
            os_log_error(s_log, "CemuBridge_LaunchTitle: %{public}s", s_titleBootResult.errorMessage);
            return s_titleBootResult;
        }
        if (!isReg) {
            snprintf(s_titleBootResult.errorMessage, sizeof(s_titleBootResult.errorMessage),
                     "Path '%s' is neither a valid directory nor a regular file",
                     [fileName UTF8String]);
            os_log_error(s_log, "CemuBridge_LaunchTitle: %{public}s", s_titleBootResult.errorMessage);
            return s_titleBootResult;
        }

        // Check whether this is a complete title (an extracted tree or a
        // directly selected WUA/WUD/WUX archive) before considering the
        // standalone-RPX diagnostic path.
        TitleInfo launchTitle{ launchFsPath };
        const bool isHostTitle = launchTitle.IsValid() && launchTitle.GetFormat() == TitleInfo::TitleDataFormat::HOST_FS;
        const bool isWuaTitle = launchTitle.IsValid() && launchTitle.GetFormat() == TitleInfo::TitleDataFormat::WIIU_ARCHIVE;
        const bool isDiscTitle = launchTitle.IsValid() && launchTitle.GetFormat() == TitleInfo::TitleDataFormat::WUD;
        if (isHostTitle || isWuaTitle || isDiscTitle) {
            const char* detectedFormat = isHostTitle ? "HOST_FS" : (isWuaTitle ? "WUA" : "WUD/WUX");
            const char* launchMode = isHostTitle ? "ExtractedTitle (HOST_FS)" : (isWuaTitle ? "Wii U Archive (WUA)" : "Disc Image (WUD/WUX)");
            strlcpy(s_titleBootResult.detectedFormat, detectedFormat, sizeof(s_titleBootResult.detectedFormat));
            strlcpy(s_titleBootResult.launchMode, launchMode, sizeof(s_titleBootResult.launchMode));
            s_titleBootResult.hostFsValid = isHostTitle;
            s_titleBootResult.metaXmlParsed = (launchTitle.GetMetaInfo() != nullptr);
            s_titleBootResult.cosXmlParsed = (launchTitle.GetCosInfo() != nullptr);

            std::string metaName = launchTitle.GetMetaTitleName();
            if (!metaName.empty()) {
                strlcpy(s_titleBootResult.titleName, metaName.c_str(), sizeof(s_titleBootResult.titleName));
            }

            std::string argStr = launchTitle.GetArgStr();
            if (!argStr.empty()) {
                strlcpy(s_titleBootResult.cosArgStr, argStr.c_str(), sizeof(s_titleBootResult.cosArgStr));
            }

            char pathMsg[256];
            snprintf(pathMsg, sizeof(pathMsg), "[JIT] Title path validated: %s (isDirectory=%d, scope=%s)",
                     [fileName UTF8String], isDirectory, s_securityScopeStatus);
            CemuBridge_LogJITMilestone(pathMsg);

            uint64_t appTitleId = launchTitle.GetAppTitleId();
            s_titleBootResult.appTitleId = appTitleId;

            CafeTitleList::AddTitleFromPath(launchFsPath);

            TitleId baseTitleId;
            if (!CafeTitleList::FindBaseTitleId(appTitleId, baseTitleId)) {
                snprintf(s_titleBootResult.errorMessage, sizeof(s_titleBootResult.errorMessage),
                         "FindBaseTitleId failed for title ID 0x%016llX (title: '%s')", (unsigned long long)appTitleId, s_titleBootResult.titleName);
                os_log_error(s_log, "CemuBridge_LaunchTitle: %{public}s", s_titleBootResult.errorMessage);
                return s_titleBootResult;
            }

            s_titleBootResult.baseTitleId = baseTitleId;
            // This must happen before PrepareForegroundTitle: preparation loads
            // and activates graphic packs, so disabling a first-run timing patch
            // afterwards cannot undo code that was already patched.
            DisableSplatoonPlazaFPSPatchForFirstRun(baseTitleId);
            EnsureZeldaBOTWDefaultFPSPackEnabled(baseTitleId);
            EnsureZeldaBOTWIPhoneThermalSettings(baseTitleId);
            char prepMsg[256];
            snprintf(prepMsg, sizeof(prepMsg), "[JIT] Calling CafeSystem::PrepareForegroundTitle(0x%016llX)...", (unsigned long long)baseTitleId);
            CemuBridge_LogJITMilestone(prepMsg);

            const auto prepareStatus = CafeSystem::PrepareForegroundTitle(baseTitleId);
            if (prepareStatus != CafeSystem::PREPARE_STATUS_CODE::SUCCESS) {
                snprintf(s_titleBootResult.errorMessage, sizeof(s_titleBootResult.errorMessage),
                         "PrepareForegroundTitle failed with code %d for '%s' (TitleId: 0x%016llX)", (int)prepareStatus, s_titleBootResult.titleName, (unsigned long long)baseTitleId);
                os_log_error(s_log, "CemuBridge_LaunchTitle: %{public}s", s_titleBootResult.errorMessage);
                return s_titleBootResult;
            }
            CemuBridge_LogJITMilestone("[JIT] CafeSystem title preparation returned SUCCESS");
        } else {
            std::string extension = _pathToUtf8(launchFsPath.extension());
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (extension == ".wua" || extension == ".wud" || extension == ".wux" || extension == ".iso") {
                const char* reason = "The game archive is invalid or unreadable";
                if (launchTitle.GetInvalidReason() == TitleInfo::InvalidReason::NO_DISC_KEY)
                    reason = "The disc key is missing from keys.txt";
                else if (launchTitle.GetInvalidReason() == TitleInfo::InvalidReason::BAD_PATH_OR_INACCESSIBLE)
                    reason = "The game archive is no longer accessible; select it again";
                snprintf(s_titleBootResult.errorMessage, sizeof(s_titleBootResult.errorMessage), "%s (%s)",
                         reason, [fileName UTF8String]);
                os_log_error(s_log, "CemuBridge_LaunchTitle: %{public}s", s_titleBootResult.errorMessage);
                return s_titleBootResult;
            }

            // Standalone RPX
            char pathMsg[256];
            snprintf(pathMsg, sizeof(pathMsg), "[JIT] Title path validated: %s (isDirectory=%d, scope=%s)",
                     [fileName UTF8String], isDirectory, s_securityScopeStatus);
            CemuBridge_LogJITMilestone(pathMsg);

            strlcpy(s_titleBootResult.detectedFormat, "StandaloneRPX", sizeof(s_titleBootResult.detectedFormat));
            strlcpy(s_titleBootResult.launchMode, "StandaloneRPX", sizeof(s_titleBootResult.launchMode));
            s_titleBootResult.hostFsValid = false;

            char prepRpxMsg[256];
            snprintf(prepRpxMsg, sizeof(prepRpxMsg), "[JIT] Calling CafeSystem::PrepareForegroundTitleFromStandaloneRPX(%s)...", [fileName UTF8String]);
            CemuBridge_LogJITMilestone(prepRpxMsg);

            const auto prepareStatus = CafeSystem::PrepareForegroundTitleFromStandaloneRPX(launchFsPath);
            if (prepareStatus != CafeSystem::PREPARE_STATUS_CODE::SUCCESS) {
                snprintf(s_titleBootResult.errorMessage, sizeof(s_titleBootResult.errorMessage),
                         "PrepareForegroundTitleFromStandaloneRPX failed with code %d for '%s'", (int)prepareStatus, [fileName UTF8String]);
                os_log_error(s_log, "CemuBridge_LaunchTitle: %{public}s", s_titleBootResult.errorMessage);
                return s_titleBootResult;
            }
            CemuBridge_LogJITMilestone("[JIT] CafeSystem title preparation returned SUCCESS");
        }
    }

    s_titleBootResult.titlePrepared = true;
    s_titleBootResult.titleId = CafeSystem::GetForegroundTitleId();
    s_runningTitleId.store(s_titleBootResult.titleId, std::memory_order_release);
        s_titleEverLaunched = true;
    titleLaunch.Commit();

    auto createRenderer = ^{
        if (!g_renderer) {
            g_renderer = std::make_unique<MetalRenderer>();
            auto metal_renderer = MetalRenderer::GetInstance();
            if (metal_renderer) {
                Vector2i layerSize(winInfo.phys_width, winInfo.phys_height);
                metal_renderer->InitializeLayer(layerSize, true);
            }
        }

        auto metal_renderer = MetalRenderer::GetInstance();
        if (metal_renderer && s_drcMetalLayer && !metal_renderer->IsPadWindowActive()) {
            metal_renderer->InitializeLayer({kDRCDrawableWidth, kDRCDrawableHeight}, false);
        }
        CemuBridge_RestoreDRCPreviewPublishCallback();
    };
    if ([NSThread isMainThread]) createRenderer();
    else dispatch_sync(dispatch_get_main_queue(), createRenderer);

    os_log(s_log, "CemuBridge_LaunchTitle: Successfully prepared '%{public}s' (TitleId: 0x%016llX, Mode: %{public}s); launching foreground title...",
           s_titleBootResult.titleName, (unsigned long long)s_titleBootResult.titleId, s_titleBootResult.launchMode);

    CafeSystem::LaunchForegroundTitle();

    s_titleBootResult.titleLoaded = true;
    s_titleBootResult.launchRequested = true;

    return s_titleBootResult;
}

void CemuBridge_StopTitle(void)
{
    @autoreleasepool {
        os_log(s_log, "CemuBridge_StopTitle: Stopping active title...");
        CemuBridge_LogJITMilestone("[TITLE] CemuBridge_StopTitle called");

        if (CafeSystem::IsTitleRunning()) {
            CafeSystem::ShutdownTitle();
        }

        auto destroyRenderer = ^{
            @autoreleasepool {
                if (g_renderer) {
                    g_renderer.reset();
                }
            }
        };
        if ([NSThread isMainThread]) destroyRenderer();
        else dispatch_sync(dispatch_get_main_queue(), destroyRenderer);

        memset(&s_titleBootResult, 0, sizeof(s_titleBootResult));
        s_titleEverLaunched.store(false, std::memory_order_release);
        s_titleWasObservedRunning.store(false, std::memory_order_release);
        s_titleSaveLifecycleState.store(0, std::memory_order_release);
        s_runningTitleId.store(0, std::memory_order_release);
        activeThreadCount = 0;

        CemuBridge_LogJITMilestone("[TITLE] CemuBridge_StopTitle completed; ready for new title launch.");
    }
}

CemuTitleBootInfo CemuBridge_GetTitleBootInfo(void)
{
    strlcpy(s_titleBootResult.securityScopeStatus, s_securityScopeStatus, sizeof(s_titleBootResult.securityScopeStatus));
    if (s_titleBootResult.launchRequested || s_titleBootResult.titlePrepared) {
        s_titleBootResult.titleLaunched = CafeSystem::IsTitleRunning();
        s_titleBootResult.schedulerRunning = coreinit::OSIsSchedulerActive();
        s_titleBootResult.activeThreadsCount = (uint32_t)activeThreadCount;

        if (CafeSystem::IsTitleRunning()) {
            uint64_t currentId = CafeSystem::GetForegroundTitleId();
            if (currentId != 0 && currentId != s_titleBootResult.titleId) {
                s_titleBootResult.titleId = currentId;
                s_runningTitleId.store(currentId, std::memory_order_release);
                                s_titleBootResult.baseTitleId = currentId;
                std::string currentName = CafeSystem::GetForegroundTitleName();
                if (!currentName.empty()) {
                    strlcpy(s_titleBootResult.titleName, currentName.c_str(), sizeof(s_titleBootResult.titleName));
                }
            }
        }

        OSThread_t* defaultThread1 = coreinit::OSGetDefaultThread(1);
        s_titleBootResult.mainThreadCreated = (defaultThread1 != nullptr && defaultThread1->state != OSThread_t::THREAD_STATE::STATE_NONE);

        // Default heaps exist once __preinit_user has run
        s_titleBootResult.wutPreinitCalled = (coreinit::MEMGetBaseHeapHandle(0) != nullptr || coreinit::MEMGetBaseHeapHandle(1) != nullptr);

        // Sample live PPC interpreter state
        PPCInterpreter_t* cpu = PPCInterpreter_getCurrentInstance();
        if (cpu && cpu->instructionPointer != 0) {
            s_titleBootResult.currentPPC_PC = cpu->instructionPointer;
            s_titleBootResult.currentPPC_LR = cpu->spr.LR;
            s_titleBootResult.ppcExecutionEntered = true;
            s_titleBootResult.firstInstructionExecuted = true;
        } else if (coreinit::g_lastSavedGuestPC != 0) {
            s_titleBootResult.currentPPC_PC = coreinit::g_lastSavedGuestPC;
            s_titleBootResult.currentPPC_LR = coreinit::g_lastSavedLR;
            s_titleBootResult.ppcExecutionEntered = true;
            s_titleBootResult.firstInstructionExecuted = true;
        }

        // Name the HLE function at the sampled PC
        if (s_titleBootResult.currentPPC_PC != 0) {
            RPLStoredSymbol* sym = rplSymbolStorage_getByAddress(s_titleBootResult.currentPPC_PC);
            if (sym && sym->symbolName) {
                snprintf(s_titleBootResult.lastHLEFunction, sizeof(s_titleBootResult.lastHLEFunction), "%s.%s",
                         (const char*)sym->libName, (const char*)sym->symbolName);
            } else {
                RPLStoredSymbol* closestSym = rplSymbolStorage_getByClosestAddress(s_titleBootResult.currentPPC_PC);
                if (closestSym && closestSym->symbolName) {
                    snprintf(s_titleBootResult.lastHLEFunction, sizeof(s_titleBootResult.lastHLEFunction), "%s.%s+0x%X",
                             (const char*)closestSym->libName, (const char*)closestSym->symbolName, (uint32_t)(s_titleBootResult.currentPPC_PC - closestSym->address));
                } else {
                    snprintf(s_titleBootResult.lastHLEFunction, sizeof(s_titleBootResult.lastHLEFunction), "0x%08X", s_titleBootResult.currentPPC_PC);
                }
            }
        }

        // Walk the active thread list under the reader lock
        srwlock_activeThreadList.LockRead();
        sint32 count = activeThreadCount;
        for (sint32 i = 0; i < count && i < 256; ++i) {
            if (activeThread[i] == 0) continue;
            OSThread_t* thr = (OSThread_t*)memory_getPointerFromVirtualOffset(activeThread[i]);
            if (!thr || !thr->IsValidMagic()) continue;

            uint32_t thrPC = thr->context.srr0;
            uint32_t thrLR = _swapEndianU32(thr->context.lr);
            uint32_t entry = (uint32_t)thr->entrypoint.GetMPTR();

            const char* namePtr = thr->threadName.GetPtr();
            const char* stateStr = "Unknown";
            switch (thr->state) {
                case OSThread_t::THREAD_STATE::STATE_NONE: stateStr = "None"; break;
                case OSThread_t::THREAD_STATE::STATE_READY: stateStr = "Ready"; break;
                case OSThread_t::THREAD_STATE::STATE_RUNNING: stateStr = "Running"; break;
                case OSThread_t::THREAD_STATE::STATE_WAITING: stateStr = "Waiting"; break;
                case OSThread_t::THREAD_STATE::STATE_MORIBUND: stateStr = "Moribund"; break;
            }
            RPLStoredSymbol* pcSym = rplSymbolStorage_getByAddress(thrPC);
            cemuLog_log(LogType::Force, "[Thread {:2d}] '{:24s}' ({:7s}) PC=0x{:08x} ({}.{}) LR=0x{:08x}",
                i, namePtr ? namePtr : "unnamed", stateStr, thrPC,
                pcSym ? (const char*)pcSym->libName : "?", pcSym ? (const char*)pcSym->symbolName : "?",
                thrLR);

            // Attributed sampled thread
            if (thrPC == s_titleBootResult.currentPPC_PC || (thr->state == OSThread_t::THREAD_STATE::STATE_RUNNING)) {
                const char* namePtr = thr->threadName.GetPtr();
                if (namePtr) strlcpy(s_titleBootResult.sampledThreadName, namePtr, sizeof(s_titleBootResult.sampledThreadName));
                s_titleBootResult.sampledThreadEntrypoint = entry;
                s_titleBootResult.sampledThreadPC = thrPC;
                s_titleBootResult.sampledThreadLR = thrLR;
                s_titleBootResult.sampledThreadIsEntrypoint = (thrPC == entry);
                switch (thr->state) {
                    case OSThread_t::THREAD_STATE::STATE_NONE: strlcpy(s_titleBootResult.sampledThreadState, "None", sizeof(s_titleBootResult.sampledThreadState)); break;
                    case OSThread_t::THREAD_STATE::STATE_READY: strlcpy(s_titleBootResult.sampledThreadState, "Ready", sizeof(s_titleBootResult.sampledThreadState)); break;
                    case OSThread_t::THREAD_STATE::STATE_RUNNING: strlcpy(s_titleBootResult.sampledThreadState, "Running", sizeof(s_titleBootResult.sampledThreadState)); break;
                    case OSThread_t::THREAD_STATE::STATE_WAITING: strlcpy(s_titleBootResult.sampledThreadState, "Waiting", sizeof(s_titleBootResult.sampledThreadState)); break;
                    case OSThread_t::THREAD_STATE::STATE_MORIBUND: strlcpy(s_titleBootResult.sampledThreadState, "Moribund", sizeof(s_titleBootResult.sampledThreadState)); break;
                    default: strlcpy(s_titleBootResult.sampledThreadState, "Unknown", sizeof(s_titleBootResult.sampledThreadState)); break;
                }
            }

            // Main application thread (type == TYPE_APP)
            if (thr->type == OSThread_t::THREAD_TYPE::TYPE_APP && (s_titleBootResult.mainThreadPC == 0 || i == 0)) {
                s_titleBootResult.mainThreadPC = thrPC;
                s_titleBootResult.mainThreadLR = thrLR;
                switch (thr->state) {
                    case OSThread_t::THREAD_STATE::STATE_NONE: strlcpy(s_titleBootResult.mainThreadState, "None", sizeof(s_titleBootResult.mainThreadState)); break;
                    case OSThread_t::THREAD_STATE::STATE_READY: strlcpy(s_titleBootResult.mainThreadState, "Ready", sizeof(s_titleBootResult.mainThreadState)); break;
                    case OSThread_t::THREAD_STATE::STATE_RUNNING: strlcpy(s_titleBootResult.mainThreadState, "Running", sizeof(s_titleBootResult.mainThreadState)); break;
                    case OSThread_t::THREAD_STATE::STATE_WAITING: strlcpy(s_titleBootResult.mainThreadState, "Waiting", sizeof(s_titleBootResult.mainThreadState)); break;
                    case OSThread_t::THREAD_STATE::STATE_MORIBUND: strlcpy(s_titleBootResult.mainThreadState, "Moribund", sizeof(s_titleBootResult.mainThreadState)); break;
                    default: strlcpy(s_titleBootResult.mainThreadState, "Unknown", sizeof(s_titleBootResult.mainThreadState)); break;
                }
                if (thr->waitingForMutex) {
                    strlcpy(s_titleBootResult.mainThreadWaitReason, "OSMutex", sizeof(s_titleBootResult.mainThreadWaitReason));
                } else if (thr->waitingForFastMutex) {
                    strlcpy(s_titleBootResult.mainThreadWaitReason, "OSFastMutex", sizeof(s_titleBootResult.mainThreadWaitReason));
                } else if (thr->waitAlarm) {
                    strlcpy(s_titleBootResult.mainThreadWaitReason, "OSAlarm (Timer)", sizeof(s_titleBootResult.mainThreadWaitReason));
                } else if (!thr->currentWaitQueue.IsNull()) {
                    strlcpy(s_titleBootResult.mainThreadWaitReason, "WaitQueue / Event / VSync", sizeof(s_titleBootResult.mainThreadWaitReason));
                } else {
                    strlcpy(s_titleBootResult.mainThreadWaitReason, "None / Active", sizeof(s_titleBootResult.mainThreadWaitReason));
                }
            }
        }
        srwlock_activeThreadList.UnlockRead();

        // Track whether the main thread PC is advancing
        s_titleBootResult.mainThreadSampleCount++;
        if (s_titleBootResult.prevMainThreadPC != 0) {
            bool changed = (s_titleBootResult.mainThreadPC != s_titleBootResult.prevMainThreadPC);
            s_titleBootResult.mainThreadPCChanged = changed;
            if (changed) {
                s_titleBootResult.mainThreadPCChangeCount++;
            }
        }
        s_titleBootResult.prevMainThreadPC = s_titleBootResult.mainThreadPC;

        // GameController and VPAD state
        auto vpad = InputManager::instance().get_vpad_controller(0);
        s_titleBootResult.vpadConfigured = (vpad != nullptr);
        @autoreleasepool {
            NSArray<GCController*>* controllers = [GCController controllers];
            if (controllers.count > 1) {
                s_titleBootResult.physicalControllerConnected = true;
                std::string summary = fmt::format("{} controllers (P1: {}", controllers.count, [(controllers[0].vendorName ?: @"Game Controller") UTF8String]);
                for (NSUInteger i = 1; i < controllers.count && i < 4; ++i) {
                    summary += fmt::format(", P{}: {}", i + 1, [(controllers[i].vendorName ?: @"Game Controller") UTF8String]);
                }
                summary += ")";
                strlcpy(s_titleBootResult.physicalControllerName, summary.c_str(), sizeof(s_titleBootResult.physicalControllerName));
            } else if (controllers.count == 1) {
                s_titleBootResult.physicalControllerConnected = true;
                NSString* name = controllers[0].vendorName ?: @"Game Controller";
                strlcpy(s_titleBootResult.physicalControllerName, [name UTF8String], sizeof(s_titleBootResult.physicalControllerName));
            } else if ([GCController current] != nil) {
                s_titleBootResult.physicalControllerConnected = true;
                NSString* name = [GCController current].vendorName ?: @"Game Controller";
                strlcpy(s_titleBootResult.physicalControllerName, [name UTF8String], sizeof(s_titleBootResult.physicalControllerName));
            } else {
                s_titleBootResult.physicalControllerConnected = false;
                strlcpy(s_titleBootResult.physicalControllerName, "None", sizeof(s_titleBootResult.physicalControllerName));
            }
        }

        if (LaunchSettings::ForceInterpreter()) {
            strlcpy(s_titleBootResult.cpuMode, "Single-core interpreter", sizeof(s_titleBootResult.cpuMode));
            s_titleBootResult.schedulerCores = 1;
        } else if (LaunchSettings::ForceMultiCoreInterpreter()) {
            strlcpy(s_titleBootResult.cpuMode, "Multi-core interpreter", sizeof(s_titleBootResult.cpuMode));
            s_titleBootResult.schedulerCores = 3;
        } else {
            auto mode = ActiveSettings::GetCPUMode();
            strlcpy(s_titleBootResult.cpuMode, fmt::format("{}", mode).c_str(), sizeof(s_titleBootResult.cpuMode));
            s_titleBootResult.schedulerCores = (mode == CPUMode::MulticoreRecompiler) ? 3 : 1;
        }

        s_titleBootResult.gx2DrawCount = LatteGPUState.drawCallCounter;
        s_titleBootResult.metalDrawCount = LatteGPUState.drawCallCounter;
        s_titleBootResult.completedHLECount = 0;
        s_titleBootResult.totalInstructionsExecuted = 0;
    }
    return s_titleBootResult;
}

bool CemuBridge_IsTitleRunning(void)
{
    const bool running = CafeSystem::IsTitleRunning();
    if (running)
        s_titleWasObservedRunning.store(true, std::memory_order_release);
    return running;
}

void CemuBridge_SetMetalLayer(CAMetalLayer* _Nonnull layer)
{
    // Store as weak reference - the layer is owned by the UIView
    s_metalLayer = layer;

    if (s_metalDevice) {
        layer.device = s_metalDevice;
    }
    layer.maximumDrawableCount = 3;
    s_drawableWidth = (float)layer.drawableSize.width;
    s_drawableHeight = (float)layer.drawableSize.height;

    auto& winInfo = WindowSystem::GetWindowInfo();
    winInfo.window_main.surface = (__bridge void*)layer;
    winInfo.window_main.backend = WindowSystem::WindowHandleInfo::Backend::Cocoa;
    winInfo.phys_width = (int32_t)layer.drawableSize.width;
    winInfo.phys_height = (int32_t)layer.drawableSize.height;
    CGFloat scale = layer.contentsScale > 0 ? layer.contentsScale : 1.0;
    winInfo.width = (int32_t)(layer.drawableSize.width / scale);
    winInfo.height = (int32_t)(layer.drawableSize.height / scale);
    winInfo.dpi_scale = (double)scale;

    os_log(s_log,
           "CemuBridge_SetMetalLayer: layer=%p device=%{public}s pixelFormat=%lu drawableSize=%.0fx%.0f contentsScale=%.1f",
           (__bridge void*)layer,
           s_deviceName,
           (unsigned long)layer.pixelFormat,
           layer.drawableSize.width,
           layer.drawableSize.height,
           layer.contentsScale);
}

void CemuBridge_ClearMetalLayer(void)
{
    os_log(s_log, "CemuBridge_ClearMetalLayer: clearing layer reference (was %p)", (__bridge void*)s_metalLayer);
    s_metalLayer = nil;
}

void CemuBridge_UpdateDrawableSize(float width, float height)
{
    s_drawableWidth = width;
    s_drawableHeight = height;

    if (s_metalLayer) {
        auto& winInfo = WindowSystem::GetWindowInfo();
        winInfo.phys_width = (int32_t)width;
        winInfo.phys_height = (int32_t)height;
        CGFloat scale = s_metalLayer.contentsScale > 0 ? s_metalLayer.contentsScale : 1.0;
        winInfo.width = (int32_t)(width / scale);
        winInfo.height = (int32_t)(height / scale);
        winInfo.dpi_scale = (double)scale;
    }

    os_log(s_log,
           "CemuBridge_UpdateDrawableSize: %.0f x %.0f",
           width, height);
}

void CemuBridge_RecordDisplayLinkPeriod(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.001 || seconds > 0.25)
        return;
    RuntimePerformance::displayLinkPeriodUs.store(
        static_cast<uint32_t>(std::lround(seconds * 1.0e6)), std::memory_order_relaxed);
}

float CemuBridge_GetTargetFrameRate(void)
{
    const int fgTarget = CemuBridge_GetRunningGameFrameGenerationTarget();
    if (fgTarget == 120)
        return 120.0f;
    sint32 customFreq = 0;
    if (LatteTiming_getCustomVsyncFrequency(customFreq) && customFreq > 0)
        return (float)customFreq;
    if (fgTarget == 30)
        return 30.0f;
    return 60.0f;
}

void CemuBridge_SetDRCMetalLayer(CAMetalLayer* _Nonnull layer)
{
    s_drcMetalLayer = layer;

    if (s_metalDevice) {
        layer.device = s_metalDevice;
    }
    layer.drawableSize = CGSizeMake(kDRCDrawableWidth, kDRCDrawableHeight);

    auto& winInfo = WindowSystem::GetWindowInfo();
    winInfo.window_pad.surface = (__bridge void*)layer;
    winInfo.window_pad.backend = WindowSystem::WindowHandleInfo::Backend::Cocoa;
    winInfo.pad_width = kDRCDrawableWidth;
    winInfo.pad_height = kDRCDrawableHeight;
    winInfo.phys_pad_width = kDRCDrawableWidth;
    winInfo.phys_pad_height = kDRCDrawableHeight;
    winInfo.pad_dpi_scale = 1.0;
    winInfo.pad_open = true;

    auto metalRenderer = MetalRenderer::GetInstance();
    if (metalRenderer && !metalRenderer->IsPadWindowActive()) {
        metalRenderer->InitializeLayer({kDRCDrawableWidth, kDRCDrawableHeight}, false);
    }

    // The SwiftUI preview can outlive a renderer/title transition. Restore the
    // renderer-side callback from the bridge-owned handler when the DRC layer
    // is attached again.
    {
        std::lock_guard<std::mutex> lock(s_drcPreviewTextureMutex);
        MetalRenderer_SetDRCPreviewPublishCallback(
            s_drcPreviewPublicationHandler ? CemuBridge_DRCPreviewPublished : nullptr);
    }

    os_log(s_log,
           "CemuBridge_SetDRCMetalLayer: layer=%p drawableSize=%dx%d",
           (__bridge void*)layer,
           kDRCDrawableWidth,
           kDRCDrawableHeight);
}

void CemuBridge_ClearDRCMetalLayer(void)
{
    os_log(s_log, "CemuBridge_ClearDRCMetalLayer: clearing layer reference (was %p)",
           (__bridge void*)s_drcMetalLayer);

    auto metalRenderer = MetalRenderer::GetInstance();
    if (metalRenderer && metalRenderer->IsPadWindowActive()) {
        metalRenderer->ShutdownLayer(false);
    }

    auto& winInfo = WindowSystem::GetWindowInfo();
    winInfo.pad_open = false;
    winInfo.pad_width = 0;
    winInfo.pad_height = 0;
    winInfo.phys_pad_width = 0;
    winInfo.phys_pad_height = 0;
    winInfo.window_pad.surface = nullptr;
    s_drcMetalLayer = nil;
}

void CemuBridge_UpdateDRCDrawableSize(float /*width*/, float /*height*/)
{
    if (!s_drcMetalLayer) {
        return;
    }

    s_drcMetalLayer.drawableSize = CGSizeMake(kDRCDrawableWidth, kDRCDrawableHeight);

    auto& winInfo = WindowSystem::GetWindowInfo();
    winInfo.pad_width = kDRCDrawableWidth;
    winInfo.pad_height = kDRCDrawableHeight;
    winInfo.phys_pad_width = kDRCDrawableWidth;
    winInfo.phys_pad_height = kDRCDrawableHeight;
    winInfo.pad_dpi_scale = 1.0;

    auto metalRenderer = MetalRenderer::GetInstance();
    if (metalRenderer && metalRenderer->IsPadWindowActive()) {
        metalRenderer->ResizeLayer({kDRCDrawableWidth, kDRCDrawableHeight}, false);
    }
}

void CemuBridge_SetMenuControllerCapture(bool captured) { GCController_SetMenuCaptured(captured); }

void CemuBridge_SetTouchInputEnabled(bool enabled) { GCController_SetTouchEnabled(enabled); }
bool CemuBridge_IsSoftwareKeyboardActive(void) { return swkbd_hasKeyboardInputHook(); }

void CemuBridge_SendSoftwareKeyboardString(const char* utf8Text)
{
    if (!utf8Text) return;
    swkbd_setText(utf8Text);
}

void CemuBridge_SendSoftwareKeyboardKey(uint32_t keyCode)
{
    swkbd_keyInput(keyCode);
}

void CemuBridge_ConfirmSoftwareKeyboard(void)
{
    swkbd_finishInput();
}

const char* CemuBridge_GetSoftwareKeyboardText(void)
{
    static std::string s_currentText;
    s_currentText = swkbd_getText();
    return s_currentText.c_str();
}

int32_t CemuBridge_GetSoftwareKeyboardMaxTextLength(void)
{
    return swkbd_getMaxTextLength();
}

void CemuBridge_UpdateMainTouch(bool active, float normalizedX, float normalizedY)
{
    const float x = std::clamp(normalizedX, 0.0f, 1.0f);
    const float y = std::clamp(normalizedY, 0.0f, 1.0f);

    int physW = 1280, physH = 720;
    WindowSystem::GetWindowPhysSize(physW, physH);
    if (physW <= 0) physW = 1280;
    if (physH <= 0) physH = 720;

    auto& input = InputManager::instance();
    {
        auto& touch = input.m_main_touch;
        auto& mouse = input.m_main_mouse;
        std::scoped_lock lock(touch.m_mutex, mouse.m_mutex);
        touch.position = {
            static_cast<int32_t>(std::lround(x * physW)),
            static_cast<int32_t>(std::lround(y * physH))
        };
        mouse.position = touch.position;
        if (active && !touch.left_down)
            touch.left_down_toggle = true;
        touch.left_down = active;
    }
    {
        auto& touch = input.m_pad_touch;
        auto& mouse = input.m_pad_mouse;
        std::scoped_lock lock(touch.m_mutex, mouse.m_mutex);
        touch.position = {
            static_cast<int32_t>(std::lround(x * kDRCDrawableWidth)),
            static_cast<int32_t>(std::lround(y * kDRCDrawableHeight))
        };
        mouse.position = touch.position;
        if (active && !touch.left_down)
            touch.left_down_toggle = true;
        touch.left_down = active;
    }
}

void CemuBridge_UpdateTouchInput(uint64_t buttons, float lx, float ly, float rx, float ry)
{
    GCController_UpdateTouch(buttons, lx, ly, rx, ry);
}
void CemuBridge_SetTouchMotionEnabled(bool enabled) { GCController_SetTouchMotionEnabled(enabled); }
void CemuBridge_UpdateTouchMotion(float dt, float gx, float gy, float gz, float ax, float ay, float az, bool isPortrait)
{
    GCController_UpdateTouchMotion(dt, gx, gy, gz, ax, ay, az, isPortrait);
}

void CemuBridge_UpdateDRCTouch(bool active, float normalizedX, float normalizedY)
{
    const float x = std::clamp(normalizedX, 0.0f, 1.0f);
    const float y = std::clamp(normalizedY, 0.0f, 1.0f);

    auto& input = InputManager::instance();
    auto& touch = input.m_pad_touch;
    auto& mouse = input.m_pad_mouse;
    std::scoped_lock lock(touch.m_mutex, mouse.m_mutex);
    touch.position = {
        static_cast<int32_t>(std::lround(x * kDRCDrawableWidth)),
        static_cast<int32_t>(std::lround(y * kDRCDrawableHeight))
    };
    mouse.position = touch.position;
    if (active && !touch.left_down)
        touch.left_down_toggle = true;
    touch.left_down = active;
}

id<MTLTexture> CemuBridge_GetDRCPreviewTexture(void)
{
    auto metalRenderer = MetalRenderer::GetInstance();
    if (metalRenderer) {
        auto frame = metalRenderer->AcquireCompletedDRCPreviewFrame();
        if (frame.valid) {
            // AcquireCompletedDRCPreviewFrame returns one retain on each object.
            // ARC consumes those retains and the bridge keeps the newest valid
            // Pad-only publication alive across texture-cache recreation.
            id<MTLTexture> texture =
                (__bridge_transfer id<MTLTexture>)((void*)frame.texture);
            id<MTLCommandQueue> commandQueue =
                (__bridge_transfer id<MTLCommandQueue>)((void*)frame.commandQueue);
            if (texture.device != nil && commandQueue.device == texture.device &&
                texture.textureType == MTLTextureType2D &&
                texture.width > 0 && texture.height > 0) {
                std::lock_guard<std::mutex> lock(s_drcPreviewTextureMutex);
                if (frame.generation > s_retainedDRCPreviewGeneration) {
                    s_retainedDRCPreviewTexture = texture;
                    s_retainedDRCPreviewCommandQueue = commandQueue;
                    s_retainedDRCPreviewGeneration = frame.generation;
                }
            }
        }
    }

    // TV-only observations and in-flight Pad copies do not clear the last
    // completed Pad publication.
    std::lock_guard<std::mutex> lock(s_drcPreviewTextureMutex);
    return s_retainedDRCPreviewTexture;
}

uint64_t CemuBridge_GetDRCPreviewGeneration(void)
{
    std::lock_guard<std::mutex> lock(s_drcPreviewTextureMutex);
    return s_retainedDRCPreviewGeneration;
}

id<MTLCommandQueue> CemuBridge_GetDRCPreviewCommandQueue(void)
{
    std::lock_guard<std::mutex> lock(s_drcPreviewTextureMutex);
    return s_retainedDRCPreviewCommandQueue;
}

void CemuBridge_SetDRCPreviewPublicationHandler(void (^ _Nullable handler)(void))
{
    {
        std::lock_guard<std::mutex> lock(s_drcPreviewTextureMutex);
        s_drcPreviewPublicationHandler = [handler copy];
    }
    CemuBridge_RestoreDRCPreviewPublishCallback();
}

static std::mutex s_jitLogMutex;
static FILE* s_jitLogFile = nullptr;
static std::deque<std::string> s_recentJITLines;
constexpr size_t kMaxRecentJITLines = 64;

void CemuBridge_LogJITMilestone(const char* message)
{
    if (!message || !s_diagnosticLoggingEnabled.load(std::memory_order_relaxed)) return;

    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    char timeStr[32];
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &tm_buf);

    char formattedLine[512];
    snprintf(formattedLine, sizeof(formattedLine), "[%s.%03d] %s", timeStr, (int)ms.count(), message);

    @autoreleasepool {
        std::lock_guard<std::mutex> lock(s_jitLogMutex);
        if (s_recentJITLines.size() >= kMaxRecentJITLines)
            s_recentJITLines.pop_front();
        s_recentJITLines.emplace_back(formattedLine);

        if (!s_jitLogFile) {
            s_jitLogFile = fopen(CemuBridge_GetJITDiagnosticsPath(), "a");
            if (s_jitLogFile)
                setvbuf(s_jitLogFile, nullptr, _IOLBF, 0);
        }
        if (s_jitLogFile) {
            long currentPos = ftell(s_jitLogFile);
            if (currentPos > 256 * 1024) {
                fflush(s_jitLogFile);
                if (ftruncate(fileno(s_jitLogFile), 0) == 0) {
                    rewind(s_jitLogFile);
                }
            }
            fprintf(s_jitLogFile, "%s\n", formattedLine);
            fflush(s_jitLogFile);
        }
    }
}

bool CemuBridge_IsDiagnosticLoggingEnabled(void)
{
    return s_diagnosticLoggingEnabled.load(std::memory_order_relaxed);
}

size_t CemuBridge_GetFramePerformanceReportText(char* buffer, size_t bufferSize)
{
    if (!buffer || !bufferSize) return 0;
    const char* report = "Frame performance diagnostics unavailable in standard release.\n";
    const size_t written = std::min(strlen(report), bufferSize - 1);
    memcpy(buffer, report, written);
    buffer[written] = '\0';
    return written;
}

bool CemuBridge_IsGPUFrameCaptureSupported(void)
{
    // Capture support is fixed when Metal initializes for this process. Cache
    // the capability so SwiftUI diagnostics do not repeatedly query Metal.
    static const bool supported = [[MTLCaptureManager sharedCaptureManager]
        supportsDestination:MTLCaptureDestinationGPUTraceDocument];
    return supported;
}

bool CemuBridge_RequestGPUFrameCapture(void)
{
    if (!CafeSystem::IsTitleRunning() || !CemuBridge_IsGPUFrameCaptureSupported())
        return false;
    MetalRenderer_RequestFrameCapture();
    return true;
}

const char* CemuBridge_GetThermalStateName(void)
{
    switch (NSProcessInfo.processInfo.thermalState) {
    case NSProcessInfoThermalStateNominal: return "nominal";
    case NSProcessInfoThermalStateFair: return "fair";
    case NSProcessInfoThermalStateSerious: return "serious";
    case NSProcessInfoThermalStateCritical: return "critical";
    default: return "unknown";
    }
}

void CemuBridge_SetDiagnosticLoggingEnabled(bool enabled)
{
    s_diagnosticLoggingEnabled.store(enabled, std::memory_order_relaxed);
    cemuLog_setOutputEnabled(enabled);
    [NSUserDefaults.standardUserDefaults setBool:enabled forKey:@"CemuIOS.DiagnosticLoggingEnabled"];
    if (enabled) {
        cemuLog_createLogFile(false);
        CemuBridge_LogJITMilestone("[LOGGING] Detailed diagnostics enabled by user");
    }
}

const char* CemuBridge_GetJITDiagnosticsPath(void)
{
    static std::string s_jitLogPath;
    if (s_jitLogPath.empty()) {
        NSArray* paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
        NSString* docsDir = [paths firstObject];
        if (docsDir) {
            std::error_code ec;
            fs::create_directories(fs::path(docsDir.UTF8String) / "Logs", ec);
            s_jitLogPath = std::string([docsDir UTF8String]) + "/Logs/jit_diagnostics.log";
        } else {
            s_jitLogPath = "/tmp/jit_diagnostics.log";
        }
    }
    return s_jitLogPath.c_str();
}

void CemuBridge_GetJITDiagnosticsRecent(char* buffer, size_t bufferSize, int maxLines)
{
    if (!buffer || bufferSize == 0) return;
    buffer[0] = '\0';

    std::lock_guard<std::mutex> lock(s_jitLogMutex);
    if (s_recentJITLines.empty()) {
        const char* path = CemuBridge_GetJITDiagnosticsPath();
        FILE* fp = fopen(path, "r");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long fileSize = ftell(fp);
            if (fileSize > 0) {
                long readLen = std::min(fileSize, (long)(16 * 1024));
                fseek(fp, fileSize - readLen, SEEK_SET);
                std::vector<char> fileChunk(readLen + 1, 0);
                size_t bytesRead = fread(fileChunk.data(), 1, readLen, fp);
                fileChunk[bytesRead] = '\0';
                std::string text(fileChunk.data());
                size_t start = 0;
                while (start < text.size()) {
                    size_t end = text.find('\n', start);
                    if (end == std::string::npos) {
                        std::string line = text.substr(start);
                        if (!line.empty()) s_recentJITLines.push_back(line);
                        break;
                    }
                    std::string line = text.substr(start, end - start);
                    if (!line.empty()) s_recentJITLines.push_back(line);
                    start = end + 1;
                }
                while (s_recentJITLines.size() > kMaxRecentJITLines)
                    s_recentJITLines.pop_front();
            }
            fclose(fp);
        }
    }

    size_t count = s_recentJITLines.size();
    size_t firstIdx = (maxLines > 0 && count > (size_t)maxLines) ? (count - maxLines) : 0;

    std::string result;
    for (size_t i = firstIdx; i < count; ++i) {
        result += s_recentJITLines[i];
        result += "\n";
    }

    strlcpy(buffer, result.c_str(), bufferSize);
}

void CemuBridge_ClearJITDiagnosticsLog(void)
{
    std::lock_guard<std::mutex> lock(s_jitLogMutex);
    s_recentJITLines.clear();
    if (s_jitLogFile)
    {
        fflush(s_jitLogFile);
        if (ftruncate(fileno(s_jitLogFile), 0) == 0)
        {
            rewind(s_jitLogFile);
            return;
        }
        fclose(s_jitLogFile);
        s_jitLogFile = nullptr;
    }
    FILE* fp = fopen(CemuBridge_GetJITDiagnosticsPath(), "w");
    if (fp)
        fclose(fp);
}

const char* CemuBridge_GetMainLogPath(void)
{
    // Export the same file the logger writes, including its iOS Logs directory.
    thread_local std::string currentLogPath;
    currentLogPath = cemuLog_GetLogFilePath().string();
    return currentLogPath.empty() ? s_mainLogPath.c_str() : currentLogPath.c_str();
}

const char* CemuBridge_GetPreviousCrashReportPath(void)
{
	return s_previousCrashReportPath.c_str();
}

const char* CemuBridge_GetPreviousLogDirectory(void)
{
    return s_previousLogDirectory.c_str();
}

static const char* CemuBridge_MetalResourceCrashEventName(MetalResourceCrashEventType type)
{
	switch (type)
	{
	case MetalResourceCrashEventType::TextureCreate: return "TEX_CREATE";
	case MetalResourceCrashEventType::TextureDestroy: return "TEX_DESTROY";
	case MetalResourceCrashEventType::TextureCacheEvict: return "CACHE_EVICT";
	case MetalResourceCrashEventType::TextureReplace: return "RT_REPLACE";
	case MetalResourceCrashEventType::PipelineCreateBegin: return "PIPE_BEGIN";
	case MetalResourceCrashEventType::PipelineCreateSuccess: return "PIPE_OK";
	case MetalResourceCrashEventType::PipelineCreateError: return "PIPE_ERROR";
	case MetalResourceCrashEventType::CommandBufferCommit: return "CB_COMMIT";
	case MetalResourceCrashEventType::CommandBufferComplete: return "CB_COMPLETE";
	case MetalResourceCrashEventType::CommandBufferError: return "CB_ERROR";
	}
	return "UNKNOWN";
}

size_t CemuBridge_GetMetalResourceCrashHistoryText(char* buffer, size_t bufferSize)
{
	if (!buffer || bufferSize == 0)
		return 0;
	buffer[0] = '\0';
	size_t used = 0;
	auto append = [&](const char* format, auto... args) {
		if (used >= bufferSize - 1)
			return;
		const int written = snprintf(buffer + used, bufferSize - used, format, args...);
		if (written <= 0)
			return;
		const size_t available = bufferSize - used;
		used += static_cast<size_t>(written) < available
			? static_cast<size_t>(written) : available - 1;
	};

	const uint32 count = MetalResourceCrashDiagnostics_GetEventCount();
	append("=== Metal resource history (%u / 200, oldest to newest) ===\n", count);
	for (uint32 index = 0; index < count && used < bufferSize - 1; ++index)
	{
		MetalResourceCrashEvent event{};
		if (!MetalResourceCrashDiagnostics_GetEvent(index, event))
			continue;
		append("#%llu %s hostNs=%llu tex=0x%llx owner=0x%llx obj=%llu guest=%08x "
			"%ux%u pitch=%u fmt=%u tile=%u cb=0x%llx status=%u related=0x%llx flags=%08x\n",
			(unsigned long long)event.sequence, CemuBridge_MetalResourceCrashEventName(event.type),
			(unsigned long long)event.hostTimeNs, (unsigned long long)event.texturePtr,
			(unsigned long long)event.ownerPtr, (unsigned long long)event.ownerObjectId,
			event.guestAddress, event.width, event.height, event.pitch, event.pixelFormat,
			event.tileMode, (unsigned long long)event.commandBufferPtr, event.commandBufferStatus,
			(unsigned long long)event.relatedPtr, event.flags);
	}
	buffer[used] = '\0';
	return used;
}

int32_t CemuBridge_GetSetting(CemuIOSSetting setting)
{
	const auto& config = GetConfig();
	switch (setting)
	{
	case CemuIOSSettingConsoleLanguage: return static_cast<int32_t>(config.console_language.GetValue());
	case CemuIOSSettingVSync: return config.vsync.GetValue();
	case CemuIOSSettingGX2DrawDoneSync: return config.gx2drawdone_sync.GetValue();
	case CemuIOSSettingAsyncCompile: return config.async_compile.GetValue();
	case CemuIOSSettingUpscaleFilter: return config.upscale_filter.GetValue();
	case CemuIOSSettingDownscaleFilter: return config.downscale_filter.GetValue();
	case CemuIOSSettingFullscreenScaling: return config.fullscreen_scaling.GetValue();
	case CemuIOSSettingPlayBootSound: return config.play_boot_sound.GetValue();
	case CemuIOSSettingTVVolume: return config.tv_volume;
	case CemuIOSSettingPadVolume: return config.pad_volume;
	case CemuIOSSettingInputVolume: return config.input_volume;
	case CemuIOSSettingAudioDelay: return config.audio_delay;
	case CemuIOSSettingOverlayPosition: return static_cast<int32_t>(config.overlay.position);
	case CemuIOSSettingOverlayFPS: return config.overlay.fps;
	case CemuIOSSettingOverlayDrawCalls: return config.overlay.drawcalls;
	case CemuIOSSettingOverlayCPUUsage: return config.overlay.cpu_usage;
	case CemuIOSSettingOverlayRAMUsage: return config.overlay.ram_usage;
	case CemuIOSSettingShaderNotifications: return config.notification.shader_compiling;
	case CemuIOSSettingMetalFXSpatial: return config.metalfx_spatial.GetValue();
	case CemuIOSSettingExperimentalMaxFPS: return 1;
	case CemuIOSSettingExperimentalFrameGeneration: return config.frame_generation_target.GetValue();
	}
	return 0;
}

void CemuBridge_SetSetting(CemuIOSSetting setting, int32_t value)
{
	auto& config = GetConfig();
	switch (setting)
	{
	case CemuIOSSettingConsoleLanguage: config.console_language = static_cast<CafeConsoleLanguage>(std::clamp(value, 0, 11)); break;
	case CemuIOSSettingVSync: config.vsync = std::clamp(value, 0, 1); break;
	case CemuIOSSettingGX2DrawDoneSync: config.gx2drawdone_sync = value != 0; break;
	case CemuIOSSettingAsyncCompile: config.async_compile = value != 0; break;
	case CemuIOSSettingUpscaleFilter: config.upscale_filter = std::clamp(value, 0, 3); break;
	case CemuIOSSettingDownscaleFilter: config.downscale_filter = std::clamp(value, 0, 3); break;
	case CemuIOSSettingFullscreenScaling: config.fullscreen_scaling = std::clamp(value, 0, 1); break;
	case CemuIOSSettingPlayBootSound: config.play_boot_sound = value != 0; break;
	case CemuIOSSettingTVVolume:
		config.tv_volume = std::clamp(value, 0, 100);
		g_padVolume.store(config.pad_volume, std::memory_order_release);
		{
			std::shared_lock lock(g_audioMutex);
			if (g_tvAudio) g_tvAudio->SetVolume(config.tv_volume);
			if (g_padAudio) g_padAudio->SetVolume(config.pad_volume);
		}
		break;
	case CemuIOSSettingPadVolume:
		config.pad_volume = std::clamp(value, 0, 100);
		g_padVolume.store(config.pad_volume, std::memory_order_release);
		{
			std::shared_lock lock(g_audioMutex);
			if (g_tvAudio) g_tvAudio->SetVolume(config.tv_volume);
			if (g_padAudio) g_padAudio->SetVolume(config.pad_volume);
		}
		break;
	case CemuIOSSettingInputVolume:
		config.input_volume = std::clamp(value, 0, 100);
		{ std::shared_lock lock(g_audioInputMutex); if (g_inputAudio) g_inputAudio->SetVolume(config.input_volume); }
		break;
	case CemuIOSSettingAudioDelay: config.audio_delay = std::clamp(value, 0, 24); break;
	case CemuIOSSettingOverlayPosition: config.overlay.position = static_cast<ScreenPosition>(std::clamp(value, 0, 6)); break;
	case CemuIOSSettingOverlayFPS: config.overlay.fps = value != 0; break;
	case CemuIOSSettingOverlayDrawCalls: config.overlay.drawcalls = value != 0; break;
	case CemuIOSSettingOverlayCPUUsage: config.overlay.cpu_usage = value != 0; break;
	case CemuIOSSettingOverlayRAMUsage: config.overlay.ram_usage = value != 0; break;
	case CemuIOSSettingShaderNotifications: config.notification.shader_compiling = value != 0; break;
	case CemuIOSSettingMetalFXSpatial: config.metalfx_spatial = value != 0; break;
	case CemuIOSSettingExperimentalMaxFPS: config.vk_accurate_barriers = false; break;
	case CemuIOSSettingExperimentalFrameGeneration:
	{
		const int target = (value == 60 || value == 80 || value == 100 || value == 120) ? value : 0;
		config.frame_generation_target = target;
		break;
	}
	}
	GetConfigHandle().Save();
}

void CemuBridge_SetAppAudioMuted(bool muted)
{
	const auto& config = GetConfig();
	const int32_t tvVolume = muted ? 0 : config.tv_volume;
	const int32_t padVolume = muted ? 0 : config.pad_volume;
	std::shared_lock lock(g_audioMutex);
	if (g_tvAudio)
		g_tvAudio->SetVolume(tvVolume);
	if (g_padAudio)
		g_padAudio->SetVolume(padVolume);
}

void CemuBridge_RestoreAudioOutput(void)
{
	CemuBridge_ConfigureAudioSession();
	CemuBridge_UpdateMicrophoneInputSelection();
	const auto& config = GetConfig();
	std::shared_lock lock(g_audioMutex);
	if (g_tvAudio)
	{
		g_tvAudio->Stop();
		g_tvAudio->SetVolume(config.tv_volume);
		g_tvAudio->Play();
	}
	if (g_padAudio)
	{
		g_padAudio->Stop();
		g_padAudio->SetVolume(config.pad_volume);
		g_padAudio->Play();
	}
}

void CemuBridge_RestoreControllerAudioRoute(void)
{
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		AVAudioSession* session = [AVAudioSession sharedInstance];
		CemuBridge_KeepControllerFromCapturingAudio(session);
		const auto& config = GetConfig();
		std::shared_lock lock(g_audioMutex);
		if (g_tvAudio)
			g_tvAudio->SetVolume(config.tv_volume);
		if (g_padAudio)
			g_padAudio->SetVolume(config.pad_volume);
	});
}

void CemuBridge_SetAppSuspended(bool suspended)
{
	const bool paused = suspended;
	CemuMetal_SetFrameGenerationSuspended(paused);
	PPCTimer_setHostSuspended(paused);
}

void CemuBridge_SetControllerMotionEnabled(bool enabled)
{
	GCController_SetMotionEnabled(enabled);
}

void CemuBridge_RecenterControllerMotion(void)
{
	GCController_RequestMotionRecenter();
}

int32_t CemuBridge_GetMicrophonePermissionStatus(void)
{
	if (@available(iOS 17.0, *))
	{
		switch ([AVAudioApplication sharedInstance].recordPermission)
		{
		case AVAudioApplicationRecordPermissionGranted: return 2;
		case AVAudioApplicationRecordPermissionDenied: return 1;
		default: return 0;
		}
	}
	else
	{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		switch ([AVAudioSession sharedInstance].recordPermission)
		{
		case AVAudioSessionRecordPermissionGranted: return 2;
		case AVAudioSessionRecordPermissionDenied: return 1;
		default: return 0;
		}
#pragma clang diagnostic pop
	}
}

void CemuBridge_RequestMicrophonePermission(void)
{
	auto completion = ^(BOOL granted) {
		os_log(s_log, "CemuBridge: microphone permission %{public}s", granted ? "granted" : "denied");
		if (granted)
			CemuBridge_ConfigureAudioSession();
		CemuBridge_UpdateMicrophoneInputSelection();
		GetConfigHandle().Save();
	};

	if (@available(iOS 17.0, *))
	{
		[AVAudioApplication requestRecordPermissionWithCompletionHandler:completion];
	}
	else
	{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		[[AVAudioSession sharedInstance] requestRecordPermission:completion];
#pragma clang diagnostic pop
	}
}

CemuOnlineServiceInfo CemuBridge_GetOnlineServiceInfo(void)
{
	CemuOnlineServiceInfo info{};
	auto& config = GetConfigHandle().data();
	const Account& account = Account::GetAccount(config.account.m_persistent_id.GetValue());
	info.persistentId = account.GetPersistentId();
	info.selectedService = static_cast<int32_t>(config.GetAccountNetworkService(info.persistentId));

	const OnlineValidator validation = account.ValidateOnlineFiles();
	info.accountValid = validation.valid_account;
	info.onlineFilesValid = validation.otp == OnlineValidator::FileState::Ok &&
		validation.seeprom == OnlineValidator::FileState::Ok && validation.missing_files.empty();
	info.pretendoReady = info.accountValid && info.onlineFilesValid &&
		info.selectedService == static_cast<int32_t>(NetworkService::Pretendo);
	return info;
}

bool CemuBridge_SetOnlineService(int32_t service)
{
	if (CafeSystem::IsTitleRunning())
		return false;
	if (service != static_cast<int32_t>(NetworkService::Offline) &&
		service != static_cast<int32_t>(NetworkService::Pretendo))
		return false;

	auto& config = GetConfigHandle().data();
	const Account& account = Account::GetAccount(config.account.m_persistent_id.GetValue());

	config.SetAccountSelectedService(account.GetPersistentId(), static_cast<NetworkService>(service));
	config.account.legacy_online_enabled = (service != static_cast<int32_t>(NetworkService::Offline));
	config.account.legacy_active_service = (service == static_cast<int32_t>(NetworkService::Pretendo) ? 1 : 0);
	GetConfigHandle().Save();
	return true;
}

const char* CemuBridge_GetOnlineAccountName(void)
{
	static std::string accountName;
	const auto& config = GetConfigHandle().data();
	const Account& account = Account::GetAccount(config.account.m_persistent_id.GetValue());
	accountName = account.GetAccountId().empty()
		? boost::nowide::narrow(account.GetMiiName())
		: std::string(account.GetAccountId());
	return accountName.c_str();
}

const char* CemuBridge_GetOnlineServiceStatus(void)
{
	static std::string status;
	const auto info = CemuBridge_GetOnlineServiceInfo();
	const auto& config = GetConfigHandle().data();
	const Account& account = Account::GetAccount(config.account.m_persistent_id.GetValue());
	const OnlineValidator validation = account.ValidateOnlineFiles();

	if (!info.accountValid)
		status = "A valid dumped Wii U PNID account is required.";
	else if (validation.otp != OnlineValidator::FileState::Ok)
		status = "otp.bin is missing or invalid.";
	else if (validation.seeprom != OnlineValidator::FileState::Ok)
		status = "seeprom.bin is missing or invalid.";
	else if (!validation.missing_files.empty())
		status = "Required Wii U online certificate files are missing.";
	else if (info.selectedService == static_cast<int32_t>(NetworkService::Pretendo))
		status = "Pretendo is selected and ready for the next game launch.";
	else
		status = "Online play is disabled.";
	return status.c_str();
}

uint32_t CemuBridge_GetAccountCount(void)
{
	const auto& accounts = Account::GetAccounts();
	return static_cast<uint32_t>(accounts.size());
}

bool CemuBridge_GetAccountAtIndex(uint32_t index, CemuAccountItem* outItem)
{
	if (!outItem)
		return false;
	memset(outItem, 0, sizeof(CemuAccountItem));
	const auto& accounts = Account::GetAccounts();
	if (index >= accounts.size())
		return false;
	const auto& account = accounts[index];
	outItem->persistentId = account.GetPersistentId();

	std::string miiName = boost::nowide::narrow(account.GetMiiName());
	std::string accountId = std::string(account.GetAccountId());
	strlcpy(outItem->miiName, miiName.c_str(), sizeof(outItem->miiName));
	strlcpy(outItem->accountId, accountId.c_str(), sizeof(outItem->accountId));

	const auto validation = account.ValidateOnlineFiles();
	outItem->isOnlineValid = validation.IsValid();

	auto& config = GetConfigHandle().data();
	outItem->isActive = (account.GetPersistentId() == config.account.m_persistent_id.GetValue());
	outItem->selectedService = static_cast<int32_t>(config.GetAccountNetworkService(account.GetPersistentId()));
	return true;
}

bool CemuBridge_SelectAccount(uint32_t persistentId)
{
	if (CafeSystem::IsTitleRunning())
		return false;
	const auto& account = Account::GetAccount(persistentId);
	if (account.GetPersistentId() != persistentId)
		return false;
	auto& config = GetConfigHandle().data();
	config.account.m_persistent_id.SetValue(persistentId);
	GetConfigHandle().Save();
	return true;
}

bool CemuBridge_DeleteAccount(uint32_t persistentId)
{
	if (CafeSystem::IsTitleRunning())
		return false;
	const auto& accounts = Account::GetAccounts();
	if (accounts.size() <= 1)
		return false;

	const auto& account = Account::GetAccount(persistentId);
	if (account.GetPersistentId() != persistentId)
		return false;

	const fs::path path = account.GetFileName();
	std::error_code ec;
	fs::remove_all(path.parent_path(), ec);
	if (ec)
		return false;

	Account::RefreshAccounts();

	auto& config = GetConfigHandle().data();
	if (config.account.m_persistent_id.GetValue() == persistentId)
	{
		const auto& remaining = Account::GetAccounts();
		if (!remaining.empty())
		{
			config.account.m_persistent_id.SetValue(remaining.front().GetPersistentId());
		}
	}
	GetConfigHandle().Save();
	return true;
}

// Lab snapshots may run while a renderer is being constructed or torn down.
// Read only published counters here; never dereference g_renderer or its memory
// manager and never stat cache files from the once-per-second recording loop.
extern "C" CemuPerformanceCacheInfo CemuBridge_GetPerformanceCacheCounters(void)
{
    CemuPerformanceCacheInfo info{};
    info.titleId = s_runningTitleId.load(std::memory_order_acquire);
    info.memoryWarningCount = s_memoryWarningCount.load(std::memory_order_relaxed);
    info.cleanupRequests = LatteTC_GetMemoryPressureCleanupRequestCount();
    info.cleanupCompleted = LatteTC_GetMemoryPressureCleanupCompletedCount();
    info.lastCleanupDeletedTextures = LatteTC_GetMemoryPressureCleanupLastDeletedCount();
    return info;
}

void CemuBridge_HandleMemoryWarning(void)
{
	s_memoryWarningCount.fetch_add(1, std::memory_order_relaxed);
	LatteTC_RequestMemoryPressureCleanup();
	os_log(s_log, "Memory warning: footprint=%llu bytes, available=%zu bytes; texture cleanup requested",
		static_cast<unsigned long long>(ProcessFootprint()), os_proc_available_memory());
}

bool CemuBridge_ClearShaderCacheForTitle(uint64_t titleId)
{
    if (titleId == 0 || !CemuBridge_CanManageSaves())
        return false;
    SaveOperationScope operation;
    if (!operation)
        return false;

	std::vector<uint64_t> ids = { titleId };
	uint64_t baseId = titleId & ~0x0000000E00000000ULL;
	if (baseId != titleId && baseId != 0)
		ids.push_back(baseId);
	uint64_t updateId = titleId | 0x0000000E00000000ULL;
	if (updateId != titleId && updateId != 0)
		ids.push_back(updateId);

	std::error_code ec;
	for (uint64_t tid : ids)
	{
		std::vector<fs::path> paths = {
			ActiveSettings::GetCachePath("shaderCache/driver/vk/{:016x}.bin", tid),
			ActiveSettings::GetCachePath("shaderCache/precompiled/{:016x}_spirv.bin", tid),
			ActiveSettings::GetCachePath("shaderCache/precompiled/{:016x}_gl.bin", tid),
			ActiveSettings::GetCachePath("shaderCache/precompiled/{:016x}_air.bin", tid),
			ActiveSettings::GetCachePath("shaderCache/precompiled/metal/{:016x}", tid),
			ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}.bin", tid),
			ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_shaders.bin", tid),
			ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_mtlshaders.bin", tid),
			ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_vkpipeline.bin", tid),
			ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_mtlpipeline.bin", tid),
		};
		for (const auto& p : paths)
		{
            fs::remove_all(p, ec); // A missing cache is already a clean cache.
            if (ec)
            {
                os_log_error(s_log, "Shader cache reset failed for %{public}s: %{public}s",
                             p.c_str(), ec.message().c_str());
                return false;
            }
		}
	}
	os_log(s_log, "CemuBridge_ClearShaderCacheForTitle: cleared shader and pipeline caches for title 0x%016llx", titleId);
	return true;
}

bool CemuBridge_ClearAllShaderCaches(void)
{
    // Cache files remain open during gameplay and shader loading. Use the same
    // lifecycle gate as title launch so a launch cannot race a reset.
    if (!CemuBridge_CanManageSaves())
        return false;
    SaveOperationScope operation;
    if (!operation)
        return false;

    std::error_code ec;
    const auto cacheDir = ActiveSettings::GetCachePath("shaderCache");
    fs::remove_all(cacheDir, ec);
    if (ec)
    {
        os_log_error(s_log, "Shader cache reset failed for %{public}s: %{public}s",
                     cacheDir.c_str(), ec.message().c_str());
        return false;
    }
    for (const auto& directory : {"shaderCache/transferable", "shaderCache/precompiled", "shaderCache/driver/vk", "shaderCache/precompiled/metal"})
    {
        const auto path = ActiveSettings::GetCachePath(directory);
        fs::create_directories(path, ec);
        if (ec)
        {
            os_log_error(s_log, "Shader cache directory reset failed for %{public}s: %{public}s",
                         path.c_str(), ec.message().c_str());
            return false;
        }
    }
    os_log(s_log, "CemuBridge_ClearAllShaderCaches: all 77U shader and pipeline records removed");
    return true;
}

CemuMemoryLimitInfo CemuBridge_GetMemoryLimitInfo(void)
{
    CemuMemoryLimitInfo info{};
    info.inLiveContainer = getenv("LC_HOME_PATH") != nullptr;
    info.physicalMemoryBytes = [NSProcessInfo processInfo].physicalMemory;

    task_vm_info_data_t vmInfo;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vmInfo, &count) == KERN_SUCCESS)
    {
        info.currentFootprintBytes = vmInfo.phys_footprint;
    }

#if TARGET_OS_IPHONE
    info.availableMemoryBytes = os_proc_available_memory();
    info.totalLimitBytes = info.currentFootprintBytes + info.availableMemoryBytes;
#else
    info.availableMemoryBytes = (info.physicalMemoryBytes > info.currentFootprintBytes) ?
        (info.physicalMemoryBytes - info.currentFootprintBytes) : 0;
    info.totalLimitBytes = info.physicalMemoryBytes;
#endif

    bool entitlementFound = false;
    typedef struct __SecTask* SecTaskRef;
    typedef SecTaskRef (*SecTaskCreateFromSelfFunc)(CFAllocatorRef);
    typedef CFTypeRef (*SecTaskCopyValueForEntitlementFunc)(SecTaskRef, CFStringRef, CFErrorRef*);

    SecTaskCreateFromSelfFunc taskCreate = (SecTaskCreateFromSelfFunc)dlsym(RTLD_DEFAULT, "SecTaskCreateFromSelf");
    SecTaskCopyValueForEntitlementFunc copyEntitlement = (SecTaskCopyValueForEntitlementFunc)dlsym(RTLD_DEFAULT, "SecTaskCopyValueForEntitlement");
    if (taskCreate && copyEntitlement)
    {
        SecTaskRef task = taskCreate(NULL);
        if (task)
        {
            CFErrorRef err = NULL;
            CFTypeRef val = copyEntitlement(task, CFSTR("com.apple.developer.kernel.increased-memory-limit"), &err);
            // Only the running process's signed Boolean is evidence of activation.
            // A profile authorizes signing; it does not prove that the app was signed
            // with the capability, nor that this device grants a higher allowance.
            info.entitlementStatusKnown = !err && (!val || CFGetTypeID(val) == CFBooleanGetTypeID());
            if (val)
            {
                if (info.entitlementStatusKnown && CFGetTypeID(val) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)val))
                {
                    entitlementFound = true;
                }
                CFRelease(val);
            }
            if (err)
                CFRelease(err);
            // SecTask refers to the running host process even when LiveContainer
            // replaces NSBundle.mainBundle with the guest's bundle. Its signed App
            // ID is the one the user must update in GetMoreRAM.
            CFErrorRef identifierError = nullptr;
            CFTypeRef identifier = copyEntitlement(task, CFSTR("application-identifier"), &identifierError);
            if (!identifierError && identifier && CFGetTypeID(identifier) == CFStringGetTypeID())
                CFStringGetCString((CFStringRef)identifier, info.signedApplicationIdentifier,
                                   sizeof(info.signedApplicationIdentifier), kCFStringEncodingUTF8);
            if (identifier)
                CFRelease(identifier);
            if (identifierError)
                CFRelease(identifierError);
            CFRelease(task);
        }
    }

    info.entitlementPresent = entitlementFound;
    info.increasedMemoryLimitActive = entitlementFound;

    return info;
}

BOOL CemuBridge_PerformBlockSafely(void (NS_NOESCAPE ^_Nonnull block)(void))
{
    if (!block)
        return YES;

    @try {
        try {
            block();
            return YES;
        } catch (const std::exception& e) {
            os_log_error(s_log, "CemuBridge_PerformBlockSafely: caught C++ std::exception: %{public}s", e.what());
            return NO;
        } catch (...) {
            os_log_error(s_log, "CemuBridge_PerformBlockSafely: caught unknown C++ exception");
            return NO;
        }
    } @catch (NSException* exception) {
        os_log_error(s_log, "CemuBridge_PerformBlockSafely: caught NSException '%{public}@' reason: '%{public}@'",
                     exception.name ?: @"Unknown",
                     exception.reason ?: @"Unknown");
        return NO;
    }
}

const char* CemuBridge_GetStartupSoundPath(void)
{
    static thread_local std::string path;
    path.clear();
    const auto mlc = ActiveSettings::GetMlcPath();
    std::error_code ec;
    for (const char* id : {"10040000", "10040100", "10040200"}) {
        const auto candidate = mlc / "sys/title/00050010" / id / "meta/bootSound.btsnd";
        if (fs::is_regular_file(candidate, ec)) { path = candidate.string(); return path.c_str(); }
    }
    std::vector<fs::path> candidates;
    for (const char* kind : {"0005000e", "00050000"}) {
        fs::directory_iterator it(mlc / "usr/title" / kind, ec), end;
        if (ec) { ec.clear(); continue; }
        while (it != end) {
            const auto candidate = it->path() / "meta/bootSound.btsnd";
            if (fs::is_regular_file(candidate, ec)) candidates.push_back(candidate);
            ec.clear(); it.increment(ec);
            if (ec) break;
        }
    }
    std::sort(candidates.begin(), candidates.end());
    if (!candidates.empty()) path = candidates.front().string();
    return path.c_str();
}
