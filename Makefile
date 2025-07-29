CC ?= gcc
CFLAGS += -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE -D_LARGEFILE_SOURCE -DHAVE_FLV2MPEG4_CONVERTER
CFLAGS += -DAUDIO_GET_PTS=VIDEO_GET_PTS
LDFLAGS += -lswscale -ldl -lpthread -lavformat -lavcodec -lavutil -lswresample

SRC_DIR := src
OBJ_DIR := obj
BIN := exteplayer3

SOURCE_FILES := \
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
    external/plugins/src/png.c \
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
    output/writer/mipsel/vc1.c

OBJS := $(patsubst %.c, $(OBJ_DIR)/%.o, $(SOURCE_FILES))

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $(BIN)

$(OBJ_DIR)/%.o: %.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iinclude -Iexternal -Iexternal/flv2mpeg4 -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

clean:
	rm -rf $(OBJ_DIR) $(BIN)
