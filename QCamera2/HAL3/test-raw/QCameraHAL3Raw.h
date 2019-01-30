/* Copyright (c) 2019, The Linux Foundation. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above
*       copyright notice, this list of conditions and the following
*       disclaimer in the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of The Linux Foundation nor the names of its
*       contributors may be used to endorse or promote products derived
*       from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
* ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
* BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
* OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
* IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*/


#ifndef __HAL3APPRAW_H__
#define __HAL3APPRAW_H__

#include <iostream>
#include <list>
#include <stdlib.h>
#include <log/log.h>
#include <hardware/camera3.h>
#include <dlfcn.h>
#include <linux/ion.h>
#include <linux/msm_ion.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <thread>
#include <pthread.h>
#include <poll.h>
#include <camera/CameraMetadata.h>


#define CDBG_ERROR(fmt, args...) \
  ALOGE("%s:%d " fmt "\n", __func__, __LINE__, ##args)

#define CDBG_LOW(fmt, args...) \
  ALOGD("%s:%d " fmt "\n", __func__, __LINE__, ##args)

#define CDBG_INFO(fmt, args...) \
  ALOGI("%s:%d " fmt "\n", __func__, __LINE__, ##args)

#undef LOG_TAG
#define LOG_TAG "haltest"

#define CAM_YUV_PREVIEW_WIDTH 320
#define CAM_YUV_PREVIEW_HEIGHT 240
#define CAM_FRONT_RAW_PREVIEW_WIDTH 4208  //1280
#define CAM_FRONT_RAW_PREVIEW_HEIGHT 3120 //800
//OV5195
#define CAM_BACK_RAW_PREVIEW_WIDTH 2592
#define CAM_BACK_RAW_PREVIEW_HEIGHT 1944

#define LIB_PATH "/system/lib/hw/camera.sdm660.so"
#define DUMP_CNT 35
#define DEFAULT_RUN_TIME_SEC 150
#define POST_TIMEOUT 3

#define MIPI_RAW8 1
#define MIPI_RAW10 10/8 //2 //10/8
#define MIPI_RAW16 2
#define YUV_NV12 3/2

#define HAL3_DATASPACE_UNKNOWN 0x0
#define HAL3_DATASPACE_ARBITRARY 0x1
#define HAL3_DATASPACE_JFIF 0x101
#define FLAGS_VIDEO_ENCODER 0x00010000

#define QCAMERA3_VENDOR_STREAM_CONFIGURATION_RAW_ONLY_MODE  0x8000
#define PAD_TO_SIZE(size, padding)  (((size) + padding - 1) / padding * padding)


enum cam_multi_stream_t {
    STREAM_RAW,
    STREAM_5M,
    STREAM_1280x960,
    STREAM_VGA,
    STREAM_MAX,
};

enum cam_data_fmt_t {
    CAM_MIPI_RAW8 = 0,
    CAM_MIPI_RAW10 = 1,
    CAM_MIPI_RAW16 = 2,
    CAM_YUV_NV12 = 3,
    CAM_FMT_MAX,
};

enum cam_function {
    CAM_FUNC_BACK = 0,
    CAM_FUNC_FRONT = 1,
    CAM_FUNC_STEREO_LEFT = 2,
    CAM_FUNC_STEREO_RIGHT = 3,
    CAM_FUNC_MAX,
};

enum cam_id_t {
    CAM_BACK = 0,
    CAM_FRONT = 1,
    CAM_MAX,
};

enum test_mode_t
{
    YUV_ONLY = 0,
    RAW_ONLY,
    RAW_YUV,
    RUN_MODE_MAX,
};

struct test_config_t
{
    cam_function func;
    cam_id_t camera_id;
    test_mode_t run_mode;
    bool dump_frame;
    int dump_num;
    int run_time;
};

typedef enum {
    CAM_PAD_NONE = 1,
    CAM_PAD_TO_2 = 2,
    CAM_PAD_TO_4 = 4,
    CAM_PAD_TO_WORD = CAM_PAD_TO_4,
    CAM_PAD_TO_8 = 8,
    CAM_PAD_TO_16 = 16,
    CAM_PAD_TO_32 = 32,
    CAM_PAD_TO_64 = 64,
    CAM_PAD_TO_128 = 128,
    CAM_PAD_TO_256 = 256,
    CAM_PAD_TO_512 = 512,
    CAM_PAD_TO_1K = 1024,
    CAM_PAD_TO_2K = 2048,
    CAM_PAD_TO_4K = 4096,
    CAM_PAD_TO_8K = 8192
} cam_pad_format_t;

typedef struct {
    int fd;
    int ion_fd;
    ion_user_handle_t ion_handle;
    size_t size;
    void *vaddr;
} camera_meminfo_t;

typedef struct {
    int width;
    int height;
    cam_data_fmt_t fmt;
} camera_size_t;


static void camera_device_status_change(const struct camera_module_callbacks
                    *callbacks, int camera_id,
                    int new_status);

static void torch_mode_status_change(const struct camera_module_callbacks
                     *callbacks, const char *camera_id,
                     int new_status);

static void notify(
        const camera3_callback_ops *cb,
        const camera3_notify_msg *msg);

static void process_capture_result(
        const camera3_callback_ops *cb,
        const camera3_capture_result *result);

static camera3_stream_configuration configure_stream(
        int opmode, int num_streams);

static camera3_stream_t* init_stream(int streamtype,
        int camid, int w, int h, int usage, int format, int dataspace);

void free_ion_mem(camera_meminfo_t *meminfo);

native_handle_t *allocate_buffers(int stream_id, int width, int height,
       cam_data_fmt_t fmt, camera_meminfo_t *req_meminfo);

static void stream_allocate_buffer(int stream_id, int width, int height, cam_data_fmt_t fmt, int num);

void post_process(camera3_device_t *device, test_config_t config, int stream_id);

void request_process_thread(camera3_device_t *device, test_config_t config);

static inline void print_usage(int code);

int parse_command_line(int argc, char* argv[], test_config_t *cfg);

#endif


