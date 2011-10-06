# need this for Doxygen integration
include $(top_srcdir)/aminclude.am

AM_CPPFLAGS = -I${srcdir}/include -I${srcdir}/tests -I${srcdir}/generated
AM_CFLAGS = -Wall -Werror 
AM_CXXFLAGS = -Wall $(USEIPV6)

LIB_LDFLAGS = -no-undefined -version-info 2

if WANT_SASL
SASL_CFLAGS = -DSASL
SASL_LIBS = -lsasl2
SASL_SRC = src/zk_sasl.c
endif

pkginclude_HEADERS = include/zookeeper.h include/zookeeper_version.h include/zookeeper_log.h include/proto.h include/recordio.h generated/zookeeper.jute.h
EXTRA_DIST=LICENSE

HASHTABLE_SRC = src/hashtable/hashtable_itr.h src/hashtable/hashtable_itr.c \
    src/hashtable/hashtable_private.h src/hashtable/hashtable.h src/hashtable/hashtable.c

noinst_LTLIBRARIES = libhashtable.la
libhashtable_la_SOURCES = $(HASHTABLE_SRC)

COMMON_SRC = src/zookeeper.c include/zookeeper.h include/zookeeper_version.h include/zookeeper_log.h\
    src/recordio.c include/recordio.h include/proto.h \
    src/zk_adaptor.h generated/zookeeper.jute.c \
    src/zookeeper_log.h src/zk_log.c src/zk_hashtable.h src/zk_hashtable.c

# These are the symbols (classes, mostly) we want to export from our library.
EXPORT_SYMBOLS = '(zoo_|zookeeper_|zhandle|Z|format_log_message|log_message|logLevel|deallocate_|zerror|is_unrecoverable)'
noinst_LTLIBRARIES += libzkst.la
libzkst_la_SOURCES =$(COMMON_SRC) $(SASL_SRC) src/st_adaptor.c
libzkst_la_CFLAGS = $(SASL_CFLAGS)
libzkst_la_LIBADD = -lm

lib_LTLIBRARIES = libzookeeper_st.la
libzookeeper_st_la_SOURCES =
libzookeeper_st_la_LIBADD=libzkst.la libhashtable.la
libzookeeper_st_la_DEPENDENCIES=libzkst.la libhashtable.la
libzookeeper_st_la_LDFLAGS = $(LIB_LDFLAGS) -export-symbols-regex $(EXPORT_SYMBOLS) $(SASL_LIBS)


if WANT_SYNCAPI
noinst_LTLIBRARIES += libzkmt.la
libzkmt_la_SOURCES =$(COMMON_SRC) $(SASL_SRC) src/mt_adaptor.c
libzkmt_la_CFLAGS = -DTHREADED $(SASL_CFLAGS)
libzkmt_la_LIBADD = -lm

lib_LTLIBRARIES += libzookeeper_mt.la
libzookeeper_mt_la_SOURCES =
libzookeeper_mt_la_LIBADD=libzkmt.la libhashtable.la -lpthread $(SASL_LIBS)
libzookeeper_mt_la_DEPENDENCIES=libzkmt.la libhashtable.la
libzookeeper_mt_la_LDFLAGS = $(LIB_LDFLAGS) -export-symbols-regex $(EXPORT_SYMBOLS)
endif

bin_PROGRAMS = cli_st

cli_st_SOURCES = src/cli.c
cli_st_LDADD = libzookeeper_st.la
cli_st_CFLAGS = $(SASL_CFLAGS)

if WANT_SYNCAPI
bin_PROGRAMS += cli_mt load_gen

cli_mt_SOURCES = src/cli.c
cli_mt_LDADD = libzookeeper_mt.la
cli_mt_CFLAGS = -DTHREADED $(SASL_CFLAGS)

load_gen_SOURCES = src/load_gen.c
load_gen_LDADD = libzookeeper_mt.la
load_gen_CFLAGS = -DTHREADED $(SASL_CFLAGS)

endif

#########################################################################
# build and run unit tests

EXTRA_DIST+=$(wildcard ${srcdir}/tests/*.cc) $(wildcard ${srcdir}/tests/*.h) \
    ${srcdir}/tests/wrappers.opt ${srcdir}/tests/wrappers-mt.opt

TEST_SOURCES = tests/TestDriver.cc tests/LibCMocks.cc tests/LibCSymTable.cc \
    tests/MocksBase.cc  tests/ZKMocks.cc tests/Util.cc tests/ThreadingUtil.cc \
    tests/TestClientRetry.cc \
    tests/TestOperations.cc tests/TestZookeeperInit.cc \
    tests/TestZookeeperClose.cc tests/TestClient.cc \
    tests/TestMulti.cc tests/TestWatchers.cc


SYMBOL_WRAPPERS=$(shell cat ${srcdir}/tests/wrappers.opt)

check_PROGRAMS = zktest-st
nodist_zktest_st_SOURCES = $(TEST_SOURCES)
zktest_st_LDADD = libzkst.la libhashtable.la $(CPPUNIT_LIBS) $(SASL_LIBS)
zktest_st_CXXFLAGS = $(CPPUNIT_CFLAGS) $(USEIPV6) $(SASL_CFLAGS)
zktest_st_LDFLAGS = $(SYMBOL_WRAPPERS)

if WANT_SYNCAPI
  check_PROGRAMS += zktest-mt
  nodist_zktest_mt_SOURCES = $(TEST_SOURCES) tests/PthreadMocks.cc
  zktest_mt_LDADD = libzkmt.la libhashtable.la -lpthread $(CPPUNIT_LIBS) $(SASL_LIBS)
  zktest_mt_CXXFLAGS = -DTHREADED $(CPPUNIT_CFLAGS) $(USEIPV6) $(SASL_CFLAGS)
  SYMBOL_WRAPPERS_MT=$(SYMBOL_WRAPPERS) $(shell cat ${srcdir}/tests/wrappers-mt.opt)
  zktest_mt_LDFLAGS = $(SYMBOL_WRAPPERS_MT)
endif

run-check: check
	./zktest-st $(TEST_OPTIONS)
if WANT_SYNCAPI
	./zktest-mt $(TEST_OPTIONS)
endif

clean-local: clean-check
	$(RM) $(DX_CLEANFILES)

clean-check:
	$(RM) $(nodist_zktest_st_OBJECTS) $(nodist_zktest_mt_OBJECTS)
