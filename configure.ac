#
# Process this file with autoconf to produce a configure script.
#

m4_define([nabud_VERSION], [1.3.4])

AC_INIT([nabud],
	m4_defn([nabud_VERSION]),
	[Jason Thorpe <thorpej@me.com>],
	nabud-m4_defn([nabud_VERSION]),
	[https://github.com/thorpej/nabud])
AC_PREREQ(2.69)

AC_CONFIG_SRCDIR([nabud/adaptor.c])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIRS([m4])
AC_CONFIG_HEADERS([config.h])

AC_CANONICAL_TARGET
CANONICAL_TARGET=$target
AC_SUBST([CANONICAL_TARGET])

AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AM_MAINTAINER_MODE

AC_PROG_CC
AC_PROG_INSTALL
AM_PROG_AR

LT_INIT

# Compiler characteristics
#
AC_LANG(C)
AC_MSG_CHECKING([if using gcc])
AS_IF([test "$GCC" = "yes"],
	[CFLAGS="$CFLAGS -std=gnu99"
	 WARNCFLAGS="-Werror -Wall -Wstrict-prototypes -Wmissing-prototypes \
		     -Wpointer-arith -Wno-sign-compare -Wreturn-type -Wswitch \
		     -Wshadow -Wcast-qual -Wwrite-strings \
		     -Wformat=2 -Wno-format-zero-length"
	 AC_MSG_RESULT([yes; adding to CFLAGS: "$WARNCFLAGS"])],
	[WARNCFLAGS=""
	 AC_MSG_RESULT([no])])

case "$CANONICAL_TARGET" in
	*-*-netbsd*)
		# Provide a service start-up script for NetBSD.
		EXTRAS_OS="netbsd"
		EXTRAS_SUBDIRS="extras/$EXTRAS_OS/rc.conf.d \
				extras/$EXTRAS_OS/rc.d"
		;;

	*-*-freebsd*)
		# Provide a service start-up script for FreeBSD.
		EXTRAS_OS="freebsd"
		EXTRAS_SUBDIRS="extras/$EXTRAS_OS/rc.conf.d \
				extras/$EXTRAS_OS/rc.d"
		;;

	*-*-openbsd*)
		# Provide a service start-up script for OpenBSD.
		EXTRAS_OS="openbsd"
		EXTRAS_SUBDIRS="extras/$EXTRAS_OS/rc.d"
		;;

	*-*-darwin*)
		# Provide a launchd plist for macOS.
		EXTRAS_OS="darwin"
		EXTRAS_SUBDIRS="extras/$EXTRAS_OS/launchd"
		;;

	*-*-linux*)
		# Enable _GNU_SOURCE for things like asprintf(3).
		CPPFLAGS="$CPPFLAGS -D_GNU_SOURCE"
		EXTRAS_OS="linux"
		EXTRAS_SUBDIRS="extras/$EXTRAS_OS/systemd"
		;;

	*-*-cygwin*)
		# Enable _GNU_SOURCE for things like asprintf(3).
		CPPFLAGS="$CPPFLAGS -D_GNU_SOURCE"
		;;
esac

AC_SUBST([EXTRAS_OS])
AC_SUBST([EXTRAS_SUBDIRS])

# MD5 is marked as deprecated on some platforms, but we're
# kind of forced to use it regardless.
WARNCFLAGS="$WARNCFLAGS -Wno-deprecated-declarations"

AC_SUBST([WARNCFLAGS])

# Enable Large File Support on platforms where this extra step
# is necessary.
#
AC_SYS_LARGEFILE

# Check for some common system headers.
#
AC_CHECK_HEADERS([inttypes.h sys/ucred.h])

# Deal with the crypto libraries we need.
#
WITH_SSL=no
WITH_PAK=no

# Check for CommonCrypto (the native crypto library on macOS).
#
AC_CHECK_HEADER([CommonCrypto/CommonCrypto.h],
	[AC_DEFINE([HAVE_COMMONCRYPTO_H], [1],
		   [Define to 1 if you have <CommonCrypto/CommonCrypto.h>])
	 WITH_COMMONCRYPTO="CommonCrypto"])

# Check for SecureTransport (the old native SSL/TLS framework on macOS).
# It is deprecated, but we will use it until we can make libfetch use
# the Network framework.
#
AC_CHECK_HEADER([Security/SecureTransport.h],
	[AC_DEFINE([HAVE_SECURETRANSPORT], [1],
		   [Define to 1 if you have <Security/SecureTransport.h>])
	 WITH_SECURETRANSPORT="SecureTransport"])

