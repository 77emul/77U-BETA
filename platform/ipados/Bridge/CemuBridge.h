#pragma once

#import <stdint.h>
#import <stddef.h>
#import <stdbool.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Title boot state.
typedef struct {
    bool launchRequested;
    bool titleLoaded;
    bool titlePrepared;
    bool wutPreinitCalled;
    bool mainThreadCreated;
    bool ppcExecutionEntered;
    bool firstInstructionExecuted;
    bool titleLaunched;
    bool schedulerRunning;
    uint64_t titleId;
    uint64_t appTitleId;
    uint64_t baseTitleId;
    char titleName[128];
    char rpxPath[256];
    char launchMode[32]; // "StandaloneRPX" or "ExtractedTitle (HOST_FS)"
    char detectedFormat[32]; // "HOST_FS" or "StandaloneRPX"
    bool hostFsValid;
    bool metaXmlParsed;
    bool cosXmlParsed;
    char cosArgStr[64];
    char securityScopeStatus[32]; // "Yes", "No", "Not Required"
    char cpuMode[64];
    uint32_t schedulerCores;
    uint64_t totalInstructionsExecuted;
    uint32_t completedHLECount;
    uint32_t activeThreadsCount;
    uint32_t currentPPC_PC;
    uint32_t currentPPC_LR;
    char lastHLEFunction[64];
    char sampledThreadName[64];
    uint32_t sampledThreadEntrypoint;
    uint32_t sampledThreadPC;
    uint32_t sampledThreadLR;
    char sampledThreadState[32];
    bool sampledThreadIsEntrypoint;
    uint32_t mainThreadPC;
    uint32_t mainThreadLR;
    char mainThreadState[32];
    char mainThreadWaitReason[64];
    uint32_t prevMainThreadPC;
    bool mainThreadPCChanged;
    uint32_t mainThreadSampleCount;
    uint32_t mainThreadPCChangeCount;
    bool physicalControllerConnected;
    char physicalControllerName[64];
    bool vpadConfigured;
    uint32_t lastVPADHold;
    uint32_t gx2DrawCount;
    uint32_t metalDrawCount;
    char errorMessage[256];
} CemuTitleBootInfo;

/// Read-only summary of one title's normal Cemu save and latest automatic rollback.
typedef struct {
    bool success;
    bool saveExists;
    bool hasUserSaveData;
    bool rollbackExists;
    uint64_t totalBytes;
    int64_t modifiedUnixSeconds;
    char latestRollbackPath[1024];
    char errorMessage[512];
} CemuSaveInfo;

/// Installed update/DLC information associated with a base title ID.
typedef struct {
    bool hasUpdate;
    bool hasDLC;
    uint16_t updateVersion;
    uint16_t dlcVersion;
} CemuInstalledContentInfo;

/// Metadata and validation result for a directly selected Wii U game file.
typedef struct {
    bool success;
    uint64_t titleId;
    uint16_t titleVersion;
    uint32_t region;
    char titleName[128];
    char format[16];
    char errorMessage[256];
} CemuGameFileInfo;

/// Result of an iPadOS save export or restore operation.
typedef struct {
    bool success;
    char outputPath[1024];
    char rollbackPath[1024];
    char errorMessage[512];
} CemuSaveOperationResult;

/// Result of installing an extracted Wii U update or DLC into Cemu's MLC.
typedef struct {
    bool success;
    uint64_t titleId;
    uint16_t titleVersion;
    /// 1 = update, 2 = DLC.
    uint8_t contentType;
    char installPath[1024];
    char errorMessage[512];
} CemuTitleContentInstallResult;

/// A Cemu graphic pack available for one Wii U title.
typedef struct {
    bool enabled;
    bool compatible;
    bool universal;
    bool defaultEnabled;
    bool hasPresets;
    char identifier[512];
    char virtualPath[512];
    char name[192];
    char description[1024];
    char category[64];
} CemuGraphicPackInfo;

/// One selectable preset from a Cemu graphic pack rules.txt file.
typedef struct {
    bool active;
    bool visible;
    bool isDefault;
    char category[128];
    char name[192];
} CemuGraphicPackPresetInfo;

