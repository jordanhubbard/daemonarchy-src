/* SPDX-License-Identifier: BSD-2-Clause */
/* Production file-save logic with fake firmware I/O, not disk acceptance. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t EFI_STATUS;
typedef uint64_t UINT64;
typedef size_t UINTN;
typedef uint16_t CHAR16;
#define EFI_SUCCESS 0
#define EFI_NOT_FOUND 1
#define EFI_DEVICE_ERROR 2
#define EFI_INVALID_PARAMETER 3
#define EFI_VOLUME_FULL 4
#define EFI_WRITE_PROTECTED 5
#define EFI_FILE_MODE_READ 1
#define EFI_FILE_MODE_WRITE 2
#define EFI_FILE_MODE_CREATE UINT64_C(0x8000000000000000)
typedef struct mock_file EFI_FILE_PROTOCOL;
struct mock_file {
	EFI_STATUS (*Open)(EFI_FILE_PROTOCOL *, EFI_FILE_PROTOCOL **, CHAR16 *, UINT64, UINT64);
	EFI_STATUS (*Close)(EFI_FILE_PROTOCOL *);
	EFI_STATUS (*Read)(EFI_FILE_PROTOCOL *, UINTN *, void *);
	EFI_STATUS (*Write)(EFI_FILE_PROTOCOL *, UINTN *, void *);
	EFI_STATUS (*Flush)(EFI_FILE_PROTOCOL *);
	int kind, index;
	size_t offset;
};
#define NC_REPORT_FIXTURE
#include "netconsole_report.c"

static EFI_FILE_PROTOCOL root, directory, handle;
static char files[NC_REPORT_SLOTS + 1][128];
static size_t lengths[NC_REPORT_SLOTS + 1];
static int exists[NC_REPORT_SLOTS + 1];
static int marker_ok, marker_present, readonly, writes, flush_error, corrupt;
static int zero_write, read_error, close_error;
static size_t chunk;

static EFI_STATUS
mock_close(EFI_FILE_PROTOCOL *file)
{
	return (close_error && file->kind == 2 ? EFI_DEVICE_ERROR : EFI_SUCCESS);
}

static EFI_STATUS
mock_flush(EFI_FILE_PROTOCOL *file)
{
	(void)file;
	return (flush_error ? EFI_DEVICE_ERROR : EFI_SUCCESS);
}

static EFI_STATUS
mock_read(EFI_FILE_PROTOCOL *file, UINTN *size, void *buffer)
{
	const char *data;
	size_t length;
	if (read_error && file->kind == 2)
		return (EFI_DEVICE_ERROR);
	data = file->kind == 1 ? (marker_ok ? NC_REPORT_MARKER : "wrong marker") : files[file->index];
	length = file->kind == 1 ? strlen(data) : lengths[file->index];
	if (*size > length - file->offset)
		*size = length - file->offset;
	if (*size > chunk)
		*size = chunk;
	memcpy(buffer, data + file->offset, *size);
	if (corrupt && file->kind == 2 && *size != 0)
		((char *)buffer)[0] ^= 1;
	file->offset += *size;
	return (EFI_SUCCESS);
}

static EFI_STATUS
mock_write(EFI_FILE_PROTOCOL *file, UINTN *size, void *buffer)
{
	writes++;
	if (readonly)
		return (EFI_WRITE_PROTECTED);
	if (zero_write)
		*size = 0;
	if (*size > chunk)
		*size = chunk;
	assert(file->offset + *size < sizeof(files[0]));
	memcpy(files[file->index] + file->offset, buffer, *size);
	file->offset += *size;
	lengths[file->index] = file->offset;
	return (EFI_SUCCESS);
}

static EFI_STATUS
mock_open(EFI_FILE_PROTOCOL *parent, EFI_FILE_PROTOCOL **file,
    CHAR16 *wide, UINT64 mode, UINT64 attributes)
{
	char name[64];
	unsigned slot;
	size_t i;
	(void)attributes;
	for (i = 0; wide[i] != 0; i++) {
		assert(i < sizeof(name) - 1);
		name[i] = wide[i];
	}
	name[i] = 0;
	if (parent == &root) {
		if (strcmp(name, "\\DAEMDBG\\PROBE.TAG") == 0) {
			if (!marker_present)
				return (EFI_NOT_FOUND);
			handle.kind = 1;
			handle.offset = 0;
			*file = &handle;
			return (EFI_SUCCESS);
		}
		assert(strcmp(name, "\\DAEMDBG") == 0);
		if (readonly)
			return (EFI_WRITE_PROTECTED);
		*file = &directory;
		return (EFI_SUCCESS);
	}
	assert(parent == &directory && sscanf(name, "BOOT%4u.TXT", &slot) == 1);
	assert(slot > 0 && slot <= NC_REPORT_SLOTS);
	if (!exists[slot] && !(mode & EFI_FILE_MODE_CREATE))
		return (EFI_NOT_FOUND);
	if (mode & EFI_FILE_MODE_CREATE) {
		assert(!exists[slot]); /* Never open an existing report for write. */
		exists[slot] = 1;
	}
	handle.kind = 2;
	handle.index = slot;
	handle.offset = 0;
	*file = &handle;
	return (EFI_SUCCESS);
}