# Check for OpenSSL
#
m4_include([m4/ax_check_openssl.m4])
AX_CHECK_OPENSSL([AC_DEFINE([HAVE_OPENSSL], [1],
			    [Define to 1 if you have OpenSSL.])]
		 WITH_OPENSSL="OpenSSL")

# Check for which SSL implementation to use.
#
if test x$WITH_SECURETRANSPORT != x; then
	WITH_SSL="$WITH_SECURETRANSPORT"
	SSL_LIBS="-framework Security -framework CoreFoundation"
elif test x$WITH_OPENSSL != x; then
	WITH_SSL="$WITH_OPENSSL"
	SSL_LDFLAGS="$OPENSSL_LDFLAGS"
	SSL_LIBS="$OPENSSL_LIBS"
	SSL_INCLUDES="$OPENSSL_INCLUDES"
else
	AC_MSG_WARN([Building without SSL support])
fi
AC_SUBST([SSL_LDFLAGS])
AC_SUBST([SSL_LIBS])
AC_SUBST([SSL_INCLUDES])

# Check for which crypto (md5 + des) library to use for PAK files.
#
if test x$WITH_COMMONCRYPTO != x; then
	WITH_PAK="$WITH_COMMONCRYPTO"
elif test x$WITH_OPENSSL != x; then
	WITH_PAK="$WITH_OPENSSL"
	PAK_LDFLAGS="$OPENSSL_LDFLAGS"
	PAK_LIBS="-lcrypto"
	PAK_INCLUDES="$OPENSSL_INCLUDES"
else
	AC_MSG_WARN([Building without PAK file support])
fi
AC_SUBST([PAK_LDFLAGS])
AC_SUBST([PAK_LIBS])
AC_SUBST([PAK_INCLUDES])

# Check for POSIX threads
#
m4_include([m4/ax_pthread.m4])
AX_PTHREAD([],
	[AC_MSG_ERROR([Missing POSIX threads])])

# Check for struct sockaddr_in::sin_len.
#
AC_MSG_CHECKING(for sin_len member in struct sockaddr_in)
AC_COMPILE_IFELSE([
	AC_LANG_PROGRAM([[
		#include <sys/socket.h>
		#include <netinet/in.h>
	]],[[
		struct sockaddr_in dummy;
		dummy.sin_len = 0;
	]])],[
		AC_DEFINE(HAVE_SOCKADDR_IN_SIN_LEN, 1, [Define if the struct sockaddr_in has a member sin_len on your OS])
		AC_MSG_RESULT(yes)],[
		AC_MSG_RESULT(no)])

# Check for struct sockaddr_un::sun_len.
#
AC_MSG_CHECKING(for sun_len member in struct sockaddr_un)
AC_COMPILE_IFELSE([
	AC_LANG_PROGRAM([[
		#include <sys/socket.h>
		#include <sys/un.h>
	]],[[
		struct sockaddr_un dummy;
		dummy.sun_len = 0;
	]])],[
		AC_DEFINE(HAVE_SOCKADDR_UN_SUN_LEN, 1, [Define if the struct sockaddr_un has a member sun_len on your OS])
		AC_MSG_RESULT(yes)],[
		AC_MSG_RESULT(no)])

# Check for struct xucred::cr_pid.  FreeBSD has this, but, while macOS
# and DragonFly have struct xucred, they do not have the cr_pid member.
#
AC_MSG_CHECKING(for cr_pid member in struct xucred)
AC_COMPILE_IFELSE([
	AC_LANG_PROGRAM([[
		#include <sys/types.h>
		#include <sys/ucred.h>
	]],[[
		struct xucred dummy;
		dummy.cr_pid = 0;
	]])],[
		AC_DEFINE(HAVE_XUCRED_CR_PID, 1, [Define if the struct xucred has a member cr_pid on your OS])
		AC_MSG_RESULT(yes)],[
		AC_MSG_RESULT(no)])

# Check for struct sockpeercred in <sys/socket.h> (this is an OpenBSD-ism).
#
AC_MSG_CHECKING(for struct sockpeercred in <sys/socket.h>)
AC_COMPILE_IFELSE([
	AC_LANG_PROGRAM([[
		#include <sys/socket.h>
	]],[[
		struct sockpeercred dummy;
		dummy.pid = 0;
	]])],[
		AC_DEFINE(HAVE_STRUCT_SOCKPEERCRED, 1, [Define if <sys/socket.h> has struct sockpeercred])
		AC_MSG_RESULT(yes)],[
		AC_MSG_RESULT(no)])

