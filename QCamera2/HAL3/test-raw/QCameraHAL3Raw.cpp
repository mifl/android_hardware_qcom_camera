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


#include "QCameraHAL3Raw.h"

using namespace std;


unsigned char g_is_running = 0;

int num_streams = 0;
camera_size_t g_size[CAM_MAX][STREAM_MAX] = {
//rear
    {
        {2592, 1944, CAM_MIPI_RAW8}, //reserve for raw data
        {1280, 960, CAM_YUV_NV12},
        {640, 480, CAM_YUV_NV12},
        {2592, 1944, CAM_YUV_NV12},
    },
//front
    {
        {4208, 3120, CAM_MIPI_RAW10}, //reserve for raw data
        {1920, 1080, CAM_YUV_NV12},
        {1280, 720, CAM_YUV_NV12},
        {1280, 960, CAM_YUV_NV12},
    },
};

pthread_mutex_t lock_fq[STREAM_MAX];
pthread_cond_t  cond_fq[STREAM_MAX];

pthread_mutex_t post_lock[STREAM_MAX];
pthread_cond_t  cond_post_process[STREAM_MAX];

std::list<uint32_t> frame_queue[STREAM_MAX];
std::list<std::pair<uint32_t, uint32_t>> post_process_queue[STREAM_MAX];

camera3_stream_t *list_stream[STREAM_MAX];
const camera_metadata_t *p_metadata[STREAM_MAX];
camera3_capture_request frame_request;
camera3_stream_configuration stream_config;

int mIonFd;
int bufCnt[STREAM_MAX];
int stream_buffer_allocated[STREAM_MAX];

native_handle_t  **stream_handle[STREAM_MAX];
camera_meminfo_t **stream_meminfo[STREAM_MAX];
camera_meminfo_t **stream_rawmeminfo[STREAM_MAX];


static void camera_device_status_change(const struct camera_module_callbacks
                    *callbacks, int camera_id,
                    int new_status)
{
    CDBG_LOW("%d %d %p", camera_id, new_status, callbacks);
}

static void torch_mode_status_change(const struct camera_module_callbacks
                     *callbacks, const char *camera_id,
                     int new_status)
{
    CDBG_LOW("%s %d %p", camera_id, new_status, callbacks);
}

static void notify(
        const camera3_callback_ops *cb,
        const camera3_notify_msg *msg)
{
  int id = 0;
  int frame_number =  msg->message.error.frame_number;
  CDBG_LOW("%p type %d", cb, msg->type);

  switch(msg->type)
  {
    case CAMERA3_MSG_ERROR:
        switch(msg->message.error.error_code)
        {
            case CAMERA3_MSG_ERROR_DEVICE:
                CDBG_ERROR("CAMERA%d_MSG_ERROR [CAMERA3_MSG_ERROR_DEVICE] in frame_number %d\n",
                    id, frame_number);
                exit(-1);
                break;

            case CAMERA3_MSG_ERROR_REQUEST:
                CDBG_ERROR("CAMERA%d_MSG_ERROR [CAMERA3_MSG_ERROR_REQUEST] in frame_number %d\n",
                    id, frame_number);
                break;

            case CAMERA3_MSG_ERROR_RESULT:
                CDBG_ERROR("CAMERA%d_MSG_ERROR [CAMERA3_MSG_ERROR_RESULT] in frame_number %d\n",
                    id, frame_number);
                break;

            case CAMERA3_MSG_ERROR_BUFFER:
                CDBG_ERROR("CAMERA%d_MSG_ERROR [CAMERA3_MSG_ERROR_BUFFER] in frame_number %d\n",
                    id, frame_number);
                break;
        }
        break;

    case CAMERA3_MSG_SHUTTER:
        CDBG_LOW("CAMERA%d_MSG_SHUTTER\n", id);
        break;

    default:
        CDBG_LOW("CAMERA%d unknown notify msg type[%d]\n", id, msg->type);

  }
}

