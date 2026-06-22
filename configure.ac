# Process this file with autoconf to produce a configure script.
AC_PREREQ([2.69])
AC_INIT([flight-safety-system], [1.1.0], [sjp@canterburyairpatrol.org])
AM_INIT_AUTOMAKE([-Wall -Werror foreign -Wno-portability])

m4_include([m4/ax_cxx_compile_stdcxx.m4])

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIRS([m4])

# Checks for programs.
AC_PROG_CXX
AC_PROG_CC
AM_PROG_AR

AX_CXX_COMPILE_STDCXX([17],[],[mandatory])

LT_INIT([disable-static pic-only])

# Setup so we can install pkg-config
PKG_INSTALLDIR

# Check for required pkg-config
PKG_CHECK_MODULES([JSONCPP], [jsoncpp])
PKG_CHECK_MODULES([GNUTLS], [gnutls >= 3.6.0])

AC_ARG_ENABLE([server],AS_HELP_STRING([--enable-server], [Build the server (fss-server)]))
AC_ARG_ENABLE([tests],AS_HELP_STRING([--enable-tests], [Build the tests (Requires catch2)]))
AC_ARG_ENABLE([coverage],AS_HELP_STRING([--enable-coverage], [Build with coverage support (gcov)]))
AC_ARG_ENABLE([fake-client],AS_HELP_STRING([--enable-fake-client], [Build the example client (fss-fake-client)]))
AC_ARG_ENABLE([tsan],AS_HELP_STRING([--enable-tsan], [Build the whole tree (libraries, tests, and the dedicated concurrency binary) under ThreadSanitizer so "make check" runs the full unit suite race-checked (requires --enable-tests; mutually exclusive with --enable-coverage)]))

AM_CONDITIONAL([SERVER], [test "x$enable_server" == "xyes"])
AM_CONDITIONAL([ENABLE_TESTS], [test "x$enable_tests" == "xyes"])
AM_CONDITIONAL([COVERAGE], [test "x$enable_coverage" == "xyes"])
AM_CONDITIONAL([FAKE_CLIENT], [test "x$enable_fake_client" == "xyes"])
AM_CONDITIONAL([ENABLE_TSAN], [test "x$enable_tsan" == "xyes"])

AS_IF([test "x$enable_tsan" == "xyes" -a "x$enable_coverage" == "xyes"], [
    AC_MSG_ERROR([--enable-tsan and --enable-coverage are mutually exclusive: TSan and gcov instrumentation cannot coexist in the same binary])
])
AS_IF([test "x$enable_tsan" == "xyes" -a "x$enable_tests" != "xyes"], [
    AC_MSG_ERROR([--enable-tsan requires --enable-tests])
])

# Whole-tree ThreadSanitizer. These flags are appended to AM_CXXFLAGS/AM_LDFLAGS
# in src/, tests/ and examples/ (the same way --enable-coverage instruments the
# tree) so the libraries all_test links are instrumented too — a per-target flag
# on just the concurrency binary would leave the rest of the suite blind.
AS_IF([test "x$enable_tsan" == "xyes"], [
    TSAN_CXXFLAGS="-fsanitize=thread -fno-omit-frame-pointer"
    TSAN_LDFLAGS="-fsanitize=thread"
], [
    TSAN_CXXFLAGS=""
    TSAN_LDFLAGS=""
])
AC_SUBST([TSAN_CXXFLAGS])
AC_SUBST([TSAN_LDFLAGS])

AS_IF([test "x$enable_coverage" == "xyes"], [
    CODE_COVERAGE_CFLAGS="--coverage"
    CODE_COVERAGE_CXXFLAGS="--coverage"
    CODE_COVERAGE_CPPFLAGS=""
    CODE_COVERAGE_LDFLAGS="--coverage"
], [
    CODE_COVERAGE_CFLAGS=""
    CODE_COVERAGE_CXXFLAGS=""
    CODE_COVERAGE_CPPFLAGS=""
    CODE_COVERAGE_LDFLAGS=""
])
AC_SUBST([CODE_COVERAGE_CFLAGS])
AC_SUBST([CODE_COVERAGE_CXXFLAGS])
AC_SUBST([CODE_COVERAGE_CPPFLAGS])
AC_SUBST([CODE_COVERAGE_LDFLAGS])

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

AS_IF([test "x$enable_server" == "xyes" -o "x$enable_tests" == "xyes"], [
    PKG_CHECK_MODULES([ECPG], [libecpg])
])


AS_IF([test "x$enable_tests" == "xyes"], [
AC_LANG_PUSH([C++])
    AC_CHECK_HEADERS([catch2/catch_all.hpp], [PKG_CHECK_MODULES([CATCH2], [catch2-with-main])], [
    AC_CHECK_HEADERS([catch2/catch.hpp], [], [
	AC_CHECK_HEADERS([catch/catch.hpp], [], [
            AC_CHECK_HEADERS([catch.hpp])])])])
AC_LANG_POP([C++])
])

AC_CHECK_LIB([pthread], [pthread_create], [
       have_pthread=yes
       pthread_LIBS="-pthread"],
    [AC_MSG_ERROR([pthread is required for this program])])
AC_SUBST(pthread_LIBS)

AC_CHECK_LIB([gnutlsxx], [_ZN6gnutls11credentialsC1E25gnutls_credentials_type_t])


# Output Makefile files.
AC_CONFIG_FILES([Makefile src/Makefile src/fss.pc src/fss-transport.pc src/fss-transport-ssl.pc src/fss-client.pc src/fss-client-ssl.pc examples/Makefile tests/Makefile])
AC_OUTPUT
