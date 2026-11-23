#include "Common/unix/FileStream_unix.h"
#include <cstdarg>
#include <sys/stat.h>
#include <unistd.h>

fs::path findPathCI(const fs::path& path)
{
#if defined(__APPLE__)
	// On Apple platforms (iOS, iPadOS, macOS), APFS and HFS+ are case-insensitive by default.
	// Bypassing redundant path lookups avoids unnecessary directory walks and prevents
	// fatal EPERM / filesystem_error crashes on sandboxed or restricted paths.
	return path;
#else
	try
	{
		std::error_code ec;
		if (fs::exists(path, ec) || ec)
			return path;

		fs::path fName = path.filename();
		fs::path parentPath = path.parent_path();
		if (parentPath.empty())
			parentPath = ".";
		else
		{
			std::error_code parentEc;
			if (!fs::exists(parentPath, parentEc) && !parentEc)
				parentPath = findPathCI(parentPath);
		}

		std::error_code listErr;
		for (auto&& dirEntry : fs::directory_iterator(parentPath, listErr))
		{
			if (listErr)
				break;
			if (boost::iequals(dirEntry.path().filename().string(), fName.string()))
				return dirEntry.path();
		}

		return parentPath / fName;
	}
	catch (...)
	{
		return path;
	}
#endif
}

FileStream* FileStream::openFile(std::string_view path)
{
	return openFile2(path, false);
}

FileStream* FileStream::openFile(const wchar_t* path, bool allowWrite)
{
	return openFile2(path, allowWrite);
}

FileStream* FileStream::openFile2(const fs::path& path, bool allowWrite)
{
	FileStream* fs = new FileStream(path, true, allowWrite);
	if (fs->m_isValid)
		return fs;
	delete fs;
	return nullptr;
}

FileStream* FileStream::createFile(const wchar_t* path)
{
	return createFile2(path);
}

FileStream* FileStream::createFile(std::string_view path)
{
	return createFile2(path);
}

FileStream* FileStream::createFile2(const fs::path& path)
{
	FileStream* fs = new FileStream(path, false, false);
	if (fs->m_isValid)
		return fs;
	delete fs;
	return nullptr;
}

std::optional<std::vector<uint8>> FileStream::LoadIntoMemory(const fs::path& path)
{
	FileStream* fs = openFile2(path);
	if (!fs)
		return std::nullopt;
	uint64 fileSize = fs->GetSize();
	if (fileSize > 0xFFFFFFFFull)
	{
		delete fs;
		return std::nullopt;
	}
	std::optional<std::vector<uint8>> v(fileSize);
	if (fs->readData(v->data(), (uint32)fileSize) != (uint32)fileSize)
	{
		delete fs;
		return std::nullopt;
	}
	delete fs;
	return v;
}

void FileStream::SetPosition(uint64 pos)
{
	cemu_assert(m_isValid);
	if (pos > static_cast<uint64>(std::numeric_limits<off_t>::max()))
		return;
	// Explicit seeks must recover from short reads at EOF.
	std::clearerr(m_fileStream);
	fseeko(m_fileStream, static_cast<off_t>(pos), SEEK_SET);
}

uint64 FileStream::GetSize()
{
	cemu_assert(m_isValid);
	// fstat leaves the cursor and read-buffer state untouched. Flush buffered
	// writes first so size includes bytes already acknowledged to the guest.
	if (m_prevOperationWasWrite && !Flush())
		return 0;
	struct stat info{};
	if (fstat(fileno(m_fileStream), &info) != 0 || info.st_size < 0)
		return 0;
	return static_cast<uint64>(info.st_size);
}

bool FileStream::SetEndOfFile()
{
	if (!m_isValid || !m_isWritable || !Flush())
		return false;
	const auto position = ftello(m_fileStream);
	if (position < 0)
		return false;
	// Truncate the open file, not its old path: games may rename an open save.
	return ftruncate(fileno(m_fileStream), position) == 0;
}