# Check for several O_* open flags in <sys/fcntl.h>.
#
AC_MSG_CHECKING([for O_TEXT in <sys/fcntl.h>])
AC_COMPILE_IFELSE([
	AC_LANG_PROGRAM([[
		#include <sys/types.h>
		#include <sys/fcntl.h>
	]],[[
		int dummy = O_TEXT;
	]])],[
		AC_DEFINE(HAVE_O_TEXT, 1, [Define if your OS has O_TEXT in <sys/fcntl.h>])
		AC_MSG_RESULT(yes)],[
		AC_MSG_RESULT(no)])
AC_MSG_CHECKING([for O_BINARY in <sys/fcntl.h>])
AC_COMPILE_IFELSE([
	AC_LANG_PROGRAM([[
		#include <sys/types.h>
		#include <sys/fcntl.h>
	]],[[
		int dummy = O_BINARY;
	]])],[
		AC_DEFINE(HAVE_O_BINARY, 1, [Define if your OS has O_BINARY in <sys/fcntl.h>])
		AC_MSG_RESULT(yes)],[
		AC_MSG_RESULT(no)])
AC_MSG_CHECKING([for O_NOCTTY in <sys/fcntl.h>])
AC_COMPILE_IFELSE([
	AC_LANG_PROGRAM([[
		#include <sys/types.h>
		#include <sys/fcntl.h>
	]],[[
		int dummy = O_NOCTTY;
	]])],[
		AC_DEFINE(HAVE_O_NOCTTY, 1, [Define if your OS has O_NOCTTY in <sys/fcntl.h>])
		AC_MSG_RESULT(yes)],[
		AC_MSG_RESULT(no)])
AC_MSG_CHECKING([for O_DIRECTORY in <sys/fcntl.h>])
AC_COMPILE_IFELSE([
	AC_LANG_PROGRAM([[
		#include <sys/types.h>
		#include <sys/fcntl.h>
	]],[[
		int dummy = O_DIRECTORY;
	]])],[
		AC_DEFINE(HAVE_O_DIRECTORY, 1, [Define if your OS has O_DIRECTORY in <sys/fcntl.h>])
		AC_MSG_RESULT(yes)],[
		AC_MSG_RESULT(no)])
AC_MSG_CHECKING([for O_REGULAR in <sys/fcntl.h>])
AC_COMPILE_IFELSE([
	AC_LANG_PROGRAM([[
		#include <sys/types.h>
		#include <sys/fcntl.h>
	]],[[
		int dummy = O_REGULAR;
	]])],[
		AC_DEFINE(HAVE_O_REGULAR, 1, [Define if your OS has O_REGULAR in <sys/fcntl.h>])
		AC_MSG_RESULT(yes)],[
		AC_MSG_RESULT(no)])

# Check for some specific errno values that aren't universal.
#
AC_MSG_CHECKING([for EFTYPE in <errno.h>])
AC_COMPILE_IFELSE([
	AC_LANG_PROGRAM([[
		#include <errno.h>
	]],[[
		int dummy = EFTYPE;
	]])],[
		AC_DEFINE(HAVE_EFTYPE, 1, [Define if your OS has EFTYPE in <errno.h>])
		AC_MSG_RESULT(yes)],[
		AC_MSG_RESULT(no)])

# Check for struct stat::st_birthtime.  This extension is available
# on macOS and some of the BSDs.
#
AC_MSG_CHECKING([for st_birthtime member in struct stat])
AC_COMPILE_IFELSE([
	AC_LANG_PROGRAM([[
		#include <sys/types.h>
		#include <sys/stat.h>
	]],[[
		struct stat dummy;
		dummy.st_birthtime = 0;
	]])],[
		AC_DEFINE(HAVE_STAT_ST_BIRTHTIME, 1, [Define if the struct stat has a member st_birthtime on your OS])
		AC_MSG_RESULT(yes)],[
		AC_MSG_RESULT(no)])

# Check for termios2 in <linux/termios.h>.
#
AC_MSG_CHECKING([for termios2 in <linux/termios.h>])
AC_COMPILE_IFELSE([
	AC_LANG_PROGRAM([[
		#include <linux/termios.h>
	]],[[
		struct termios2 t2;
		t2.c_cflag = BOTHER;
	]])],[
		AC_DEFINE(HAVE_LINUX_TERMIOS2, 1, [Define if struct termios2 exists in <linux/termios.h>])
		AC_MSG_RESULT(yes)],[
		AC_MSG_RESULT(no)])

