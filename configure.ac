dnl configure.ac -- winview.  Copyright (C) 2026 David Walther.  GPL-3.0-or-later.
AC_PREREQ([2.69])
AC_INIT([winview],[0.2.0],[],[winview])
AC_CONFIG_SRCDIR([winview.h])
AC_PROG_CC
AC_PROG_INSTALL
AC_SEARCH_LIBS([floor],[m],[],[AC_MSG_ERROR([the math library is required])])
AC_CHECK_HEADERS([poll.h sys/socket.h netinet/in.h arpa/inet.h],[],[AC_MSG_ERROR([a POSIX sockets/poll environment is required])])
dnl pytest drives the test suite (a project venv is preferred; see requirements.txt)
AC_MSG_CHECKING([for pytest])
if test -x "$srcdir/venv/bin/pytest"; then PYTEST="$srcdir/venv/bin/pytest"
else PYTEST=`command -v pytest 2>/dev/null`; fi
AC_MSG_RESULT([${PYTEST:-no}])
AC_SUBST([PYTEST])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