/// Result of mutating or installing Cemu graphic packs.
typedef struct {
    bool success;
    bool requiresRestart;
    uint32_t discoveredPackCount;
    char message[512];
} CemuGraphicPackOperationResult;

/// Result of importing a Dumpling Wii U Online Files dump for Pretendo.
typedef struct {
    bool success;
    uint32_t persistentId;
    uint32_t importedAccountCount;
    char accountName[64];
    char errorMessage[512];
} CemuPretendoImportResult;

/// Information about the bridge, Metal, and Cemu core state.
typedef struct {
    /// Name of the Metal GPU device, or NULL if unavailable.
    const char* _Nullable metalDeviceName;
    /// Whether a Metal device was successfully created.
    bool metalAvailable;
    /// Whether a CAMetalLayer has been attached via CemuBridge_SetMetalLayer.
    bool metalLayerAttached;
    /// Cemu version string.
    const char* _Nullable cemuVersion;
    /// State of the current or most recent title launch.
    CemuTitleBootInfo titleBootResult;
} CemuBridgeInfo;

/// Load and start a real Wii U title / RPX through Cemu's normal title-loading architecture
CemuTitleBootInfo CemuBridge_LaunchTitle(const char* _Nonnull utf8Path);

/// Stops the running title and resets emulation state so another title can be launched.
void CemuBridge_StopTitle(void);

/// Returns whether an attached iOS debugger is available to service the JIT breakpoint protocol.
bool CemuBridge_IsJITEnabled(void);
// Reserve executable memory while the JIT debugger is still servicing requests.
bool CemuBridge_PrepareJITMemory(void);
bool CemuBridge_IsJITMemoryReady(void);
bool CemuBridge_HasJITDebugger(void);
void CemuBridge_DetachJITDebugger(void);
bool CemuBridge_IsXcodeDebuggerAttached(void);

/// Save management is allowed only before a Cafe title starts in this process.
bool CemuBridge_CanManageSaves(void);

/// Query normal Cemu save data for a Wii U title ID.
CemuSaveInfo CemuBridge_GetSaveInfo(uint64_t titleId);

/// Query update/DLC content already installed in Cemu's MLC.
CemuInstalledContentInfo CemuBridge_GetInstalledContentInfo(uint64_t titleId);

/// Validate and read metadata from a WUA/WUD/WUX game file.
CemuGameFileInfo CemuBridge_InspectGameFile(const char* _Nonnull utf8Path);

/// Extract the title icon from a WUA/WUD/WUX file to an uncompressed TGA file.
/// The caller owns the destination and may remove it after decoding.
bool CemuBridge_ExtractGameFileIcon(const char* _Nonnull utf8Path,
                                    const char* _Nonnull utf8DestinationPath);

/// Extract the selected title's TV boot artwork from WUA/WUD/WUX without launching it.
bool CemuBridge_ExtractGameBootMedia(const char* _Nonnull utf8Path, const char* _Nonnull utf8DestinationPath, bool sound);

/// Export the complete per-title save directory to a 77U ZIP archive.
CemuSaveOperationResult CemuBridge_ExportSave(uint64_t titleId,
                                             const char* _Nonnull titleName,
                                             const char* _Nonnull outputPath);

/// Validate and restore a 77U ZIP, preserving the current save as a rollback first.
CemuSaveOperationResult CemuBridge_RestoreSave(uint64_t titleId,
                                              const char* _Nonnull archivePath);

/// Restore the latest automatic rollback, first preserving the current save.
CemuSaveOperationResult CemuBridge_RestoreRollback(uint64_t titleId);

/// Permanently remove the active per-title save, leaving exported backups untouched.
CemuSaveOperationResult CemuBridge_DeleteSave(uint64_t titleId);

/// Validate and install an extracted Wii U update or DLC directory.
/// This is available only before a Cafe title starts in this process.
CemuTitleContentInstallResult CemuBridge_InstallTitleContent(const char* _Nonnull sourcePath);

