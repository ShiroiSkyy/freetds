SUBDIRS		=	. unittests
AM_CPPFLAGS	=	-I$(top_srcdir)/include $(ODBC_INC)

lib_LTLIBRARIES	=	libtdsodbc.la
##EXTRA_LTLIBRARIES	=	libtdsodbc.la
libtdsodbc_la_SOURCES =	odbc.c connectparams.c convert_tds2sql.c \
	descriptor.c prepare_query.c odbc_util.c bcp.c \
	native.c sql2tds.c error.c odbc_checks.c sqlwchar.c sqlwparams.h \
	odbc_export.h error_export.h odbc_data.c unixodbc.c
# -module is needed by Darwin (Mac OS X)
libtdsodbc_la_LDFLAGS = \
	$(ODBCINST_LDFLAGS) $(LTLIBICONV) $(FREETDS_LIBGCC) \
	$(FREETDS_SYMBOLIC) $(FREETDS_ODBC_MODULE)
libtdsodbc_la_LIBADD =	../tds/libtds.la ../replacements/libreplacements.la

if MINGW32
libtdsodbc_la_SOURCES += winlogin.c winsetup.c winmain.c
endif

if ODBC_WIDE
ODBC_DEF_FILE = odbc_w.def
else
ODBC_DEF_FILE = odbc.def
endif
if MINGW32
libtdsodbc_la_LIBADD +=	setup.res
libtdsodbc_la_LDFLAGS += -Wl,--kill-at -Wl,--enable-stdcall-fixup -Wl,-s -Wl,@srcdir@/$(ODBC_DEF_FILE) -Wl,setup.res

.rc.res:
	$(RC) -i $< --input-format=rc -o $@ -O coff -I @builddir@
else
if !MACOSX
libtdsodbc_la_LDFLAGS += -export-symbols-regex '^(SQL|ODBCINST).*'
endif
endif

EXTRA_DIST = CMakeLists.txt winmain.c winlogin.c winsetup.c \
	     version.rc version.rc.in setup.rc resource.h \
	     odbc.def odbc_w.def
CLEANFILES = setup.res

## Need blank statement to avoid compiling odbc.c
odbc: $(EXTRA_LTLIBRARIES)
	@echo ''

if HAVE_PERL_SOURCES
BUILT_SOURCES = odbc_export.h error_export.h

clean-local:
	cd $(srcdir) && rm -f $(BUILT_SOURCES)
	
odbc_export.h: odbc_export.pl Makefile odbc.c
	perl $(srcdir)/odbc_export.pl $(srcdir)/odbc.c > $@.tmp
	mv $@.tmp $@

error_export.h: odbc_export.pl Makefile error.c
	perl $(srcdir)/odbc_export.pl $(srcdir)/error.c > $@.tmp
	mv $@.tmp $@

endif
