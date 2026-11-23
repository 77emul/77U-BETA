#pragma once
#include <chrono>
#include <filesystem>
#include <string>
#include <utility>

namespace CemuIOSDocuments {
// Move known app files only. Keep conflicting versions under a distinct name;
// never overwrite user data or follow directory symlinks during migration.
inline bool movePreserving(const std::filesystem::path& source,
                           const std::filesystem::path& destination)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(source, ec)) return !ec;
    fs::create_directories(destination.parent_path(), ec);
    if (ec) return false;
    if (!fs::exists(destination, ec)) {
        fs::rename(source, destination, ec);
        return !ec;
    }
    if (fs::is_directory(source, ec) && !fs::is_symlink(source, ec) &&
        fs::is_directory(destination, ec) && !fs::is_symlink(destination, ec)) {
        fs::directory_iterator it(source, ec), end;
        if (ec) return false;
        while (it != end) {
            const auto entry = it->path();
            it.increment(ec);
            if (ec || !movePreserving(entry, destination / entry.filename())) return false;
        }
        fs::remove(source, ec);
        return !ec;
    }
    auto preserved = destination;
    for (unsigned n = 1; fs::exists(preserved, ec); ++n)
        preserved = destination.string() + ".previous-" + std::to_string(n);
    if (ec) return false;
    fs::rename(source, preserved, ec);
    return !ec;
}

// Run before either logger opens its file. Keep session boundaries intact even
// when iOS kills the process without delivering a catchable crash signal.
inline std::filesystem::path preservePreviousLogs(const std::filesystem::path& documents)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto logs = documents / "Logs";
    const bool hasRootLog = fs::is_regular_file(documents / "log.txt", ec);
    const bool hasJITLog = fs::is_regular_file(logs / "jit_diagnostics.log", ec);
    const std::pair<fs::path, const char*> sources[] = {
        {documents / "log.txt", "log.txt"},
        {logs / "log.txt", hasRootLog ? "legacy_log.txt" : "log.txt"},
        {logs / "jit_diagnostics.log", "jit_diagnostics.log"},
        {documents / "jit_diagnostics.log", hasJITLog ? "legacy_jit_diagnostics.log" : "jit_diagnostics.log"}
    };
    bool found = false;
    for (const auto& source : sources)
        found |= fs::is_regular_file(source.first, ec);
    if (!found) return {};

    const auto sessions = logs / "Sessions";
    fs::create_directories(sessions, ec);
    if (ec) return {};
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    fs::path directory;
    for (unsigned n = 0;; ++n) {
        directory = sessions / ("session-" + std::to_string(stamp) + "-" + std::to_string(n));
        if (fs::create_directory(directory, ec)) break;
        if (ec) return {};
    }
    bool saved = false;
    for (const auto& source : sources) {
        if (fs::is_regular_file(source.first, ec) &&
            movePreserving(source.first, directory / source.second))
            saved = true;
    }
    return saved ? directory : fs::path{};
}

inline std::filesystem::path prepare(const std::filesystem::path& documents)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const auto* name : {"Logs", "System", "JIT"})
        fs::create_directories(documents / name, ec);
    const auto previousLogs = preservePreviousLogs(documents);
    for (const auto* name : {"log.txt", "jit_diagnostics.log", "last_crash_report.txt",
                             "crash_report.pending", "jit-activation-result.txt"})
        movePreserving(documents / name, documents / "Logs" / name);
    for (const auto* name : {"config", "cache", "title_list_cache.xml"})
        movePreserving(documents / name, documents / "System" / name);
    movePreserving(documents / "universal.js", documents / "JIT" / "universal.js");
    return previousLogs;
}
}