/// Enumerate Cemu graphic packs that apply to a title. Universal packs are included.
size_t CemuBridge_GetGraphicPackCount(uint64_t titleId);
bool CemuBridge_GetGraphicPackInfo(uint64_t titleId, size_t index,
                                   CemuGraphicPackInfo* _Nonnull output);
size_t CemuBridge_GetGraphicPackPresetCount(const char* _Nonnull identifier);
bool CemuBridge_GetGraphicPackPresetInfo(const char* _Nonnull identifier, size_t index,
                                         CemuGraphicPackPresetInfo* _Nonnull output);

/// Target presentation rate (30, 60, 120) for the currently running title when JIT is active.
/// Returns 0 when frame generation is off or when running with No JIT.
int CemuBridge_GetRunningGameFrameGenerationTarget(void);
int CemuBridge_GetFrameGenerationTargetForTitle(uint64_t titleId);
bool CemuBridge_GetRunningGameFrameGenerationFast(void);

typedef struct {
    double sourceFPS;
    double displayedFPS;
} CemuFrameGenerationRates;
/// Source game cadence and actual display cadence; both zero when FG is inactive.
CemuFrameGenerationRates CemuBridge_GetFrameGenerationRates(void);

/// Mutations are accepted whenever no Cafe title is currently running.
CemuGraphicPackOperationResult CemuBridge_SetGraphicPackEnabled(const char* _Nonnull identifier,
                                                                bool enabled);
CemuGraphicPackOperationResult CemuBridge_SetGraphicPackPreset(const char* _Nonnull identifier,
                                                               const char* _Nonnull category,
                                                               const char* _Nonnull presetName);
/// Install an official Cemu community graphic-pack ZIP downloaded by the app.
CemuGraphicPackOperationResult CemuBridge_InstallGraphicPackArchive(const char* _Nonnull archivePath,
                                                                    const char* _Nullable releaseName);

/// Validate and import a Dumpling Online Files directory into Cemu's user data.
/// This is available only before a Cafe title starts in this process.
CemuPretendoImportResult CemuBridge_ImportPretendoDump(const char* _Nonnull sourcePath);

/// Set the security-scoped access status for diagnostics reporting.
void CemuBridge_SetSecurityScopeStatus(const char* _Nonnull status);

/// Add a host directory to Cemu's title scan paths (e.g. Documents/Games).
void CemuBridge_AddGameScanPath(const char* _Nonnull utf8Path);

/// Trigger a rescan of all configured scan paths and persistent MLC titles.
void CemuBridge_RescanTitles(void);

/// Query live real-title boot state
CemuTitleBootInfo CemuBridge_GetTitleBootInfo(void);
bool CemuBridge_IsTitleRunning(void);

/// Returns the Cemu build version string from CemuCommon (e.g. "Cemu 2.0.0").
const char* _Nullable CemuBridge_GetCemuVersion(void);

/// Initialize the bridge: creates MTLDevice, logs startup.
/// Returns true on success.
bool CemuBridge_Initialize(void);

/// Get current bridge state info.
CemuBridgeInfo CemuBridge_GetInfo(void);

/// Set the CAMetalLayer the renderer will draw into.
/// The bridge holds a weak (non-retaining) reference.
/// Called by EmulatorMetalView when the layer is ready.
void CemuBridge_SetMetalLayer(CAMetalLayer* _Nonnull layer);

/// Clear the Metal layer reference.
/// Called by EmulatorMetalView when removed from the window.
void CemuBridge_ClearMetalLayer(void);

/// Notify the bridge of a drawable size change (in pixels).
void CemuBridge_UpdateDrawableSize(float width, float height);

/// Record CADisplayLink's reported period only while Detailed logs is enabled.
void CemuBridge_RecordDisplayLinkPeriod(double seconds);
/// Returns the current target framerate in Hz (e.g. 60.0 or higher if a graphic pack is configured).
float CemuBridge_GetTargetFrameRate(void);

/// Attach the Wii U GamePad CAMetalLayer to the existing pad renderer path.
void CemuBridge_SetDRCMetalLayer(CAMetalLayer* _Nonnull layer);

/// Detach the Wii U GamePad CAMetalLayer.
void CemuBridge_ClearDRCMetalLayer(void);

