# This build script corresponds to a library containing many definitions
# common to both the guest and the host. They relate to
#
LOCAL_PATH := $(call my-dir)

commonSources := \
        GLClientState.cpp \
        GLESTextureUtils.cpp \
        ChecksumCalculator.cpp \
        GLSharedGroup.cpp \
        glUtils.cpp \
        IndexRangeCache.cpp \
        SocketStream.cpp \
        TcpStream.cpp \

ifeq (true,$(GOLDFISH_OPENGL_BUILD_FOR_HOST))

commonSources += \
        goldfish_dma_host.cpp \
        qemu_pipe_host.cpp \

else

commonSources += \
        goldfish_dma.cpp \

endif

### CodecCommon  guest ##############################################
$(call emugl-begin-static-library,libOpenglCodecCommon$(GOLDFISH_OPENGL_LIB_SUFFIX))

LOCAL_SRC_FILES := $(commonSources)

LOCAL_CFLAGS += -DLOG_TAG=\"eglCodecCommon\"

$(call emugl-export,SHARED_LIBRARIES,libcutils libutils liblog)
$(call emugl-export,C_INCLUDES,$(LOCAL_PATH))
$(call emugl-end-module)
