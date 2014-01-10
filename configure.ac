dnl Process this file with autoconf to produce a configure script.
AC_PREREQ(2.50)
AC_INIT(trackcutter, 0.1.1, [rodgersb][@][it.net.au])
AM_INIT_AUTOMAKE(trackcutter, 0.1.1)
AM_CONFIG_HEADER(config.h)

dnl Check if a C compiler is available
AC_PROG_CC

dnl Check if C compiler supports `const' keyword
AC_C_CONST

dnl Check if the type `size_t' is defined
AC_TYPE_SIZE_T

dnl Check if the ANSI C headers are available
AC_HEADER_STDC
AC_CHECK_HEADERS(limits.h stdlib.h string.h unistd.h, [], 
[
    AC_MSG_ERROR([one or more standard C headers are unavailable], 1)
])

dnl Check if debugging assertions are to be enabled
AC_ARG_ENABLE(assert,
    AS_HELP_STRING(
        [--enable-assert],
        [Enable assertion checking code for debugging]),
    [enable_assert=$enableval],
    [enable_assert=no]
)
if test "$enable_assert" = no; then
    AC_DEFINE(NDEBUG,, [Define if compiling a production version])
fi

dnl Check if compiler optimisations should be disabled. Currently this
dnl only works for GCC; will need to look up the optimisation flags for
dnl other compilers such as Clang, Intel C compiler, etc.
AC_ARG_ENABLE(opt,
    AS_HELP_STRING(
        [--disable-opt],
        [Disable GCC compiler optimisations (default=enabled)]),
    [enable_opt=$enableval],
    [enable_opt=yes]
)
DISABLE_OPT_CFLAGS=""
if test "$enable_opt" = no; then
    dnl The shell variable $GCC will be set to "yes" if autoconf
    dnl recognises the system's compiler as GCC.
    if test "$GCC" = yes; then
        dnl This is intended to override the -O2 that autoconf normally
        dnl includes in CFLAGS.
        DISABLE_OPT_CFLAGS="-O0"
    fi
fi
AC_SUBST(DISABLE_OPT_CFLAGS)

dnl Check if debugging assertions are to be enabled
AC_ARG_ENABLE(assert,
    AS_HELP_STRING(
        [--enable-assert],
        [Enable assertion checking code for debugging]),
    [enable_assert=$enableval],
    [enable_assert=no]
)
if test "$enable_assert" = no; then
    AC_DEFINE(NDEBUG,, [Define if compiling a production version])
fi

dnl Check if compiler optimisations should be disabled. Currently this
dnl only works for GCC; will need to look up the optimisation flags for
dnl other compilers such as Clang, Intel C compiler, etc.
AC_ARG_ENABLE(opt,
    AS_HELP_STRING(
        [--disable-opt],
        [Disable GCC compiler optimisations (default=enabled)]),
    [enable_opt=$enableval],
    [enable_opt=yes]
)
DISABLE_OPT_CFLAGS=""
if test "$enable_opt" = no; then
    dnl The shell variable $GCC will be set to "yes" if autoconf
    dnl recognises the system's compiler as GCC.
    if test "$GCC" = yes; then
        dnl This is intended to override the -O2 that autoconf normally
        dnl includes in CFLAGS.
        DISABLE_OPT_CFLAGS="-O0"
    fi
fi
AC_SUBST(DISABLE_OPT_CFLAGS)

dnl Check if compiler source code strictness should be disabled. As with
dnl --enable-opt, this currently only works for GCC; will need to look
dnl up strictness flags for other compilers. The idea behind this is
dnl that while the extra QA provided by the strictness flags should be
dnl enabled by default, it's possible for the end-user to disable it
dnl (should a future version of GCC become too anal and decides our code
dnl isn't kosher), or if they want to hack the codebase themselves and
dnl they want to target a later C dialect or C++ instead.
AC_ARG_ENABLE(strict,
    AS_HELP_STRING(
        [--enable-strict],
        [Enable GCC strict code validation (default=no)]),
    [enable_strict=$enableval],
    [enable_strict=no]
)
ENABLE_STRICT_CFLAGS=""
if test "$enable_strict" = yes; then
    dnl The shell variable $GCC will be set to "yes" if autoconf
    dnl recognises the system's compiler as GCC.
    if test "$GCC" = yes; then
        ENABLE_STRICT_CFLAGS="-Wall -Wpedantic -Wextra -Werror -std=c99"
    fi
fi
AC_SUBST(ENABLE_STRICT_CFLAGS)

dnl Check if the C library is GNU libc
AC_MSG_CHECKING(if C library is GNU libc)
AC_EGREP_CPP(GLIBC,
[
    #include <features.h>
    #ifdef __GLIBC__
        GLIBC
    #endif
],[
    AC_DEFINE(_GNU_SOURCE, [], [Define if you are using GNU libc])
    AC_MSG_RESULT(yes)
],[
    AC_MSG_RESULT(no)
    AC_MSG_ERROR([currently trackcutter needs GNU libc, sorry.
This dependency will be removed in a future version.], 1)
])

dnl Determine if pkg-config is available
PKG_PROG_PKG_CONFIG

dnl Check if libsndfile is installed
PKG_CHECK_MODULES(libsndfile, sndfile,
[
    AC_SUBST(libsndfile_CFLAGS)
    AC_SUBST(libsndfile_LIBS)
],
[
    AC_MSG_ERROR([cannot locate the libsndfile library.
Trackcutter needs libsndfile to be able to read/write various audio file formats.
You can obtain libsndfile from <http://www.mega-nerd.com/libsndfile/>.], 1)
])

dnl Generate makefiles for building the package
AC_OUTPUT(Makefile)

dnl Inform the user of completion, and tell them what to do next.
AC_MSG_NOTICE([

    Configuration of ${PACKAGE_NAME} is complete.

    Next, you'll need to compile the program source code. Please type:
        \$ make

    Finally, to install* the executables/documentation on your system, type:
        \$ make install
            (*if installing into a system-wide path such as /usr or
            /usr/local, you'll need to run this command with super-user
            privileges.)

])