static void process_capture_result(
        const camera3_callback_ops *cb,
        const camera3_capture_result *result)
{
    const native_handle_t* nh = NULL;
    const camera3_stream_buffer_t *buffer = result->output_buffers;
    int num;
    int i;
    int found = 0;
    int size1, size2;

    CDBG_INFO("%p num_output_buffers %d num_streams %d", cb, result->num_output_buffers, num_streams);
    if (result->num_output_buffers == 1) {
        if (buffer && buffer->buffer) {
            nh = *((const native_handle_t**)buffer->buffer);
        } else {
            CDBG_ERROR("Returning buffer is empty##");
            exit(0);
            return;
        }

        found = 0;
        for (i = 0; i < num_streams; i++) {
          for (num = 0; num < stream_buffer_allocated[i]; num++) {
            CDBG_ERROR("num_streams %d, i %d, num %d, stream_buffer_allocated %d. %d %d",
                num_streams, i, num, stream_buffer_allocated[i], nh->data[0], stream_handle[i][num]->data[0]);
            if (nh->data[0] == stream_handle[i][num]->data[0]) { //cn: shared fd
                found = 1;
                break;
            }
          }
          if (found)
              break;
        }

        if (buffer->status == 0 && found) {
            CDBG_INFO("Frame num %d frame_number %d width:%d and height:%d format:%d",
                    num,
                    result->frame_number,
                    result->output_buffers->stream->width,
                    result->output_buffers->stream->height,
                    result->output_buffers->stream->format);
            CDBG_INFO("Taking lock before signalling the condition");
            pthread_mutex_lock(&post_lock[i]);
            size1 = post_process_queue[i].size();
            post_process_queue[i].push_back(std::pair<uint32_t, uint32_t>(result->frame_number, num));
            size2 = post_process_queue[i].size();
            CDBG_INFO("stream id %d. Signaling post process condition. post_process_queue empty %d, size1 %d, size2 %d lock %p %p",
                i, post_process_queue[i].empty(), size1, size2, &cond_post_process[i], &post_lock[i]);
            pthread_cond_signal(&cond_post_process[i]);
            pthread_mutex_unlock(&post_lock[i]);
        } else { //error status
            CDBG_ERROR("Buffer status(%d) is non zero. found %d, num %d result id %d",
                buffer->status, found, num, result->frame_number);
            pthread_mutex_lock(&lock_fq[i]);
            frame_queue[i].push_back(num);
            pthread_cond_signal(&cond_fq[i]);
            pthread_mutex_unlock(&lock_fq[i]);
        }
    } else {
        // calls without buffers
    }
}


static camera3_stream_configuration configure_stream(
        int opmode, int num_streams)
{
    camera3_stream_configuration requested_config;
    requested_config.operation_mode  = opmode;
    requested_config.num_streams = num_streams;
    requested_config.streams = new camera3_stream_t *[num_streams];
    return requested_config;
}

static camera3_stream_t* init_stream(int streamtype,
        int camid, int w, int h, int usage, int format, int dataspace)
{
    camera3_stream_t *stream;

    CDBG_LOW("Stream init for Camera : %d", camid);
    stream =  new camera3_stream_t;
    memset(stream, 0, sizeof(camera3_stream_t));

    stream->stream_type = streamtype;
    stream->width = w;
    stream->height = h;
    stream->format = format;
    stream->usage = usage;
    stream->data_space = (android_dataspace_t)dataspace;
    stream->rotation = CAMERA3_STREAM_ROTATION_0;
    return stream;
}

void free_ion_mem(camera_meminfo_t *meminfo)
{
    ioctl(meminfo->ion_fd, ION_IOC_FREE, &meminfo->ion_handle);
    close(meminfo->ion_fd);
    meminfo->ion_fd = -1;
    CDBG_LOW("free_ion");
}