/// Notify the bridge of the GamePad drawable size (fixed at 854x480).
void CemuBridge_UpdateDRCDrawableSize(float width, float height);

/// Returns the most recent completed, retained DRC source texture observed by
/// the existing Metal backbuffer copy path.
id<MTLTexture> _Nullable CemuBridge_GetDRCPreviewTexture(void);
uint64_t CemuBridge_GetDRCPreviewGeneration(void);

/// Returns the renderer command queue paired with the retained DRC texture.
/// Preview work uses this queue to preserve GPU submission ordering.
id<MTLCommandQueue> _Nullable CemuBridge_GetDRCPreviewCommandQueue(void);

/// Installs the passive renderer-driven preview update handler. Publications
/// are coalesced onto the main queue and never block the renderer thread.
void CemuBridge_SetDRCPreviewPublicationHandler(void (^ _Nullable handler)(void));

/// Update the Wii U GamePad touch state from the visible iPadOS DRC preview.
/// Coordinates are normalized to the displayed DRC image (0...1).
void CemuBridge_UpdateDRCTouch(bool active, float normalizedX, float normalizedY);

/// Atomic snapshots of on-screen buttons (AppleGCController button IDs) and sticks.
void CemuBridge_SetTouchInputEnabled(bool enabled);
void CemuBridge_SetMenuControllerCapture(bool captured);
bool CemuBridge_IsSoftwareKeyboardActive(void);
void CemuBridge_SendSoftwareKeyboardString(const char* _Nonnull utf8Text);
void CemuBridge_SendSoftwareKeyboardKey(uint32_t keyCode);
void CemuBridge_ConfirmSoftwareKeyboard(void);
const char* _Nonnull CemuBridge_GetSoftwareKeyboardText(void);
int32_t CemuBridge_GetSoftwareKeyboardMaxTextLength(void);
void CemuBridge_UpdateMainTouch(bool active, float normalizedX, float normalizedY);
void CemuBridge_UpdateTouchInput(uint64_t buttons, float lx, float ly, float rx, float ry);
void CemuBridge_SetTouchMotionEnabled(bool enabled);
void CemuBridge_UpdateTouchMotion(float dt, float gx, float gy, float gz, float ax, float ay, float az, bool isPortrait);


/// Writes a crash-safe persistent JIT diagnostic entry to Documents/jit_diagnostics.log.
void CemuBridge_LogJITMilestone(const char* _Nonnull message);

/// Detailed runtime logs are opt-in on iPadOS to avoid formatting and file-I/O
/// overhead during normal play. Crash reports remain enabled independently.
bool CemuBridge_IsDiagnosticLoggingEnabled(void);
/// Read only for sampled diagnostics; no per-draw thermal query.
const char* _Nonnull CemuBridge_GetThermalStateName(void);
void CemuBridge_SetDiagnosticLoggingEnabled(bool enabled);

/// Available only when the process enables Metal frame capture instrumentation.
/// Detailed logs remain available without it.
bool CemuBridge_IsGPUFrameCaptureSupported(void);
/// Explicit one-frame GPU capture for an active game. No automatic captures.
bool CemuBridge_RequestGPUFrameCapture(void);

/// Returns the full path to the JIT diagnostics log file.
const char* _Nonnull CemuBridge_GetJITDiagnosticsPath(void);

/// Reads and returns the recent complete lines from the JIT diagnostics log file into buffer.
void CemuBridge_GetJITDiagnosticsRecent(char* _Nonnull buffer, size_t bufferSize, int maxLines);

/// Clears the JIT diagnostics log file.
void CemuBridge_ClearJITDiagnosticsLog(void);

/// Gets the current presented frames per second (sampled every 2s).
double CemuBridge_GetCurrentFPS(void);

/// Real-time zero-overhead performance snapshot.
typedef struct {
    double fps;
    double cpuWaitMs;
    double gpuMs;
    uint32_t drawCalls;
    uint32_t renderPasses;
} CemuPerfStatsSnapshot;

/// Gets the real-time performance snapshot.
CemuPerfStatsSnapshot CemuBridge_GetPerfStatsSnapshot(void);

