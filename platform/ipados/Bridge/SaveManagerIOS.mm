#include "SaveManagerIOS.h"

#include "Cafe/TitleList/TitleId.h"
#include "config/ActiveSettings.h"

#include <zip.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

#include <sys/stat.h>

namespace SaveManagerIOS
{
namespace
{
constexpr std::string_view kManifestName = "77u-save.json";
constexpr uint32_t kArchiveVersion = 1;
constexpr zip_int64_t kMaxArchiveEntries = 50000;
constexpr uint64_t kMaxFileBytes = 512ull * 1024ull * 1024ull;
constexpr uint64_t kMaxTotalBytes = 8ull * 1024ull * 1024ull * 1024ull;
constexpr uint64_t kMaxManifestBytes = 64ull * 1024ull;

std::filesystem::path SavePath(uint64_t titleId)
{
	const uint64_t baseTitleId = TitleIdParser::MakeBaseTitleId(titleId);
	// Cemu creates Wii U save directories with lower-case hexadecimal names.
	// iPadOS app containers are case-sensitive, so upper-case formatting makes
	// an existing save look like a different, missing directory.
	return ActiveSettings::GetMlcPath("usr/save/{:08x}/{:08x}",
		static_cast<uint32_t>(baseTitleId >> 32), static_cast<uint32_t>(baseTitleId));
}

std::filesystem::path BackupRoot(uint64_t titleId)
{
	return ActiveSettings::GetUserDataPath("SaveBackups/{:016X}",
		static_cast<uint64_t>(TitleIdParser::MakeBaseTitleId(titleId)));
}

std::string PathString(const std::filesystem::path& path)
{
	return path.generic_string();
}

std::string ZipError(zip_t* archive)
{
	const char* message = archive ? zip_strerror(archive) : nullptr;
	return message ? message : "Unknown ZIP error";
}

std::string EscapeJSON(std::string_view value)
{
	std::string escaped;
	escaped.reserve(value.size());
	for (const unsigned char character : value)
	{
		switch (character)
		{
		case '\\': escaped += "\\\\"; break;
		case '"': escaped += "\\\""; break;
		case '\b': escaped += "\\b"; break;
		case '\f': escaped += "\\f"; break;
		case '\n': escaped += "\\n"; break;
		case '\r': escaped += "\\r"; break;
		case '\t': escaped += "\\t"; break;
		default:
			if (character < 0x20)
			{
				char buffer[7]{};
				std::snprintf(buffer, sizeof(buffer), "\\u%04X", character);
				escaped += buffer;
			}
			else
				escaped += static_cast<char>(character);
			break;
		}
	}
	return escaped;
}

std::string MakeManifest(uint64_t titleId, std::string_view titleName)
{
	titleId = TitleIdParser::MakeBaseTitleId(titleId);
	const auto now = std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	char titleIdString[17]{};
	std::snprintf(titleIdString, sizeof(titleIdString), "%016llX", static_cast<unsigned long long>(titleId));
	return fmt::format(
		"{{\n  \"formatVersion\": {},\n  \"titleId\": \"{}\",\n  \"titleName\": \"{}\",\n  \"createdUnixSeconds\": {},\n  \"source\": \"77U iPadOS\"\n}}\n",
		kArchiveVersion, titleIdString, EscapeJSON(titleName), now);
}

bool IsValidUTF8(std::string_view value)
{
	size_t index = 0;
	while (index < value.size())
	{
		const uint8_t first = static_cast<uint8_t>(value[index]);
		if (first < 0x80)
		{
			++index;
			continue;
		}

		size_t continuationCount = 0;
		uint32_t codePoint = 0;
		if ((first & 0xE0) == 0xC0)
		{
			continuationCount = 1;
			codePoint = first & 0x1F;
			if (codePoint == 0)
				return false;
		}
		else if ((first & 0xF0) == 0xE0)
		{
			continuationCount = 2;
			codePoint = first & 0x0F;
		}
		else if ((first & 0xF8) == 0xF0)
		{
			continuationCount = 3;
			codePoint = first & 0x07;
		}
		else
			return false;

		if (index + continuationCount >= value.size())
			return false;
		for (size_t offset = 1; offset <= continuationCount; ++offset)
		{
			const uint8_t continuation = static_cast<uint8_t>(value[index + offset]);
			if ((continuation & 0xC0) != 0x80)
				return false;
			codePoint = (codePoint << 6) | (continuation & 0x3F);
		}

		if ((continuationCount == 1 && codePoint < 0x80) ||
			(continuationCount == 2 && codePoint < 0x800) ||
			(continuationCount == 3 && codePoint < 0x10000) ||
			codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
			return false;
		index += continuationCount + 1;
	}
	return true;
}

bool IsGameSaveFile(std::string_view relativeName)
{
	if (!relativeName.starts_with("user/"))
		return false;
	relativeName.remove_prefix(sizeof("user/") - 1);
	const size_t separator = relativeName.find('/');
	if (separator == std::string_view::npos || separator + 1 == relativeName.size())
		return false;
	const auto folder = relativeName.substr(0, separator);
	if (folder == "common")
		return true;
	uint32_t accountId = 0;
	const auto parsed = std::from_chars(folder.data(), folder.data() + folder.size(), accountId, 16);
	return folder.size() == 8 && parsed.ec == std::errc{} &&
		parsed.ptr == folder.data() + folder.size();
}

bool ValidateArchivePath(std::string_view name, bool& isDirectory, std::string& normalized, std::string& error)
{
	if (name.empty() || name.front() == '/' || name.front() == '\\' || name.find('\\') != std::string_view::npos ||
		name.find('\0') != std::string_view::npos || !IsValidUTF8(name))
	{
		error = "Archive contains an invalid path";
		return false;
	}

	isDirectory = name.back() == '/';
	if (isDirectory)
		name.remove_suffix(1);
	if (name.empty())
	{
		error = "Archive contains an empty path";
		return false;
	}

	size_t start = 0;
	while (start <= name.size())
	{
		const size_t separator = name.find('/', start);
		const size_t end = separator == std::string_view::npos ? name.size() : separator;
		const auto component = name.substr(start, end - start);
		if (component.empty() || component == "." || component == ".." || component.find(':') != std::string_view::npos)
		{
			error = "Archive contains an unsafe path component";
			return false;
		}
		if (separator == std::string_view::npos)
			break;
		start = separator + 1;
	}

	normalized.assign(name);
	if (normalized != kManifestName && normalized != "save" && !normalized.starts_with("save/"))
	{
		error = "Archive contains data outside its save directory";
		return false;
	}
	return true;
}

bool IsUnsupportedZipEntry(zip_t* archive, zip_uint64_t index, bool isDirectory)
{
	zip_uint8_t operatingSystem = 0;
	zip_uint32_t attributes = 0;
	if (zip_file_get_external_attributes(archive, index, 0, &operatingSystem, &attributes) != 0)
		return true;
	if (operatingSystem != ZIP_OPSYS_UNIX)
		return false;

	const mode_t mode = static_cast<mode_t>(attributes >> 16);
	if (mode == 0)
		return false;
	return isDirectory ? !S_ISDIR(mode) : !S_ISREG(mode);
}

bool ParseManifestTitleId(std::string_view manifest, uint64_t& titleId)
{
	const auto versionKey = manifest.find("\"formatVersion\"");
	if (versionKey == std::string_view::npos)
		return false;
	const auto versionColon = manifest.find(':', versionKey);
	if (versionColon == std::string_view::npos)
		return false;
	const auto versionStart = manifest.find_first_not_of(" \t\r\n", versionColon + 1);
	if (versionStart == std::string_view::npos || manifest[versionStart] != '1')
		return false;
	const auto versionEnd = manifest.find_first_not_of("0123456789", versionStart);
	if (manifest.substr(versionStart, versionEnd - versionStart) != "1")
		return false;

	const auto titleKey = manifest.find("\"titleId\"");
	if (titleKey == std::string_view::npos)
		return false;
	const auto titleColon = manifest.find(':', titleKey);
	const auto openingQuote = titleColon == std::string_view::npos ? std::string_view::npos : manifest.find('"', titleColon + 1);
	const auto closingQuote = openingQuote == std::string_view::npos ? std::string_view::npos : manifest.find('"', openingQuote + 1);
	if (closingQuote == std::string_view::npos || closingQuote - openingQuote != 17)
		return false;

	const auto value = manifest.substr(openingQuote + 1, 16);
	const auto result = std::from_chars(value.data(), value.data() + value.size(), titleId, 16);
	return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

std::filesystem::path UniqueSibling(const std::filesystem::path& parent, std::string_view prefix)
{
	const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	for (uint32_t suffix = 0; suffix < 1000; ++suffix)
	{
		auto candidate = parent / fmt::format("{}-{}-{}", prefix, now, suffix);
		std::error_code ec;
		if (!std::filesystem::exists(candidate, ec) && !ec)
			return candidate;
	}
	return {};
}

int64_t FileTimeToUnixSeconds(std::filesystem::file_time_type value)
{
	const auto systemTime = std::chrono::time_point_cast<std::chrono::seconds>(
		value - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
	return systemTime.time_since_epoch().count();
}
}

SaveInfo GetSaveInfo(uint64_t titleId)
{
	SaveInfo result{};
	if (titleId == 0)
	{
		result.error = "The game does not have a valid title ID";
		return result;
	}

	const auto savePath = SavePath(titleId);
	std::error_code ec;
	result.saveExists = std::filesystem::is_directory(savePath, ec);
	if (ec == std::errc::no_such_file_or_directory)
	{
		// A title legitimately has no save directory until it writes save data
		// for the first time. Treat that as an empty state, not an I/O failure.
		ec.clear();
		result.saveExists = false;
	}
	if (ec)
	{
		result.error = fmt::format("Could not inspect the save directory: {}", ec.message());
		return result;
	}

	if (result.saveExists)
	{
		for (std::filesystem::recursive_directory_iterator iterator(savePath,
			std::filesystem::directory_options::skip_permission_denied, ec), end; iterator != end; iterator.increment(ec))
		{
			if (ec)
			{
				result.error = fmt::format("Could not inspect the complete save: {}", ec.message());
				return result;
			}
			const auto status = iterator->symlink_status(ec);
			if (ec)
			{
				result.error = fmt::format("Could not inspect a save entry: {}", ec.message());
				return result;
			}
			if (std::filesystem::is_regular_file(status))
			{
				const std::string relativeName = PathString(iterator->path().lexically_relative(savePath));
				if (IsGameSaveFile(relativeName))
					result.hasUserSaveData = true;
				const auto size = iterator->file_size(ec);
				if (ec || size > std::numeric_limits<uint64_t>::max() - result.totalBytes)
				{
					result.error = ec ? fmt::format("Could not read a save file size: {}", ec.message()) : "Save size overflow";
					return result;
				}
				result.totalBytes += size;
			}
			const auto modified = iterator->last_write_time(ec);
			if (!ec)
				result.modifiedUnixSeconds = std::max(result.modifiedUnixSeconds, FileTimeToUnixSeconds(modified));
			else
				ec.clear();
		}
	}

	const auto backupRoot = BackupRoot(titleId);
	if (std::filesystem::is_directory(backupRoot, ec) && !ec)
	{
		std::filesystem::file_time_type latestTime{};
		for (const auto& entry : std::filesystem::directory_iterator(backupRoot, ec))
		{
			if (ec)
				break;
			if (!entry.is_directory(ec) || ec)
			{
				ec.clear();
				continue;
			}
			const auto modified = entry.last_write_time(ec);
			if (!ec && (!result.rollbackExists || modified > latestTime))
			{
				result.rollbackExists = true;
				latestTime = modified;
				result.latestRollbackPath = entry.path();
			}
			else
				ec.clear();
		}
	}
	result.success = true;
	return result;
}

OperationResult ExportSave(uint64_t titleId, const std::string& titleName, const std::filesystem::path& outputPath)
{
	OperationResult result{};
	const auto savePath = SavePath(titleId);
	std::error_code ec;
	if (titleId == 0 || !std::filesystem::is_directory(savePath, ec) || ec)
	{
		result.error = "No save data exists for this game";
		return result;
	}
	const auto saveInfo = GetSaveInfo(titleId);
	if (!saveInfo.success || !saveInfo.hasUserSaveData)
	{
		result.error = saveInfo.success ? "The title has metadata but no game save data to export" : saveInfo.error;
		return result;
	}
	if (outputPath.empty())
	{
		result.error = "The export destination is invalid";
		return result;
	}

	std::filesystem::create_directories(outputPath.parent_path(), ec);
	if (ec)
	{
		result.error = fmt::format("Could not create the export directory: {}", ec.message());
		return result;
	}
	std::filesystem::remove(outputPath, ec);
	ec.clear();

	int zipError = 0;
	zip_t* archive = zip_open(PathString(outputPath).c_str(), ZIP_CREATE | ZIP_TRUNCATE, &zipError);
	if (!archive)
	{
		zip_error_t error;
		zip_error_init_with_code(&error, zipError);
		result.error = fmt::format("Could not create the ZIP: {}", zip_error_strerror(&error));
		zip_error_fini(&error);
		return result;
	}

	bool failed = false;
	auto fail = [&](std::string message) {
		if (!failed)
			result.error = std::move(message);
		failed = true;
	};

	const std::string manifest = MakeManifest(titleId, titleName);
	zip_source_t* manifestSource = zip_source_buffer(archive, manifest.data(), manifest.size(), 0);
	if (!manifestSource || zip_file_add(archive, kManifestName.data(), manifestSource, ZIP_FL_ENC_UTF_8) < 0)
	{
		if (manifestSource)
			zip_source_free(manifestSource);
		fail(fmt::format("Could not add the save manifest: {}", ZipError(archive)));
	}
	if (!failed && zip_dir_add(archive, "save", ZIP_FL_ENC_UTF_8) < 0)
		fail(fmt::format("Could not add the save root: {}", ZipError(archive)));

	zip_int64_t exportedEntries = 2;
	uint64_t exportedBytes = 0;
	for (std::filesystem::recursive_directory_iterator iterator(savePath,
		std::filesystem::directory_options::skip_permission_denied, ec), end; !failed && iterator != end; iterator.increment(ec))
	{
		if (ec)
		{
			fail(fmt::format("Could not enumerate the save: {}", ec.message()));
			break;
		}
		const auto status = iterator->symlink_status(ec);
		if (ec || (!std::filesystem::is_directory(status) && !std::filesystem::is_regular_file(status)))
		{
			fail(ec ? fmt::format("Could not inspect a save entry: {}", ec.message()) : "The save contains an unsupported file type");
			break;
		}
		const auto relative = std::filesystem::relative(iterator->path(), savePath, ec);
		if (ec)
		{
			fail(fmt::format("Could not create an archive path: {}", ec.message()));
			break;
		}
		std::string archiveName = "save/" + PathString(relative);
		if (!IsValidUTF8(archiveName) || ++exportedEntries > kMaxArchiveEntries)
		{
			fail("The save contains too many entries or an invalid file name");
			break;
		}
		if (std::filesystem::is_directory(status))
		{
			if (zip_dir_add(archive, archiveName.c_str(), ZIP_FL_ENC_UTF_8) < 0 && zip_error_code_zip(zip_get_error(archive)) != ZIP_ER_EXISTS)
				fail(fmt::format("Could not add a save directory: {}", ZipError(archive)));
		}
		else
		{
			const auto fileSize = iterator->file_size(ec);
			if (ec || fileSize > kMaxFileBytes || fileSize > kMaxTotalBytes - exportedBytes)
			{
				fail(ec ? fmt::format("Could not read a save file size: {}", ec.message()) : "The save is too large to archive safely");
				break;
			}
			exportedBytes += fileSize;
			zip_source_t* source = zip_source_file(archive, PathString(iterator->path()).c_str(), 0, 0);
			if (!source || zip_file_add(archive, archiveName.c_str(), source, ZIP_FL_ENC_UTF_8) < 0)
			{
				if (source)
					zip_source_free(source);
				fail(fmt::format("Could not add a save file: {}", ZipError(archive)));
			}
		}
	}

	if (failed)
	{
		zip_discard(archive);
		std::filesystem::remove(outputPath, ec);
		return result;
	}
	if (zip_close(archive) != 0)
	{
		result.error = fmt::format("Could not finish the ZIP: {}", ZipError(archive));
		zip_discard(archive);
		std::filesystem::remove(outputPath, ec);
		return result;
	}

	result.success = true;
	result.outputPath = outputPath;
	return result;
}

OperationResult RestoreSave(uint64_t titleId, const std::filesystem::path& archivePath)
{
	OperationResult result{};
	if (titleId == 0 || archivePath.empty())
	{
		result.error = "The game or archive is invalid";
		return result;
	}

	int zipError = 0;
	zip_t* archive = zip_open(PathString(archivePath).c_str(), ZIP_RDONLY, &zipError);
	if (!archive)
	{
		zip_error_t error;
		zip_error_init_with_code(&error, zipError);
		result.error = fmt::format("Could not open the ZIP: {}", zip_error_strerror(&error));
		zip_error_fini(&error);
		return result;
	}

	struct Entry
	{
		zip_uint64_t index{};
		std::string normalized;
		uint64_t size{};
		bool directory{};
	};
	std::vector<Entry> entries;
	std::unordered_set<std::string> uniquePaths;
	uint64_t totalBytes = 0;
	std::string manifest;
	bool valid = true;
	bool hasUserSaveData = false;

	const zip_int64_t entryCount = zip_get_num_entries(archive, 0);
	if (entryCount <= 0 || entryCount > kMaxArchiveEntries)
	{
		result.error = "The archive contains an invalid number of entries";
		valid = false;
	}

	for (zip_uint64_t index = 0; valid && index < static_cast<zip_uint64_t>(entryCount); ++index)
	{
		zip_stat_t stat{};
		if (zip_stat_index(archive, index, ZIP_FL_ENC_GUESS, &stat) != 0 || !stat.name)
		{
			result.error = "Could not inspect an archive entry";
			valid = false;
			break;
		}

		bool directory = false;
		std::string normalized;
		if (!ValidateArchivePath(stat.name, directory, normalized, result.error) ||
			!uniquePaths.emplace(normalized).second)
		{
			if (result.error.empty())
				result.error = "The archive contains duplicate paths";
			valid = false;
			break;
		}
		if (IsUnsupportedZipEntry(archive, index, directory))
		{
			result.error = "The archive contains a symbolic link or unsupported file";
			valid = false;
			break;
		}
		if (!directory && (stat.size > kMaxFileBytes || stat.size > kMaxTotalBytes - totalBytes))
		{
			result.error = "The archive is too large";
			valid = false;
			break;
		}
		if (!directory)
		{
			totalBytes += stat.size;
			if (normalized.starts_with("save/") && IsGameSaveFile(std::string_view(normalized).substr(sizeof("save/") - 1)))
				hasUserSaveData = true;
		}

		if (normalized == kManifestName)
		{
			if (directory || stat.size == 0 || stat.size > kMaxManifestBytes)
			{
				result.error = "The save manifest is invalid";
				valid = false;
				break;
			}
			zip_file_t* file = zip_fopen_index(archive, index, 0);
			if (!file)
			{
				result.error = "Could not read the save manifest";
				valid = false;
				break;
			}
			manifest.resize(static_cast<size_t>(stat.size));
			const zip_int64_t read = zip_fread(file, manifest.data(), manifest.size());
			zip_fclose(file);
			if (read != static_cast<zip_int64_t>(manifest.size()))
			{
				result.error = "The save manifest is incomplete";
				valid = false;
				break;
			}
		}
		entries.push_back({index, std::move(normalized), stat.size, directory});
	}

	uint64_t archiveTitleId = 0;
	if (valid && (!ParseManifestTitleId(manifest, archiveTitleId) ||
		TitleIdParser::MakeBaseTitleId(archiveTitleId) != TitleIdParser::MakeBaseTitleId(titleId)))
	{
		result.error = archiveTitleId != 0 ? "This backup belongs to a different game" : "The save manifest is missing or invalid";
		valid = false;
	}
	if (valid && !hasUserSaveData)
	{
		result.error = "The backup contains title metadata but no game save data";
		valid = false;
	}
	if (valid && std::none_of(entries.begin(), entries.end(), [](const Entry& entry) {
		return entry.normalized == "save" || entry.normalized.starts_with("save/");
	}))
	{
		result.error = "The archive does not contain a save directory";
		valid = false;
	}
	if (!valid)
	{
		zip_close(archive);
		return result;
	}

	const auto savePath = SavePath(titleId);
	const auto saveParent = savePath.parent_path();
	std::error_code ec;
	std::filesystem::create_directories(saveParent, ec);
	if (ec)
	{
		result.error = fmt::format("Could not prepare the save directory: {}", ec.message());
		zip_close(archive);
		return result;
	}
	const auto stagingPath = UniqueSibling(saveParent, ".77u-staging");
	if (stagingPath.empty() || !std::filesystem::create_directory(stagingPath, ec) || ec)
	{
		result.error = "Could not create a temporary restore directory";
		zip_close(archive);
		return result;
	}

	std::array<char, 64 * 1024> buffer{};
	for (const auto& entry : entries)
	{
		if (entry.normalized == kManifestName)
			continue;
		if (entry.normalized == "save")
			continue;
		const std::string relativeName = entry.normalized.substr(sizeof("save/") - 1);
		const auto destination = stagingPath / std::filesystem::path(relativeName);
		if (entry.directory)
		{
			std::filesystem::create_directories(destination, ec);
			if (ec)
			{
				result.error = fmt::format("Could not create a restored directory: {}", ec.message());
				break;
			}
			continue;
		}

		std::filesystem::create_directories(destination.parent_path(), ec);
		if (ec)
		{
			result.error = fmt::format("Could not prepare a restored file: {}", ec.message());
			break;
		}
		zip_file_t* input = zip_fopen_index(archive, entry.index, 0);
		std::ofstream output(destination, std::ios::binary | std::ios::trunc);
		if (!input || !output)
		{
			if (input)
				zip_fclose(input);
			result.error = "Could not create a restored save file";
			break;
		}

		uint64_t remaining = entry.size;
		while (remaining > 0)
		{
			const auto request = static_cast<zip_uint64_t>(std::min<uint64_t>(remaining, buffer.size()));
			const zip_int64_t read = zip_fread(input, buffer.data(), request);
			if (read <= 0)
			{
				result.error = "A restored save file was incomplete";
				break;
			}
			output.write(buffer.data(), read);
			if (!output)
			{
				result.error = "Could not write a restored save file";
				break;
			}
			remaining -= static_cast<uint64_t>(read);
		}
		zip_fclose(input);
		if (!result.error.empty())
			break;
	}
	zip_close(archive);

	if (!result.error.empty())
	{
		std::filesystem::remove_all(stagingPath, ec);
		return result;
	}

	// Wii U saves are scoped to an account's persistent ID. A backup from a
	// different installation can contain valid data yet be invisible to the
	// account selected on this device. Remap only when the source is unambiguous.
	const auto userPath = stagingPath / "user";
	const uint32_t activeAccount = ActiveSettings::GetPersistentId() == 0
		? 0x80000001 : ActiveSettings::GetPersistentId();
	const std::string activeFolder = fmt::format("{:08x}", activeAccount);
	std::vector<std::filesystem::path> accountFolders;
	for (const auto& entry : entries)
	{
		if (entry.directory || !entry.normalized.starts_with("save/user/"))
			continue;
		const std::string_view relative = std::string_view(entry.normalized).substr(sizeof("save/user/") - 1);
		const size_t separator = relative.find('/');
		if (separator == std::string_view::npos)
			continue;
		const std::string_view folder = relative.substr(0, separator);
		if (folder == "common")
			continue;
		uint32_t accountId = 0;
		const auto parsed = std::from_chars(folder.data(), folder.data() + folder.size(), accountId, 16);
		if (folder.size() != 8 || parsed.ec != std::errc{} || parsed.ptr != folder.data() + folder.size())
		{
			result.error = "The backup has an unsupported Wii U account folder";
			std::filesystem::remove_all(stagingPath, ec);
			return result;
		}
		const auto folderPath = userPath / std::string(folder);
		if (std::find(accountFolders.begin(), accountFolders.end(), folderPath) == accountFolders.end())
			accountFolders.push_back(folderPath);
	}
	const auto activePath = userPath / activeFolder;
	if (std::find(accountFolders.begin(), accountFolders.end(), activePath) == accountFolders.end() &&
		!accountFolders.empty())
	{
		if (accountFolders.size() != 1)
		{
			result.error = "The backup contains saves for multiple Wii U accounts; select the matching account before restoring";
			std::filesystem::remove_all(stagingPath, ec);
			return result;
		}
		if (std::filesystem::exists(activePath, ec) && !ec && std::filesystem::is_empty(activePath, ec) && !ec)
			std::filesystem::remove(activePath, ec);
		if (ec)
		{
			result.error = fmt::format("Could not prepare the selected Wii U account folder: {}", ec.message());
			std::filesystem::remove_all(stagingPath, ec);
			return result;
		}
		std::filesystem::rename(accountFolders.front(), activePath, ec);
		if (ec)
		{
			result.error = fmt::format("Could not assign the save to the selected Wii U account: {}", ec.message());
			std::filesystem::remove_all(stagingPath, ec);
			return result;
		}
	}

	std::filesystem::path rollbackPath;
	const bool existingSave = std::filesystem::exists(savePath, ec);
	if (ec)
	{
		result.error = fmt::format("Could not inspect the current save: {}", ec.message());
		std::filesystem::remove_all(stagingPath, ec);
		return result;
	}
	if (existingSave)
	{
		const auto backupRoot = BackupRoot(titleId);
		std::filesystem::create_directories(backupRoot, ec);
		rollbackPath = UniqueSibling(backupRoot, "before-restore");
		if (ec || rollbackPath.empty())
		{
			result.error = "Could not prepare the automatic rollback backup";
			std::filesystem::remove_all(stagingPath, ec);
			return result;
		}
		std::filesystem::rename(savePath, rollbackPath, ec);
		if (ec)
		{
			result.error = fmt::format("Could not preserve the current save: {}", ec.message());
			std::filesystem::remove_all(stagingPath, ec);
			return result;
		}
	}

	std::filesystem::rename(stagingPath, savePath, ec);
	if (ec)
	{
		const std::string replacementError = ec.message();
		std::error_code rollbackError;
		if (existingSave)
			std::filesystem::rename(rollbackPath, savePath, rollbackError);
		if (rollbackError)
			result.error = fmt::format("Restore failed ({}), and the old save could not be moved back ({}). Both folders were retained.", replacementError, rollbackError.message());
		else
			result.error = fmt::format("Restore failed; the original save was restored: {}", replacementError);
		return result;
	}

	result.success = true;
	result.rollbackPath = rollbackPath;
	if (!rollbackPath.empty())
		std::filesystem::last_write_time(rollbackPath, std::filesystem::file_time_type::clock::now(), ec);
	return result;
}

OperationResult RestoreRollback(uint64_t titleId)
{
	OperationResult result{};
	if (titleId == 0)
	{
		result.error = "The game does not have a valid title ID";
		return result;
	}
	const auto info = GetSaveInfo(titleId);
	if (!info.success || !info.rollbackExists)
	{
		result.error = info.error.empty() ? "No automatic rollback is available" : info.error;
		return result;
	}
	const auto backupRoot = BackupRoot(titleId);
	const auto rollback = info.latestRollbackPath;
	std::error_code ec;
	const auto rollbackStatus = std::filesystem::symlink_status(rollback, ec);
	if (ec || !std::filesystem::is_directory(rollbackStatus))
	{
		result.error = "The automatic rollback is not a readable save directory";
		return result;
	}
	const auto canonicalRoot = std::filesystem::weakly_canonical(backupRoot, ec);
	if (ec)
	{
		result.error = "Could not inspect the rollback folder";
		return result;
	}
	const auto canonicalParent = std::filesystem::weakly_canonical(rollback.parent_path(), ec);
	if (ec || canonicalParent != canonicalRoot)
	{
		result.error = "The rollback does not belong to this game";
		return result;
	}
	const auto savePath = SavePath(titleId);
	const auto currentStatus = std::filesystem::symlink_status(savePath, ec);
	if (ec == std::errc::no_such_file_or_directory)
		ec.clear();
	if (ec || (std::filesystem::exists(currentStatus) && !std::filesystem::is_directory(currentStatus)))
	{
		result.error = "The current save path is not a readable directory";
		return result;
	}
	std::filesystem::create_directories(savePath.parent_path(), ec);
	if (ec)
	{
		result.error = fmt::format("Could not prepare the save folder: {}", ec.message());
		return result;
	}
	const bool existingSave = std::filesystem::is_directory(currentStatus);
	if (existingSave)
	{
		result.rollbackPath = UniqueSibling(backupRoot, "before-rollback");
		if (result.rollbackPath.empty())
		{
			result.error = "Could not prepare a backup of the current save";
			return result;
		}
		std::filesystem::rename(savePath, result.rollbackPath, ec);
		if (ec)
		{
			result.error = fmt::format("Could not preserve the current save: {}", ec.message());
			return result;
		}
	}
	std::filesystem::rename(rollback, savePath, ec);
	if (ec)
	{
		const auto replacementError = ec.message();
		std::error_code recoveryError;
		if (existingSave)
			std::filesystem::rename(result.rollbackPath, savePath, recoveryError);
		if (recoveryError)
			result.error = fmt::format("Rollback failed ({}), and the current save could not be moved back ({}). Both backups were retained.", replacementError, recoveryError.message());
		else
		{
			result.rollbackPath.clear();
			result.error = fmt::format("Rollback failed; your current save was kept: {}", replacementError);
		}
		return result;
	}
	if (existingSave)
		std::filesystem::last_write_time(result.rollbackPath, std::filesystem::file_time_type::clock::now(), ec);
	result.success = true;
	return result;
}

OperationResult DeleteSave(uint64_t titleId)
{
	OperationResult result{};
	if (titleId == 0)
	{
		result.error = "The game does not have a valid title ID";
		return result;
	}
	const auto savePath = SavePath(titleId);
	std::error_code ec;
	const auto status = std::filesystem::symlink_status(savePath, ec);
	if (ec == std::errc::no_such_file_or_directory)
	{
		result.success = true;
		return result;
	}
	if (ec || !std::filesystem::is_directory(status))
	{
		result.error = ec ? fmt::format("Could not inspect the save: {}", ec.message())
			: "The save path is not a directory";
		return result;
	}
	std::filesystem::remove_all(savePath, ec);
	if (ec)
	{
		result.error = fmt::format("Could not completely delete the save: {}", ec.message());
		return result;
	}
	result.success = true;
	return result;
}
}
