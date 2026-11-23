#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace SaveManagerIOS
{
struct SaveInfo
{
	bool success{};
	bool saveExists{};
	bool hasUserSaveData{};
	bool rollbackExists{};
	uint64_t totalBytes{};
	int64_t modifiedUnixSeconds{};
	std::filesystem::path latestRollbackPath;
	std::string error;
};

struct OperationResult
{
	bool success{};
	std::filesystem::path outputPath;
	std::filesystem::path rollbackPath;
	std::string error;
};

SaveInfo GetSaveInfo(uint64_t titleId);
OperationResult ExportSave(uint64_t titleId, const std::string& titleName, const std::filesystem::path& outputPath);
OperationResult RestoreSave(uint64_t titleId, const std::filesystem::path& archivePath);
OperationResult RestoreRollback(uint64_t titleId);
OperationResult DeleteSave(uint64_t titleId);
}
