ACLOCAL_AMFLAGS = -I m4

bin_PROGRAMS = exteplayer3

AM_CPPFLAGS = \
    -I$(srcdir)/include \
    -I$(srcdir)/external \
    -I$(srcdir)/external/flv2mpeg4 \
    $(FFMPEG_CFLAGS) \
    $(XML2_CFLAGS) \
    $(SSL_CFLAGS) \
    $(BLURAY_CFLAGS) \
    $(RTMP_CFLAGS) \
    $(UDFREAD_CFLAGS) \
    $(ALSA_CFLAGS)

AM_CFLAGS = -Wall -Wno-unused-parameter -Wno-unused-but-set-variable

exteplayer3_SOURCES = \
    main/exteplayer.c \
    container/container.c \
    container/container_ffmpeg.c \
    manager/manager.c \
    manager/audio.c \
    manager/video.c \
    manager/subtitle.c \
    output/output_subtitle.c \
    output/output.c \
    output/writer/common/pes.c \
    output/writer/common/misc.c \
    output/writer/common/writer.c \
    output/linuxdvb_buffering.c \
    output/graphic_subtitle.c \
    output/linuxdvb_mipsel.c \
    output/writer/mipsel/writer.c \
    output/writer/mipsel/aac.c \
    output/writer/mipsel/ac3.c \
    output/writer/mipsel/bcma.c \
    output/writer/mipsel/mp3.c \
    output/writer/mipsel/pcm.c \
    output/writer/mipsel/lpcm.c \
    output/writer/mipsel/dts.c \
    output/writer/mipsel/amr.c \
    output/writer/mipsel/h265.c \
    output/writer/mipsel/h264.c \
    output/writer/mipsel/mjpeg.c \
    output/writer/mipsel/mpeg2.c \
    output/writer/mipsel/mpeg4.c \
    output/writer/mipsel/divx3.c \
    output/writer/mipsel/vp.c \
    output/writer/mipsel/wmv.c \
    output/writer/mipsel/vc1.c \
    playback/playback.c \
    tools/debug.c \
    tools/strbuffer.c \
    external/ffmpeg/src/bitstream.c \
    external/ffmpeg/src/latmenc.c \
    external/ffmpeg/src/mpeg4audio.c \
    external/ffmpeg/src/xiph.c \
    external/flv2mpeg4/src/m4vencode.c \
    external/flv2mpeg4/src/flvdecoder.c \
    external/flv2mpeg4/src/dcprediction.c \
    external/flv2mpeg4/src/flv2mpeg4.c \
    external/plugins/src/png.c

if HAVE_DREAMNEXTGEN
exteplayer3_SOURCES += output/dream_audio.c output/dream_video.c
endif

exteplayer3_LDADD = \
    $(FFMPEG_LIBS) \
    $(XML2_LIBS) \
    $(SSL_LIBS) \
    $(BLURAY_LIBS) \
    $(RTMP_LIBS) \
    $(UDFREAD_LIBS) \
    $(ALSA_LIBS) \
    $(ZLIB_LIBS) \
    $(BZ2_LIBS) \
    $(LZMA_LIBS) \
    $(PTHREAD_LIBS) \
    $(LIBM_LIBS) \
    $(DL_LIBS)

EXTRA_DIST = autogen.sh README.md
