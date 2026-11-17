/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Maksym Sobolyev <sobomax@sippysoft.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Once fsync(2) or fdatasync(2) returns, or a write(2) to a descriptor
 * opened with O_SYNC or O_DSYNC returns, the data written must be
 * retrievable after a crash.  When the write extended the file or
 * allocated a block, that includes the new file size and the block
 * pointers leading to the new block, whether they are in the inode or in
 * an indirect block.
 *
 * Each test runs on an md(4)-backed UFS and, right after the synchronous
 * operation returns, reads the metadata and the data straight from the
 * device with libufs.  There is no buffer cache in front of the disk
 * device, so this shows exactly what would survive a crash at that
 * moment.  Nothing else writes the file's metadata within the test's
 * lifetime: the syncer only gets to it after kern.filedelay (30s by
 * default).
 */

#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include <ufs/ufs/dinode.h>
#include <ufs/ffs/fs.h>

#include <atf-c.h>
#include <errno.h>
#include <fcntl.h>
#include <libufs.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define	MNT	"mnt"
#define	MDFILE	"md.unit"
#define	TFILE	MNT "/file"
#define	PATTERN	0xa5

enum sync_method {
	M_FSYNC,
	M_FDATASYNC,
	M_OSYNC,
	M_ODSYNC,
};

/*
 * The file is prepared with "prefill" blocks of data (or "prefill" blocks
 * of hole when "sparse"), synced with fsync(), and then block "lbn" is
 * written with one of the synchronous methods.  If "unsynced" is not -1,
 * that block is written first without syncing, as another descriptor of a
 * process that does not care about durability would.  With O_SYNC and
 * O_DSYNC on soft updates, the open that follows must already have made it
 * durable.
 */
struct scenario {
	int	prefill;
	bool	sparse;
	int	lbn;
	int	unsynced;
};

static const struct scenario s_extend = { 1, false, 1, -1 };
static const struct scenario s_fill_hole = { 4, true, 2, -1 };
/* First block past the direct ones: allocates the indirect block too. */
static const struct scenario s_extend_newindir =
    { UFS_NDADDR, false, UFS_NDADDR, -1 };
/* The indirect block exists, only a pointer in it is added. */
static const struct scenario s_extend_indir =
    { UFS_NDADDR + 1, false, UFS_NDADDR + 1, -1 };
/*
 * With soft updates, an inode written while the allocation of a direct
 * block is incomplete has its size cut back to that block and all the
 * pointers after it cleared, those to the indirect blocks included.
 */
static const struct scenario s_extend_unsynced = { 2, false, 4, 2 };
static const struct scenario s_extend_newindir_unsynced =
    { 2, false, UFS_NDADDR, 2 };

static char mddev[64];

static void
xsystem(const char *fmt, ...)
{
	char cmd[256];
	va_list ap;
	int rv;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	rv = system(cmd);
	ATF_REQUIRE_MSG(rv == 0, "\"%s\" failed: %d", cmd, rv);
}

/*
 * Create a swap-backed md(4), newfs it with the given options and mount it.
 * NULL options mean a file system without soft updates, which newfs(8)
 * enables by default.  The md unit is recorded in MDFILE so that cleanup
 * can find it.
 */
static void
ufs_setup(const char *newfs_opts)
{
	struct statfs sfs;
	FILE *fp;
	size_t len;

	fp = popen("mdconfig -a -t swap -s 64m", "r");
	ATF_REQUIRE(fp != NULL);
	ATF_REQUIRE(fgets(mddev, sizeof(mddev), fp) != NULL);
	ATF_REQUIRE_EQ(0, pclose(fp));
	len = strcspn(mddev, "\n");
	mddev[len] = '\0';
	ATF_REQUIRE(len > 0);

	fp = fopen(MDFILE, "w");
	ATF_REQUIRE(fp != NULL);
	fprintf(fp, "%s\n", mddev);
	ATF_REQUIRE_EQ(0, fclose(fp));

	if (newfs_opts != NULL) {
		xsystem("newfs %s /dev/%s >/dev/null", newfs_opts, mddev);
	} else {
		xsystem("newfs /dev/%s >/dev/null", mddev);
		xsystem("tunefs -n disable /dev/%s >/dev/null", mddev);
	}
	ATF_REQUIRE_EQ(0, mkdir(MNT, 0755));
	xsystem("mount /dev/%s %s", mddev, MNT);
	ATF_REQUIRE_EQ(0, statfs(MNT, &sfs));
	ATF_REQUIRE_EQ_MSG(newfs_opts != NULL,
	    (sfs.f_flags & MNT_SOFTDEP) != 0,
	    "unexpected soft updates state, mount flags %#jx",
	    (uintmax_t)sfs.f_flags);
}

