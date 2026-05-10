#pragma once

#include <stdint.h>
#include <stddef.h>

extern "C" {
int bx_uwp_file_open(const char *uri, unsigned flags, unsigned long long *fsize);
void bx_uwp_file_close(int handle);
long long bx_uwp_file_seek(int handle, long long offset, int whence);
intptr_t bx_uwp_file_read(int handle, void *buf, size_t count);
intptr_t bx_uwp_file_write(int handle, const void *buf, size_t count);
int bx_uwp_file_flush(int handle);
int bx_uwp_file_set_size(int handle, unsigned long long size);
int bx_uwp_file_get_size(int handle, unsigned long long *fsize);
}
