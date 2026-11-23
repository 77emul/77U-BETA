#include "config/ActiveSettings.h"
#include "Cafe/Filesystem/fsc.h"
#include "Cafe/Filesystem/fscDeviceHostFS.h"

#include "Common/FileStream.h"
#include <limits>

#if defined(_WIN32)
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

/* FSCVirtualFile implementation for HostFS */

FSCVirtualFile_Host::~FSCVirtualFile_Host()
{
	if (m_type == FSC_TYPE_FILE)
		delete m_fs;
}

sint32 FSCVirtualFile_Host::fscGetType()
{
	return m_type;
}

uint32 FSCVirtualFile_Host::fscDeviceHostFSFile_getFileSize()
{
	if (m_type == FSC_TYPE_FILE)
	{
		if (m_fileSize > 0xFFFFFFFFULL)
			cemu_assert_suspicious(); // files larger than 4GB are not supported by Wii U filesystem
		return (uint32)m_fileSize;
	}
	else if (m_type == FSC_TYPE_DIRECTORY)
	{
		// todo
		return (uint32)0;
	}
	return 0;
}

uint64 FSCVirtualFile_Host::fscQueryValueU64(uint32 id)
{
	if (m_type == FSC_TYPE_FILE)
	{
		if (id == FSC_QUERY_SIZE)
			return fscDeviceHostFSFile_getFileSize();
		else if (id == FSC_QUERY_WRITEABLE)
			return m_isWritable;
		else
			cemu_assert_unimplemented();
	}
	else if (m_type == FSC_TYPE_DIRECTORY)
	{
		if (id == FSC_QUERY_SIZE)
			return fscDeviceHostFSFile_getFileSize();
		else
			cemu_assert_unimplemented();
	}
	cemu_assert_unimplemented();
	return 0;
}

uint32 FSCVirtualFile_Host::fscWriteData(void* buffer, uint32 size)
{
	if (m_type != FSC_TYPE_FILE)
		return 0;
	if (size >= (2UL * 1024UL * 1024UL * 1024UL))
	{
		cemu_assert_suspicious();
		return 0;
	}
	sint32 writtenBytes = m_fs->writeData(buffer, (sint32)size);
	m_seek += (uint64)writtenBytes;
	m_fileSize = std::max(m_fileSize, m_seek);
	return (uint32)writtenBytes;
}

uint32 FSCVirtualFile_Host::fscReadData(void* buffer, uint32 size)
{
	if (m_type != FSC_TYPE_FILE)
		return 0;
	if (size >= (2UL * 1024UL * 1024UL * 1024UL))
	{
		cemu_assert_suspicious();
		return 0;
	}
	uint32 bytesLeft = (uint32)(m_fileSize - m_seek);
	bytesLeft = std::min(bytesLeft, 0x7FFFFFFFu);
	sint32 bytesToRead = std::min(bytesLeft, size);
	uint32 bytesRead = m_fs->readData(buffer, bytesToRead);
	m_seek += bytesRead;
	return bytesRead;
}

void FSCVirtualFile_Host::fscSetSeek(uint64 seek)
{
	if (m_type != FSC_TYPE_FILE)
		return;
	this->m_seek = seek;
	cemu_assert_debug(seek <= m_fileSize);
	m_fs->SetPosition(seek);
}

uint64 FSCVirtualFile_Host::fscGetSeek()
{
	if (m_type != FSC_TYPE_FILE)
		return 0;
	return m_seek;
}

bool FSCVirtualFile_Host::fscFlush()
{
	if (m_type != FSC_TYPE_FILE || !m_fs)
		return false;
	// Read-only handles have no pending writes and cannot use FlushFileBuffers on Windows.
	return !m_isWritable || m_fs->Flush();
}

bool FSCVirtualFile_Host::fscSetFileLength(uint64 endOffset)
{
	if (m_type != FSC_TYPE_FILE)
		return false;
	m_fs->SetPosition(endOffset);
	const bool success = m_fs->SetEndOfFile();
	if (success)
	{
		m_seek = std::min(m_seek, endOffset);
		m_fileSize = endOffset;
	}
	m_fs->SetPosition(m_seek);
	if (!success)
		cemuLog_log(LogType::Force, "fscSetFileLength: Failed to set size to 0x{:x}", endOffset);
	return success;
}

