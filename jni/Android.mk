#Android.mk
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE:= codec_demo
LOCAL_SRC_FILES := ../NdkMediacodec.cpp
#LOCAL_SHARED_LIBRARIES := libandroid libmediandk liblog
LOCAL_LDLIBS := -lmediandk
LOCAL_LDFLAGS += -pie -fPIE
include $(BUILD_EXECUTABLE)