/// Updates the real-time performance snapshot (called from MetalRenderer/SwapBuffers).
void CemuBridge_UpdatePerfStats(double fps, double cpuWaitMs, double gpuMs, uint32_t drawCalls, uint32_t renderPasses);

/// Sets the current presented frames per second.
void CemuBridge_SetCurrentFPS(double fps);

/// Gets the CPU diagnostic mode (0 = SinglecoreRecompiler / JIT, 1 = Interpreter, 2 = MulticoreRecompiler / JIT).
int CemuBridge_GetCPUDiagnosticMode(void);

/// Sets the CPU diagnostic mode before title launch (0 = SinglecoreRecompiler / JIT, 1 = Interpreter, 2 = MulticoreRecompiler / JIT).
void CemuBridge_SetCPUDiagnosticMode(int mode);


/// Gets the Texture Cache Diagnostic Mode (0 = Normal, 1 = Force Refresh On Bind).
int CemuBridge_GetTextureCacheDiagMode(void);

/// Sets the Texture Cache Diagnostic Mode (0 = Normal, 1 = Force Refresh On Bind).
void CemuBridge_SetTextureCacheDiagMode(int mode);

/// Gets the Render Target Store Diagnostic Mode (0 = Normal, 1 = Force Store Color Attachments).
int CemuBridge_GetRTStoreDiagMode(void);

/// Sets the Render Target Store Diagnostic Mode (0 = Normal, 1 = Force Store Color Attachments).
void CemuBridge_SetRTStoreDiagMode(int mode);

/// Sets the measured guest clock speed percentage.
void CemuBridge_SetGuestClockSpeedPercent(double pct);

/// Sets the measured emulated guest cycle execution speed percentage.
void CemuBridge_SetEmuSpeedPercent(double pct);

/// Returns the sandbox path of Cemu's current log.txt.
const char* _Nonnull CemuBridge_GetMainLogPath(void);

/// Returns the sandbox path of the most recently recovered fatal crash report.
const char* _Nonnull CemuBridge_GetPreviousCrashReportPath(void);

/// Session logs preserved before this process opened its current log files.
const char* _Nonnull CemuBridge_GetPreviousLogDirectory(void);

/// Formats the existing bounded Metal resource crash history as plain text.
/// Returns the number of bytes written, excluding the terminating NUL.
size_t CemuBridge_GetMetalResourceCrashHistoryText(char* _Nonnull buffer, size_t bufferSize);

/// Formats bounded matched CPU/GPU frames and ranked performance pressure.
size_t CemuBridge_GetFramePerformanceReportText(char* _Nonnull buffer, size_t bufferSize);

typedef enum {
    CemuIOSSettingConsoleLanguage = 0,
    CemuIOSSettingVSync,
    CemuIOSSettingGX2DrawDoneSync,
    CemuIOSSettingAsyncCompile,
    CemuIOSSettingUpscaleFilter,
    CemuIOSSettingDownscaleFilter,
    CemuIOSSettingFullscreenScaling,
    CemuIOSSettingPlayBootSound,
    CemuIOSSettingTVVolume,
    CemuIOSSettingPadVolume,
    CemuIOSSettingInputVolume,
    CemuIOSSettingAudioDelay,
    CemuIOSSettingOverlayPosition,
    CemuIOSSettingOverlayFPS,
    CemuIOSSettingOverlayDrawCalls,
    CemuIOSSettingOverlayCPUUsage,
    CemuIOSSettingOverlayRAMUsage,
    CemuIOSSettingShaderNotifications,
    CemuIOSSettingMetalFXSpatial,
    CemuIOSSettingExperimentalMaxFPS,
    CemuIOSSettingExperimentalFrameGeneration,
} CemuIOSSetting;

int32_t CemuBridge_GetSetting(CemuIOSSetting setting);
void CemuBridge_SetSetting(CemuIOSSetting setting, int32_t value);
/// Temporarily mutes live game output without changing the saved volume settings.
void CemuBridge_SetAppAudioMuted(bool muted);
/// Re-activates the current iOS audio route and restarts live Cemu streams.
void CemuBridge_RestoreAudioOutput(void);

