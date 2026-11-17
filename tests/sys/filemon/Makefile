PACKAGE=	tests

TESTSDIR=	${TESTSBASE}/sys/filemon

ATF_TESTS_C+=	filemon_test
CFLAGS+=	-I${SRCTOP}/sys
TEST_METADATA+=	required_files="/dev/filemon"
TEST_METADATA+=	timeout="15"

.include <bsd.test.mk>