native_handle_t *allocate_buffers(int stream_id, int width, int height,
       cam_data_fmt_t fmt, camera_meminfo_t *req_meminfo)
{
    struct ion_allocation_data alloc;
    struct ion_fd_data ion_info_fd;
    struct ion_fd_data data2;
    int rc;
    size_t buf_size;
    int stride, scanline;
    int32_t stride_in_bytes = 0;
    native_handle_t *nh_test;

    if (mIonFd <= 0) {
        mIonFd = open("/dev/ion", O_RDONLY);
    }
    if (mIonFd <= 0) {
        CDBG_ERROR("Ion dev open failed %s\n", strerror(errno));
        return NULL;
    }

    stride = PAD_TO_SIZE(width, CAM_PAD_TO_4);
    if (fmt == CAM_MIPI_RAW10) {
        stride_in_bytes = PAD_TO_SIZE(stride * MIPI_RAW10, CAM_PAD_TO_64);
    } else if (fmt == CAM_MIPI_RAW8) {
        stride_in_bytes = PAD_TO_SIZE(stride * MIPI_RAW8, CAM_PAD_TO_64);
    } else if (fmt == CAM_YUV_NV12) {
        stride_in_bytes = PAD_TO_SIZE(stride * YUV_NV12, CAM_PAD_TO_64);
    } else {
        CDBG_ERROR("format %d is not supported\n", fmt);
        return NULL;
    }
    scanline = PAD_TO_SIZE(height, CAM_PAD_TO_64);

    memset(&alloc, 0, sizeof(alloc));
    buf_size = (size_t)(PAD_TO_SIZE((uint32_t)(stride_in_bytes * scanline), CAM_PAD_TO_64));

    alloc.len = (size_t)(buf_size);
    alloc.len = (alloc.len + 4095U) & (~4095U);
    alloc.align = 4096;
    alloc.flags = ION_FLAG_CACHED;
    alloc.heap_id_mask = ION_HEAP(ION_SYSTEM_HEAP_ID);
    rc = ioctl(mIonFd, ION_IOC_ALLOC, &alloc);
    if (rc < 0) {
        CDBG_ERROR("ION allocation failed %s with rc = %d \n", strerror(errno), rc);
        return NULL;
    }
    memset(&ion_info_fd, 0, sizeof(ion_info_fd));
    ion_info_fd.handle = alloc.handle;
    rc = ioctl(mIonFd, ION_IOC_SHARE, &ion_info_fd);
    if (rc < 0) {
        CDBG_ERROR("ION map failed %s\n", strerror(errno));
        return NULL;
    }
    req_meminfo->ion_fd = mIonFd;
    req_meminfo->ion_handle = ion_info_fd.handle;
    CDBG_LOW("stream_id %d ION FD %d shared fd %d ion_handle %d len %d\n",
        stream_id, req_meminfo->ion_fd, ion_info_fd.fd,
        ion_info_fd.handle, alloc.len);
    req_meminfo->size = alloc.len;
    nh_test = native_handle_create(2, 4);
    nh_test->data[0] = ion_info_fd.fd;
    nh_test->data[1] = 0;
    nh_test->data[2] = 0;
    nh_test->data[3] = 0;
    nh_test->data[4] = alloc.len;
    nh_test->data[5] = 0;

    data2.handle = req_meminfo->ion_handle;

    rc = ioctl(req_meminfo->ion_fd, ION_IOC_MAP, &data2);

    if (rc) {
        CDBG_ERROR("ION MAP failed %s\n", strerror(errno));
        return NULL;
    }

    req_meminfo->vaddr = NULL;
    CDBG_LOW(" address before %p", req_meminfo->vaddr);
    req_meminfo->vaddr = mmap(NULL,
    alloc.len,
    PROT_READ  | PROT_WRITE,
    MAP_SHARED,
    data2.fd,
    0);

    CDBG_LOW(" address after %p", req_meminfo->vaddr);

    return nh_test;
}


static void stream_allocate_buffer(int stream_id, int width, int height, cam_data_fmt_t fmt, int num)
{
   camera_meminfo_t *mem;
   stream_handle[stream_id][num] = allocate_buffers(stream_id, width, height, fmt, stream_meminfo[stream_id][num]);

   mem = stream_rawmeminfo[stream_id][num];
   mem->vaddr = malloc(stream_meminfo[stream_id][num]->size); //allocate this buffer for MIPI RAW convert
}

