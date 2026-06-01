AC_PREREQ([2.69])
AC_INIT([exteplayer3], [181], [oe-alliance])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])

AM_INIT_AUTOMAKE([1.11 foreign no-define subdir-objects -Wall -Werror tar-pax])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AC_PROG_CC
AM_PROG_CC_C_O
AC_LANG([C])

# ---------------------------------------------------------------------------
# Mandatory deps via pkg-config
# ---------------------------------------------------------------------------
PKG_CHECK_MODULES([FFMPEG], [libavformat libavcodec libavutil libswresample libswscale])
PKG_CHECK_MODULES([XML2],   [libxml-2.0])
PKG_CHECK_MODULES([SSL],    [libssl libcrypto])
PKG_CHECK_MODULES([BLURAY], [libbluray])

# Optional pc files — fall back to AC_CHECK_LIB so older sysroots without
# .pc files still work. librtmp ships a .pc on most distros, libudfread does too.
PKG_CHECK_MODULES([RTMP], [librtmp], [],
    [AC_CHECK_LIB([rtmp], [RTMP_Alloc], [RTMP_LIBS="-lrtmp"],
        [AC_MSG_ERROR([librtmp not found])])])
PKG_CHECK_MODULES([UDFREAD], [libudfread], [],
    [AC_CHECK_LIB([udfread], [udfread_init], [UDFREAD_LIBS="-ludfread"],
        [AC_MSG_ERROR([libudfread not found])])])

# zlib / bzip2 / lzma have no canonical .pc on every sysroot — probe symbols.
AC_CHECK_LIB([z],  [deflate],     [ZLIB_LIBS="-lz"],   [AC_MSG_ERROR([zlib not found])])
AC_CHECK_LIB([bz2],[BZ2_bzCompress], [BZ2_LIBS="-lbz2"], [AC_MSG_ERROR([libbz2 not found])])
AC_CHECK_LIB([lzma],[lzma_code],  [LZMA_LIBS="-llzma"], [AC_MSG_ERROR([liblzma not found])])
AC_SUBST([ZLIB_LIBS])
AC_SUBST([BZ2_LIBS])
AC_SUBST([LZMA_LIBS])

# Common system libs
AC_CHECK_LIB([pthread], [pthread_create], [PTHREAD_LIBS="-lpthread"], [PTHREAD_LIBS=""])
AC_CHECK_LIB([m],       [sin],            [LIBM_LIBS="-lm"],         [LIBM_LIBS=""])
AC_CHECK_LIB([dl],      [dlopen],         [DL_LIBS="-ldl"],          [DL_LIBS=""])
AC_SUBST([PTHREAD_LIBS])
AC_SUBST([LIBM_LIBS])
AC_SUBST([DL_LIBS])

# ---------------------------------------------------------------------------
# Optional: flv2mpeg4 converter (on by default — matches legacy build)
# ---------------------------------------------------------------------------
AC_ARG_ENABLE([flv2mpeg4],
    [AS_HELP_STRING([--disable-flv2mpeg4], [Disable flv→mpeg4 converter])],
    [enable_flv2mpeg4=$enableval], [enable_flv2mpeg4=yes])
if test "x$enable_flv2mpeg4" = "xyes"; then
    AC_DEFINE([HAVE_FLV2MPEG4_CONVERTER], [1], [Build flv2mpeg4 converter])
fi

# ---------------------------------------------------------------------------
# Largefile + sane defaults
# ---------------------------------------------------------------------------
AC_DEFINE([_FILE_OFFSET_BITS],   [64], [Largefile])
AC_DEFINE([_LARGEFILE64_SOURCE], [1],  [Largefile64])
AC_DEFINE([_LARGEFILE_SOURCE],   [1],  [Largefile])
AC_DEFINE([AUDIO_GET_PTS], [VIDEO_GET_PTS],
          [Older kernel headers lack AUDIO_GET_PTS — alias the video ioctl])

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT

AC_MSG_NOTICE([])
AC_MSG_NOTICE([  exteplayer3 ${PACKAGE_VERSION}])
AC_MSG_NOTICE([    flv2mpeg4         : ${enable_flv2mpeg4}])
AC_MSG_NOTICE([])
