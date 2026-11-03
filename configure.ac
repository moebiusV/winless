dnl configure.ac -- winless.  Copyright (C) 2026 David Walther.  GPL-3.0-or-later.
AC_PREREQ([2.69])
AC_INIT([winless],[0.2.0],[],[winless])
AC_CONFIG_SRCDIR([winless.h])
AC_PROG_CC
AC_PROG_INSTALL
AC_SEARCH_LIBS([floor],[m],[],[AC_MSG_ERROR([the math library is required])])
AC_CHECK_HEADERS([poll.h sys/socket.h netinet/in.h arpa/inet.h],[],[AC_MSG_ERROR([a POSIX sockets/poll environment is required])])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