void FileStream::extract(std::vector<uint8>& data)
{
	uint64 fileSize = GetSize();
	SetPosition(0);
	data.resize(fileSize);
	uint64 offset = 0;
	while (offset < fileSize)
	{
		const uint32 chunkSize = static_cast<uint32>(std::min<uint64>(
			fileSize - offset, std::numeric_limits<uint32>::max()));
		const uint32 bytesRead = readData(data.data() + offset, chunkSize);
		if (bytesRead != chunkSize)
		{
			data.resize(offset + bytesRead);
			break;
		}
		offset += bytesRead;
	}
}

bool FileStream::Flush()
{
	return m_isValid && std::fflush(m_fileStream) == 0 && !std::ferror(m_fileStream);
}

uint32 FileStream::readData(void* data, uint32 length)
{
	if (!SyncReadWriteSeek(false))
		return 0;
	return static_cast<uint32>(std::fread(data, 1, length, m_fileStream));
}

bool FileStream::readU64(uint64& v)
{
	return readData(&v, sizeof(uint64)) == sizeof(uint64);
}

bool FileStream::readU32(uint32& v)
{
	return readData(&v, sizeof(uint32)) == sizeof(uint32);
}

bool FileStream::readU8(uint8& v)
{
	return readData(&v, sizeof(uint8)) == sizeof(uint8);
}

bool FileStream::readLine(std::string& line)
{
	line.clear();
	uint8 c;
	bool isEOF = true;
	while (readU8(c))
	{
		isEOF = false;
		if (c == '\r')
			continue;
		if (c == '\n')
			break;
		line.push_back((char)c);
	}
	return !isEOF;
}

sint32 FileStream::writeData(const void* data, sint32 length)
{
	if (!m_isWritable || length < 0 || !SyncReadWriteSeek(true))
		return 0;
	return static_cast<sint32>(std::fwrite(data, 1, static_cast<size_t>(length), m_fileStream));
}

void FileStream::writeU64(uint64 v)
{
	writeData(&v, sizeof(uint64));
}

void FileStream::writeU32(uint32 v)
{
	writeData(&v, sizeof(uint32));
}

void FileStream::writeU8(uint8 v)
{
	writeData(&v, sizeof(uint8));
}

void FileStream::writeStringFmt(const char* format, ...)
{
	char buffer[2048];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	writeData(buffer, (sint32)strlen(buffer));
}

void FileStream::writeString(const char* str)
{
	writeData(str, (sint32)strlen(str));
}

void FileStream::writeLine(const char* str)
{
	writeData(str, (sint32)strlen(str));
	writeData("\r\n", 2);
}

FileStream::~FileStream()
{
	if (m_fileStream)
		std::fclose(m_fileStream);
}

FileStream::FileStream(const fs::path& path, bool isOpen, bool isWriteable)
{
	const auto CIPath = findPathCI(path);
	m_isWritable = !isOpen || isWriteable;
	m_fileStream = std::fopen(CIPath.c_str(), isOpen ? (isWriteable ? "r+b" : "rb") : "w+b");
	if (!m_fileStream)
		return;
	struct stat info{};
	if (fstat(fileno(m_fileStream), &info) != 0 || S_ISDIR(info.st_mode))
	{
		std::fclose(m_fileStream);
		m_fileStream = nullptr;
		return;
	}
	m_isValid = true;
}

bool FileStream::SyncReadWriteSeek(bool nextOpIsWrite)
{
	if (!m_isValid)
		return false;
	if (nextOpIsWrite == m_prevOperationWasWrite)
		return !std::ferror(m_fileStream);
	// C update streams require a positioning operation when switching from
	// reading to writing. This also clears EOF without moving the guest cursor.
	if (fseeko(m_fileStream, 0, SEEK_CUR) != 0)
		return false;
	m_prevOperationWasWrite = nextOpIsWrite;
	return !std::ferror(m_fileStream);
}
