sbin_PROGRAMS = ser2net
ACLOCAL_AMFLAGS = -I m4
AM_CFLAGS=-Wall -I$(top_srcdir)
ser2net_SOURCES = controller.c dataxfer.c readconfig.c \
	ser2net.c led.c led_sysfs.c devio_devcfg.c devio_sol.c
ser2net_LDADD = $(top_builddir)/utils/libutils.a \
		$(top_builddir)/genio/libgenio.a $(OPENSSL_LIBS)
noinst_HEADERS = controller.h dataxfer.h readconfig.h \
	ser2net.h led.h led_sysfs.h devio.h
man_MANS = ser2net.8
EXTRA_DIST = $(man_MANS) ser2net.conf ser2net.spec ser2net.init \
	linux-serial-echo/serialsim.c linux-serial-echo/Makefile

SUBDIRS = utils genio tests

DIST_SUBDIRS = $(SUBDIRS)
