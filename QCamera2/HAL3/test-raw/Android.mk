LOCAL_PATH:=$(call my-dir)

# Build command line test app: mm-hal3-lab
include $(CLEAR_VARS)

ifeq ($(TARGET_SUPPORT_HAL1),false)
LOCAL_CFLAGS += -DQCAMERA_HAL3_SUPPORT
endif

LOCAL_C_INCLUDES += $(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ/usr/include

LOCAL_C_INCLUDES += \
    hardware/libhardware/include/hardware \
    system/media/camera/include \
    system/media/private/camera/include \
    $(LOCAL_PATH)/../ \
    $(LOCAL_PATH)/../../stack/mm-camera-interface/inc \
    hardware/libhardware/include/hardware \
    hardware/qcom/media/libstagefrighthw \
    hardware/qcom/media/mm-core/inc \
    system/core/include/cutils \
    system/core/include/system \
    system/media/camera/include/system


LOCAL_SRC_FILES := \
    QCameraHAL3Raw.cpp


LOCAL_SHARED_LIBRARIES:= libutils libcamera_client liblog libcamera_metadata libcutils

LOCAL_32_BIT_ONLY := $(BOARD_QTI_CAMERA_32BIT_ONLY)

LOCAL_MODULE:= hal3-test-raw

LOCAL_CFLAGS += -Wall -Wextra -Werror

LOCAL_CFLAGS += -std=c++11 -std=gnu++0x

include $(BUILD_EXECUTABLE)
