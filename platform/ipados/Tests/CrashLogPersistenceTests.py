#!/usr/bin/env python3
"""Exercise the production file writer and startup layout across process exits."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[3]
exception_source = (repo / 'src/Common/ExceptionHandler/ExceptionHandler.cpp').read_text()
logging_source = (repo / 'src/Cemu/Logging/CemuLogging.cpp').read_text()


def definition(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


writer = exception_source[exception_source.index('namespace\n{'):
                          exception_source.index('\nvoid ExceptionHandler_InstallAltStack()')]
writer += '\n' + definition(exception_source, 'void ExceptionHandler_SetPersistentCrashLogPaths(')
writer += '\n' + definition(exception_source, 'void ExceptionHandler_FlushPersistentCrashLog(')

fixture = r'''
#include <atomic>
#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <unistd.h>
#include "DocumentsLayout.h"
using uint32 = uint32_t;
using uint64 = uint64_t;
namespace fs = std::filesystem;
namespace ActiveSettings {
    fs::path root;
    fs::path GetUserDataPath(const char* suffix) { return root / suffix; }
}
struct {
    std::mutex log_mutex;
    std::ofstream file_stream;
    std::atomic<bool> threadRunning{false};
    std::thread log_writer;
} LogContext;
bool outputEnabled = true;
bool cemuLog_isOutputEnabled() { return outputEnabled; }
void cemuLog_setOutputEnabled(bool value) { outputEnabled = value; }
void cemuLog_thread() {}
#define cemu_assert_debug(value) assert(value)
'''
fixture += '\n' + writer
fixture += '\n' + definition(logging_source, 'fs::path cemuLog_GetLogFilePath()')
fixture += '\n' + definition(logging_source, 'void cemuLog_createLogFile(')
fixture += r'''
void captureSignal(int sig) {
    s_persistentCrashLogActive = 1;
    PersistentCrashLog_OpenDescriptorsIfNeeded();
    constexpr char report[] = "test fatal signal=11\nregisters persisted before exit\n";
    PersistentCrashLog_Write(report, sizeof(report) - 1);
    ExceptionHandler_FlushPersistentCrashLog();
    _Exit(128 + sig);
}
int main(int argc, char** argv) {
    assert(argc == 3);
    const fs::path documents(argv[1]);
    const std::string mode(argv[2]);
    const auto previous = CemuIOSDocuments::prepare(documents);
    ActiveSettings::root = documents;
    const auto primary = (documents / "Logs/last_crash_report.txt").string();
    const auto pending = (documents / "Logs/crash_report.pending").string();
    ExceptionHandler_SetPersistentCrashLogPaths(primary.c_str(), pending.c_str());
    if (mode == "setup") return 0; // Registering paths must not truncate a report.
    std::cout << previous.string() << std::endl;
    cemuLog_createLogFile(false);
    LogContext.file_stream << mode << " main log\n";
    LogContext.file_stream.flush();
    LogContext.log_writer.join();
    std::ofstream(documents / "Logs/jit_diagnostics.log", std::ios::app)
        << mode << " JIT log\n";
    if (mode == "crash") {
        std::signal(SIGSEGV, captureSignal);
        std::raise(SIGSEGV);
        return 2;
    }
    if (mode == "kill") std::raise(SIGKILL); // No crash handler can run.
}
'''

with tempfile.TemporaryDirectory(prefix='cemu-crash-persistence-') as tmp:
    tmp = Path(tmp)
    source = tmp / 'test.cpp'
    binary = tmp / 'test'
    source.write_text(fixture)
    subprocess.run(['xcrun', 'clang++', '-std=c++20', '-UTARGET_OS_IPHONE', '-DTARGET_OS_IPHONE=1',
                    '-I', str(repo / 'platform/ipados/Bridge'), str(source),
                    '-o', str(binary)], check=True)

    def run(documents, mode):
        return subprocess.run([str(binary), str(documents), mode], capture_output=True, text=True)

    # Each invocation is a fresh process, as on app relaunch after a fault.
    for layout in ['fresh', 'migrated']:
        documents = tmp / layout
        logs = documents / 'Logs'
        logs.mkdir(parents=True)
        if layout == 'migrated':
            (documents / 'log.txt').write_text('old active root log\n')
            (logs / 'log.txt').write_text('older migrated log\n')
            (logs / 'jit_diagnostics.log').write_text('old JIT log\n')
        result = run(documents, 'crash')
        assert result.returncode == 139, result
        report = (logs / 'last_crash_report.txt').read_bytes()
        assert b'signal=11' in report
        assert (logs / 'crash_report.pending').read_bytes() == report
        assert (logs / 'log.txt').read_text() == 'crash main log\n'
        if layout == 'migrated':
            prior = Path(result.stdout.strip())
            assert (prior / 'log.txt').read_text() == 'old active root log\n'
            assert (prior / 'legacy_log.txt').read_text() == 'older migrated log\n'
            assert (prior / 'jit_diagnostics.log').read_text() == 'old JIT log\n'
        result = run(documents, 'restart')
        assert result.returncode == 0, result.stderr
        archive = Path(result.stdout.strip())
        assert (archive / 'log.txt').read_text() == 'crash main log\n'
        assert (archive / 'jit_diagnostics.log').read_text() == 'crash JIT log\n'
        assert (logs / 'log.txt').read_text() == 'restart main log\n'
        assert (logs / 'jit_diagnostics.log').read_text() == 'restart JIT log\n'
        assert (logs / 'last_crash_report.txt').read_bytes() == report
        assert (logs / 'crash_report.pending').read_bytes() == report
        assert run(documents, 'setup').returncode == 0
        assert (logs / 'last_crash_report.txt').read_bytes() == report
        assert (archive / 'log.txt').read_text() == 'crash main log\n'

    # If session archiving fails, the real logger must append, never truncate.
    documents = tmp / 'archive-failure'
    logs = documents / 'Logs'
    logs.mkdir(parents=True)
    (logs / 'Sessions').write_text('blocking file')
    (logs / 'log.txt').write_text('unarchived evidence\n')
    result = run(documents, 'restart')
    assert result.returncode == 0, result.stderr
    assert (logs / 'log.txt').read_text() == 'unarchived evidence\nrestart main log\n'

    # An uncatchable process kill still leaves its written session exportable.
    documents = tmp / 'killed-process'
    result = run(documents, 'kill')
    assert result.returncode == -9, result
    assert not (documents / 'Logs/last_crash_report.txt').exists()
    result = run(documents, 'restart')
    assert result.returncode == 0, result.stderr
    archive = Path(result.stdout.strip())
    assert (archive / 'log.txt').read_text() == 'kill main log\n'
    assert (archive / 'jit_diagnostics.log').read_text() == 'kill JIT log\n'

print('PASS: fatal-signal file writes, uncatchable kill, fresh-process restarts, '
      'current/legacy session separation, persistent crash reports, '
      'and no truncation when archiving fails')
