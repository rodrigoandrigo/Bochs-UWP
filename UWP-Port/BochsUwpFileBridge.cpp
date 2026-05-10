#include "pch.h"
#include "BochsUwpFileBridge.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <map>
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
		unsigned long long position;
		bool writable;
	};

	std::mutex g_fileMutex;
	std::map<int, UwpFileHandle> g_files;
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
		reinterpret_cast<IInspectable *>(buffer)->QueryInterface(IID_PPV_ARGS(&byteAccess));

		unsigned char *bytes = nullptr;
		byteAccess->Buffer(&bytes);
		return bytes;
	}

	int StoreHandle(IRandomAccessStream^ stream, bool writable)
	{
		std::lock_guard<std::mutex> lock(g_fileMutex);
		int handle = g_nextHandle++;
		g_files[handle] = UwpFileHandle{ stream, 0, writable };
		return handle;
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
			std::lock_guard<std::mutex> lock(g_fileMutex);
			auto it = g_files.find(handle);
			if (it != g_files.end())
			{
				it->second.position = stream->Size;
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
	IRandomAccessStream^ stream = nullptr;
	bool writable = false;
	{
		std::lock_guard<std::mutex> lock(g_fileMutex);
		auto it = g_files.find(handle);
		if (it == g_files.end())
		{
			return;
		}
		stream = it->second.stream;
		writable = it->second.writable;
		g_files.erase(it);
	}

	try
	{
		if (writable)
		{
			create_task(stream->FlushAsync()).get();
		}
	}
	catch (...)
	{
	}
}

extern "C" long long bx_uwp_file_seek(int handle, long long offset, int whence)
{
	try
	{
		std::lock_guard<std::mutex> lock(g_fileMutex);
		auto it = g_files.find(handle);
		if (it == g_files.end())
		{
			return -1;
		}

		long long base = 0;
		if (whence == SEEK_CUR)
		{
			base = static_cast<long long>(it->second.position);
		}
		else if (whence == SEEK_END)
		{
			base = static_cast<long long>(it->second.stream->Size);
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

		it->second.stream->Seek(static_cast<unsigned long long>(position));
		it->second.position = static_cast<unsigned long long>(position);
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
		std::lock_guard<std::mutex> lock(g_fileMutex);
		auto it = g_files.find(handle);
		if (it == g_files.end())
		{
			return -1;
		}

		unsigned int bytesRequested = static_cast<unsigned int>((std::min)(count, static_cast<size_t>(UINT_MAX)));
		IBuffer^ buffer = ref new Buffer(bytesRequested);
		it->second.stream->Seek(it->second.position);
		buffer = create_task(it->second.stream->ReadAsync(buffer, bytesRequested, InputStreamOptions::None)).get();

		unsigned int bytesRead = buffer->Length;
		if (bytesRead > 0)
		{
			memcpy(buf, GetBufferBytes(buffer), bytesRead);
			it->second.position += bytesRead;
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
		std::lock_guard<std::mutex> lock(g_fileMutex);
		auto it = g_files.find(handle);
		if (it == g_files.end() || !it->second.writable)
		{
			return -1;
		}

		unsigned int bytesRequested = static_cast<unsigned int>((std::min)(count, static_cast<size_t>(UINT_MAX)));
		IBuffer^ buffer = ref new Buffer(bytesRequested);
		buffer->Length = bytesRequested;
		memcpy(GetBufferBytes(buffer), buf, bytesRequested);

		it->second.stream->Seek(it->second.position);
		unsigned int bytesWritten = create_task(it->second.stream->WriteAsync(buffer)).get();
		it->second.position += bytesWritten;
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
		std::lock_guard<std::mutex> lock(g_fileMutex);
		auto it = g_files.find(handle);
		if (it == g_files.end() || !it->second.writable)
		{
			return -1;
		}
		create_task(it->second.stream->FlushAsync()).get();
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
		std::lock_guard<std::mutex> lock(g_fileMutex);
		auto it = g_files.find(handle);
		if (it == g_files.end() || !it->second.writable)
		{
			return -1;
		}
		it->second.stream->Size = size;
		if (it->second.position > size)
		{
			it->second.position = size;
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
		std::lock_guard<std::mutex> lock(g_fileMutex);
		auto it = g_files.find(handle);
		if (it == g_files.end())
		{
			return -1;
		}
		*fsize = it->second.stream->Size;
		return 0;
	}
	catch (...)
	{
		return -1;
	}
}
