.include <bsd.own.mk>

TESTS_SUBDIRS+=	common

.if ${MACHINE_CPUARCH} == "i386"
TESTS_SUBDIRS+= i386
.endif

.if ${MACHINE_CPUARCH} == "amd64"
TESTS_SUBDIRS+= amd64
.endif

.PATH:		${.CURDIR}/tools
SCRIPTSDIR=	${TESTSDIR}
SCRIPTS=	dtest.sh

ATF_TESTS_SH+=	dtrace_header_test

.include <bsd.test.mk>