void post_process(camera3_device_t *device, test_config_t config, int stream_id) {
    int num;
    camera_meminfo_t *mem;
    camera_meminfo_t *raw;
    size_t size;
    char ext[100];
    char fname[1000];
    void *data;
    //static int frame_number = 0;
    std::pair<uint32_t, uint32_t> frame_index_pair;
    //int flag = 0;
    static int cnt=0;
    int w, h;
    raw = NULL;
    CDBG_LOW("camera_id %d stream_id %d", config.camera_id, stream_id);

    if (config.run_mode == YUV_ONLY) {
        w = CAM_YUV_PREVIEW_WIDTH;
        h = CAM_YUV_PREVIEW_HEIGHT;
    } else {
        if (config.camera_id  == CAM_FRONT) {
            w = CAM_FRONT_RAW_PREVIEW_WIDTH;
            h = CAM_FRONT_RAW_PREVIEW_HEIGHT;
        } else {
            w = CAM_BACK_RAW_PREVIEW_WIDTH;
            h = CAM_BACK_RAW_PREVIEW_HEIGHT;
        }
    }

    while(g_is_running) {
        pthread_mutex_lock(&post_lock[stream_id]);
        if (post_process_queue[stream_id].empty()) {
            CDBG_INFO("stream_id %d %p %p, Post process queue empty waiting for signal",
                stream_id, &cond_post_process[stream_id], &post_lock[stream_id]);
            pthread_cond_wait(&cond_post_process[stream_id], &post_lock[stream_id]);
            pthread_mutex_unlock(&post_lock[stream_id]);
            continue;
        }

        frame_index_pair = post_process_queue[stream_id].front();
        num = frame_index_pair.second;
        post_process_queue[stream_id].pop_front();

        pthread_mutex_unlock(&post_lock[stream_id]);

        mem = stream_meminfo[stream_id][num];
        size = mem->size;

        if (config.run_mode == YUV_ONLY) {
            sprintf(ext, "yuv");
            data = mem->vaddr;
        } else if (config.run_mode == RAW_ONLY){
            sprintf(ext, "raw");
            raw = stream_rawmeminfo[stream_id][num];
            data = mem->vaddr;
        } else if (config.run_mode == RAW_YUV) {
            if (stream_id != 0) {
                sprintf(ext, "yuv");
                data = mem->vaddr;
            } else {
                sprintf(ext, "raw");
                raw = stream_rawmeminfo[stream_id][num];
                data = mem->vaddr;
            }
        }
        if (cnt <= config.dump_num) {
          sprintf(fname, "/data/misc/camera/cam%d_fun%d_sid%d_%dx%d_fid%d.%s",
            config.camera_id, config.func, stream_id, w, h, frame_index_pair.first, ext);
          CDBG_INFO("stream_id %d, save to %s\n", stream_id, fname);
          if (config.dump_frame) {
              FILE* fd = fopen(fname, "w");
              CDBG_LOW("wrote %d bytes to %s\n", size, fname);
              fwrite(data, size, 1, fd);
              fclose(fd);
          }
          cnt++;
        } else {
            //close
            for (int i = 0; i < bufCnt[stream_id]; i++) {
                free_ion_mem(stream_meminfo[stream_id][i]);
            }
            device->common.close(&device->common);
            exit(0);
        }

        pthread_mutex_lock(&lock_fq[stream_id]);
        frame_queue[stream_id].push_back(num);
        CDBG_ERROR("stream_id %d. Signaling frame queue condition", stream_id);
        pthread_cond_signal(&cond_fq[stream_id]);
        pthread_mutex_unlock(&lock_fq[stream_id]);
    }
}

