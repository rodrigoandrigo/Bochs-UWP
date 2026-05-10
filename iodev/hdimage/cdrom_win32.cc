/////////////////////////////////////////////////////////////////////////
// $Id$
/////////////////////////////////////////////////////////////////////////
//
//  Copyright (C) 2002-2021  The Bochs Project
//
//  This library is free software; you can redistribute it and/or
//  modify it under the terms of the GNU Lesser General Public
//  License as published by the Free Software Foundation; either
//  version 2 of the License, or (at your option) any later version.
//
//  This library is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//  Lesser General Public License for more details.
//
//  You should have received a copy of the GNU Lesser General Public
//  License along with this library; if not, write to the Free Software
//  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
/////////////////////////////////////////////////////////////////////////

// These are the low-level CDROM functions which are called
// from 'harddrv.cc'.  They effect the OS specific functionality
// needed by the CDROM emulation in 'harddrv.cc'.  Mostly, just
// ioctl() calls and such.  Should be fairly easy to add support
// for your OS if it is not supported yet.

#include "bochs.h"
#if BX_SUPPORT_CDROM

#include "cdrom.h"
#include "cdrom_win32.h"

#define LOG_THIS /* no SMF tricks here, not needed */

extern "C" {
#include <errno.h>
}

#if defined(BX_UWP_CORE_LIBRARY)
extern "C" {
  int bx_uwp_file_open(const char *uri, unsigned flags, Bit64u *fsize);
  void bx_uwp_file_close(int handle);
  Bit64s bx_uwp_file_seek(int handle, Bit64s offset, int whence);
  ssize_t bx_uwp_file_read(int handle, void *buf, size_t count);
}

static bool bx_is_uwp_file_uri(const char *path)
{
  return (path != NULL) && !strncmp(path, "uwp://", 6);
}
#endif

#if defined(WIN32)
// windows.h included by bochs.h
#include <winioctl.h>


static BOOL isWindowsXP;

#define BX_CD_FRAMESIZE 2048
#define CD_FRAMESIZE    2048

// READ_TOC_EX structure(s) and #defines

#define CDROM_READ_TOC_EX_FORMAT_TOC      0x00
#define CDROM_READ_TOC_EX_FORMAT_SESSION  0x01
#define CDROM_READ_TOC_EX_FORMAT_FULL_TOC 0x02
#define CDROM_READ_TOC_EX_FORMAT_PMA      0x03
#define CDROM_READ_TOC_EX_FORMAT_ATIP     0x04
#define CDROM_READ_TOC_EX_FORMAT_CDTEXT   0x05

#define IOCTL_CDROM_BASE              FILE_DEVICE_CD_ROM
#define IOCTL_CDROM_READ_TOC_EX       CTL_CODE(IOCTL_CDROM_BASE, 0x0015, METHOD_BUFFERED, FILE_READ_ACCESS)
#ifndef IOCTL_DISK_GET_LENGTH_INFO
#define IOCTL_DISK_GET_LENGTH_INFO    CTL_CODE(IOCTL_DISK_BASE, 0x0017, METHOD_BUFFERED, FILE_READ_ACCESS)
#endif

typedef struct _CDROM_READ_TOC_EX {
    UCHAR Format    : 4;
    UCHAR Reserved1 : 3; // future expansion
    UCHAR Msf       : 1;
    UCHAR SessionTrack;
    UCHAR Reserved2;     // future expansion
    UCHAR Reserved3;     // future expansion
} CDROM_READ_TOC_EX, *PCDROM_READ_TOC_EX;

typedef struct _TRACK_DATA {
    UCHAR Reserved;
    UCHAR Control : 4;
    UCHAR Adr : 4;
    UCHAR TrackNumber;
    UCHAR Reserved1;
    UCHAR Address[4];
} TRACK_DATA, *PTRACK_DATA;

typedef struct _CDROM_TOC_SESSION_DATA {
    // Header
    UCHAR Length[2];  // add two bytes for this field
    UCHAR FirstCompleteSession;
    UCHAR LastCompleteSession;
    // One track, representing the first track
    // of the last finished session
    TRACK_DATA TrackData[1];
} CDROM_TOC_SESSION_DATA, *PCDROM_TOC_SESSION_DATA;

// End READ_TOC_EX structure(s) and #defines

#include <stdio.h>

BOOL Is_WinXP_SP2_or_Later()
{
   OSVERSIONINFOEX osvi;
   DWORDLONG dwlConditionMask = 0;
   int op = VER_GREATER_EQUAL;

   // Initialize the OSVERSIONINFOEX structure.

   ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));
   osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
   osvi.dwMajorVersion = 5;
   osvi.dwMinorVersion = 1;
   osvi.wServicePackMajor = 2;
   osvi.wServicePackMinor = 0;

   // Initialize the condition mask.

   VER_SET_CONDITION( dwlConditionMask, VER_MAJORVERSION, op );
   VER_SET_CONDITION( dwlConditionMask, VER_MINORVERSION, op );
   VER_SET_CONDITION( dwlConditionMask, VER_SERVICEPACKMAJOR, op );
   VER_SET_CONDITION( dwlConditionMask, VER_SERVICEPACKMINOR, op );

   // Perform the test.

   return VerifyVersionInfo(
      &osvi,
      VER_MAJORVERSION | VER_MINORVERSION |
      VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
      dwlConditionMask);
}

