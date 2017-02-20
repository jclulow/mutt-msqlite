## Process this file with automake to produce Makefile.in
include $(top_srcdir)/flymake.am

AUTOMAKE_OPTIONS = 1.6 foreign

AM_CPPFLAGS = -I$(top_srcdir)

noinst_LIBRARIES = libmsqlite.a
noinst_HEADERS = msqlite.h sqlite3.h

libmsqlite_a_SOURCES = msqlite.c sqlite3.c

