/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Daemonarchy contributors. */

#ifndef NC_REPORT_FIXTURE
#include <stand.h>
#include <efi.h>
#include <Protocol/SimpleFileSystem.h>
#endif
#include "netconsole_report.h"

static void
report_path(CHAR16 *wide, const char *ascii)
{
	while ((*wide++ = (unsigned char)*ascii++) != 0)
		;
}

static EFI_STATUS
report_verify(EFI_FILE_PROTOCOL *file, const char *data, size_t length)
{
	char buffer[256];
	UINTN size;
	size_t offset;
	EFI_STATUS status;

	for (offset = 0; offset < length; offset += size) {
		size = length - offset;
		if (size > sizeof(buffer))
			size = sizeof(buffer);
		status = file->Read(file, &size, buffer);
		if (status != EFI_SUCCESS)
			return (status);
		if (size == 0 || size > sizeof(buffer) || size > length - offset ||
		    memcmp(buffer, data + offset, size) != 0)
			return (EFI_DEVICE_ERROR);
	}
	size = 1;
	status = file->Read(file, &size, buffer);
	return (status != EFI_SUCCESS ? status :
	    size == 0 ? EFI_SUCCESS : EFI_DEVICE_ERROR);
}

EFI_STATUS
nc_report_write(EFI_FILE_PROTOCOL *root, const char *data, size_t length,
    unsigned *saved_slot)
{
	EFI_FILE_PROTOCOL *directory = NULL, *file = NULL;
	EFI_STATUS status, closed;
	CHAR16 path[64];
	char name[16];
	unsigned slot;
	size_t offset;
	UINTN size;

	if (saved_slot == NULL)
		return (EFI_INVALID_PARAMETER);
	*saved_slot = 0;
	if (root == NULL || data == NULL || length == 0 || length > NC_REPORT_LIMIT)
		return (EFI_INVALID_PARAMETER);
	/* Require the purpose-built medium's existing marker before any write. */
	report_path(path, "\\DAEMDBG\\PROBE.TAG");
	status = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
	if (status != EFI_SUCCESS)
		return (status);
	status = report_verify(file, NC_REPORT_MARKER, sizeof(NC_REPORT_MARKER) - 1);
	closed = file->Close(file);
	file = NULL;
	if (status == EFI_SUCCESS)
		status = closed;
	if (status != EFI_SUCCESS)
		return (status);
	report_path(path, "\\DAEMDBG");
	status = root->Open(root, &directory, path,
	    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
	if (status != EFI_SUCCESS)
		return (status);
	for (slot = 1; slot <= NC_REPORT_SLOTS; slot++) {
		snprintf(name, sizeof(name), "BOOT%04u.TXT", slot);
		report_path(path, name);
		status = directory->Open(directory, &file, path, EFI_FILE_MODE_READ, 0);
		if (status == EFI_NOT_FOUND)
			break;
		if (status != EFI_SUCCESS)
			goto done;
		status = file->Close(file);
		file = NULL;
		if (status != EFI_SUCCESS)
			goto done;
	}
	if (slot > NC_REPORT_SLOTS) {
		status = EFI_VOLUME_FULL;
		goto done;
	}
	status = directory->Open(directory, &file, path,
	    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
	if (status != EFI_SUCCESS)
		goto done;
	for (offset = 0; offset < length; offset += size) {
		size = length - offset;
		status = file->Write(file, &size, (void *)(data + offset));
		if (status != EFI_SUCCESS)
			goto done;
		if (size == 0 || size > length - offset) {
			status = EFI_DEVICE_ERROR;
			goto done;
		}
	}
	status = file->Flush(file);
	if (status != EFI_SUCCESS)
		goto done;
	status = file->Close(file);
	file = NULL;
	if (status != EFI_SUCCESS)
		goto done;
	status = directory->Flush(directory);
	if (status != EFI_SUCCESS)
		goto done;
	/* Reopen and verify exact bytes before claiming a successful save. */
	status = directory->Open(directory, &file, path, EFI_FILE_MODE_READ, 0);
	if (status != EFI_SUCCESS)
		goto done;
	status = report_verify(file, data, length);
done:
	if (file != NULL) {
		closed = file->Close(file);
		if (status == EFI_SUCCESS)
			status = closed;
	}
	closed = directory->Close(directory);
	if (status == EFI_SUCCESS)
		status = closed;
	if (status == EFI_SUCCESS)
		*saved_slot = slot;
	return (status);
}
