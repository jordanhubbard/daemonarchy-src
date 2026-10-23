/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _NETCONSOLE_REPORT_H_
#define _NETCONSOLE_REPORT_H_

/* Include EFI and SimpleFileSystem declarations before this header. */
#define NC_REPORT_MARKER "Daemonarchy firmware inventory USB v1\n"
#define NC_REPORT_LIMIT 32768
#define NC_REPORT_SLOTS 999

EFI_STATUS nc_report_write(EFI_FILE_PROTOCOL *, const char *, size_t, unsigned *);
int netconsole_save_report(void);

#endif
