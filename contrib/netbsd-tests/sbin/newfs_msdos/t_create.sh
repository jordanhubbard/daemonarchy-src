# $NetBSD: t_create.sh,v 1.3 2014/01/05 12:59:03 martin Exp $
#
# Copyright (c) 2012 The NetBSD Foundation, Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
# ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
# TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
# BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.
#

atf_test_case validfat32
validfat32_head() {
	atf_set "descr" "Verifies that fat32 created filesystems are valid"
}
validfat32_body() {

	atf_check -s eq:0 -o ignore -e ignore \
	    newfs_msdos -b 512 -C 33m -F 32 msdos.img
#	fsck_msdos/newfs_msdos have been fixed
#	atf_expect_fail "PR bin/46743"
	atf_check -s eq:0 -o not-match:FIXED -e empty fsck_msdos -p msdos.img
	atf_expect_pass
}


atf_test_case sector_size
sector_size_head() {
	atf_set "descr" "File geometry uses the requested sector size"
}
sector_size_body() {
	local bps sectors

	for bps in 512 1024 2048 4096; do
		atf_check -s exit:0 -o ignore -e ignore \
		    newfs_msdos -C 8m -F 12 -c 4 -S ${bps} msdos.img
		sectors=$(od -An -v -tu1 -j 19 -N 2 msdos.img | \
		    awk '{ print $1 + 256 * $2 }')
		atf_check_equal "${sectors}" "$((8 * 1024 * 1024 / bps))"
		atf_check -s exit:0 -o ignore -e empty \
		    fsck_msdos -n msdos.img
	done
}

atf_init_test_cases() {
	atf_add_test_case validfat32
	atf_add_test_case sector_size
}
