LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE    := touch_filter
LOCAL_SRC_FILES := touch_filter.c
LOCAL_LDFLAGS   := -static
include $(BUILD_EXECUTABLE)
