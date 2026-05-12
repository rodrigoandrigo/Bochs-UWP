#include "pch.h"
#include "BochsUwpFileBridge.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <robuffer.h>
#include <string>

using namespace concurrency;
using namespace Platform;
using namespace Microsoft::WRL;
using namespace Windows::Storage;
using namespace Windows::Storage::AccessCache;
using namespace Windows::Storage::Streams;

namespace
{
	struct UwpFileHandle
	{
		IRandomAccessStream^ stream;
		IBuffer^ readBuffer;
		IBuffer^ writeBuffer;
		unsigned int readBufferCapacity;
		unsigned int writeBufferCapacity;
		unsigned long long position;
		bool writable;
		std::mutex ioMutex;
	};

	const unsigned int MaxTransferBytes = 4u * 1024u * 1024u;
	std::mutex g_fileMutex;
	std::map<int, std::shared_ptr<UwpFileHandle>> g_files;
	int g_nextHandle = 1;

	std::wstring Utf8ToWide(const char *value)
	{
		if (value == nullptr || value[0] == 0)
		{
			return std::wstring();
		}

		int length = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
		if (length <= 1)
		{
			return std::wstring();
		}

		std::wstring result(static_cast<size_t>(length - 1), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, value, -1, &result[0], length);
		return result;
	}

	std::wstring ExtractFutureAccessToken(const char *uri)
	{
		const char prefix[] = "uwp://";
		if (uri == nullptr || strncmp(uri, prefix, sizeof(prefix) - 1) != 0)
		{
			return std::wstring();
		}

		const char *tokenStart = uri + sizeof(prefix) - 1;
		const char *tokenEnd = strchr(tokenStart, '/');
		std::string token = tokenEnd
			? std::string(tokenStart, tokenEnd)
			: std::string(tokenStart);
		return Utf8ToWide(token.c_str());
	}

	bool StartsWith(const char *value, const char *prefix)
	{
		if (value == nullptr || prefix == nullptr)
		{
			return false;
		}
		return strncmp(value, prefix, strlen(prefix)) == 0;
	}

	StorageFile^ ResolveStorageFile(const char *uri)
	{
		std::wstring token = ExtractFutureAccessToken(uri);
		if (!token.empty())
		{
			return create_task(
				StorageApplicationPermissions::FutureAccessList->GetFileAsync(ref new String(token.c_str()))).get();
		}

		std::wstring path = Utf8ToWide(uri);
		if (path.empty())
		{
			return nullptr;
		}

		if (StartsWith(uri, "file:///"))
		{
			path = Utf8ToWide(uri + 8);
		}
		return create_task(StorageFile::GetFileFromPathAsync(ref new String(path.c_str()))).get();
	}

	unsigned char *GetBufferBytes(IBuffer^ buffer)
	{
		ComPtr<IBufferByteAccess> byteAccess;
		if (FAILED(reinterpret_cast<IInspectable *>(buffer)->QueryInterface(IID_PPV_ARGS(&byteAccess))))
		{
			return nullptr;
		}

		unsigned char *bytes = nullptr;
		byteAccess->Buffer(&bytes);
		return bytes;
	}

	int StoreHandle(IRandomAccessStream^ stream, bool writable)
	{
		std::shared_ptr<UwpFileHandle> file = std::make_shared<UwpFileHandle>();
		file->stream = stream;
		file->readBuffer = nullptr;
		file->writeBuffer = nullptr;
		file->readBufferCapacity = 0;
		file->writeBufferCapacity = 0;
		file->position = 0;
		file->writable = writable;

		std::lock_guard<std::mutex> lock(g_fileMutex);
		int handle = g_nextHandle++;
		g_files[handle] = file;
		return handle;
	}

	std::shared_ptr<UwpFileHandle> FindHandle(int handle)
	{
		std::lock_guard<std::mutex> lock(g_fileMutex);
		auto it = g_files.find(handle);
		return it == g_files.end() ? std::shared_ptr<UwpFileHandle>() : it->second;
	}

	IBuffer^ EnsureBuffer(IBuffer^ current, unsigned int& capacity, unsigned int bytesRequested)
	{
		if (current == nullptr || capacity < bytesRequested)
		{
			current = ref new Buffer(bytesRequested);
			capacity = bytesRequested;
		}
		return current;
	}

	bool IsWriteRequested(unsigned flags)
	{
		return (flags & (O_WRONLY | O_RDWR)) != 0;
	}

	bool IsTruncateRequested(unsigned flags)
	{
		return (flags & O_TRUNC) != 0;
	}

	bool IsAppendRequested(unsigned flags)
	{
		return (flags & O_APPEND) != 0;
	}
}

extern "C" int bx_uwp_file_open(const char *uri, unsigned flags, unsigned long long *fsize)
{
	try
	{
		StorageFile^ file = ResolveStorageFile(uri);
		if (file == nullptr)
		{
			return -1;
		}

		IRandomAccessStream^ stream = nullptr;
		bool writable = IsWriteRequested(flags);
		try
		{
			stream = create_task(file->OpenAsync(writable ? FileAccessMode::ReadWrite : FileAccessMode::Read)).get();
		}
		catch (...)
		{
			if (writable)
			{
				return -1;
			}
			writable = false;
			stream = create_task(file->OpenAsync(FileAccessMode::Read)).get();
		}

		if (writable && IsTruncateRequested(flags))
		{
			stream->Size = 0;
		}

		if (fsize != nullptr)
		{
			*fsize = stream->Size;
		}

		int handle = StoreHandle(stream, writable);
		if (IsAppendRequested(flags))
		{
			std::shared_ptr<UwpFileHandle> file = FindHandle(handle);
			if (file)
			{
				std::lock_guard<std::mutex> ioLock(file->ioMutex);
				file->position = stream->Size;
			}
		}
		return handle;
	}
	catch (...)
	{
		return -1;
	}
}