bool FSCVirtualFile_Host::fscDirNext(FSCDirEntry* dirEntry)
{
	if (m_type != FSC_TYPE_DIRECTORY)
		return false;

	if (!m_dirIterator)
	{
		// init iterator on first iteration attempt
		m_dirIterator.reset(new fs::directory_iterator(*m_path));
		if (!m_dirIterator)
		{
			cemuLog_log(LogType::Force, "Failed to iterate directory: {}", _pathToUtf8(*m_path));
			return false;
		}
	}
	if (*m_dirIterator == fs::end(*m_dirIterator))
		return false;

	const fs::directory_entry& entry = **m_dirIterator;
	
	std::string fileName = entry.path().filename().generic_string();
	if (fileName.size() >= sizeof(dirEntry->path) - 1)
		fileName.resize(sizeof(dirEntry->path) - 1);
	strncpy(dirEntry->path, fileName.data(), sizeof(dirEntry->path));
	if (entry.is_directory())
	{
		dirEntry->isDirectory = true;
		dirEntry->isFile = false;
		dirEntry->fileSize = 0;
	}
	else
	{
		dirEntry->isDirectory = false;
		dirEntry->isFile = true;
		const uintmax_t fileSize = entry.file_size();
		dirEntry->fileSize = static_cast<uint32>(std::min<uintmax_t>(fileSize, std::numeric_limits<uint32>::max()));
	}

	(*m_dirIterator)++;
	return true;
}

static fs::path ResolveCaseInsensitive(const fs::path& inputPath)
{
	std::error_code ec;
	if (fs::exists(inputPath, ec))
		return inputPath;

	fs::path resolved = inputPath.root_path();
	for (const auto& element : inputPath.relative_path())
	{
		fs::path candidate = resolved / element;
		if (fs::exists(candidate, ec))
		{
			resolved = candidate;
			continue;
		}
		bool found = false;
		if (fs::is_directory(resolved, ec))
		{
			std::string elemStr = element.string();
			for (const auto& dirEntry : fs::directory_iterator(resolved, ec))
			{
				std::string filename = dirEntry.path().filename().string();
				if (filename.size() == elemStr.size() && strcasecmp(filename.c_str(), elemStr.c_str()) == 0)
				{
					resolved = dirEntry.path();
					found = true;
					break;
				}
			}
		}
		if (!found)
			resolved = candidate;
	}
	return resolved;
}

FSCVirtualFile* FSCVirtualFile_Host::OpenFile(const fs::path& path, FSC_ACCESS_FLAG accessFlags, sint32& fscStatus)
{
	if (!HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_FILE) && !HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_DIR))
		cemu_assert_debug(false); // not allowed. At least one of both flags must be set

	// Exact paths are overwhelmingly common. Avoid a stat for every component
	// and only pay for case-insensitive traversal after the native open fails.
	fs::path resolvedPath = path;

	// attempt to open as file
	if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_FILE))
	{
		FileStream* fs{};
		bool writeAccessRequested = HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::WRITE_PERMISSION);
		if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::FILE_ALLOW_CREATE))
		{
			fs = FileStream::openFile2(resolvedPath, writeAccessRequested);
			if (!fs)
			{
				resolvedPath = ResolveCaseInsensitive(path);
				if (resolvedPath != path)
					fs = FileStream::openFile2(resolvedPath, writeAccessRequested);
			}
			if (!fs)
			{
				cemu_assert_debug(writeAccessRequested);
				fs = FileStream::createFile2(resolvedPath);
				if (!fs)
				{
					std::error_code ec;
					fs::create_directories(resolvedPath.parent_path(), ec);
					fs = FileStream::createFile2(resolvedPath);
				}
				if (!fs)
					cemuLog_log(LogType::Force, "FSC: File create failed for {}", _pathToUtf8(resolvedPath));
			}
		}
		else if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::FILE_ALWAYS_CREATE))
		{
			resolvedPath = ResolveCaseInsensitive(path);
			fs = FileStream::createFile2(resolvedPath);
			if (!fs)
			{
				std::error_code ec;
				fs::create_directories(resolvedPath.parent_path(), ec);
				fs = FileStream::createFile2(resolvedPath);
			}
			if (!fs)
				cemuLog_log(LogType::Force, "FSC: File create failed for {}", _pathToUtf8(resolvedPath));
		}
		else
		{
			fs = FileStream::openFile2(resolvedPath, writeAccessRequested);
			if (!fs)
			{
				resolvedPath = ResolveCaseInsensitive(path);
				if (resolvedPath != path)
					fs = FileStream::openFile2(resolvedPath, writeAccessRequested);
			}
		}
		if (fs)
		{
			FSCVirtualFile_Host* vf = new FSCVirtualFile_Host(FSC_TYPE_FILE);
			vf->m_fs = fs;
			vf->m_isWritable = writeAccessRequested;
			vf->m_fileSize = fs->GetSize();
			fscStatus = FSC_STATUS_OK;
			return vf;
		}
	}

	// attempt to open as directory
	if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_DIR))
	{
		std::error_code ec;
		bool isExistingDir = fs::is_directory(resolvedPath, ec);
		if (!isExistingDir)
		{
			resolvedPath = ResolveCaseInsensitive(path);
			isExistingDir = fs::is_directory(resolvedPath, ec);
		}
		if (isExistingDir)
		{
			FSCVirtualFile_Host* vf = new FSCVirtualFile_Host(FSC_TYPE_DIRECTORY);
			vf->m_path.reset(new std::filesystem::path(resolvedPath));
			fscStatus = FSC_STATUS_OK;
			return vf;
		}
	}
	fscStatus = FSC_STATUS_FILE_NOT_FOUND;
	return nullptr;
}


