
## This file is generated by autogen.py, do not edit
## Process this file with automake to produce Makefile.in
## autogen includes dependencies so automake doesn't need to generated them

AUTOMAKE_OPTIONS = no-dependencies 1.4 foreign

lib_opt_sql_append_la_CFLAGS=-DLIBOPT_SQL_APPEND $(AM_CFLAGS)
opt_sql_appenddir = $(libdir)/monetdb5
if WIN32
LIB33 = ../../../gdk/libbat.la
else
LIB33 =
endif
if WIN32
LIB32 = ../../tools/libmonetdb5.la
else
LIB32 =
endif
lib_opt_sql_append_la_LIBADD = $(LIB32) $(LIB33)
$(do)install-opt_sql_appendLTLIBRARIES : 
lib_opt_sql_append_la-opt_sql_append.lo: opt_sql_append.c opt_sql_append.h ../../optimizer/opt_support.h ../../mal/mal.h ../../../gdk/gdk.h ../../mal/mal_function.h ../../mal/mal_instruction.h ../../mal/mal_type.h ../../mal/mal_stack.h ../../mal/mal_namespace.h ../../mal/mal_module.h ../../mal/mal_resolve.h ../../mal/mal_exception.h ../../mal/mal_errors.h ../../mal/mal_listing.h ../../mal/mal_scenario.h ../../mal/mal_import.h ../../mal/mal_builder.h ../../optimizer/opt_prelude.h ../../mal/mal_client.h ../../mal/mal_profiler.h ../../mal/mal_factory.h ../../mal/mal_interpreter.h ../../../gdk/gdk_atomic.h ../../../gdk/gdk_system.h ../../../gdk/gdk_posix.h ../../../common/stream/stream.h ../../../gdk/gdk_delta.h ../../../gdk/gdk_hash.h ../../../gdk/gdk_atoms.h ../../../gdk/gdk_bbp.h ../../../common/options/monet_options.h ../../../gdk/gdk_utils.h ../../../gdk/gdk_calc.h ../../mal/mal_session.h ../../mal/mal_utils.h
	$(LIBTOOL) --tag=CC --mode=compile $(CC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(lib_opt_sql_append_la_CFLAGS) $(CFLAGS) $(opt_sql_append_CFLAGS) -c -o lib_opt_sql_append_la-opt_sql_append.lo `test -f 'opt_sql_append.c' || echo '$(srcdir)/'`opt_sql_append.c
nodist_lib_opt_sql_append_la_SOURCES =
dist_lib_opt_sql_append_la_SOURCES = opt_sql_append.c opt_sql_append.h
lib_opt_sql_append_la_LDFLAGS = -module -avoid-version
opt_sql_append.o opt_sql_append.lo: opt_sql_append.c opt_sql_append.h ../../optimizer/opt_support.h ../../mal/mal.h ../../../gdk/gdk.h ../../mal/mal_function.h ../../mal/mal_instruction.h ../../mal/mal_type.h ../../mal/mal_stack.h ../../mal/mal_namespace.h ../../mal/mal_module.h ../../mal/mal_resolve.h ../../mal/mal_exception.h ../../mal/mal_errors.h ../../mal/mal_listing.h ../../mal/mal_scenario.h ../../mal/mal_import.h ../../mal/mal_builder.h ../../optimizer/opt_prelude.h ../../mal/mal_client.h ../../mal/mal_profiler.h ../../mal/mal_factory.h ../../mal/mal_interpreter.h ../../../gdk/gdk_atomic.h ../../../gdk/gdk_system.h ../../../gdk/gdk_posix.h ../../../common/stream/stream.h ../../../gdk/gdk_delta.h ../../../gdk/gdk_hash.h ../../../gdk/gdk_atoms.h ../../../gdk/gdk_bbp.h ../../../common/options/monet_options.h ../../../gdk/gdk_utils.h ../../../gdk/gdk_calc.h ../../mal/mal_session.h ../../mal/mal_utils.h
install-exec-local-opt_sql_append.mal: opt_sql_append.mal
	-mkdir -p $(DESTDIR)$(libdir)/monetdb5
	-$(RM) $(DESTDIR)$(libdir)/monetdb5/opt_sql_append.mal
	$(INSTALL_DATA) $< $(DESTDIR)$(libdir)/monetdb5/opt_sql_append.mal

uninstall-local-opt_sql_append.mal: 
	$(RM) $(DESTDIR)$(libdir)/monetdb5/opt_sql_append.mal

AM_CPPFLAGS = -I$(srcdir) -I../../optimizer -I$(srcdir)/../../optimizer -I../../mal -I$(srcdir)/../../mal -I../../../gdk -I$(srcdir)/../../../gdk -I../../../common/stream -I$(srcdir)/../../../common/stream -I../../../common/options -I$(srcdir)/../../../common/options
install-exec-local-91_opt_sql_append.mal: 91_opt_sql_append.mal
	-mkdir -p $(DESTDIR)$(libdir)/monetdb5/autoload
	-$(RM) $(DESTDIR)$(libdir)/monetdb5/autoload/91_opt_sql_append.mal
	$(INSTALL_DATA) $< $(DESTDIR)$(libdir)/monetdb5/autoload/91_opt_sql_append.mal

uninstall-local-91_opt_sql_append.mal: 
	$(RM) $(DESTDIR)$(libdir)/monetdb5/autoload/91_opt_sql_append.mal

CFLAGS += $(THREAD_SAVE_FLAGS)
dist-hook:
	mkdir -p $(distdir)/Tests
	cp -pR $(srcdir)/Tests/* $(distdir)/Tests
BUILT_SOURCES =
MOSTLYCLEANFILES =
EXTRA_DIST = Makefile.ag Makefile.msc 91_opt_sql_append.mal opt_sql_append.c opt_sql_append.h opt_sql_append.mal
opt_sql_append_LTLIBRARIES = lib_opt_sql_append.la
uninstall-local: uninstall-local-91_opt_sql_append.mal uninstall-local-opt_sql_append.mal
install-exec-local: install-exec-local-91_opt_sql_append.mal install-exec-local-opt_sql_append.mal

  include $(top_srcdir)/buildtools/conf/rules.mk