static void
ufs_cleanup(void)
{
	char cmd[128], unit[64];
	FILE *fp;

	(void)unmount(MNT, MNT_FORCE);
	fp = fopen(MDFILE, "r");
	if (fp == NULL)
		return;
	if (fgets(unit, sizeof(unit), fp) != NULL) {
		unit[strcspn(unit, "\n")] = '\0';
		snprintf(cmd, sizeof(cmd), "mdconfig -d -u %s", unit);
		(void)system(cmd);
	}
	fclose(fp);
}

struct ondisk {
	off_t		size;
	ufs2_daddr_t	blkno;		/* 0 if not allocated on disk */
	bool		data_ok;	/* block contents are PATTERN */
};

/*
 * Read inode "ino" from the device and follow its block pointers to
 * logical block "lbn" (a direct block or one in the single indirect block).
 */
static void
ondisk_read(ino_t ino, int lbn, struct ondisk *od)
{
	char dev[80];
	struct uufsd disk;
	union dinodep dp;
	struct fs *fs;
	ufs2_daddr_t ib;
	uint8_t *buf;
	long i;

	ATF_REQUIRE(lbn < UFS_NDADDR + 1024);
	snprintf(dev, sizeof(dev), "/dev/%s", mddev);
	ATF_REQUIRE_MSG(ufs_disk_fillout(&disk, dev) == 0,
	    "ufs_disk_fillout(%s): %s", dev, disk.d_error);
	fs = &disk.d_fs;
	ATF_REQUIRE_MSG(getinode(&disk, &dp, ino) == 0,
	    "getinode(%ju): %s", (uintmax_t)ino, disk.d_error);
	buf = malloc(fs->fs_bsize);
	ATF_REQUIRE(buf != NULL);

	od->size = disk.d_ufs == 1 ? dp.dp1->di_size : dp.dp2->di_size;
	if (lbn < UFS_NDADDR) {
		od->blkno = disk.d_ufs == 1 ? dp.dp1->di_db[lbn] :
		    dp.dp2->di_db[lbn];
	} else {
		ib = disk.d_ufs == 1 ? dp.dp1->di_ib[0] : dp.dp2->di_ib[0];
		od->blkno = 0;
		if (ib != 0) {
			ATF_REQUIRE(bread(&disk, fsbtodb(fs, ib), buf,
			    fs->fs_bsize) == fs->fs_bsize);
			od->blkno = disk.d_ufs == 1 ?
			    __DECONST(ufs1_daddr_t *, buf)[lbn - UFS_NDADDR] :
			    __DECONST(ufs2_daddr_t *, buf)[lbn - UFS_NDADDR];
		}
	}

	od->data_ok = false;
	if (od->blkno != 0) {
		ATF_REQUIRE(bread(&disk, fsbtodb(fs, od->blkno), buf,
		    fs->fs_bsize) == fs->fs_bsize);
		for (i = 0; i < fs->fs_bsize && buf[i] == PATTERN; i++)
			continue;
		od->data_ok = i == fs->fs_bsize;
	}
	free(buf);
	ufs_disk_close(&disk);
}

static void
fill(int fd, off_t off, size_t len)
{
	char *buf;

	buf = malloc(len);
	ATF_REQUIRE(buf != NULL);
	memset(buf, PATTERN, len);
	ATF_REQUIRE_EQ((ssize_t)len, pwrite(fd, buf, len, off));
	free(buf);
}