void request_process_thread(camera3_device_t *device, test_config_t config) {
    int rc = 0;
    int stream_id;
    int qsize = 0;
    int buf_idx;
    camera3_stream_buffer_t mlist_streamBuffs[STREAM_MAX];
    CDBG_INFO("camera_id %d", config.camera_id);

    while(g_is_running) {
      frame_request.num_output_buffers = 0;

      for (stream_id = 0; stream_id < num_streams; stream_id++) {
            pthread_mutex_lock(&lock_fq[stream_id]);
            if(frame_queue[stream_id].empty()){
                CDBG_INFO("stream_id %d, Waiting for queue not empty", stream_id);
                pthread_cond_wait(&cond_fq[stream_id], &lock_fq[stream_id]);
                pthread_mutex_unlock(&lock_fq[stream_id]);
            }

            buf_idx = frame_queue[stream_id].front(); //get the first item
            frame_queue[stream_id].pop_front(); //pop the first item

            qsize = frame_queue[stream_id].size();

            pthread_mutex_unlock(&lock_fq[stream_id]);

            frame_request.input_buffer = NULL;
            mlist_streamBuffs[stream_id].stream = list_stream[stream_id];
            mlist_streamBuffs[stream_id].status = CAMERA3_BUFFER_STATUS_OK;
            mlist_streamBuffs[stream_id].buffer = (const native_handle_t**)&stream_handle[stream_id][buf_idx];
            mlist_streamBuffs[stream_id].release_fence = -1;
            mlist_streamBuffs[stream_id].acquire_fence = -1;
            frame_request.output_buffers = mlist_streamBuffs;
            frame_request.num_output_buffers++;

            CDBG_LOW("camera_id %d stream_id %d Calling request frame id %d. frame_queue index %d. available buffer :%d ",
                config.camera_id, stream_id, frame_request.frame_number, buf_idx, qsize);
        }

        rc = device->ops->process_capture_request(device, &frame_request);

        frame_request.frame_number++;

        if (rc) {
            CDBG_ERROR("camera_id %d stream_id %d capture request failed %d",
                config.camera_id, stream_id, rc);
        }
    }
}

static inline void print_usage(int code)
{
    printf(
        "Camera API test application \n"
        "\n"
        "usage: hal3-test-raw [options]\n"
        "\n"
        "  -f <type>       camera type\n"
        "                    - back\n"
        "                    - front\n"
        "                    - left \n"
        "                    - right \n"
        "  -m <mode>       run mode\n"
        "                    - PIX\n"
        "                    - RDI\n"
        "                    - PIXRDI\n"
        "  -d [num]        dump frames\n"
        "                     num : the number of dump frames (default:%d)\n"
        "  -h              print this message\n"
        , DUMP_CNT);
    exit(code);
}

int parse_command_line(int argc, char* argv[], test_config_t *cfg)
{
    int c;
    if (cfg == NULL) {
        printf("null pointer of cfg.\n");
        return -1;
    }

    while ((c = getopt(argc, argv, "hf:m:d::t:")) != -1) {
        switch (c) {
        case 'f':
            {
                string str(optarg);
                if (str == "back") {
                    cfg->func = CAM_FUNC_BACK;
                    cfg->camera_id = CAM_BACK;
                } else if (str == "front") {
                    cfg->func = CAM_FUNC_FRONT;
                    cfg->camera_id = CAM_FRONT;
                } else if (str == "left") {
                    cfg->func = CAM_FUNC_STEREO_LEFT;
                    cfg->camera_id = CAM_BACK;
                } else if (str == "right") {
                    cfg->func = CAM_FUNC_STEREO_RIGHT;
                    cfg->camera_id = CAM_FRONT;
                } else {
                    printf("error in usage -f.\n");
                    abort();
                }
                break;
            }
         case 'm':
            {
                string str(optarg);
                if (str == "PIX") {
                    cfg->run_mode = YUV_ONLY;
                } else if (str == "RDI") {
                    cfg->run_mode = RAW_ONLY;
                } else if (str == "PIXRDI") {
                    cfg->run_mode = RAW_YUV;
                } else {
                    printf("error in usage -m.\n");
                    abort();
                }
                break;
            }
        case 'd':
            cfg->dump_frame = true;
            if (optarg != NULL)
                cfg->dump_num = atoi(optarg);
            if (!cfg->dump_num)
                cfg->dump_num = DUMP_CNT;
            break;
        case 't':
            cfg->run_time = atoi(optarg);
            break;
        case 'h':
        case '?':
            print_usage(0);
        default:
            abort();
        }
    }

    return 0;
}

