# SPDX-License-Identifier: BSD-2-Clause

atf_test_case automatic_fat
automatic_fat_head()
{
	atf_set "descr" "Automatically sized FATs cover the advertised data area"
	atf_set "require.progs" "makefs fsck_msdosfs"
}
automatic_fat_body()
{
	local spec fat size spc bps

	mkdir -p input/subdir
	printf 'FAT geometry payload\n' > input/subdir/payload.txt
	# Include the 64 MiB FAT32 boundary and multi-sector cluster alignment.
	for spec in '12 1m 1 512' '16 16m 4 512' '32 64m 1 512' \
	    '12 8m 1 4096' '16 64m 2 4096' '32 288m 1 4096' \
	    '32 128m 2 512'; do
		set -- ${spec}
		fat=$1 size=$2 spc=$3 bps=$4
		atf_check -s exit:0 -o ignore -e ignore makefs -t msdos \
		    -o fat_type=${fat},sectors_per_cluster=${spc},bytes_per_sector=${bps} \
		    -s ${size} image input
		atf_check -s exit:0 -o ignore -e empty fsck_msdosfs -n image
		rm image
	done
}

atf_test_case explicit_small_fat
explicit_small_fat_head()
{
	atf_set "descr" "An explicit small FAT limits the advertised filesystem size"
	atf_set "require.progs" "makefs fsck_msdosfs"
}
explicit_small_fat_body()
{
	mkdir empty
	atf_check -s exit:0 -o ignore \
	    -e match:'sectors/FAT limits file system' makefs -t msdos \
	    -o fat_type=32,sectors_per_cluster=1,sectors_per_fat=1008 \
	    -s 64m image empty
	atf_check -s exit:0 -o ignore -e empty fsck_msdosfs -n image
}

atf_test_case rejected_geometry
rejected_geometry_head()
{
	atf_set "descr" "makefs reports a formatter geometry failure to its caller"
	atf_set "require.progs" "makefs"
}
rejected_geometry_body()
{
	mkdir empty
	atf_check -s not-exit:0 -o ignore -e match:'too few clusters' \
	    makefs -t msdos -o fat_type=32 -s 64m image empty
}

atf_init_test_cases()
{
	atf_add_test_case automatic_fat
	atf_add_test_case explicit_small_fat
	atf_add_test_case rejected_geometry
}