static void
reset(void)
{
	root = (EFI_FILE_PROTOCOL){.Open = mock_open};
	directory = (EFI_FILE_PROTOCOL){.Open = mock_open, .Close = mock_close, .Flush = mock_flush};
	handle = (EFI_FILE_PROTOCOL){.Close = mock_close, .Read = mock_read, .Write = mock_write, .Flush = mock_flush};
	memset(exists, 0, sizeof(exists));
	memset(lengths, 0, sizeof(lengths));
	memset(files, 0, sizeof(files));
	marker_ok = marker_present = 1;
	readonly = writes = flush_error = corrupt = zero_write = read_error = close_error = 0;
	chunk = 3; /* Real short reads/writes must finish, not claim partial success. */
}

int
main(void)
{
	const char data[] = "PCI Ethernet\r\nUNDI status\r\nEND INVENTORY\r\n";
	unsigned slot, i;
	reset();
	assert(nc_report_write(&root, data, strlen(data), &slot) == EFI_SUCCESS && slot == 1);
	assert(lengths[1] == strlen(data) && memcmp(files[1], data, strlen(data)) == 0);
	assert(nc_report_write(&root, data, strlen(data), &slot) == EFI_SUCCESS && slot == 2);
	assert(memcmp(files[1], data, strlen(data)) == 0);
	for (i = 0; i < 7; i++) {
		reset();
		switch (i) {
		case 0: marker_present = 0; break;
		case 1: marker_ok = 0; break;
		case 2: readonly = 1; break;
		case 3: zero_write = 1; break;
		case 4: flush_error = 1; break;
		case 5: corrupt = 1; break;
		case 6: read_error = 1; break;
		}
		slot = 999;
		assert(nc_report_write(&root, data, strlen(data), &slot) != EFI_SUCCESS && slot == 0);
		if (i < 3)
			assert(writes == 0);
	}
	reset();
	for (i = 1; i <= NC_REPORT_SLOTS; i++) exists[i] = 1;
	assert(nc_report_write(&root, data, strlen(data), &slot) == EFI_VOLUME_FULL && writes == 0);
	reset(); close_error = 1;
	assert(nc_report_write(&root, data, strlen(data), &slot) == EFI_DEVICE_ERROR && slot == 0);
	assert(nc_report_write(&root, data, NC_REPORT_LIMIT + 1, &slot) == EFI_INVALID_PARAMETER);
	puts("USB report writer: preservation, bounded slots, short I/O and failure controls passed");
	return (0);
}