int main(int argc, char* argv[]) {
    int rc = 0;
    int w, h = 0;
    int i, j;
    int numCam = 0;
    camera3_device_t *device;
    //android::CameraMetadata meta;
    camera3_callback_ops cbOps;
    camera_module_t *cam_module = NULL;
    camera_module_callbacks_t mModule_cb;
    struct camera_info cam_info;
    test_config_t config;
    const char *camIds[] = {"0", "1", "2"};

    std::thread *post_process_thread[STREAM_MAX];
    std::thread *request_thread;

    for (i = 0; i < STREAM_MAX; i++) {
      pthread_cond_init(&cond_fq[i], NULL);
      pthread_cond_init(&cond_post_process[i], NULL);
      CDBG_LOW("cond_fq %p, cond_post_process %p", &cond_fq[i], &cond_post_process[i]);
      rc = pthread_mutex_init(&lock_fq[i], NULL);
      if (rc) {
        CDBG_ERROR("mutex init failed");
      }
      rc = pthread_mutex_init(&post_lock[i], NULL);
      if (rc) {
        CDBG_ERROR("mutex init failed");
      }
    }

    //default config
    config.func = CAM_FUNC_FRONT;
    config.camera_id = CAM_FRONT;
    config.run_mode = RAW_ONLY;
    config.dump_frame = false;
    config.dump_num = DUMP_CNT;
    config.run_time = 0;

    parse_command_line(argc, argv, &config);

    printf("run with func %d, camera_id %d, run_mode %d, dump_frame %d, dump_num %d, run_time %d.\n",
        config.func, config.camera_id, config.run_mode, config.dump_frame, config.dump_num, config.run_time);
    CDBG_INFO("run with func %d, camera_id %d, run_mode %d, dump_frame %d, dump_num %d, run_time %d",
        config.func, config.camera_id, config.run_mode, config.dump_frame, config.dump_num, config.run_time);

    if (config.func < 0 || config.func > CAM_FUNC_MAX) {
        printf("Error config of camera func.\n");
        return -1;
    }
    if (config.camera_id < 0 || config.camera_id > CAM_MAX) {
        printf("Error config of camera id.\n");
        return -1;
    }
    if (config.run_mode < 0 || config.camera_id > RUN_MODE_MAX) {
        printf("Error config of camera id.\n");
        return -1;
    }

    CDBG_INFO("Starting");
#if 1 // SDM660/SDA660
    void *ptr = dlopen(LIB_PATH, RTLD_NOW);

    if (!ptr) {
        CDBG_ERROR("Error opening HAL libraries %s\n", dlerror());
        return -1;
    }
    cam_module = (camera_module_t*)dlsym(ptr, HAL_MODULE_INFO_SYM_AS_STR);
#else
    rc = hw_get_module(CAMERA_HARDWARE_MODULE_ID,
            (const hw_module_t **)&cam_module);

    if (rc) {
        CDBG_ERROR("Load camera module failed %d", rc);
        return rc;
    }
#endif

    numCam = cam_module->get_number_of_cameras();

    CDBG_INFO("Cameras found %d", numCam);

    mModule_cb.torch_mode_status_change = &torch_mode_status_change;
    mModule_cb.camera_device_status_change = &camera_device_status_change;

    cam_module->set_callbacks(&mModule_cb);


    cam_module->get_camera_info(config.camera_id, &cam_info);

    CDBG_LOW("\tcamera info\n");
    CDBG_LOW("\tcamera facing %d\n", cam_info.facing);
    CDBG_LOW("\torientation %d\n", cam_info.orientation);
    CDBG_LOW("\tdevice version %u\n", cam_info.device_version);

    //meta = (cam_info.static_camera_characteristics);

    rc = cam_module->common.methods->open(&(cam_module->common), camIds[config.camera_id],
            reinterpret_cast<hw_device_t**>(&device));

    if (rc) {
        CDBG_ERROR("camera open failed %d %d", config.camera_id, rc);
        return 0;
    }

    CDBG_INFO("Camera %d Opened Successfully", config.camera_id);

    cbOps.notify = &notify;
    cbOps.process_capture_result = &process_capture_result;

    rc = device->ops->initialize(device, &cbOps);

    if (rc) {
        CDBG_ERROR("camera initialization failed %d %d", config.camera_id, rc);
    }

    CDBG_INFO("Camera %d Init Successs", config.camera_id);

    if (config.run_mode == YUV_ONLY) {
        w = CAM_YUV_PREVIEW_WIDTH;
        h = CAM_YUV_PREVIEW_HEIGHT;
    } else {
        if (config.camera_id == CAM_FRONT) {
            w = CAM_FRONT_RAW_PREVIEW_WIDTH;
            h = CAM_FRONT_RAW_PREVIEW_HEIGHT;
        } else {
            w = CAM_BACK_RAW_PREVIEW_WIDTH;
            h = CAM_BACK_RAW_PREVIEW_HEIGHT;
        }
    }

    //stream init
    num_streams = 0;
    if (config.run_mode == YUV_ONLY) {
        g_size[config.camera_id][num_streams].fmt = CAM_YUV_NV12;
        g_size[config.camera_id][num_streams].width = CAM_YUV_PREVIEW_WIDTH;
        g_size[config.camera_id][num_streams].height = CAM_YUV_PREVIEW_HEIGHT;
        list_stream[num_streams] = init_stream(CAMERA3_STREAM_OUTPUT, config.camera_id,
            g_size[config.camera_id][num_streams].width, g_size[config.camera_id][num_streams].height,
            0, HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, HAL3_DATASPACE_UNKNOWN);
        num_streams++;

        stream_config = configure_stream(CAMERA3_STREAM_CONFIGURATION_NORMAL_MODE, num_streams);
        stream_config.streams[0] = list_stream[0];
        CDBG_INFO("stream: only YUV config for camera id %d size %dx%d num_streams %d",
            config.camera_id, w, h, num_streams);

    } else if (config.run_mode == RAW_ONLY) {
        list_stream[num_streams] = init_stream(CAMERA3_STREAM_OUTPUT, config.camera_id,
            g_size[config.camera_id][num_streams].width, g_size[config.camera_id][num_streams].height,
            0, HAL_PIXEL_FORMAT_RAW_OPAQUE, HAL3_DATASPACE_ARBITRARY);
        num_streams++;

        stream_config = configure_stream(QCAMERA3_VENDOR_STREAM_CONFIGURATION_RAW_ONLY_MODE, num_streams);
        stream_config.streams[0] = list_stream[0];
        CDBG_INFO("stream: only RAW config for camera id %d size %dx%d num_streams %d",
            config.camera_id, w, h, num_streams);

    } else if (config.run_mode == RAW_YUV) { // RAW + YUV
        list_stream[num_streams] = init_stream(CAMERA3_STREAM_OUTPUT, config.camera_id,
            g_size[config.camera_id][num_streams].width, g_size[config.camera_id][num_streams].height,
            0, HAL_PIXEL_FORMAT_RAW_OPAQUE, HAL3_DATASPACE_ARBITRARY);
        num_streams++;

        list_stream[num_streams] = init_stream(CAMERA3_STREAM_OUTPUT, config.camera_id,
            g_size[config.camera_id][num_streams].width, g_size[config.camera_id][num_streams].height,
            0, HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, HAL3_DATASPACE_UNKNOWN);
        num_streams++;

        list_stream[num_streams] = init_stream(CAMERA3_STREAM_OUTPUT, config.camera_id,
            g_size[config.camera_id][num_streams].width, g_size[config.camera_id][num_streams].height,
            FLAGS_VIDEO_ENCODER, HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, HAL3_DATASPACE_UNKNOWN);
        num_streams++;

        list_stream[num_streams] = init_stream(CAMERA3_STREAM_OUTPUT, config.camera_id,
            g_size[config.camera_id][num_streams].width, g_size[config.camera_id][num_streams].height,
            0, HAL_PIXEL_FORMAT_BLOB, HAL3_DATASPACE_UNKNOWN);
        num_streams++;

        stream_config = configure_stream(CAMERA3_STREAM_CONFIGURATION_NORMAL_MODE, num_streams);

        for (i = 0; i < num_streams; i++) {
          stream_config.streams[i] = list_stream[i];
        }

        CDBG_INFO("stream: RAW+YUV config for camera id %d size %dx%d num_streams %d",
          config.camera_id, w, h, num_streams);
    }

    rc = device->ops->configure_streams(device, &stream_config);

    if (rc) {
        CDBG_ERROR("configure stream failed %d", rc);
    }

    for (i = 0; i < num_streams; i++) {
      bufCnt[i] = stream_config.streams[i]->max_buffers;
      CDBG_INFO("stream[%d] config successfull max buffer size = %d",
        i, stream_config.streams[i]->max_buffers);
    }
    p_metadata[0]= device->ops->construct_default_request_settings(device,
            CAMERA3_TEMPLATE_PREVIEW);

    p_metadata[1] = device->ops->construct_default_request_settings(device,
            CAMERA3_TEMPLATE_STILL_CAPTURE);

    for (i = 0; i < num_streams; i++) {
      stream_handle[i] = new native_handle_t *[bufCnt[i]];
      stream_meminfo[i] = new camera_meminfo_t *[bufCnt[i]];
      stream_rawmeminfo[i] = new camera_meminfo_t *[bufCnt[i]];
      for (j = 0; j < bufCnt[i]; j++) {
        stream_handle[i][j] = new native_handle_t;
        stream_meminfo[i][j] = new camera_meminfo_t;
        stream_rawmeminfo[i][j] = new camera_meminfo_t;
      }
    }

    g_is_running = 1;

    for (i = 0; i < num_streams; i++) {
      for (j = 0; j < bufCnt[i]; j++) {
        stream_allocate_buffer(i,
            g_size[config.camera_id][i].width, g_size[config.camera_id][i].height,
            g_size[config.camera_id][i].fmt, j);
        CDBG_INFO("stream id %d buffer idx %d. allocate buffer size %dx%d, fmt %d",
            i, j, g_size[config.camera_id][i].width, g_size[config.camera_id][i].height,
            g_size[config.camera_id][i].fmt);
        pthread_mutex_lock(&lock_fq[i]);
        frame_queue[i].push_back(j);
        pthread_mutex_unlock(&lock_fq[i]);
        stream_buffer_allocated[i]++;
      }
    }

    frame_request.frame_number = 0;
    frame_request.settings = p_metadata[0];

    int32_t fps_range[2];
    fps_range[0] = 15;
    fps_range[1] = 15;
    android::CameraMetadata hal3app_metadata_settings;
    hal3app_metadata_settings = p_metadata[0];
    CDBG_INFO("Setting FPS to %d-%d", fps_range[0], fps_range[1]);
    hal3app_metadata_settings.update(ANDROID_CONTROL_AE_TARGET_FPS_RANGE, fps_range, 2);
    frame_request.settings = hal3app_metadata_settings.release();

    request_thread = new std::thread(request_process_thread, device, config);

    for (i = 0; i < num_streams; i++) {
      post_process_thread[i] = new std::thread(post_process, device, config, i);
    }


    sleep(DEFAULT_RUN_TIME_SEC);

    g_is_running = 0;
    sleep(2);

    for (i = 0; i < num_streams; i++) {
      request_thread->join();
      post_process_thread[i]->join();
    }
    //close
    for (i = 0; i < num_streams; i++) {
      for (j = 0; j < bufCnt[i]; j++) {
        free_ion_mem(stream_meminfo[i][j]);
      }
    }

    rc = device->common.close(&device->common);

    if (rc) {
        CDBG_ERROR("camera close failed %d %d", config.camera_id, rc);
    }

    CDBG_INFO("Camera %d Closed Successfully", config.camera_id);

    CDBG_INFO("Close");

    return 0;
}

