dnl configure.ac -- winless.  Copyright (C) 2026 David Walther.  GPL-3.0-or-later.
AC_PREREQ([2.69])
AC_INIT([winless],[0.2.0],[],[winless])
AC_CONFIG_SRCDIR([winless.h])
AC_PROG_CC
AC_PROG_INSTALL
AC_SEARCH_LIBS([floor],[m],[],[AC_MSG_ERROR([the math library is required])])
AC_CHECK_HEADERS([poll.h sys/socket.h netinet/in.h arpa/inet.h],[],[AC_MSG_ERROR([a POSIX sockets/poll environment is required])])
AC_CHECK_HEADER([sqlite3.h],[],[AC_MSG_ERROR([sqlite3.h is required (the annotation database): install libsqlite3-dev])])
AC_CHECK_LIB([sqlite3],[sqlite3_open_v2],[],[AC_MSG_ERROR([libsqlite3 is required (the annotation database)])])
AC_SEARCH_LIBS([dlopen],[dl],[],[AC_MSG_WARN([no dlopen: sources cannot be loaded from libraries])])
AC_ARG_WITH([source],
  [AS_HELP_STRING([--with-source=NAME:LIBDIR],[link the source NAME (LIBDIR/lib NAME.a, exporting wl_source_entry) into winless; the static build that sshw uploads cannot load libraries])],
  [SOURCE_NAME=${withval%%:*}; SOURCE_DIR=${withval#*:}
   AS_IF([test -f "$SOURCE_DIR/lib$SOURCE_NAME.a"],[],[AC_MSG_ERROR([$SOURCE_DIR/lib$SOURCE_NAME.a not found])])
   SOURCE_BUILTIN="-DWL_BUILTIN_SOURCE"
   SOURCE_LIBS="-L$SOURCE_DIR -l$SOURCE_NAME -lsqlite3 -ljson-c -lz"],
  [SOURCE_BUILTIN=; SOURCE_LIBS=])
AC_SUBST([SOURCE_BUILTIN])
AC_SUBST([SOURCE_LIBS])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
