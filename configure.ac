# Process this file with autoconf to produce a configure script.
AC_PREREQ([2.69])
AC_INIT([flight-safety-system], [0.0.8], [sjp@canterburyairpatrol.org])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])

m4_include([m4/ax_cxx_compile_stdcxx.m4])

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIRS([m4])

# Checks for programs.
AC_PROG_CXX
AC_PROG_CC
AM_PROG_AR

AX_CXX_COMPILE_STDCXX([11],[],[mandatory])

LT_INIT([disable-static pic-only])

# Setup so we can install pkg-config
PKG_INSTALLDIR

# Check for required pkg-config
PKG_CHECK_MODULES([JSONCPP], [jsoncpp])

AC_ARG_ENABLE([server],AS_HELP_STRING([--enable-server], [Build the server (fss-server)]))
AC_ARG_ENABLE([tests],AS_HELP_STRING([--enable-tests], [Build the tests (Requires catch2)]))
AC_ARG_ENABLE([coverage],AS_HELP_STRING([--enable-coverage], [Build with coverage support (gcov)]))
AC_ARG_ENABLE([fake-client],AS_HELP_STRING([--enable-fake-client], [Build the example client (fss-fake-client)]))

AM_CONDITIONAL([SERVER], [test "x$enable_server" == "xyes"])
AM_CONDITIONAL([ENABLE_TESTS], [test "x$enable_tests" == "xyes"])
AM_CONDITIONAL([COVERAGE], [test "x$enable_coverage" == "xyes"])
AM_CONDITIONAL([FAKE_CLIENT], [test "x$enable_fake_client" == "xyes"])

PKG_PROG_PKG_CONFIG
AC_ARG_WITH([systemdsystemunitdir],
     [AS_HELP_STRING([--with-systemdsystemunitdir=DIR], [Directory for systemd service files])],,
     [with_systemdsystemunitdir=auto])
AS_IF([test "x$with_systemdsystemunitdir" = "xyes" -o "x$with_systemdsystemunitdir" = "xauto"], [
     def_systemdsystemunitdir=$($PKG_CONFIG --variable=systemdsystemunitdir systemd)

     AS_IF([test "x$def_systemdsystemunitdir" = "x"],
   [AS_IF([test "x$with_systemdsystemunitdir" = "xyes"],
    [AC_MSG_ERROR([systemd support requested but pkg-config unable to query systemd package])])
    with_systemdsystemunitdir=no],
   [with_systemdsystemunitdir="$def_systemdsystemunitdir"])])
AS_IF([test "x$with_systemdsystemunitdir" != "xno"],
      [AC_SUBST([systemdsystemunitdir], [$with_systemdsystemunitdir])])
AM_CONDITIONAL([HAVE_SYSTEMD], [test "x$with_systemdsystemunitdir" != "xno"])

AS_IF([test "x$enable_server" == "xyes"], [
    PKG_CHECK_MODULES([ECPG], [libecpg])
])

AS_IF([test "x$enable_tests" == "xyes"], [
AC_LANG_PUSH([C++])
    AC_CHECK_HEADERS([catch2/catch.hpp], [], [
	AC_CHECK_HEADERS([catch/catch.hpp], [], [
            AC_CHECK_HEADERS([catch.hpp])])])
AC_LANG_POP([C++])
])

AC_CHECK_LIB([pthread], [pthread_create], [
       have_pthread=yes
       pthread_LIBS="-pthread"],
    [AC_MSG_ERROR([pthread is required for this program])])
AC_SUBST(pthread_LIBS)


# Output Makefile files.
AC_CONFIG_FILES([Makefile src/Makefile src/fss.pc src/fss-transport.pc src/fss-client.pc examples/Makefile tests/Makefile])
AC_OUTPUT