# Check for _Static_assert() in the compiler.
#
AC_MSG_CHECKING([for _Static_assert() support in the compiler])
AC_COMPILE_IFELSE([
	AC_LANG_PROGRAM([[
		#include <assert.h>
		#include <stddef.h>
	]],[[
		_Static_assert(sizeof(char) == 1, "My whole life has been a lie.");
	]])],[
		AC_DEFINE(HAVE_STATIC_ASSERT, 1, [Define if your compiler supports _Static_assert()])
		AC_MSG_RESULT(yes)],[
		AC_MSG_RESULT(no)])

# Check for the BSD libedit and a readline() function in libedit.
#
AC_CHECK_LIB(edit, readline, [LIBEDIT_LIB="-ledit"])

# Apparently, some systems have a libreadline library that has a
# readline() function compatible with libedit's; We'll allow that, too.
#
if test x$LIBEDIT_LIB = x; then
	AC_CHECK_LIB(readline, readline, [LIBEDIT_LIB="-lreadline"])
fi

if test x$LIBEDIT_LIB != x; then
	editdirs="/usr/include /usr/include/edit"
	LIBEDIT_INCLUDES=
	libedit_works=
	AC_MSG_CHECKING([how to include <readline/readline.h>])
	for editdir in $editdirs; do
		save_CPPFLAGS="$CPPFLAGS"
		LIBEDIT_INCLUDES="-I$editdir"
		CPPFLAGS="$LIBEDIT_INCLUDES $CPPFLAGS"
		AC_COMPILE_IFELSE([
			AC_LANG_PROGRAM([[
				#include <stdio.h>
				#include <readline/readline.h>
			]],[[
				int dummy = 0;
			]])],[
				libedit_works=true
				break],[])
		CPPFLAGS="$save_CPPFLAGS"
	done
	if test x$libedit_works = xtrue; then
		AC_MSG_RESULT([$LIBEDIT_INCLUDES])
		AC_DEFINE(HAVE_LIBEDIT_READLINE, 1,
			   [Define if you have a library compatible with BSD libedit's readline() function on your OS])
		CLI_LIBS="$LIBEDIT_LIB"
		CLI_INCLUDES="$LIBEDIT_INCLUDES"
	else
		AC_MSG_RESULT([unable])
	fi
fi

AC_SUBST(CLI_LIBS)
AC_SUBST(CLI_INCLUDES)

# Check for some useful BSD functions that, sadly, are not universal.
#
AC_CHECK_FUNCS(getprogname)

# Generate the Makefiles
#
AC_CONFIG_FILES([
	Makefile
	examples/Makefile
	extras/darwin/launchd/Makefile
	extras/freebsd/rc.conf.d/Makefile
	extras/freebsd/rc.d/Makefile
	extras/linux/systemd/Makefile
	extras/netbsd/rc.conf.d/Makefile
	extras/netbsd/rc.d/Makefile
	extras/openbsd/rc.d/Makefile
	libfetch/Makefile
	libmj/Makefile
	libnabud/Makefile
	nabud/Makefile
	nabuclient/Makefile
	nabuctl/Makefile
])

AC_OUTPUT

# Display feature results
#
AC_MSG_NOTICE([ === Configuration results ===])
AC_MSG_NOTICE([Version:                  $PACKAGE_VERSION])
AC_MSG_NOTICE([Installation prefix:      $prefix])
AC_MSG_NOTICE([Extras:                   $EXTRAS_SUBDIRS])
AC_MSG_NOTICE([C compiler:               $CC])
AC_MSG_NOTICE([POSIX threads C compiler: $PTHREAD_CC])
AC_MSG_NOTICE([POSIX threads CFLAGS:     $PTHREAD_CFLAGS])
AC_MSG_NOTICE([POSIX threads libraries:  $PTHREAD_LIBS])
AC_MSG_NOTICE([Warning CFLAGS:           $WARNCFLAGS])
AC_MSG_NOTICE([SSL support:              $WITH_SSL])
AC_MSG_NOTICE([SSL includes:             $SSL_INCLUDES])
AC_MSG_NOTICE([SSL libraries:            $SSL_LIBS])
AC_MSG_NOTICE([SSL LDFLAGS:              $SSL_LDFLAGS])
AC_MSG_NOTICE([PAK file support:         $WITH_PAK])
AC_MSG_NOTICE([PAK file includes:        $PAK_INCLUDES])
AC_MSG_NOTICE([PAK file libraries:       $PAK_LIBS])
AC_MSG_NOTICE([PAK file LDFLAGS:         $PAK_LDFLAGS])
AC_MSG_NOTICE([CLI includes:             $CLI_INCLUDES])
AC_MSG_NOTICE([CLI libraries:            $CLI_LIBS])
AC_MSG_NOTICE([=============================])