static void
test_sync(const char *newfs_opts, const struct scenario *sc,
    enum sync_method method)
{
	struct ondisk od;
	struct stat sb;
	off_t bsize, expsize;
	int fd, i, oflags;

	ufs_setup(newfs_opts);
	fd = open(TFILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	ATF_REQUIRE(fd >= 0);
	ATF_REQUIRE_EQ(0, fstat(fd, &sb));
	bsize = sb.st_blksize;
	if (sc->sparse) {
		ATF_REQUIRE_EQ(0, ftruncate(fd, sc->prefill * bsize));
	} else {
		for (i = 0; i < sc->prefill; i++)
			fill(fd, i * bsize, bsize);
	}
	ATF_REQUIRE_EQ(0, fsync(fd));
	expsize = MAX(MAX(sc->prefill, sc->lbn + 1), sc->unsynced + 1) * bsize;

	/* Sanity check the starting point. */
	ondisk_read(sb.st_ino, sc->lbn, &od);
	ATF_REQUIRE_EQ(sc->prefill * bsize, od.size);
	ATF_REQUIRE_EQ(0, od.blkno);

	if (sc->unsynced != -1)
		fill(fd, sc->unsynced * bsize, bsize);

	switch (method) {
	case M_FSYNC:
	case M_FDATASYNC:
		fill(fd, sc->lbn * bsize, bsize);
		if (method == M_FSYNC)
			ATF_REQUIRE_EQ(0, fsync(fd));
		else
			ATF_REQUIRE_EQ(0, fdatasync(fd));
		break;
	case M_OSYNC:
	case M_ODSYNC:
		oflags = method == M_OSYNC ? O_SYNC : O_DSYNC;
		ATF_REQUIRE_EQ(0, close(fd));
		fd = open(TFILE, O_RDWR | oflags);
		ATF_REQUIRE(fd >= 0);
		if (sc->unsynced != -1 && newfs_opts != NULL) {
			/*
			 * With soft updates, the open syncs what was
			 * written before it.
			 */
			ondisk_read(sb.st_ino, sc->unsynced, &od);
			ATF_CHECK_MSG(od.size >= (sc->unsynced + 1) * bsize,
			    "on-disk size is %jd after open",
			    (intmax_t)od.size);
			ATF_CHECK_MSG(od.blkno != 0 && od.data_ok,
			    "unsynced block %d is not on disk after open",
			    sc->unsynced);
		}
		fill(fd, sc->lbn * bsize, bsize);
		break;
	}

	ondisk_read(sb.st_ino, sc->lbn, &od);
	ATF_CHECK_EQ_MSG(expsize, od.size,
	    "on-disk size is %jd, expected %jd", (intmax_t)od.size,
	    (intmax_t)expsize);
	ATF_CHECK_MSG(od.blkno != 0,
	    "on-disk pointer to block %d is 0", sc->lbn);
	ATF_CHECK_MSG(od.blkno == 0 || od.data_ok,
	    "on-disk block %d does not hold the data written", sc->lbn);
	ATF_REQUIRE_EQ(0, close(fd));
}

/*
 * Retaining a regular file under a new name must not turn deferred disk
 * link-count bookkeeping into another user-visible ctime change.  Drive
 * directory dependencies by publishing unrelated inodes, without sleeping.
 */
static void
check_retained_stat(const struct stat *before, const struct stat *after)
{
	ATF_REQUIRE_EQ(before->st_dev, after->st_dev);
	ATF_REQUIRE_EQ(before->st_ino, after->st_ino);
	ATF_REQUIRE_EQ(before->st_mode, after->st_mode);
	ATF_REQUIRE_EQ(before->st_nlink, after->st_nlink);
	ATF_REQUIRE_EQ(before->st_size, after->st_size);
	ATF_REQUIRE_EQ(before->st_mtim.tv_sec, after->st_mtim.tv_sec);
	ATF_REQUIRE_EQ(before->st_mtim.tv_nsec, after->st_mtim.tv_nsec);
	ATF_REQUIRE_EQ_MSG(before->st_ctim.tv_sec, after->st_ctim.tv_sec,
	    "unrelated publication changed retained inode ctime seconds");
	ATF_REQUIRE_EQ_MSG(before->st_ctim.tv_nsec, after->st_ctim.tv_nsec,
	    "unrelated publication changed retained inode ctime nanoseconds");
}

static void
test_retained_ctime(const char *newfs_opts, bool hardlink)
{
	struct stat before, after;
	uint8_t content[2000];
	int fd, dirfd, staged, i;
	size_t j;

	ufs_setup(newfs_opts);
	dirfd = open(MNT, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	ATF_REQUIRE(dirfd >= 0);
	fd = open(TFILE, O_RDWR | O_CREAT | O_EXCL, 0644);
	ATF_REQUIRE(fd >= 0);
	fill(fd, 0, sizeof(content));
	ATF_REQUIRE_EQ(0, fsync(fd));
	if (hardlink) {
		ATF_REQUIRE_EQ(0, link(TFILE, MNT "/custody"));
		ATF_REQUIRE_EQ(0, unlink(TFILE));
	} else {
		ATF_REQUIRE_EQ(0, rename(TFILE, MNT "/custody"));
	}
	ATF_REQUIRE_EQ(0, fsync(dirfd));
	ATF_REQUIRE_EQ(0, fsync(fd));
	ATF_REQUIRE_EQ(0, fstat(fd, &before));
	ATF_REQUIRE_EQ(1, before.st_nlink);
	for (i = 0; i < 50; i++) {
		staged = open(MNT "/new", O_RDWR | O_CREAT | O_EXCL, 0644);
		ATF_REQUIRE(staged >= 0);
		fill(staged, 0, sizeof(content));
		ATF_REQUIRE_EQ(0, fsync(staged));
		ATF_REQUIRE_EQ(0, close(staged));
		ATF_REQUIRE_EQ(0, link(MNT "/new", TFILE));
		ATF_REQUIRE_EQ(0, unlink(MNT "/new"));
		ATF_REQUIRE_EQ(0, fsync(dirfd));
		ATF_REQUIRE_EQ(0, fstat(fd, &after));
		check_retained_stat(&before, &after);
		ATF_REQUIRE_EQ(sizeof(content), pread(fd, content,
		    sizeof(content), 0));
		for (j = 0; j < sizeof(content); j++)
			ATF_REQUIRE_EQ(PATTERN, content[j]);
		ATF_REQUIRE_EQ(0, unlink(TFILE));
		ATF_REQUIRE_EQ(0, fsync(dirfd));
	}
	ATF_REQUIRE_EQ(0, close(fd));
	ATF_REQUIRE_EQ(0, close(dirfd));
}

static void
test_mutation_ctime(const char *newfs_opts)
{
	struct stat before, after;
	int fd;

	ufs_setup(newfs_opts);
	fd = open(TFILE, O_RDWR | O_CREAT | O_EXCL, 0644);
	ATF_REQUIRE(fd >= 0);
	fill(fd, 0, 2000);
	ATF_REQUIRE_EQ(0, fsync(fd));
	ATF_REQUIRE_EQ(0, fstat(fd, &before));
	ATF_REQUIRE_EQ(0, fchmod(fd, 0600));
	ATF_REQUIRE_EQ(0, fstat(fd, &after));
	ATF_REQUIRE(before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
	    before.st_ctim.tv_nsec != after.st_ctim.tv_nsec);
	ATF_REQUIRE_EQ(0600, after.st_mode & 0777);
	before = after;
	ATF_REQUIRE_EQ(0, link(TFILE, MNT "/alias"));
	ATF_REQUIRE_EQ(0, fstat(fd, &after));
	ATF_REQUIRE_EQ(2, after.st_nlink);
	ATF_REQUIRE(before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
	    before.st_ctim.tv_nsec != after.st_ctim.tv_nsec);
	before = after;
	ATF_REQUIRE_EQ(0, unlink(MNT "/alias"));
	ATF_REQUIRE_EQ(0, fstat(fd, &after));
	ATF_REQUIRE_EQ(1, after.st_nlink);
	ATF_REQUIRE(before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
	    before.st_ctim.tv_nsec != after.st_ctim.tv_nsec);
	ATF_REQUIRE_EQ(0, close(fd));
}

static void
ctime_head(atf_tc_t *tc, const char *description)
{
	atf_tc_set_md_var(tc, "descr", "%s", description);
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "require.progs",
	    "mdconfig newfs tunefs mount");
}

ATF_TC_WITH_CLEANUP(ctime_rename_nosu);
ATF_TC_HEAD(ctime_rename_nosu, tc)
{
	ctime_head(tc, "Retained inode rename, nosu");
}
ATF_TC_BODY(ctime_rename_nosu, tc)
{
	test_retained_ctime(NULL, false);
}
ATF_TC_CLEANUP(ctime_rename_nosu, tc)
{
	ufs_cleanup();
}

ATF_TC_WITH_CLEANUP(ctime_unlink_nosu);
ATF_TC_HEAD(ctime_unlink_nosu, tc)
{
	ctime_head(tc, "Retained inode unlink, nosu");
}
ATF_TC_BODY(ctime_unlink_nosu, tc)
{
	test_retained_ctime(NULL, true);
}
ATF_TC_CLEANUP(ctime_unlink_nosu, tc)
{
	ufs_cleanup();
}

ATF_TC_WITH_CLEANUP(ctime_mutation_nosu);
ATF_TC_HEAD(ctime_mutation_nosu, tc)
{
	ctime_head(tc, "Retained inode mutation, nosu");
}
ATF_TC_BODY(ctime_mutation_nosu, tc)
{
	test_mutation_ctime(NULL);
}
ATF_TC_CLEANUP(ctime_mutation_nosu, tc)
{
	ufs_cleanup();
}

ATF_TC_WITH_CLEANUP(ctime_rename_su);
ATF_TC_HEAD(ctime_rename_su, tc)
{
	ctime_head(tc, "Retained inode rename, su");
}
ATF_TC_BODY(ctime_rename_su, tc)
{
	test_retained_ctime("-U", false);
}
ATF_TC_CLEANUP(ctime_rename_su, tc)
{
	ufs_cleanup();
}

ATF_TC_WITH_CLEANUP(ctime_unlink_su);
ATF_TC_HEAD(ctime_unlink_su, tc)
{
	ctime_head(tc, "Retained inode unlink, su");
}
ATF_TC_BODY(ctime_unlink_su, tc)
{
	test_retained_ctime("-U", true);
}
ATF_TC_CLEANUP(ctime_unlink_su, tc)
{
	ufs_cleanup();
}

ATF_TC_WITH_CLEANUP(ctime_mutation_su);
ATF_TC_HEAD(ctime_mutation_su, tc)
{
	ctime_head(tc, "Retained inode mutation, su");
}
ATF_TC_BODY(ctime_mutation_su, tc)
{
	test_mutation_ctime("-U");
}
ATF_TC_CLEANUP(ctime_mutation_su, tc)
{
	ufs_cleanup();
}

ATF_TC_WITH_CLEANUP(ctime_rename_suj);
ATF_TC_HEAD(ctime_rename_suj, tc)
{
	ctime_head(tc, "Retained inode rename, suj");
}
ATF_TC_BODY(ctime_rename_suj, tc)
{
	test_retained_ctime("-j", false);
}
ATF_TC_CLEANUP(ctime_rename_suj, tc)
{
	ufs_cleanup();
}

ATF_TC_WITH_CLEANUP(ctime_unlink_suj);
ATF_TC_HEAD(ctime_unlink_suj, tc)
{
	ctime_head(tc, "Retained inode unlink, suj");
}
ATF_TC_BODY(ctime_unlink_suj, tc)
{
	test_retained_ctime("-j", true);
}
ATF_TC_CLEANUP(ctime_unlink_suj, tc)
{
	ufs_cleanup();
}

ATF_TC_WITH_CLEANUP(ctime_mutation_suj);
ATF_TC_HEAD(ctime_mutation_suj, tc)
{
	ctime_head(tc, "Retained inode mutation, suj");
}
ATF_TC_BODY(ctime_mutation_suj, tc)
{
	test_mutation_ctime("-j");
}
ATF_TC_CLEANUP(ctime_mutation_suj, tc)
{
	ufs_cleanup();
}

#define	UFS_TC(name, opts, sc, method)					\
ATF_TC_WITH_CLEANUP(name);						\
ATF_TC_HEAD(name, tc)							\
{									\
	atf_tc_set_md_var(tc, "descr", "Write " #sc " with " #method	\
	    ", newfs options " #opts);					\
	atf_tc_set_md_var(tc, "require.user", "root");			\
	atf_tc_set_md_var(tc, "require.progs",				\
	    "mdconfig newfs tunefs mount");				\
}									\
ATF_TC_BODY(name, tc)							\
{									\
	test_sync(opts, &sc, method);					\
}									\
ATF_TC_CLEANUP(name, tc)						\
{									\
	ufs_cleanup();							\
}

#define	UFS_TCS_FS(prefix, sc, method)					\
	UFS_TC(prefix ## _nosu, NULL, sc, method)			\
	UFS_TC(prefix ## _su, "-U", sc, method)				\
	UFS_TC(prefix ## _suj, "-j", sc, method)

#define	UFS_TCS(prefix, sc)						\
	UFS_TCS_FS(prefix ## _fsync, sc, M_FSYNC)			\
	UFS_TCS_FS(prefix ## _fdatasync, sc, M_FDATASYNC)		\
	UFS_TCS_FS(prefix ## _osync, sc, M_OSYNC)			\
	UFS_TCS_FS(prefix ## _odsync, sc, M_ODSYNC)

UFS_TCS(extend, s_extend)
UFS_TCS(fill_hole, s_fill_hole)
UFS_TCS(extend_newindir, s_extend_newindir)
UFS_TCS(extend_indir, s_extend_indir)
UFS_TCS(extend_unsynced, s_extend_unsynced)
UFS_TCS(extend_newindir_unsynced, s_extend_newindir_unsynced)

#define	UFS_TCS_ADD_FS(tp, prefix)					\
	ATF_TP_ADD_TC(tp, prefix ## _nosu);				\
	ATF_TP_ADD_TC(tp, prefix ## _su);				\
	ATF_TP_ADD_TC(tp, prefix ## _suj)

#define	UFS_TCS_ADD(tp, prefix)						\
	UFS_TCS_ADD_FS(tp, prefix ## _fsync);				\
	UFS_TCS_ADD_FS(tp, prefix ## _fdatasync);			\
	UFS_TCS_ADD_FS(tp, prefix ## _osync);				\
	UFS_TCS_ADD_FS(tp, prefix ## _odsync)

ATF_TP_ADD_TCS(tp)
{
	UFS_TCS_ADD(tp, extend);
	UFS_TCS_ADD(tp, fill_hole);
	UFS_TCS_ADD(tp, extend_newindir);
	UFS_TCS_ADD(tp, extend_indir);
	UFS_TCS_ADD(tp, extend_unsynced);
	UFS_TCS_ADD(tp, extend_newindir_unsynced);
	ATF_TP_ADD_TC(tp, ctime_rename_nosu);
	ATF_TP_ADD_TC(tp, ctime_rename_su);
	ATF_TP_ADD_TC(tp, ctime_rename_suj);
	ATF_TP_ADD_TC(tp, ctime_unlink_nosu);
	ATF_TP_ADD_TC(tp, ctime_unlink_su);
	ATF_TP_ADD_TC(tp, ctime_unlink_suj);
	ATF_TP_ADD_TC(tp, ctime_mutation_nosu);
	ATF_TP_ADD_TC(tp, ctime_mutation_su);
	ATF_TP_ADD_TC(tp, ctime_mutation_suj);
	return (atf_no_error());
}
