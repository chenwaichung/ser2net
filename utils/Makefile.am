
noinst_LIBRARIES = libutils.a

noinst_HEADERS = selector.h utils.h heap.h locking.h telnet.h buffer.h \
		waiter.h uucplock.h

noinst_lib_LTLIBRARIES = libser2net_utils.la
noinst_libdir = $(shell readlink -f $(top_builddir)/dummy_install)

MY_SOURCES = utils.c selector.c telnet.c buffer.c waiter.c uucplock.c

libser2net_utils_la_SOURCES = $(MY_SOURCES)

libutils_a_SOURCES =  $(MY_SOURCES)
libutils_a_CFLAGS = $(AM_CFLAGS)