/// Correct a controller-owned audio route without restarting live game streams.
void CemuBridge_RestoreControllerAudioRoute(void);
/// Prevents suspended wall time from being added to the emulated guest clock.
void CemuBridge_SetAppSuspended(bool suspended);
void CemuBridge_SetControllerMotionEnabled(bool enabled);
void CemuBridge_RecenterControllerMotion(void);
/// 0 = undetermined, 1 = denied/restricted, 2 = granted.
int32_t CemuBridge_GetMicrophonePermissionStatus(void);
void CemuBridge_RequestMicrophonePermission(void);

typedef struct {
    uint32_t persistentId;
    int32_t selectedService;
    bool accountValid;
    bool onlineFilesValid;
    bool pretendoReady;
} CemuOnlineServiceInfo;

typedef struct {
    uint32_t persistentId;
    char miiName[64];
    char accountId[64];
    bool isOnlineValid;
    bool isActive;
    int32_t selectedService;
} CemuAccountItem;

/// Returns readiness for Cemu's existing online-service implementation.
CemuOnlineServiceInfo CemuBridge_GetOnlineServiceInfo(void);
/// Accepts 0 (Offline) or 2 (Pretendo). Changes apply on the next game launch.
bool CemuBridge_SetOnlineService(int32_t service);
const char* _Nonnull CemuBridge_GetOnlineAccountName(void);
const char* _Nonnull CemuBridge_GetOnlineServiceStatus(void);

/// Returns total number of accounts discovered in MLC.
uint32_t CemuBridge_GetAccountCount(void);
/// Fetches account info at index.
bool CemuBridge_GetAccountAtIndex(uint32_t index, CemuAccountItem* _Nonnull outItem);
/// Selects active account by persistentId.
bool CemuBridge_SelectAccount(uint32_t persistentId);
/// Deletes account by persistentId (when more than 1 account exists).
bool CemuBridge_DeleteAccount(uint32_t persistentId);

typedef struct {
    uint64_t titleId;
    uint64_t shaderCacheBytes;
    uint64_t pipelineCacheBytes;
    int32_t metalAllocatedMB;
    int32_t metalRecommendedMB;
    uint32_t memoryWarningCount;
    uint32_t cleanupRequests;
    uint32_t cleanupCompleted;
    uint32_t lastCleanupDeletedTextures;
} CemuPerformanceCacheInfo;

typedef struct {
    bool increasedMemoryLimitActive;
    bool entitlementPresent;
    bool entitlementStatusKnown;
    bool inLiveContainer;
    char signedApplicationIdentifier[256];
    uint64_t availableMemoryBytes;
    uint64_t currentFootprintBytes;
    uint64_t totalLimitBytes;
    uint64_t physicalMemoryBytes;
} CemuMemoryLimitInfo;

/// Returns current process memory limit status and whether Increased Memory Limit is active.
CemuMemoryLimitInfo CemuBridge_GetMemoryLimitInfo(void);

/// Records an iPadOS memory warning and schedules conservative texture cleanup
/// at the next renderer-thread frame-end cleanup point.
void CemuBridge_HandleMemoryWarning(void);

/// Clears 77U shader/pipeline records for the given titleId before title launch.
/// Save data, game files, and user progress are completely untouched.
bool CemuBridge_ClearShaderCacheForTitle(uint64_t titleId);

/// Clears all 77U shader/pipeline records before title launch; false on any error.
/// Save data, game files, and user progress are completely untouched.
bool CemuBridge_ClearAllShaderCaches(void);

#if defined(__OBJC__)
/// Executes a block safely, catching both Objective-C NSException and C++ exceptions.
/// Returns YES on success, NO if an exception was caught (and logged to os_log).
/// Swift imports this as: CemuBridge_PerformBlockSafely(() -> Void) -> Bool
BOOL CemuBridge_PerformBlockSafely(void (NS_NOESCAPE ^_Nonnull block)(void));
#endif

// Existing user-supplied Wii U boot sound; empty when no title provides one.
const char* _Nonnull CemuBridge_GetStartupSoundPath(void);

#ifdef __cplusplus
}
#endif