static HANDLE open_image_file_for_read(const char *path)
{
#if defined(WINAPI_FAMILY) && (WINAPI_FAMILY == WINAPI_FAMILY_APP)
  int wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
  UINT code_page = CP_UTF8;
  DWORD flags = MB_ERR_INVALID_CHARS;
  if (wide_len <= 0) {
    code_page = CP_ACP;
    flags = 0;
    wide_len = MultiByteToWideChar(code_page, flags, path, -1, NULL, 0);
  }
  if (wide_len <= 0) {
    return INVALID_HANDLE_VALUE;
  }

  wchar_t *wide_path = new wchar_t[wide_len];
  MultiByteToWideChar(code_page, flags, path, -1, wide_path, wide_len);
  HANDLE handle = CreateFile2(wide_path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, NULL);
  delete [] wide_path;
  return handle;
#else
  return CreateFile((char *)path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
#endif
}

cdrom_win32_c::cdrom_win32_c(const char *dev)
{
  char prefix[6];

  sprintf(prefix, "CD%d", ++bx_cdrom_count);
  put(prefix);
  fd = -1; // File descriptor not yet allocated

  if (dev == NULL) {
    path = NULL;
  } else {
    path = strdup(dev);
  }
  using_file = 0;
  hFile = INVALID_HANDLE_VALUE;
#if defined(BX_UWP_CORE_LIBRARY)
  using_uwp_file = 0;
  uwp_handle = -1;
  uwp_size = 0;
#endif
  isWindowsXP = Is_WinXP_SP2_or_Later();
}

cdrom_win32_c::~cdrom_win32_c(void)
{
  if (fd >= 0) {
#if defined(BX_UWP_CORE_LIBRARY)
    if (using_uwp_file) {
      bx_uwp_file_close(uwp_handle);
      using_uwp_file = 0;
      uwp_handle = -1;
    } else
#endif
    if (hFile != INVALID_HANDLE_VALUE)
      CloseHandle(hFile);
    fd = -1;
  }
}

bool cdrom_win32_c::insert_cdrom(const char *dev)
{
  unsigned char buffer[BX_CD_FRAMESIZE];

  // Load CD-ROM. Returns 0 if CD is not ready.
  if (dev != NULL) path = strdup(dev);
  BX_INFO (("load cdrom with path='%s'", path));
#if defined(BX_UWP_CORE_LIBRARY)
  if (bx_is_uwp_file_uri(path)) {
    uwp_handle = bx_uwp_file_open(path, O_RDONLY, &uwp_size);
    if (uwp_handle < 0) {
      BX_ERROR(("open UWP cd failed for %s", path));
      return 0;
    }
    using_uwp_file = 1;
    using_file = 1;
    fd = 1;
    fd = (read_block(buffer, 0, 2048)) ? 1 : -1;
    if (fd < 0) {
      bx_uwp_file_close(uwp_handle);
      using_uwp_file = 0;
      uwp_handle = -1;
    }
    return (fd == 1);
  }
#endif
  char drive[256];
  if ((path[1] == ':') && (strlen(path) == 2))
  {
    if (isWindowsXP) {
      // Use direct device access under Windows XP or newer

      // With all the backslashes it's hard to see, but to open D: drive
      // the name would be: \\.\d:
      sprintf(drive, "\\\\.\\%s", path);
      BX_INFO (("Using direct access for cdrom."));
      // This trick only works for Win2k and WinNT, so warn the user of that.
      using_file = 0;
    } else {
      BX_ERROR(("Your Windows version is no longer supported for direct access."));
      return 0;
    }
  } else {
    strcpy(drive,path);
    using_file = 1;
    BX_INFO (("Opening image file as a cd"));
  }
  if (using_file) {
    hFile = open_image_file_for_read(drive);
    if (hFile != INVALID_HANDLE_VALUE) {
      fd = 1;
    }
  } else if (isWindowsXP) {
    hFile = CreateFile((char *)&drive, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
      fd = 1;
      if (!using_file) {
        DWORD lpBytesReturned;
        DeviceIoControl(hFile, IOCTL_STORAGE_LOAD_MEDIA, NULL, 0, NULL, 0, &lpBytesReturned, NULL);
      }
    }
  }
  if (fd < 0) {
     BX_ERROR(("open cd failed for %s: %s", path, strerror(errno)));
     return 0;
  }

  // I just see if I can read a sector to verify that a
  // CD is in the drive and readable.
  fd = (read_block(buffer, 0, 2048)) ? 1 : -1;
  return (fd == 1);
}

void cdrom_win32_c::eject_cdrom()
{
  // Logically eject the CD.  I suppose we could stick in
  // some ioctl() calls to really eject the CD as well.

  if (fd >= 0) {
#if defined(BX_UWP_CORE_LIBRARY)
    if (using_uwp_file) {
      bx_uwp_file_close(uwp_handle);
      using_uwp_file = 0;
      uwp_handle = -1;
      uwp_size = 0;
    } else
#endif
    if (using_file == 0) {
      if (isWindowsXP) {
        DWORD lpBytesReturned;
        DeviceIoControl(hFile, IOCTL_STORAGE_EJECT_MEDIA, NULL, 0, NULL, 0, &lpBytesReturned, NULL);
      }
    } else {
      if (hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(hFile);
        hFile = INVALID_HANDLE_VALUE;
      }
    }
    fd = -1;
  }
}

bool cdrom_win32_c::read_toc(Bit8u* buf, int* length, bool msf, int start_track, int format)
{
  // Read CD TOC. Returns 0 if start track is out of bounds.

  if (fd < 0) {
    BX_PANIC(("cdrom: read_toc: file not open."));
    return 0;
  }

  // This is a hack and works okay if there's one rom track only
  if (using_file) {
    return cdrom_base_c::read_toc(buf, length, msf, start_track, format);
  } else if (isWindowsXP) {
    // the implementation below is the platform-dependent code required
    // to read the TOC from a physical cdrom.
    // This only works with WinXP or newer
    CDROM_READ_TOC_EX input;
    memset(&input, 0, sizeof(input));
    input.Format = format;
    input.Msf = msf;
    input.SessionTrack = start_track;

    // We have to allocate a chunk of memory to make sure it is aligned on a sector base.
    UCHAR *data = (UCHAR *) VirtualAlloc(NULL, 2048*2, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    DWORD iBytesReturned;
    DeviceIoControl(hFile, IOCTL_CDROM_READ_TOC_EX, &input, sizeof(input), data, 804, &iBytesReturned, NULL);
    // now copy it to the users buffer and free our buffer
    *length = data[1] + (data[0] << 8) + 2;
    memcpy(buf, data, *length);
    VirtualFree(data, 0, MEM_RELEASE);

    return 1;
  } else {
    return 0;
  }
}

Bit32u cdrom_win32_c::capacity()
{
  // Return CD-ROM capacity.  I believe you want to return
  // the number of blocks of capacity the actual media has.

  if (using_file) {
#if defined(BX_UWP_CORE_LIBRARY)
    if (using_uwp_file) {
      return (Bit32u)(uwp_size / 2048);
    }
#endif
    ULARGE_INTEGER FileSize;
    FileSize.LowPart = GetFileSize(hFile, &FileSize.HighPart);
    return (Bit32u)(FileSize.QuadPart / 2048);
  } else if (isWindowsXP) {  /* direct device access for XP or newer */
    LARGE_INTEGER length;
    DWORD iBytesReturned;
    DeviceIoControl(hFile, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &length, sizeof(length), &iBytesReturned, NULL);
    return (Bit32u)(length.QuadPart / 2048);
  } else {
    return 0;
  }
}

bool BX_CPP_AttrRegparmN(3) cdrom_win32_c::read_block(Bit8u* buf, Bit32u lba, int blocksize)
{
  // Read a single block from the CD

  LARGE_INTEGER pos;
  ssize_t n = 0;
  Bit8u try_count = 3;
  Bit8u* buf1;

  if (blocksize == 2352) {
    memset(buf, 0, 2352);
    memset(buf+1, 0xff, 10);
    Bit32u raw_block = lba + 150;
    buf[12] = (raw_block / 75) / 60;
    buf[13] = (raw_block / 75) % 60;
    buf[14] = (raw_block % 75);
    buf[15] = 0x01;
    buf1 = buf + 16;
  } else {
    buf1 = buf;
  }
  do {
#if defined(BX_UWP_CORE_LIBRARY)
    if (using_uwp_file) {
      Bit64s seek_pos = bx_uwp_file_seek(uwp_handle, (Bit64s)lba * BX_CD_FRAMESIZE, SEEK_SET);
      if (seek_pos < 0) {
        BX_PANIC(("cdrom: read_block: UWP seek returned error."));
      } else {
        n = bx_uwp_file_read(uwp_handle, (void *)buf1, BX_CD_FRAMESIZE);
      }
    } else
#endif
    if (using_file || isWindowsXP) {
      DWORD bytes_read = 0;
      pos.QuadPart = (LONGLONG)lba*BX_CD_FRAMESIZE;
      pos.LowPart = SetFilePointer(hFile, pos.LowPart, &pos.HighPart, SEEK_SET);
      if ((pos.LowPart == 0xffffffff) && (GetLastError() != NO_ERROR)) {
        BX_PANIC(("cdrom: read_block: SetFilePointer returned error."));
      } else {
        ReadFile(hFile, (void *) buf1, BX_CD_FRAMESIZE, &bytes_read, NULL);
        n = bytes_read;
      }
    }
  } while ((n != BX_CD_FRAMESIZE) && (--try_count > 0));

  return (n == BX_CD_FRAMESIZE);
}

#endif /* WIN32 */

#endif /* if BX_SUPPORT_CDROM */
