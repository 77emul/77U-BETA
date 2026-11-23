#pragma once

void ExceptionHandler_Init();

// Apple/iPadOS can provide a pre-opened crash-report destination after its
// sandbox paths are known. Fatal-signal output is mirrored directly to this
// file so it does not depend on the asynchronous normal logger being flushed.
void ExceptionHandler_InstallAltStack();
void ExceptionHandler_SetPersistentCrashLogPath(const char* path);
void ExceptionHandler_SetPersistentCrashLogPaths(const char* primaryPath, const char* secondaryPath);
void ExceptionHandler_BeginPersistentCrashLog(int signalNumber, const void* faultAddress,
	void* const* backtraceEntries, size_t backtraceEntryCount, void* context = nullptr);
void ExceptionHandler_AppendPersistentBacktrace(void* const* backtraceEntries, size_t backtraceEntryCount);
void ExceptionHandler_FlushPersistentCrashLog();
void ExceptionHandler_ActivatePersistentCrashLog();
void ExceptionHandler_RecordFatalError(const char* title, const char* details);

using ExceptionHandlerAdditionalLogCallback = void (*)();
void ExceptionHandler_SetAdditionalLogCallback(ExceptionHandlerAdditionalLogCallback callback);

bool CrashLog_Create();
void CrashLog_SetOutputChannels(bool writeToStdErr, bool writeToLogTxt);
void CrashLog_WriteLine(std::string_view text, bool newLine = true);
void CrashLog_WriteHeader(const char* header);

void ExceptionHandler_LogGeneralInfo();