extern "C" void bx_uwp_file_close(int handle)
{
	std::shared_ptr<UwpFileHandle> file;
	{
		std::lock_guard<std::mutex> lock(g_fileMutex);
		auto it = g_files.find(handle);
		if (it == g_files.end())
		{
			return;
		}
		file = it->second;
		g_files.erase(it);
	}

	try
	{
		std::lock_guard<std::mutex> ioLock(file->ioMutex);
		if (file->writable)
		{
			create_task(file->stream->FlushAsync()).get();
		}
		file->stream = nullptr;
		file->readBuffer = nullptr;
		file->writeBuffer = nullptr;
	}
	catch (...)
	{
	}
}

extern "C" long long bx_uwp_file_seek(int handle, long long offset, int whence)
{
	try
	{
		std::shared_ptr<UwpFileHandle> file = FindHandle(handle);
		if (!file)
		{
			return -1;
		}
		std::lock_guard<std::mutex> ioLock(file->ioMutex);

		long long base = 0;
		if (whence == SEEK_CUR)
		{
			base = static_cast<long long>(file->position);
		}
		else if (whence == SEEK_END)
		{
			base = static_cast<long long>(file->stream->Size);
		}
		else if (whence != SEEK_SET)
		{
			return -1;
		}

		long long position = base + offset;
		if (position < 0)
		{
			return -1;
		}

		file->stream->Seek(static_cast<unsigned long long>(position));
		file->position = static_cast<unsigned long long>(position);
		return position;
	}
	catch (...)
	{
		return -1;
	}
}

extern "C" intptr_t bx_uwp_file_read(int handle, void *buf, size_t count)
{
	if (buf == nullptr || count == 0)
	{
		return 0;
	}

	try
	{
		std::shared_ptr<UwpFileHandle> file = FindHandle(handle);
		if (!file)
		{
			return -1;
		}
		std::lock_guard<std::mutex> ioLock(file->ioMutex);

		unsigned int bytesRequested = static_cast<unsigned int>(
			(std::min)(count, static_cast<size_t>(MaxTransferBytes)));
		file->readBuffer = EnsureBuffer(file->readBuffer, file->readBufferCapacity, bytesRequested);
		file->stream->Seek(file->position);
		IBuffer^ buffer = create_task(file->stream->ReadAsync(
			file->readBuffer, bytesRequested, InputStreamOptions::None)).get();

		unsigned int bytesRead = buffer->Length;
		if (bytesRead > 0)
		{
			unsigned char *bytes = GetBufferBytes(buffer);
			if (bytes == nullptr)
			{
				return -1;
			}
			memcpy(buf, bytes, bytesRead);
			file->position += bytesRead;
		}
		return static_cast<intptr_t>(bytesRead);
	}
	catch (...)
	{
		return -1;
	}
}

extern "C" intptr_t bx_uwp_file_write(int handle, const void *buf, size_t count)
{
	if (buf == nullptr || count == 0)
	{
		return 0;
	}

	try
	{
		std::shared_ptr<UwpFileHandle> file = FindHandle(handle);
		if (!file || !file->writable)
		{
			return -1;
		}
		std::lock_guard<std::mutex> ioLock(file->ioMutex);

		unsigned int bytesRequested = static_cast<unsigned int>(
			(std::min)(count, static_cast<size_t>(MaxTransferBytes)));
		file->writeBuffer = EnsureBuffer(file->writeBuffer, file->writeBufferCapacity, bytesRequested);
		IBuffer^ buffer = file->writeBuffer;
		buffer->Length = bytesRequested;
		unsigned char *bytes = GetBufferBytes(buffer);
		if (bytes == nullptr)
		{
			return -1;
		}
		memcpy(bytes, buf, bytesRequested);

		file->stream->Seek(file->position);
		unsigned int bytesWritten = create_task(file->stream->WriteAsync(buffer)).get();
		file->position += bytesWritten;
		return static_cast<intptr_t>(bytesWritten);
	}
	catch (...)
	{
		return -1;
	}
}

extern "C" int bx_uwp_file_flush(int handle)
{
	try
	{
		std::shared_ptr<UwpFileHandle> file = FindHandle(handle);
		if (!file || !file->writable)
		{
			return -1;
		}
		std::lock_guard<std::mutex> ioLock(file->ioMutex);
		create_task(file->stream->FlushAsync()).get();
		return 0;
	}
	catch (...)
	{
		return -1;
	}
}

extern "C" int bx_uwp_file_set_size(int handle, unsigned long long size)
{
	try
	{
		std::shared_ptr<UwpFileHandle> file = FindHandle(handle);
		if (!file || !file->writable)
		{
			return -1;
		}
		std::lock_guard<std::mutex> ioLock(file->ioMutex);
		file->stream->Size = size;
		if (file->position > size)
		{
			file->position = size;
		}
		return 0;
	}
	catch (...)
	{
		return -1;
	}
}

extern "C" int bx_uwp_file_get_size(int handle, unsigned long long *fsize)
{
	if (fsize == nullptr)
	{
		return -1;
	}

	try
	{
		std::shared_ptr<UwpFileHandle> file = FindHandle(handle);
		if (!file)
		{
			return -1;
		}
		std::lock_guard<std::mutex> ioLock(file->ioMutex);
		*fsize = file->stream->Size;
		return 0;
	}
	catch (...)
	{
		return -1;
	}
}