/* Device implementation */

class fscDeviceHostFSC : public fscDeviceC
{
public:
	FSCVirtualFile* fscDeviceOpenByPath(std::string_view path, FSC_ACCESS_FLAG accessFlags, void* ctx, sint32* fscStatus) override
	{
		*fscStatus = FSC_STATUS_OK;
		FSCVirtualFile* vf = FSCVirtualFile_Host::OpenFile(_utf8ToPath(path), accessFlags, *fscStatus);
		cemu_assert_debug((bool)vf == (*fscStatus == FSC_STATUS_OK));
		return vf;
	}

	bool fscDeviceCreateDir(std::string_view path, void* ctx, sint32* fscStatus) override
	{
		fs::path dirPath = _utf8ToPath(path);
		std::error_code checkEc;
		if (fs::exists(dirPath, checkEc) && !checkEc)
		{
			std::error_code dirEc;
			if (!fs::is_directory(dirPath, dirEc) || dirEc)
				cemuLog_log(LogType::Force, "CreateDir: {} already exists but is not a directory", path);
			*fscStatus = FSC_STATUS_ALREADY_EXISTS;
			return false;
		}
		std::error_code ec;
		bool r = fs::create_directories(dirPath, ec);
		if (!r)
			cemuLog_log(LogType::Force, "CreateDir: Failed to create {}", path);
		*fscStatus = FSC_STATUS_OK;
		return true;
	}

	bool fscDeviceRemoveFileOrDir(std::string_view path, void* ctx, sint32* fscStatus) override
	{
		*fscStatus = FSC_STATUS_OK;
		fs::path _path = _utf8ToPath(path);
		std::error_code ec;
		if (!fs::exists(_path, ec))
		{
			*fscStatus = FSC_STATUS_FILE_NOT_FOUND;
			return false;
		}
		if (!fs::remove(_path, ec))
		{
			cemu_assert_unimplemented(); // return correct error (e.g. if directory is non-empty)
			*fscStatus = FSC_STATUS_FILE_NOT_FOUND;
		}
		return true;
	}

	bool fscDeviceRename(std::string_view srcPath, std::string_view dstPath, void* ctx, sint32* fscStatus) override
	{
		*fscStatus = FSC_STATUS_OK;
		fs::path _srcPath = _utf8ToPath(srcPath);
		fs::path _dstPath = _utf8ToPath(dstPath);
		std::error_code ec;
		if (!fs::exists(_srcPath, ec))
		{
			*fscStatus = FSC_STATUS_FILE_NOT_FOUND;
			return false;
		}
		fs::rename(_srcPath, _dstPath, ec);
		return true;
	}

	// singleton
public:
	static fscDeviceHostFSC& instance()
	{
		static fscDeviceHostFSC _instance;
		return _instance;
	}
};

bool FSCDeviceHostFS_Mount(std::string_view mountPath, std::string_view hostTargetPath, sint32 priority)
{
	return fsc_mount(mountPath, hostTargetPath, &fscDeviceHostFSC::instance(), nullptr, priority) == FSC_STATUS_OK;
}
