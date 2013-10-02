/*--------------------------------------------------------------------------
Copyright (c) 2013, The Linux Foundation. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
* Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.
* Neither the name of The Linux Foundation nor
the names of its contributors may be used to endorse or promote
products derived from this software without specific prior written
permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
--------------------------------------------------------------------------*/

/*============================================================================
O p e n M A X   w r a p p e r s
O p e n  M A X   C o r e

*//** @file omx_vdec.cpp
This module contains the implementation of the OpenMAX core & component.

*//*========================================================================*/

//////////////////////////////////////////////////////////////////////////////
//                             Include Files
//////////////////////////////////////////////////////////////////////////////

#include <string.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include "omx_vdec_hevc_swvdec.h"
#include <fcntl.h>
#include <limits.h>
#include <media/msm_media_info.h>

#ifndef _ANDROID_
#include <sys/ioctl.h>
#include <sys/mman.h>
#endif //_ANDROID_

#ifdef _ANDROID_
#include <cutils/properties.h>
#undef USE_EGL_IMAGE_GPU
#endif

#if  defined (_ANDROID_HONEYCOMB_) || defined (_ANDROID_ICS_)
#include <gralloc_priv.h>
#endif

#include <qdMetaData.h>

#ifdef _ANDROID_
#include "DivXDrmDecrypt.h"
#endif //_ANDROID_

#ifdef USE_EGL_IMAGE_GPU
#include <EGL/egl.h>
#include <EGL/eglQCOM.h>
#define EGL_BUFFER_HANDLE_QCOM 0x4F00
#define EGL_BUFFER_OFFSET_QCOM 0x4F01
#endif

#ifdef INPUT_BUFFER_LOG
#define INPUT_BUFFER_FILE_NAME "/data/input-bitstream.\0\0\0\0"
#define INPUT_BUFFER_FILE_NAME_LEN 30
FILE *inputBufferFile1;
char inputfilename [INPUT_BUFFER_FILE_NAME_LEN] = "\0";
#endif
#ifdef OUTPUT_BUFFER_LOG
FILE *outputBufferFile1;
char outputfilename [] = "/data/output.yuv";
#endif
#ifdef OUTPUT_EXTRADATA_LOG
FILE *outputExtradataFile;
char ouputextradatafilename [] = "/data/extradata";
#endif

#define DEFAULT_FPS 30
#define MAX_INPUT_ERROR DEFAULT_FPS
#define MAX_SUPPORTED_FPS 120

#define VC1_SP_MP_START_CODE        0xC5000000
#define VC1_SP_MP_START_CODE_MASK   0xFF000000
#define VC1_AP_SEQ_START_CODE       0x0F010000
#define VC1_STRUCT_C_PROFILE_MASK   0xF0
#define VC1_STRUCT_B_LEVEL_MASK     0xE0000000
#define VC1_SIMPLE_PROFILE          0
#define VC1_MAIN_PROFILE            1
#define VC1_ADVANCE_PROFILE         3
#define VC1_SIMPLE_PROFILE_LOW_LEVEL  0
#define VC1_SIMPLE_PROFILE_MED_LEVEL  2
#define VC1_STRUCT_C_LEN            4
#define VC1_STRUCT_C_POS            8
#define VC1_STRUCT_A_POS            12
#define VC1_STRUCT_B_POS            24
#define VC1_SEQ_LAYER_SIZE          36
#define POLL_TIMEOUT 0x7fffffff

#define MEM_DEVICE "/dev/ion"
#define MEM_HEAP_ID ION_CP_MM_HEAP_ID

#ifdef _ANDROID_
extern "C"{
#include<utils/Log.h>
}
#endif//_ANDROID_

#define SZ_4K 0x1000
#define SZ_1M 0x100000

#define Log2(number, power)  { OMX_U32 temp = number; power = 0; while( (0 == (temp & 0x1)) &&  power < 16) { temp >>=0x1; power++; } }
#define Q16ToFraction(q,num,den) { OMX_U32 power; Log2(q,power);  num = q >> power; den = 0x1 << (16 - power); }
#define EXTRADATA_IDX(__num_planes) (__num_planes  - 1)

#define DEFAULT_EXTRADATA (OMX_INTERLACE_EXTRADATA)

int debug_level = PRIO_ERROR;

void* async_message_thread (void *input)
{
    OMX_BUFFERHEADERTYPE *buffer;
    struct v4l2_plane plane[VIDEO_MAX_PLANES];
    struct pollfd pfd;
    struct v4l2_buffer v4l2_buf;
    memset((void *)&v4l2_buf,0,sizeof(v4l2_buf));
    struct v4l2_event dqevent;
    omx_vdec *omx = reinterpret_cast<omx_vdec*>(input);
    pfd.events = POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM | POLLRDBAND | POLLPRI;
    pfd.fd = omx->drv_ctx.video_driver_fd;
    int error_code = 0,rc=0,bytes_read = 0,bytes_written = 0;
    DEBUG_PRINT_HIGH("omx_vdec: Async thread start\n");
    prctl(PR_SET_NAME, (unsigned long)"VideoDecCallBackThread", 0, 0, 0);
    while (1)
    {
        rc = poll(&pfd, 1, POLL_TIMEOUT);
        if (!rc) {
            DEBUG_PRINT_ERROR("Poll timedout\n");
            break;
        } else if (rc < 0) {
            DEBUG_PRINT_ERROR("Error while polling: %d\n", rc);
            break;
        }
        if ((pfd.revents & POLLIN) || (pfd.revents & POLLRDNORM)) {
            struct vdec_msginfo vdec_msg;
            v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            v4l2_buf.memory = V4L2_MEMORY_USERPTR;
            v4l2_buf.length = omx->drv_ctx.num_planes;
            v4l2_buf.m.planes = plane;
            while(!ioctl(pfd.fd, VIDIOC_DQBUF, &v4l2_buf)) {
                vdec_msg.msgcode=VDEC_MSG_RESP_OUTPUT_BUFFER_DONE;
                vdec_msg.status_code=VDEC_S_SUCCESS;
                vdec_msg.msgdata.output_frame.client_data=(void*)&v4l2_buf;
                vdec_msg.msgdata.output_frame.len=plane[0].bytesused;
                vdec_msg.msgdata.output_frame.bufferaddr=(void*)plane[0].m.userptr;
                vdec_msg.msgdata.output_frame.time_stamp= ((uint64_t)v4l2_buf.timestamp.tv_sec * (uint64_t)1000000) +
                    (uint64_t)v4l2_buf.timestamp.tv_usec;
                if (vdec_msg.msgdata.output_frame.len) {
                    vdec_msg.msgdata.output_frame.framesize.left = plane[0].reserved[2];
                    vdec_msg.msgdata.output_frame.framesize.top = plane[0].reserved[3];
                    vdec_msg.msgdata.output_frame.framesize.right = plane[0].reserved[4];
                    vdec_msg.msgdata.output_frame.framesize.bottom = plane[0].reserved[5];
                }
                if (omx->async_message_process(input,&vdec_msg) < 0) {
                    DEBUG_PRINT_HIGH("\n async_message_thread Exited  \n");
                    break;
                }
            }
        }
        if((pfd.revents & POLLOUT) || (pfd.revents & POLLWRNORM)) {
            struct vdec_msginfo vdec_msg;
            v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            v4l2_buf.memory = V4L2_MEMORY_USERPTR;
            v4l2_buf.length = 1;
            v4l2_buf.m.planes = plane;
            while(!ioctl(pfd.fd, VIDIOC_DQBUF, &v4l2_buf)) {
                vdec_msg.msgcode=VDEC_MSG_RESP_INPUT_BUFFER_DONE;
                vdec_msg.status_code=VDEC_S_SUCCESS;
                vdec_msg.msgdata.input_frame_clientdata=(void*)&v4l2_buf;
                if (omx->async_message_process(input,&vdec_msg) < 0) {
                    DEBUG_PRINT_HIGH("\n async_message_thread Exited  \n");
                    break;
                }
            }
        }
        if (pfd.revents & POLLPRI){
            rc = ioctl(pfd.fd, VIDIOC_DQEVENT, &dqevent);
            if(dqevent.type == V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_INSUFFICIENT ) {
                struct vdec_msginfo vdec_msg;
                vdec_msg.msgcode=VDEC_MSG_EVT_CONFIG_CHANGED;
                vdec_msg.status_code=VDEC_S_SUCCESS;
                DEBUG_PRINT_HIGH("\n VIDC Port Reconfig recieved \n");
                if (omx->async_message_process(input,&vdec_msg) < 0) {
                    DEBUG_PRINT_HIGH("\n async_message_thread Exited  \n");
                    break;
                }
            } else if(dqevent.type == V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_SUFFICIENT ) {
                struct vdec_msginfo vdec_msg;
                vdec_msg.msgcode=VDEC_MSG_EVT_INFO_CONFIG_CHANGED;
                vdec_msg.status_code=VDEC_S_SUCCESS;
                DEBUG_PRINT_HIGH("\n VIDC Port Reconfig recieved \n");
                if (omx->async_message_process(input,&vdec_msg) < 0) {
                    DEBUG_PRINT_HIGH("\n async_message_thread Exited  \n");
                    break;
                }
            } else if (dqevent.type == V4L2_EVENT_MSM_VIDC_FLUSH_DONE) {
                struct vdec_msginfo vdec_msg;
                vdec_msg.msgcode=VDEC_MSG_RESP_FLUSH_INPUT_DONE;
                vdec_msg.status_code=VDEC_S_SUCCESS;
                DEBUG_PRINT_HIGH("\n VIDC input Flush Done Recieved \n");
                if (omx->async_message_process(input,&vdec_msg) < 0) {
                    DEBUG_PRINT_HIGH("\n async_message_thread Exited  \n");
                    break;
                }
                vdec_msg.msgcode=VDEC_MSG_RESP_FLUSH_OUTPUT_DONE;
                vdec_msg.status_code=VDEC_S_SUCCESS;
                DEBUG_PRINT_HIGH("\n VIDC output Flush Done Recieved \n");
                if (omx->async_message_process(input,&vdec_msg) < 0) {
                    DEBUG_PRINT_HIGH("\n async_message_thread Exited  \n");
                    break;
                }
            } else if (dqevent.type == V4L2_EVENT_MSM_VIDC_CLOSE_DONE) {
                DEBUG_PRINT_HIGH("\n VIDC Close Done Recieved and async_message_thread Exited \n");
                break;
            } else if(dqevent.type == V4L2_EVENT_MSM_VIDC_SYS_ERROR) {
                struct vdec_msginfo vdec_msg;
                vdec_msg.msgcode=VDEC_MSG_EVT_HW_ERROR;
                vdec_msg.status_code=VDEC_S_SUCCESS;
                DEBUG_PRINT_HIGH("\n SYS Error Recieved \n");
                if (omx->async_message_process(input,&vdec_msg) < 0) {
                    DEBUG_PRINT_HIGH("\n async_message_thread Exited  \n");
                    break;
                }
            } else {
                DEBUG_PRINT_HIGH("\n VIDC Some Event recieved \n");
                continue;
            }
        }
    }
    DEBUG_PRINT_HIGH("omx_vdec: Async thread stop\n");
    return NULL;
}

void* message_thread(void *input)
{
    omx_vdec* omx = reinterpret_cast<omx_vdec*>(input);
    unsigned char id;
    int n;
    if (omx == NULL)
    {
        DEBUG_PRINT_ERROR("message thread null pointer rxd\n");
        return NULL;
    }

    DEBUG_PRINT_HIGH("omx_vdec: message thread start\n");
    prctl(PR_SET_NAME, (unsigned long)"VideoDecMsgThread", 0, 0, 0);
    while (1)
    {

        n = read(omx->m_pipe_in, &id, 1);

        if(0 == n)
        {
            break;
        }

        if (1 == n)
        {
            omx->process_event_cb(omx, id);
        }
        if ((n < 0) && (errno != EINTR))
        {
            DEBUG_PRINT_ERROR("\nERROR: read from pipe failed, ret %d errno %d", n, errno);
            break;
        }
    }
    DEBUG_PRINT_HIGH("omx_vdec: message thread stop\n");
    return NULL;
}

void post_message(omx_vdec *omx, unsigned char id)
{
    int ret_value;

    if (omx == NULL)
    {
        DEBUG_PRINT_ERROR("message thread null pointer rxd\n");
        return;
    }
    DEBUG_PRINT_LOW("omx_vdec: post_message %d pipe out%d\n", id,omx->m_pipe_out);
    ret_value = write(omx->m_pipe_out, &id, 1);
    DEBUG_PRINT_LOW("post_message to pipe done %d\n",ret_value);
}

// omx_cmd_queue destructor
omx_vdec::omx_cmd_queue::~omx_cmd_queue()
{
    // Nothing to do
}

// omx cmd queue constructor
omx_vdec::omx_cmd_queue::omx_cmd_queue(): m_read(0),m_write(0),m_size(0)
{
    memset(m_q,0,sizeof(m_q));
}

// omx cmd queue insert
bool omx_vdec::omx_cmd_queue::insert_entry(unsigned p1, unsigned p2, unsigned id)
{
    bool ret = true;
    if(m_size < OMX_CORE_CONTROL_CMDQ_SIZE)
    {
        m_q[m_write].id       = id;
        m_q[m_write].param1   = p1;
        m_q[m_write].param2   = p2;
        m_write++;
        m_size ++;
        if(m_write >= OMX_CORE_CONTROL_CMDQ_SIZE)
        {
            m_write = 0;
        }
    }
    else
    {
        ret = false;
        DEBUG_PRINT_ERROR("ERROR: %s()::Command Queue Full\n", __func__);
    }
    return ret;
}

// omx cmd queue pop
bool omx_vdec::omx_cmd_queue::pop_entry(unsigned *p1, unsigned *p2, unsigned *id)
{
    bool ret = true;
    if (m_size > 0)
    {
        *id = m_q[m_read].id;
        *p1 = m_q[m_read].param1;
        *p2 = m_q[m_read].param2;
        // Move the read pointer ahead
        ++m_read;
        --m_size;
        if(m_read >= OMX_CORE_CONTROL_CMDQ_SIZE)
        {
            m_read = 0;
        }
    }
    else
    {
        ret = false;
    }
    return ret;
}

// Retrieve the first mesg type in the queue
unsigned omx_vdec::omx_cmd_queue::get_q_msg_type()
{
    return m_q[m_read].id;
}

#ifdef _ANDROID_
omx_vdec::ts_arr_list::ts_arr_list()
{
    //initialize timestamps array
    memset(m_ts_arr_list, 0, sizeof(m_ts_arr_list) );
}
omx_vdec::ts_arr_list::~ts_arr_list()
{
    //free m_ts_arr_list?
}

bool omx_vdec::ts_arr_list::insert_ts(OMX_TICKS ts)
{
    bool ret = true;
    bool duplicate_ts = false;
    int idx = 0;

    //insert at the first available empty location
    for ( ; idx < MAX_NUM_INPUT_OUTPUT_BUFFERS; idx++)
    {
        if (!m_ts_arr_list[idx].valid)
        {
            //found invalid or empty entry, save timestamp
            m_ts_arr_list[idx].valid = true;
            m_ts_arr_list[idx].timestamp = ts;
            DEBUG_PRINT_LOW("Insert_ts(): Inserting TIMESTAMP (%lld) at idx (%d)",
                ts, idx);
            break;
        }
    }

    if (idx == MAX_NUM_INPUT_OUTPUT_BUFFERS)
    {
        DEBUG_PRINT_LOW("Timestamp array list is FULL. Unsuccessful insert");
        ret = false;
    }
    return ret;
}

bool omx_vdec::ts_arr_list::pop_min_ts(OMX_TICKS &ts)
{
    bool ret = true;
    int min_idx = -1;
    OMX_TICKS min_ts = 0;
    int idx = 0;

    for ( ; idx < MAX_NUM_INPUT_OUTPUT_BUFFERS; idx++)
    {

        if (m_ts_arr_list[idx].valid)
        {
            //found valid entry, save index
            if (min_idx < 0)
            {
                //first valid entry
                min_ts = m_ts_arr_list[idx].timestamp;
                min_idx = idx;
            }
            else if (m_ts_arr_list[idx].timestamp < min_ts)
            {
                min_ts = m_ts_arr_list[idx].timestamp;
                min_idx = idx;
            }
        }

    }

    if (min_idx < 0)
    {
        //no valid entries found
        DEBUG_PRINT_LOW("Timestamp array list is empty. Unsuccessful pop");
        ts = 0;
        ret = false;
    }
    else
    {
        ts = m_ts_arr_list[min_idx].timestamp;
        m_ts_arr_list[min_idx].valid = false;
        DEBUG_PRINT_LOW("Pop_min_ts:Timestamp (%lld), index(%d)",
            ts, min_idx);
    }

    return ret;

}


bool omx_vdec::ts_arr_list::reset_ts_list()
{
    bool ret = true;
    int idx = 0;

    DEBUG_PRINT_LOW("reset_ts_list(): Resetting timestamp array list");
    for ( ; idx < MAX_NUM_INPUT_OUTPUT_BUFFERS; idx++)
    {
        m_ts_arr_list[idx].valid = false;
    }
    return ret;
}
#endif

// factory function executed by the core to create instances
void *get_omx_component_factory_fn(void)
{
    return (new omx_vdec);
}

#ifdef _ANDROID_
#ifdef USE_ION
VideoHeap::VideoHeap(int devicefd, size_t size, void* base,
struct ion_handle *handle, int ionMapfd)
{
    //    ionInit(devicefd, base, size, 0 , MEM_DEVICE,handle,ionMapfd);
}
#else
VideoHeap::VideoHeap(int fd, size_t size, void* base)
{
    // dup file descriptor, map once, use pmem
    init(dup(fd), base, size, 0 , MEM_DEVICE);
}
#endif
#endif // _ANDROID_
/* ======================================================================
FUNCTION
omx_vdec::omx_vdec

DESCRIPTION
Constructor

PARAMETERS
None

RETURN VALUE
None.
========================================================================== */
omx_vdec::omx_vdec():
    m_error_propogated(false),
    m_state(OMX_StateInvalid),
    m_app_data(NULL),
    m_inp_mem_ptr(NULL),
    m_out_mem_ptr(NULL),
    m_inp_err_count(0),
    input_flush_progress (false),
    output_flush_progress (false),
    input_use_buffer (false),
    output_use_buffer (false),
    ouput_egl_buffers(false),
    m_use_output_pmem(OMX_FALSE),
    m_out_mem_region_smi(OMX_FALSE),
    m_out_pvt_entry_pmem(OMX_FALSE),
    pending_input_buffers(0),
    pending_output_buffers(0),
    m_out_bm_count(0),
    m_inp_bm_count(0),
    m_inp_bPopulated(OMX_FALSE),
    m_out_bPopulated(OMX_FALSE),
    m_flags(0),
#ifdef _ANDROID_
    m_heap_ptr(NULL),
#endif
    m_inp_bEnabled(OMX_TRUE),
    m_out_bEnabled(OMX_TRUE),
    m_in_alloc_cnt(0),
    m_platform_list(NULL),
    m_platform_entry(NULL),
    m_pmem_info(NULL),
    m_pSwVdec(NULL),
    m_pSwVdecIpBuffer(NULL),
    m_pSwVdecOpBuffer(NULL),
    m_nInputBuffer(0),
    m_nOutputBuffer(0),
    m_interm_mem_ptr(NULL),
    m_interm_flush_dsp_progress(OMX_FALSE),
    m_interm_flush_swvdec_progress(OMX_FALSE),
    m_interm_bPopulated(OMX_FALSE),
    m_interm_bEnabled(OMX_TRUE),
    m_swvdec_mode(-1),
    m_fill_internal_bufers(OMX_TRUE),
    arbitrary_bytes (true),
    psource_frame (NULL),
    pdest_frame (NULL),
    m_inp_heap_ptr (NULL),
    m_phdr_pmem_ptr(NULL),
    m_heap_inp_bm_count (0),
    codec_type_parse ((codec_type)0),
    first_frame_meta (true),
    frame_count (0),
    nal_count (0),
    nal_length(0),
    look_ahead_nal (false),
    first_frame(0),
    first_buffer(NULL),
    first_frame_size (0),
    m_device_file_ptr(NULL),
    m_vc1_profile((vc1_profile_type)0),
    h264_last_au_ts(LLONG_MAX),
    h264_last_au_flags(0),
    prev_ts(LLONG_MAX),
    rst_prev_ts(true),
    frm_int(0),
    in_reconfig(false),
    m_display_id(NULL),
    h264_parser(NULL),
    client_extradata(0),
#ifdef _ANDROID_
    m_enable_android_native_buffers(OMX_FALSE),
    m_use_android_native_buffers(OMX_FALSE),
    iDivXDrmDecrypt(NULL),
#endif
    m_desc_buffer_ptr(NULL),
    secure_mode(false),
    codec_config_flag(false)
{
    /* Assumption is that , to begin with , we have all the frames with decoder */
    DEBUG_PRINT_HIGH("In OMX vdec Constructor");
#ifdef _ANDROID_
    char property_value[PROPERTY_VALUE_MAX] = {0};
    property_get("vidc.debug.level", property_value, "0");
    debug_level = atoi(property_value);

    property_value[0] = '\0';
    property_get("vidc.dec.debug.perf", property_value, "0");
    perf_flag = atoi(property_value);
    if (perf_flag)
    {
        DEBUG_PRINT_HIGH("vidc.dec.debug.perf is %d", perf_flag);
        dec_time.start();
        proc_frms = latency = 0;
    }
    property_value[0] = '\0';
    property_get("vidc.dec.debug.ts", property_value, "0");
    m_debug_timestamp = atoi(property_value);
    DEBUG_PRINT_HIGH("vidc.dec.debug.ts value is %d",m_debug_timestamp);
    if (m_debug_timestamp)
    {
        time_stamp_dts.set_timestamp_reorder_mode(true);
        time_stamp_dts.enable_debug_print(true);
    }

    property_value[0] = '\0';
    property_get("vidc.dec.debug.concealedmb", property_value, "0");
    m_debug_concealedmb = atoi(property_value);
    DEBUG_PRINT_HIGH("vidc.dec.debug.concealedmb value is %d",m_debug_concealedmb);

#endif
    memset(&m_cmp,0,sizeof(m_cmp));
    memset(&m_cb,0,sizeof(m_cb));
    memset (&drv_ctx,0,sizeof(drv_ctx));
    memset (&h264_scratch,0,sizeof (OMX_BUFFERHEADERTYPE));
    memset (m_hwdevice_name,0,sizeof(m_hwdevice_name));
    memset(m_demux_offsets, 0, sizeof(m_demux_offsets) );
    m_demux_entries = 0;
#ifdef _ANDROID_ICS_
    memset(&native_buffer, 0 ,(sizeof(struct nativebuffer) * MAX_NUM_INPUT_OUTPUT_BUFFERS));
#endif
    memset(&drv_ctx.extradata_info, 0, sizeof(drv_ctx.extradata_info));
    drv_ctx.timestamp_adjust = false;
    drv_ctx.video_driver_fd = -1;
    m_vendor_config.pData = NULL;
    pthread_mutexattr_t attr;
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m_lock, &attr);
    pthread_mutex_init(&c_lock, &attr);
    sem_init(&m_cmd_lock,0,0);
    streaming[CAPTURE_PORT] =
        streaming[OUTPUT_PORT] = false;
#ifdef _ANDROID_
    char extradata_value[PROPERTY_VALUE_MAX] = {0};
    property_get("vidc.dec.debug.extradata", extradata_value, "0");
    m_debug_extradata = atoi(extradata_value);
    DEBUG_PRINT_HIGH("vidc.dec.debug.extradata value is %d",m_debug_extradata);
#endif
    m_fill_output_msg = OMX_COMPONENT_GENERATE_FTB;
    client_buffers.set_vdec_client(this);
}

static const int event_type[] = {
    V4L2_EVENT_MSM_VIDC_FLUSH_DONE,
    V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_SUFFICIENT,
    V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_INSUFFICIENT,
    V4L2_EVENT_MSM_VIDC_CLOSE_DONE,
    V4L2_EVENT_MSM_VIDC_SYS_ERROR
};

static OMX_ERRORTYPE subscribe_to_events(int fd)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    struct v4l2_event_subscription sub;
    int array_sz = sizeof(event_type)/sizeof(int);
    int i,rc;
    if (fd < 0) {
        printf("Invalid input: %d\n", fd);
        return OMX_ErrorBadParameter;
    }

    for (i = 0; i < array_sz; ++i) {
        memset(&sub, 0, sizeof(sub));
        sub.type = event_type[i];
        rc = ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
        if (rc) {
            printf("Failed to subscribe event: 0x%x\n", sub.type);
            break;
        }
    }
    if (i < array_sz) {
        for (--i; i >=0 ; i--) {
            memset(&sub, 0, sizeof(sub));
            sub.type = event_type[i];
            rc = ioctl(fd, VIDIOC_UNSUBSCRIBE_EVENT, &sub);
            if (rc)
                printf("Failed to unsubscribe event: 0x%x\n", sub.type);
        }
        eRet = OMX_ErrorNotImplemented;
    }
    return eRet;
}


static OMX_ERRORTYPE unsubscribe_to_events(int fd)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    struct v4l2_event_subscription sub;
    int array_sz = sizeof(event_type)/sizeof(int);
    int i,rc;
    if (fd < 0) {
        printf("Invalid input: %d\n", fd);
        return OMX_ErrorBadParameter;
    }

    for (i = 0; i < array_sz; ++i) {
        memset(&sub, 0, sizeof(sub));
        sub.type = event_type[i];
        rc = ioctl(fd, VIDIOC_UNSUBSCRIBE_EVENT, &sub);
        if (rc) {
            printf("Failed to unsubscribe event: 0x%x\n", sub.type);
            break;
        }
    }
    return eRet;
}

/* ======================================================================
FUNCTION
omx_vdec::~omx_vdec

DESCRIPTION
Destructor

PARAMETERS
None

RETURN VALUE
None.
========================================================================== */
omx_vdec::~omx_vdec()
{
    m_pmem_info = NULL;
    struct v4l2_decoder_cmd dec;
    DEBUG_PRINT_HIGH("In OMX vdec Destructor");
    if(m_pipe_in) close(m_pipe_in);
    if(m_pipe_out) close(m_pipe_out);
    m_pipe_in = -1;
    m_pipe_out = -1;
    DEBUG_PRINT_HIGH("Waiting on OMX Msg Thread exit");
    pthread_join(msg_thread_id,NULL);

    if (!m_pSwVdec || m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY)
    {
        DEBUG_PRINT_HIGH("Waiting on OMX Async Thread exit");
        dec.cmd = V4L2_DEC_CMD_STOP;
        if (ioctl(drv_ctx.video_driver_fd, VIDIOC_DECODER_CMD, &dec))
        {
            DEBUG_PRINT_ERROR("\n STOP Command failed\n");
        }
        pthread_join(async_thread_id,NULL);
        unsubscribe_to_events(drv_ctx.video_driver_fd);
        close(drv_ctx.video_driver_fd);
    }

    if (m_pSwVdec && SWVDEC_S_SUCCESS != SwVdec_Stop(m_pSwVdec))
    {
        DEBUG_PRINT_ERROR("\n SwVdec_Stop Command failed in vdec destructor\n");
        SwVdec_DeInit(m_pSwVdec);
        m_pSwVdec = NULL;
    }

    pthread_mutex_destroy(&m_lock);
    pthread_mutex_destroy(&c_lock);
    sem_destroy(&m_cmd_lock);
    if (perf_flag)
    {
        DEBUG_PRINT_HIGH("--> TOTAL PROCESSING TIME");
        dec_time.end();
    }
    DEBUG_PRINT_HIGH("Exit OMX vdec Destructor");
}

int release_buffers(omx_vdec* obj, enum vdec_buffer buffer_type) {
    struct v4l2_requestbuffers bufreq;
    int rc = 0;
    if (buffer_type == VDEC_BUFFER_TYPE_OUTPUT){
        bufreq.memory = V4L2_MEMORY_USERPTR;
        bufreq.count = 0;
        bufreq.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        rc = ioctl(obj->drv_ctx.video_driver_fd,VIDIOC_REQBUFS, &bufreq);
    }
    return rc;
}

/* ======================================================================
FUNCTION
omx_vdec::OMXCntrlProcessMsgCb

DESCRIPTION
IL Client callbacks are generated through this routine. The decoder
provides the thread context for this routine.

PARAMETERS
ctxt -- Context information related to the self.
id   -- Event identifier. This could be any of the following:
1. Command completion event
2. Buffer done callback event
3. Frame done callback event

RETURN VALUE
None.

========================================================================== */
void omx_vdec::process_event_cb(void *ctxt, unsigned char id)
{
    signed int p1; // Parameter - 1
    signed int p2; // Parameter - 2
    unsigned int ident;
    unsigned int qsize=0; // qsize
    omx_vdec *pThis = (omx_vdec *) ctxt;

    if(!pThis)
    {
        DEBUG_PRINT_ERROR("ERROR: %s()::Context is incorrect, bailing out\n",
            __func__);
        return;
    }

    // Protect the shared queue data structure
    do
    {
        /*Read the message id's from the queue*/
        pthread_mutex_lock(&pThis->m_lock);
        qsize = pThis->m_cmd_q.m_size;
        if(qsize)
        {
            pThis->m_cmd_q.pop_entry((unsigned *)&p1, (unsigned *)&p2, &ident);
        }

        if (qsize == 0 && pThis->m_state != OMX_StatePause)
        {
            qsize = pThis->m_ftb_q.m_size;
            if (qsize)
            {
                pThis->m_ftb_q.pop_entry((unsigned *)&p1, (unsigned *)&p2, &ident);
            }
        }

        if (qsize == 0 && pThis->m_state != OMX_StatePause)
        {
            qsize = pThis->m_ftb_q_dsp.m_size;
            if (qsize)
            {
                pThis->m_ftb_q_dsp.pop_entry((unsigned *)&p1, (unsigned *)&p2, &ident);
            }
        }

        if (qsize == 0 && pThis->m_state != OMX_StatePause)
        {
            qsize = pThis->m_etb_q.m_size;
            if (qsize)
            {
                pThis->m_etb_q.pop_entry((unsigned *)&p1, (unsigned *)&p2, &ident);
            }
        }

        if (qsize == 0 && pThis->m_state != OMX_StatePause)
        {
            qsize = pThis->m_etb_q_swvdec.m_size;
            if (qsize)
            {
                pThis->m_etb_q_swvdec.pop_entry((unsigned *)&p1, (unsigned *)&p2, &ident);
            }
        }

        pthread_mutex_unlock(&pThis->m_lock);

        /*process message if we have one*/
        if(qsize > 0)
        {
            id = ident;
            switch (id)
            {
            case OMX_COMPONENT_GENERATE_EVENT:
                if (pThis->m_cb.EventHandler)
                {
                    switch (p1)
                    {
                    case OMX_CommandStateSet:
                        pThis->m_state = (OMX_STATETYPE) p2;
                        DEBUG_PRINT_HIGH("\n OMX_CommandStateSet complete, m_state = %d",
                            pThis->m_state);
                        pThis->m_cb.EventHandler(&pThis->m_cmp, pThis->m_app_data,
                            OMX_EventCmdComplete, p1, p2, NULL);
                        break;

                    case OMX_EventError:
                        if(p2 == OMX_StateInvalid)
                        {
                            DEBUG_PRINT_ERROR("\n OMX_EventError: p2 is OMX_StateInvalid");
                            pThis->m_state = (OMX_STATETYPE) p2;
                            pThis->m_cb.EventHandler(&pThis->m_cmp, pThis->m_app_data,
                                OMX_EventError, OMX_ErrorInvalidState, p2, NULL);
                        }
                        else if (p2 == OMX_ErrorHardware)
                        {
                            pThis->omx_report_error();
                        }
                        else
                        {
                            pThis->m_cb.EventHandler(&pThis->m_cmp, pThis->m_app_data,
                                OMX_EventError, p2, (OMX_U32)NULL, NULL );
                        }
                        break;

                    case OMX_CommandPortDisable:
                        DEBUG_PRINT_HIGH("\n OMX_CommandPortDisable complete for port [%d]", p2);
                        if (BITMASK_PRESENT(&pThis->m_flags,
                            OMX_COMPONENT_OUTPUT_FLUSH_IN_DISABLE_PENDING))
                        {
                            BITMASK_SET(&pThis->m_flags, OMX_COMPONENT_DISABLE_OUTPUT_DEFERRED);
                            break;
                        }
                        if (p2 == OMX_CORE_OUTPUT_PORT_INDEX && pThis->in_reconfig)
                        {
                            OMX_ERRORTYPE eRet = OMX_ErrorNone;
                            if (!pThis->m_pSwVdec || pThis->m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY)
                            {
                                pThis->stream_off(OMX_CORE_OUTPUT_PORT_INDEX);
                                if(release_buffers(pThis, VDEC_BUFFER_TYPE_OUTPUT))
                                    DEBUG_PRINT_HIGH("Failed to release output buffers\n");

                                if (pThis->m_pSwVdec)
                                {
                                    DEBUG_PRINT_HIGH("In port reconfig, SwVdec_Stop");
                                    SwVdec_Stop(pThis->m_pSwVdec);
                                }
                            }
                            OMX_ERRORTYPE eRet1 = pThis->get_buffer_req_swvdec();
                            pThis->in_reconfig = false;
                            if(eRet !=  OMX_ErrorNone)
                            {
                                DEBUG_PRINT_ERROR("get_buffer_req_swvdec failed eRet = %d",eRet);
                                pThis->omx_report_error();
                                break;
                            }
                        }
                        pThis->m_cb.EventHandler(&pThis->m_cmp, pThis->m_app_data,
                            OMX_EventCmdComplete, p1, p2, NULL );
                        break;
                    case OMX_CommandPortEnable:
                        DEBUG_PRINT_HIGH("\n OMX_CommandPortEnable complete for port [%d]", p2);
                        if (p2 == OMX_CORE_OUTPUT_PORT_INDEX && pThis->m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY)
                        {
                            DEBUG_PRINT_LOW("send all interm buffers to dsp after port enabled\n");
                            pThis->fill_all_buffers_proxy_dsp(&pThis->m_cmp);
                        }
                        pThis->m_cb.EventHandler(&pThis->m_cmp, pThis->m_app_data,\
                            OMX_EventCmdComplete, p1, p2, NULL );
                        break;

                    default:
                        pThis->m_cb.EventHandler(&pThis->m_cmp, pThis->m_app_data,
                            OMX_EventCmdComplete, p1, p2, NULL );
                        break;

                    }
                }
                else
                {
                    DEBUG_PRINT_ERROR("ERROR: %s()::EventHandler is NULL\n", __func__);
                }
                break;
            case OMX_COMPONENT_GENERATE_ETB_ARBITRARY:
                if (pThis->empty_this_buffer_proxy_arbitrary((OMX_HANDLETYPE)p1,\
                    (OMX_BUFFERHEADERTYPE *)p2) != OMX_ErrorNone)
                {
                    DEBUG_PRINT_ERROR("\n empty_this_buffer_proxy_arbitrary failure");
                    pThis->omx_report_error ();
                }
                break;
            case OMX_COMPONENT_GENERATE_ETB:
                if (pThis->empty_this_buffer_proxy((OMX_HANDLETYPE)p1,\
                    (OMX_BUFFERHEADERTYPE *)p2) != OMX_ErrorNone)
                {
                    DEBUG_PRINT_ERROR("\n empty_this_buffer_proxy failure");
                    pThis->omx_report_error ();
                }
                break;

            case OMX_COMPONENT_GENERATE_FTB:
                if ( pThis->fill_this_buffer_proxy((OMX_HANDLETYPE)p1,\
                    (OMX_BUFFERHEADERTYPE *)p2) != OMX_ErrorNone)
                {
                    DEBUG_PRINT_ERROR("\n fill_this_buffer_proxy failure");
                    pThis->omx_report_error ();
                }
                break;

            case OMX_COMPONENT_GENERATE_COMMAND:
                pThis->send_command_proxy(&pThis->m_cmp,(OMX_COMMANDTYPE)p1,\
                    (OMX_U32)p2,(OMX_PTR)NULL);
                break;

            case OMX_COMPONENT_GENERATE_EBD:

                if (p2 != VDEC_S_SUCCESS && p2 != VDEC_S_INPUT_BITSTREAM_ERR)
                {
                    DEBUG_PRINT_ERROR("\n OMX_COMPONENT_GENERATE_EBD failure");
                    pThis->omx_report_error ();
                }
                else
                {
                    if (p2 == VDEC_S_INPUT_BITSTREAM_ERR && p1)
                    {
                        pThis->m_inp_err_count++;
                        pThis->time_stamp_dts.remove_time_stamp(
                            ((OMX_BUFFERHEADERTYPE *)p1)->nTimeStamp,
                            (pThis->drv_ctx.interlace != VDEC_InterlaceFrameProgressive)
                            ?true:false);
                    }
                    else
                    {
                        pThis->m_inp_err_count = 0;
                    }
                    if ( pThis->empty_buffer_done(&pThis->m_cmp,
                        (OMX_BUFFERHEADERTYPE *)p1) != OMX_ErrorNone)
                    {
                        DEBUG_PRINT_ERROR("\n empty_buffer_done failure");
                        pThis->omx_report_error ();
                    }
                    if(pThis->m_inp_err_count >= MAX_INPUT_ERROR)
                    {
                        DEBUG_PRINT_ERROR("\n Input bitstream error for consecutive %d frames.", MAX_INPUT_ERROR);
                        pThis->omx_report_error ();
                    }
                }
                break;
            case OMX_COMPONENT_GENERATE_INFO_FIELD_DROPPED:
                {
                    int64_t *timestamp = (int64_t *)p1;
                    if (p1)
                    {
                        pThis->time_stamp_dts.remove_time_stamp(*timestamp,
                            (pThis->drv_ctx.interlace != VDEC_InterlaceFrameProgressive)
                            ?true:false);
                        free(timestamp);
                    }
                }
                break;
            case OMX_COMPONENT_GENERATE_FBD:
                if (p2 != VDEC_S_SUCCESS)
                {
                    DEBUG_PRINT_ERROR("\n OMX_COMPONENT_GENERATE_FBD failure");
                    pThis->omx_report_error ();
                }
                else if ( pThis->fill_buffer_done(&pThis->m_cmp,
                    (OMX_BUFFERHEADERTYPE *)p1) != OMX_ErrorNone )
                {
                    DEBUG_PRINT_ERROR("\n fill_buffer_done failure");
                    pThis->omx_report_error ();
                }
                break;

            case OMX_COMPONENT_GENERATE_EVENT_INPUT_FLUSH:
                DEBUG_PRINT_HIGH("\n i");
                if (!pThis->input_flush_progress)
                {
                    DEBUG_PRINT_ERROR("\n WARNING: Unexpected INPUT_FLUSH from driver");
                }
                else
                {
                    pThis->execute_input_flush();
                    if (pThis->m_cb.EventHandler)
                    {
                        if (p2 != VDEC_S_SUCCESS)
                        {
                            DEBUG_PRINT_ERROR("\nOMX_COMPONENT_GENERATE_EVENT_INPUT_FLUSH failure");
                            pThis->omx_report_error ();
                        }
                        else
                        {
                            /*Check if we need generate event for Flush done*/
                            if(BITMASK_PRESENT(&pThis->m_flags,
                                OMX_COMPONENT_INPUT_FLUSH_PENDING))
                            {
                                BITMASK_CLEAR (&pThis->m_flags,OMX_COMPONENT_INPUT_FLUSH_PENDING);
                                DEBUG_PRINT_LOW("\n Input Flush completed - Notify Client");
                                pThis->m_cb.EventHandler(&pThis->m_cmp, pThis->m_app_data,
                                    OMX_EventCmdComplete,OMX_CommandFlush,
                                    OMX_CORE_INPUT_PORT_INDEX,NULL );
                            }
                            if (BITMASK_PRESENT(&pThis->m_flags,
                                OMX_COMPONENT_IDLE_PENDING))
                            {
                                if (!pThis->m_pSwVdec || pThis->m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY)
                                {
                                    if(pThis->stream_off(OMX_CORE_INPUT_PORT_INDEX)) {
                                        DEBUG_PRINT_ERROR("\n Failed to call streamoff on OUTPUT Port \n");
                                        pThis->omx_report_error ();
                                    } else {
                                        pThis->streaming[OUTPUT_PORT] = false;
                                    }
                                }
                            }
                        }
                    }
                    else
                    {
                        DEBUG_PRINT_ERROR("ERROR: %s()::EventHandler is NULL", __func__);
                    }
                }
                break;

            case OMX_COMPONENT_GENERATE_EVENT_OUTPUT_FLUSH:
                DEBUG_PRINT_HIGH("\n Driver flush o/p Port complete");
                if (!pThis->output_flush_progress)
                {
                    DEBUG_PRINT_ERROR("\n WARNING: Unexpected OUTPUT_FLUSH from driver");
                }
                else
                {
                    pThis->execute_output_flush();
                    if (pThis->m_interm_flush_dsp_progress)
                    {
                        pThis->execute_output_flush_dsp();
                    }
                    if (pThis->m_interm_flush_swvdec_progress)
                    {
                        pThis->execute_input_flush_swvdec();
                    }
                    if (pThis->m_cb.EventHandler)
                    {
                        if (p2 != VDEC_S_SUCCESS)
                        {
                            DEBUG_PRINT_ERROR("\n OMX_COMPONENT_GENERATE_EVENT_OUTPUT_FLUSH failed");
                            pThis->omx_report_error ();
                        }
                        else
                        {
                            /*Check if we need generate event for Flush done*/
                            if(BITMASK_PRESENT(&pThis->m_flags,
                                OMX_COMPONENT_OUTPUT_FLUSH_PENDING))
                            {
                                DEBUG_PRINT_LOW("\n Notify Output Flush done");
                                BITMASK_CLEAR (&pThis->m_flags,OMX_COMPONENT_OUTPUT_FLUSH_PENDING);
                                pThis->m_cb.EventHandler(&pThis->m_cmp, pThis->m_app_data,
                                    OMX_EventCmdComplete,OMX_CommandFlush,
                                    OMX_CORE_OUTPUT_PORT_INDEX,NULL );
                            }
                            if(BITMASK_PRESENT(&pThis->m_flags,
                                OMX_COMPONENT_OUTPUT_FLUSH_IN_DISABLE_PENDING))
                            {
                                DEBUG_PRINT_LOW("\n Internal flush complete");
                                BITMASK_CLEAR (&pThis->m_flags,
                                    OMX_COMPONENT_OUTPUT_FLUSH_IN_DISABLE_PENDING);
                                if (BITMASK_PRESENT(&pThis->m_flags,
                                    OMX_COMPONENT_DISABLE_OUTPUT_DEFERRED))
                                {
                                    pThis->post_event(OMX_CommandPortDisable,
                                        OMX_CORE_OUTPUT_PORT_INDEX,
                                        OMX_COMPONENT_GENERATE_EVENT);
                                    BITMASK_CLEAR (&pThis->m_flags,
                                        OMX_COMPONENT_DISABLE_OUTPUT_DEFERRED);
                                }
                            }

                            if (BITMASK_PRESENT(&pThis->m_flags ,OMX_COMPONENT_IDLE_PENDING))
                            {
                                if (!pThis->m_pSwVdec || pThis->m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY)
                                {
                                    if(pThis->stream_off(OMX_CORE_OUTPUT_PORT_INDEX)) {
                                        DEBUG_PRINT_ERROR("\n Failed to call streamoff on CAPTURE Port \n");
                                        pThis->omx_report_error ();
                                        break;
                                    }
                                    pThis->streaming[CAPTURE_PORT] = false;
                                }
                                if (!pThis->input_flush_progress)
                                {
                                    DEBUG_PRINT_LOW("\n Output flush done hence issue stop");
                                    pThis->post_event ((unsigned int)NULL, VDEC_S_SUCCESS,\
                                        OMX_COMPONENT_GENERATE_STOP_DONE);
                                }
                            }
                        }
                    }
                    else
                    {
                        DEBUG_PRINT_ERROR("ERROR: %s()::EventHandler is NULL", __func__);
                    }
                }
                break;

            case OMX_COMPONENT_GENERATE_EVENT_OUTPUT_FLUSH_DSP:
                DEBUG_PRINT_HIGH("\n Dsp Driver flush o/p Port complete");
                if (!pThis->m_interm_flush_dsp_progress)
                {
                    DEBUG_PRINT_ERROR("\n WARNING: Unexpected OUTPUT_FLUSH_DSP from driver");
                }
                else
                {
                    // check if we need to flush swvdec
                    bool bFlushSwVdec = false;
                    SWVDEC_BUFFER_FLUSH_TYPE aFlushType = SWVDEC_FLUSH_ALL;
                    if (pThis->m_interm_flush_swvdec_progress)
                    {
                        aFlushType = SWVDEC_FLUSH_ALL;
                        bFlushSwVdec = true;
                    }
                    else if (pThis->output_flush_progress)
                    {
                        DEBUG_PRINT_HIGH("\n Flush swvdec output only ");
                        aFlushType = SWVDEC_FLUSH_OUTPUT;
                        bFlushSwVdec = true;
                    }

                    DEBUG_PRINT_HIGH("\n Flush swvdec %d, interm flush %d output flush %d swvdec flushType %d",
                        bFlushSwVdec, pThis->m_interm_flush_swvdec_progress, pThis->output_flush_progress, aFlushType);

                    if (bFlushSwVdec)
                    {
                        if (SwVdec_Flush(pThis->m_pSwVdec, aFlushType) != SWVDEC_S_SUCCESS)
                        {
                            DEBUG_PRINT_ERROR("\n Flush swvdec Failed ");
                        }
                    }
                    else
                    {
                        pThis->execute_output_flush_dsp();
                    }
                }
                break;

            case OMX_COMPONENT_GENERATE_START_DONE:
                DEBUG_PRINT_HIGH("\n Rxd OMX_COMPONENT_GENERATE_START_DONE");

                if (pThis->m_cb.EventHandler)
                {
                    if (p2 != VDEC_S_SUCCESS)
                    {
                        DEBUG_PRINT_ERROR("\n OMX_COMPONENT_GENERATE_START_DONE Failure");
                        pThis->omx_report_error ();
                    }
                    else
                    {
                        DEBUG_PRINT_LOW("\n OMX_COMPONENT_GENERATE_START_DONE Success");
                        if(BITMASK_PRESENT(&pThis->m_flags,OMX_COMPONENT_EXECUTE_PENDING))
                        {
                            DEBUG_PRINT_LOW("\n Move to executing");
                            // Send the callback now
                            BITMASK_CLEAR((&pThis->m_flags),OMX_COMPONENT_EXECUTE_PENDING);
                            pThis->m_state = OMX_StateExecuting;
                            pThis->m_cb.EventHandler(&pThis->m_cmp, pThis->m_app_data,
                                OMX_EventCmdComplete,OMX_CommandStateSet,
                                OMX_StateExecuting, NULL);
                        }
                        else if (BITMASK_PRESENT(&pThis->m_flags,
                            OMX_COMPONENT_PAUSE_PENDING))
                        {
                            if (/*ioctl (pThis->drv_ctx.video_driver_fd,
                                VDEC_IOCTL_CMD_PAUSE,NULL ) < */0)
                            {
                                DEBUG_PRINT_ERROR("\n VDEC_IOCTL_CMD_PAUSE failed");
                                pThis->omx_report_error ();
                            }
                        }
                    }
                }
                else
                {
                    DEBUG_PRINT_LOW("\n Event Handler callback is NULL");
                }
                break;

            case OMX_COMPONENT_GENERATE_PAUSE_DONE:
                DEBUG_PRINT_HIGH("\n Rxd OMX_COMPONENT_GENERATE_PAUSE_DONE");
                if (pThis->m_cb.EventHandler)
                {
                    if (p2 != VDEC_S_SUCCESS)
                    {
                        DEBUG_PRINT_ERROR("OMX_COMPONENT_GENERATE_PAUSE_DONE ret failed");
                        pThis->omx_report_error ();
                    }
                    else
                    {
                        pThis->complete_pending_buffer_done_cbs();
                        if(BITMASK_PRESENT(&pThis->m_flags,OMX_COMPONENT_PAUSE_PENDING))
                        {
                            DEBUG_PRINT_LOW("\n OMX_COMPONENT_GENERATE_PAUSE_DONE nofity");
                            //Send the callback now
                            BITMASK_CLEAR((&pThis->m_flags),OMX_COMPONENT_PAUSE_PENDING);
                            pThis->m_state = OMX_StatePause;
                            pThis->m_cb.EventHandler(&pThis->m_cmp, pThis->m_app_data,
                                OMX_EventCmdComplete,OMX_CommandStateSet,
                                OMX_StatePause, NULL);
                        }
                    }
                }
                else
                {
                    DEBUG_PRINT_ERROR("ERROR: %s()::EventHandler is NULL", __func__);
                }

                break;

            case OMX_COMPONENT_GENERATE_RESUME_DONE:
                DEBUG_PRINT_HIGH("\n Rxd OMX_COMPONENT_GENERATE_RESUME_DONE");
                if (pThis->m_cb.EventHandler)
                {
                    if (p2 != VDEC_S_SUCCESS)
                    {
                        DEBUG_PRINT_ERROR("\n OMX_COMPONENT_GENERATE_RESUME_DONE failed");
                        pThis->omx_report_error ();
                    }
                    else
                    {
                        if(BITMASK_PRESENT(&pThis->m_flags,OMX_COMPONENT_EXECUTE_PENDING))
                        {
                            DEBUG_PRINT_LOW("\n Moving the decoder to execute state");
                            // Send the callback now
                            BITMASK_CLEAR((&pThis->m_flags),OMX_COMPONENT_EXECUTE_PENDING);
                            pThis->m_state = OMX_StateExecuting;
                            if (pThis->m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY)
                            {
                                pThis->fill_all_buffers_proxy_dsp(&pThis->m_cmp);
                            }
                            pThis->m_cb.EventHandler(&pThis->m_cmp, pThis->m_app_data,
                                OMX_EventCmdComplete,OMX_CommandStateSet,
                                OMX_StateExecuting,NULL);
                        }
                    }
                }
                else
                {
                    DEBUG_PRINT_ERROR("ERROR: %s()::EventHandler is NULL", __func__);
                }

                break;

            case OMX_COMPONENT_GENERATE_STOP_DONE:
                DEBUG_PRINT_HIGH("\n Rxd OMX_COMPONENT_GENERATE_STOP_DONE");
                if (pThis->m_cb.EventHandler)
                {
                    if (p2 != VDEC_S_SUCCESS)
                    {
                        DEBUG_PRINT_ERROR("\n OMX_COMPONENT_GENERATE_STOP_DONE ret failed");
                        pThis->omx_report_error ();
                    }
                    else
                    {
                        pThis->complete_pending_buffer_done_cbs();
                        if(BITMASK_PRESENT(&pThis->m_flags,OMX_COMPONENT_IDLE_PENDING))
                        {
                            DEBUG_PRINT_LOW("\n OMX_COMPONENT_GENERATE_STOP_DONE Success");
                            // Send the callback now
                            BITMASK_CLEAR((&pThis->m_flags),OMX_COMPONENT_IDLE_PENDING);
                            pThis->m_state = OMX_StateIdle;
                            DEBUG_PRINT_LOW("\n Move to Idle State");
                            pThis->m_cb.EventHandler(&pThis->m_cmp,pThis->m_app_data,
                                OMX_EventCmdComplete,OMX_CommandStateSet,
                                OMX_StateIdle,NULL);
                        }
                    }
                }
                else
                {
                    DEBUG_PRINT_ERROR("ERROR: %s()::EventHandler is NULL", __func__);
                }

                break;

            case OMX_COMPONENT_GENERATE_PORT_RECONFIG:
                DEBUG_PRINT_HIGH("\n Rxd OMX_COMPONENT_GENERATE_PORT_RECONFIG");

                if (p2 == OMX_IndexParamPortDefinition) {
                    pThis->in_reconfig = true;
                }
                if (pThis->m_cb.EventHandler) {
                    pThis->m_cb.EventHandler(&pThis->m_cmp, pThis->m_app_data,
                        OMX_EventPortSettingsChanged, p1, p2, NULL );
                } else {
                    DEBUG_PRINT_ERROR("ERROR: %s()::EventHandler is NULL", __func__);
                }

                if (pThis->drv_ctx.interlace != VDEC_InterlaceFrameProgressive)
                {
                    OMX_INTERLACETYPE format = (OMX_INTERLACETYPE)-1;
                    OMX_EVENTTYPE event = (OMX_EVENTTYPE)OMX_EventIndexsettingChanged;
                    if (pThis->drv_ctx.interlace == VDEC_InterlaceInterleaveFrameTopFieldFirst)
                        format = OMX_InterlaceInterleaveFrameTopFieldFirst;
                    else if (pThis->drv_ctx.interlace == VDEC_InterlaceInterleaveFrameBottomFieldFirst)
                        format = OMX_InterlaceInterleaveFrameBottomFieldFirst;
                    else //unsupported interlace format; raise a error
                        event = OMX_EventError;
                    if (pThis->m_cb.EventHandler) {
                        pThis->m_cb.EventHandler(&pThis->m_cmp, pThis->m_app_data,
                            event, format, 0, NULL );
                    } else {
                        DEBUG_PRINT_ERROR("ERROR: %s()::EventHandler is NULL", __func__);
                    }
                }
                break;

            case OMX_COMPONENT_GENERATE_EOS_DONE:
                DEBUG_PRINT_HIGH("\n Rxd OMX_COMPONENT_GENERATE_EOS_DONE");
                if (pThis->m_cb.EventHandler) {
                    pThis->m_cb.EventHandler(&pThis->m_cmp, pThis->m_app_data, OMX_EventBufferFlag,
                        OMX_CORE_OUTPUT_PORT_INDEX, OMX_BUFFERFLAG_EOS, NULL );
                } else {
                    DEBUG_PRINT_ERROR("ERROR: %s()::EventHandler is NULL", __func__);
                }
                pThis->prev_ts = LLONG_MAX;
                pThis->rst_prev_ts = true;
                break;

            case OMX_COMPONENT_GENERATE_HARDWARE_ERROR:
                DEBUG_PRINT_ERROR("\n OMX_COMPONENT_GENERATE_HARDWARE_ERROR");
                pThis->omx_report_error ();
                break;
            case OMX_COMPONENT_GENERATE_INFO_PORT_RECONFIG:
                {
                    DEBUG_PRINT_HIGH("\n Rxd OMX_COMPONENT_GENERATE_INFO_PORT_RECONFIG");
                    if (pThis->m_cb.EventHandler) {
                        pThis->m_cb.EventHandler(&pThis->m_cmp, pThis->m_app_data,
                            (OMX_EVENTTYPE)OMX_EventIndexsettingChanged, OMX_CORE_OUTPUT_PORT_INDEX, 0, NULL );
                    } else {
                        DEBUG_PRINT_ERROR("ERROR: %s()::EventHandler is NULL", __func__);
                    }
                }
                break;

            case OMX_COMPONENT_GENERATE_ETB_SWVDEC:
                {
                    DEBUG_PRINT_HIGH("\n Rxd OMX_COMPONENT_GENERATE_ETB_SWVDEC");
                    if (pThis->empty_this_buffer_proxy_swvdec((OMX_HANDLETYPE)p1,\
                        (OMX_BUFFERHEADERTYPE *)p2) != OMX_ErrorNone)
                    {
                        DEBUG_PRINT_ERROR("\n empty_this_buffer_proxy_swvdec failure");
                        pThis->omx_report_error ();
                    }
                }
                break;

            case OMX_COMPONENT_GENERATE_EBD_SWVDEC:
                {
                    DEBUG_PRINT_HIGH("\n Rxd OMX_COMPONENT_GENERATE_EBD_SWVDEC");
                    if (p2 != VDEC_S_SUCCESS)
                    {
                        DEBUG_PRINT_ERROR("\n OMX_COMPONENT_GENERATE_EBD_SWVDEC failure");
                        pThis->omx_report_error ();
                    }
                    else if ( pThis->empty_buffer_done_swvdec(&pThis->m_cmp,
                        (OMX_BUFFERHEADERTYPE *)p1) != OMX_ErrorNone)
                    {
                        DEBUG_PRINT_ERROR("\n empty_buffer_done_swvdec failure");
                        pThis->omx_report_error ();
                    }
                }
                break;

            case OMX_COMPONENT_GENERATE_FTB_DSP:
                {
                    DEBUG_PRINT_HIGH("\n Rxd OMX_COMPONENT_GENERATE_FTB_DSP");
                    if ( pThis->fill_this_buffer_proxy_dsp((OMX_HANDLETYPE)p1,\
                        (OMX_BUFFERHEADERTYPE *)p2) != OMX_ErrorNone)
                    {
                        DEBUG_PRINT_ERROR("\n fill_this_buffer_proxy_dsp failure");
                        pThis->omx_report_error ();
                    }
                }
                break;

            case OMX_COMPONENT_GENERATE_FBD_DSP:
                {
                    DEBUG_PRINT_HIGH("\n Rxd OMX_COMPONENT_GENERATE_FBD_DSP");
                    if (p2 != VDEC_S_SUCCESS)
                    {
                        DEBUG_PRINT_ERROR("\n OMX_COMPONENT_GENERATE_FBD_DSP failure");
                        pThis->omx_report_error ();
                    }
                    else if ( pThis->fill_buffer_done_dsp(&pThis->m_cmp,
                        (OMX_BUFFERHEADERTYPE *)p1) != OMX_ErrorNone )
                    {
                        DEBUG_PRINT_ERROR("\n fill_buffer_done failure");
                        pThis->omx_report_error ();
                    }


                }
                break;

            default:
                break;
            }
        }
        pthread_mutex_lock(&pThis->m_lock);
        qsize = pThis->m_cmd_q.m_size;
        if (pThis->m_state != OMX_StatePause)
            qsize += (pThis->m_ftb_q.m_size + pThis->m_etb_q.m_size +
            pThis->m_ftb_q_dsp.m_size + pThis->m_etb_q_swvdec.m_size);
        pthread_mutex_unlock(&pThis->m_lock);
    }
    while(qsize>0);

}

void omx_vdec::update_resolution(int width, int height)
{
    drv_ctx.video_resolution.frame_height = height;
    drv_ctx.video_resolution.frame_width = width;
    drv_ctx.video_resolution.scan_lines = height;
    drv_ctx.video_resolution.stride = width;
    rectangle.nLeft = 0;
    rectangle.nTop = 0;
    rectangle.nWidth = drv_ctx.video_resolution.frame_width;
    rectangle.nHeight = drv_ctx.video_resolution.frame_height;
}

/* ======================================================================
FUNCTION
omx_vdec::ComponentInit

DESCRIPTION
Initialize the component.

PARAMETERS
ctxt -- Context information related to the self.
id   -- Event identifier. This could be any of the following:
1. Command completion event
2. Buffer done callback event
3. Frame done callback event

RETURN VALUE
None.

========================================================================== */
OMX_ERRORTYPE omx_vdec::component_init(OMX_STRING role)
{

    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    struct v4l2_fmtdesc fdesc;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers bufreq;
    struct v4l2_control control;
    unsigned int   alignment = 0,buffer_size = 0;
    int fds[2];
    int r,ret=0;
    bool codec_ambiguous = false;

    // Copy the role information which provides the decoder kind
    strlcpy(drv_ctx.kind,role,128);
    strlcpy((char *)m_cRole, "video_decoder.hevc",OMX_MAX_STRINGNAME_SIZE);
    if(!strncmp(drv_ctx.kind, "OMX.qcom.video.decoder.hevchybrid",
        OMX_MAX_STRINGNAME_SIZE))
    {
        m_swvdec_mode = SWVDEC_MODE_DECODE_ONLY;
    }
    else if(!strncmp(drv_ctx.kind, "OMX.qcom.video.decoder.hevcswvdec",
        OMX_MAX_STRINGNAME_SIZE))
    {
        m_swvdec_mode = SWVDEC_MODE_PARSE_DECODE;
    }
    else if(!strncmp(drv_ctx.kind, "OMX.qcom.video.decoder.hevc",
        OMX_MAX_STRINGNAME_SIZE))
    {
        DEBUG_PRINT_ERROR("Full DSP mode\n");
        m_swvdec_mode = -1;
    }
    else {
        DEBUG_PRINT_ERROR("\nERROR:Unknown Component\n");
        return OMX_ErrorInvalidComponentName;
    }

#ifdef INPUT_BUFFER_LOG
    strcpy(inputfilename, INPUT_BUFFER_FILE_NAME);
    strcat(inputfilename, "265");
    inputBufferFile1 = fopen (inputfilename, "ab");
#endif
#ifdef OUTPUT_BUFFER_LOG
    outputBufferFile1 = fopen (outputfilename, "wb");
#endif
#ifdef OUTPUT_EXTRADATA_LOG
    outputExtradataFile = fopen (ouputextradatafilename, "ab");
#endif

    drv_ctx.decoder_format = VDEC_CODECTYPE_HEVC;
    eCompressionFormat = (OMX_VIDEO_CODINGTYPE)QOMX_VIDEO_CodingHevc;
    codec_type_parse = CODEC_TYPE_HEVC;
    m_frame_parser.init_start_codes (codec_type_parse);
    m_frame_parser.init_nal_length(nal_length);

    update_resolution(320, 240);
    drv_ctx.output_format = VDEC_YUV_FORMAT_NV12;
    OMX_COLOR_FORMATTYPE dest_color_format = (OMX_COLOR_FORMATTYPE)
        QOMX_COLOR_FORMATYUV420PackedSemiPlanar32m;
    if (!client_buffers.set_color_format(dest_color_format)) {
        DEBUG_PRINT_ERROR("\n Setting color format failed");
        eRet = OMX_ErrorInsufficientResources;
    }

    drv_ctx.frame_rate.fps_numerator = DEFAULT_FPS;
    drv_ctx.frame_rate.fps_denominator = 1;
    drv_ctx.ip_buf.buffer_type = VDEC_BUFFER_TYPE_INPUT;
    drv_ctx.interm_op_buf.buffer_type = VDEC_BUFFER_TYPE_OUTPUT;
    drv_ctx.op_buf.buffer_type = VDEC_BUFFER_TYPE_OUTPUT;
    if (secure_mode) {
        drv_ctx.interm_op_buf.alignment=SZ_1M;
        drv_ctx.op_buf.alignment=SZ_1M;
        drv_ctx.ip_buf.alignment=SZ_1M;
    } else {
        drv_ctx.op_buf.alignment=SZ_4K;
        drv_ctx.interm_op_buf.alignment=SZ_4K;
        drv_ctx.ip_buf.alignment=SZ_4K;
    }
    drv_ctx.interlace = VDEC_InterlaceFrameProgressive;
    drv_ctx.extradata = 0;
    drv_ctx.picture_order = VDEC_ORDER_DISPLAY;

    if (m_swvdec_mode >= 0)
    {
        // Init for SWCodec
        DEBUG_PRINT_HIGH("\n:Initializing SwVdec mode %d\n", m_swvdec_mode);
        memset(&sSwVdecParameter, 0, sizeof(SWVDEC_INITPARAMS));
        sSwVdecParameter.sDimensions.nWidth = 320;
        sSwVdecParameter.sDimensions.nHeight = 240;
        sSwVdecParameter.eDecType = SWVDEC_DECODER_HEVC;
        sSwVdecParameter.eColorFormat = SWVDEC_FORMAT_NV12;
        sSwVdecParameter.uProfile.eHevcProfile = SWVDEC_HEVC_MAIN_PROFILE;

        //SWVDEC_CALLBACK       m_callBackInfo;
        m_callBackInfo.FillBufferDone   = swvdec_fill_buffer_done_cb;
        m_callBackInfo.EmptyBufferDone  = swvdec_input_buffer_done_cb;
        m_callBackInfo.HandleEvent      = swvdec_handle_event_cb;
        m_callBackInfo.pClientHandle    = this;
        SWVDEC_STATUS sRet = SwVdec_Init(&sSwVdecParameter, &m_callBackInfo, &m_pSwVdec);
        if (sRet != SWVDEC_S_SUCCESS)
        {
            return OMX_ErrorInsufficientResources;
        }

        // set the swvdec mode
        SWVDEC_PROP prop;
        prop.ePropId = SWVDEC_PROP_ID_MODE;
        prop.uProperty.sMode.eMode = (SWVDEC_MODE_TYPE)m_swvdec_mode;
        SwVdec_SetProperty(m_pSwVdec, &prop);
    }

    if (!m_pSwVdec || m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY)
    {
        OMX_STRING device_name = (OMX_STRING)"/dev/video/q6_dec";
        drv_ctx.video_driver_fd = open(device_name, O_RDWR);

        DEBUG_PRINT_HIGH("\n omx_vdec::component_init(): Open device %s returned fd %d, errno %d",
            device_name, drv_ctx.video_driver_fd, errno);

        if(drv_ctx.video_driver_fd == 0){
            drv_ctx.video_driver_fd = open(device_name, O_RDWR);
        }

        if(drv_ctx.video_driver_fd < 0)
        {
            DEBUG_PRINT_ERROR("Omx_vdec::Comp Init Returning failure, errno %d\n", errno);
            return OMX_ErrorInsufficientResources;
        }

        ret = pthread_create(&async_thread_id,0,async_message_thread,this);
        if(ret < 0) {
            close(drv_ctx.video_driver_fd);
            DEBUG_PRINT_ERROR("\n Failed to create async_message_thread \n");
            return OMX_ErrorInsufficientResources;
        }

        capture_capability= V4L2_PIX_FMT_NV12;
        ret = subscribe_to_events(drv_ctx.video_driver_fd);
        if (ret) {
            DEBUG_PRINT_ERROR("\n Subscribe Event Failed \n");
            return OMX_ErrorInsufficientResources;
        }

        struct v4l2_capability cap;
        ret = ioctl(drv_ctx.video_driver_fd, VIDIOC_QUERYCAP, &cap);
        if (ret) {
            DEBUG_PRINT_ERROR("Failed to query capabilities\n");
            /*TODO: How to handle this case */
        } else {
            DEBUG_PRINT_HIGH("Capabilities: driver_name = %s, card = %s, bus_info = %s,"
                " version = %d, capabilities = %x\n", cap.driver, cap.card,
                cap.bus_info, cap.version, cap.capabilities);
        }
        ret=0;
        fdesc.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        fdesc.index=0;
        while (ioctl(drv_ctx.video_driver_fd, VIDIOC_ENUM_FMT, &fdesc) == 0) {
            DEBUG_PRINT_HIGH("fmt: description: %s, fmt: %x, flags = %x\n", fdesc.description,
                fdesc.pixelformat, fdesc.flags);
            fdesc.index++;
        }
        fdesc.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        fdesc.index=0;
        while (ioctl(drv_ctx.video_driver_fd, VIDIOC_ENUM_FMT, &fdesc) == 0) {

            DEBUG_PRINT_HIGH("fmt: description: %s, fmt: %x, flags = %x\n", fdesc.description,
                fdesc.pixelformat, fdesc.flags);
            fdesc.index++;
        }

        output_capability = V4L2_PIX_FMT_HEVC;
        if (m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY)
        {
            output_capability = V4L2_PIX_FMT_HEVC_HYBRID;
        }
        DEBUG_PRINT_HIGH("output_capability %d, V4L2_PIX_FMT_HEVC_HYBRID %d\n", output_capability, V4L2_PIX_FMT_HEVC_HYBRID);

        fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        fmt.fmt.pix_mp.height = drv_ctx.video_resolution.frame_height;
        fmt.fmt.pix_mp.width = drv_ctx.video_resolution.frame_width;
        fmt.fmt.pix_mp.pixelformat = output_capability;
        ret = ioctl(drv_ctx.video_driver_fd, VIDIOC_S_FMT, &fmt);
        if (ret) {
            /*TODO: How to handle this case */
            DEBUG_PRINT_ERROR("Failed to set format on output port\n");
        }
        else {
            DEBUG_PRINT_HIGH("\n Set Format was successful \n ");
        }
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        fmt.fmt.pix_mp.height = drv_ctx.video_resolution.frame_height;
        fmt.fmt.pix_mp.width = drv_ctx.video_resolution.frame_width;
        fmt.fmt.pix_mp.pixelformat = capture_capability;
        ret = ioctl(drv_ctx.video_driver_fd, VIDIOC_S_FMT, &fmt);
        if (ret) {
            /*TODO: How to handle this case */
            DEBUG_PRINT_ERROR("Failed to set format on capture port\n");
        }
        DEBUG_PRINT_HIGH("\n Set Format was successful \n ");
        if(secure_mode){
            control.id = V4L2_CID_MPEG_VIDC_VIDEO_SECURE;
            control.value = 1;
            DEBUG_PRINT_LOW("Omx_vdec:: calling to open secure device %d\n", ret);
            ret=ioctl(drv_ctx.video_driver_fd, VIDIOC_S_CTRL,&control);
            if (ret) {
                DEBUG_PRINT_ERROR("Omx_vdec:: Unable to open secure device %d\n", ret);
                close(drv_ctx.video_driver_fd);
                return OMX_ErrorInsufficientResources;
            }
        }

        /*Get the Buffer requirements for input(input buffer) and output ports(intermediate buffer) from Q6*/
        control.id = V4L2_CID_MPEG_VIDC_VIDEO_OUTPUT_ORDER;
        control.value = V4L2_MPEG_VIDC_VIDEO_OUTPUT_ORDER_DISPLAY;
        ret = ioctl(drv_ctx.video_driver_fd, VIDIOC_S_CTRL, &control);
        drv_ctx.idr_only_decoding = 0;
        eRet=get_buffer_req(&drv_ctx.ip_buf);
        DEBUG_PRINT_HIGH("Input Buffer Size =%d \n ",drv_ctx.ip_buf.buffer_size);
    }

    m_state = OMX_StateLoaded;
#ifdef DEFAULT_EXTRADATA
    if (eRet == OMX_ErrorNone && !secure_mode)
        enable_extradata(DEFAULT_EXTRADATA, true, true);
#endif

    get_buffer_req_swvdec();
    DEBUG_PRINT_HIGH("Interm Buffer Size %d Output Buffer Size =%d \n ",
        drv_ctx.interm_op_buf.buffer_size, drv_ctx.op_buf.buffer_size);

    h264_scratch.nAllocLen = drv_ctx.ip_buf.buffer_size;
    h264_scratch.pBuffer = (OMX_U8 *)malloc (drv_ctx.ip_buf.buffer_size);
    h264_scratch.nFilledLen = 0;
    h264_scratch.nOffset = 0;

    if (h264_scratch.pBuffer == NULL)
    {
        DEBUG_PRINT_ERROR("\n h264_scratch.pBuffer Allocation failed ");
        return OMX_ErrorInsufficientResources;
    }

    if(pipe(fds))
    {
        DEBUG_PRINT_ERROR("pipe creation failed\n");
        eRet = OMX_ErrorInsufficientResources;
    }
    else
    {
        if(fds[0] == 0 || fds[1] == 0)
        {
            if (pipe (fds))
            {
                DEBUG_PRINT_ERROR("pipe creation failed\n");
                return OMX_ErrorInsufficientResources;
            }
        }
        m_pipe_in = fds[0];
        m_pipe_out = fds[1];
        r = pthread_create(&msg_thread_id,0,message_thread,this);

        if(r < 0)
        {
            DEBUG_PRINT_ERROR("\n component_init(): message_thread creation failed");
            eRet = OMX_ErrorInsufficientResources;
        }
    }

    if (eRet != OMX_ErrorNone && ( (!m_pSwVdec) || (m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY) ))
    {
        DEBUG_PRINT_ERROR("\n Component Init Failed");
        DEBUG_PRINT_HIGH("\n Calling VDEC_IOCTL_STOP_NEXT_MSG");
        (void)ioctl(drv_ctx.video_driver_fd, VDEC_IOCTL_STOP_NEXT_MSG,
            NULL);
        DEBUG_PRINT_HIGH("\n Calling close() on Video Driver");
        close (drv_ctx.video_driver_fd);
        drv_ctx.video_driver_fd = -1;
    }
    else
    {
        DEBUG_PRINT_HIGH("\n omx_vdec::component_init() success");
    }
    //memset(&h264_mv_buff,0,sizeof(struct h264_mv_buffer));
    return eRet;
}

/* ======================================================================
FUNCTION
omx_vdec::GetComponentVersion

DESCRIPTION
Returns the component version.

PARAMETERS
TBD.

RETURN VALUE
OMX_ErrorNone.

========================================================================== */
OMX_ERRORTYPE  omx_vdec::get_component_version(
    OMX_IN OMX_HANDLETYPE hComp,
    OMX_OUT OMX_STRING componentName,
    OMX_OUT OMX_VERSIONTYPE* componentVersion,
    OMX_OUT OMX_VERSIONTYPE* specVersion,
    OMX_OUT OMX_UUIDTYPE* componentUUID
    )
{
    if(m_state == OMX_StateInvalid)
    {
        DEBUG_PRINT_ERROR("Get Comp Version in Invalid State\n");
        return OMX_ErrorInvalidState;
    }
    /* TBD -- Return the proper version */
    if (specVersion)
    {
        specVersion->nVersion = OMX_SPEC_VERSION;
    }
    return OMX_ErrorNone;
}
/* ======================================================================
FUNCTION
omx_vdec::SendCommand

DESCRIPTION
Returns zero if all the buffers released..

PARAMETERS
None.

RETURN VALUE
true/false

========================================================================== */
OMX_ERRORTYPE  omx_vdec::send_command(OMX_IN OMX_HANDLETYPE hComp,
                                      OMX_IN OMX_COMMANDTYPE cmd,
                                      OMX_IN OMX_U32 param1,
                                      OMX_IN OMX_PTR cmdData
                                      )
{
    DEBUG_PRINT_LOW("\n send_command: Recieved a Command from Client");
    if(m_state == OMX_StateInvalid)
    {
        DEBUG_PRINT_ERROR("ERROR: Send Command in Invalid State\n");
        return OMX_ErrorInvalidState;
    }
    if (cmd == OMX_CommandFlush && param1 != OMX_CORE_INPUT_PORT_INDEX
        && param1 != OMX_CORE_OUTPUT_PORT_INDEX && param1 != OMX_ALL)
    {
        DEBUG_PRINT_ERROR("\n send_command(): ERROR OMX_CommandFlush "
            "to invalid port: %lu", param1);
        return OMX_ErrorBadPortIndex;
    }
    post_event((unsigned)cmd,(unsigned)param1,OMX_COMPONENT_GENERATE_COMMAND);
    sem_wait(&m_cmd_lock);
    DEBUG_PRINT_LOW("\n send_command: Command Processed\n");
    return OMX_ErrorNone;
}

/* ======================================================================
FUNCTION
omx_vdec::SendCommand

DESCRIPTION
Returns zero if all the buffers released..

PARAMETERS
None.

RETURN VALUE
true/false

========================================================================== */
OMX_ERRORTYPE  omx_vdec::send_command_proxy(OMX_IN OMX_HANDLETYPE hComp,
                                            OMX_IN OMX_COMMANDTYPE cmd,
                                            OMX_IN OMX_U32 param1,
                                            OMX_IN OMX_PTR cmdData
                                            )
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    OMX_STATETYPE eState = (OMX_STATETYPE) param1;
    int bFlag = 1,sem_posted = 0,ret=0;

    DEBUG_PRINT_LOW("\n send_command_proxy(): cmd = %d", cmd);
    DEBUG_PRINT_HIGH("\n send_command_proxy(): Current State %d, Expected State %d",
        m_state, eState);

    if(cmd == OMX_CommandStateSet)
    {
        DEBUG_PRINT_HIGH("\n send_command_proxy(): OMX_CommandStateSet issued");
        DEBUG_PRINT_HIGH("\n Current State %d, Expected State %d", m_state, eState);
        /***************************/
        /* Current State is Loaded */
        /***************************/
        if(m_state == OMX_StateLoaded)
        {
            if(eState == OMX_StateIdle)
            {
                //if all buffers are allocated or all ports disabled
                if(allocate_done() ||
                    (m_inp_bEnabled == OMX_FALSE && m_out_bEnabled == OMX_FALSE))
                {
                    if (m_pSwVdec && SWVDEC_S_SUCCESS != SwVdec_Start(m_pSwVdec))
                    {
                        DEBUG_PRINT_ERROR("SWVDEC failed to start in allocate_done");
                        return OMX_ErrorInvalidState;
                    }
                    DEBUG_PRINT_LOW("send_command_proxy(): Loaded-->Idle\n");
                }
                else
                {
                    DEBUG_PRINT_LOW("send_command_proxy(): Loaded-->Idle-Pending\n");
                    BITMASK_SET(&m_flags, OMX_COMPONENT_IDLE_PENDING);
                    // Skip the event notification
                    bFlag = 0;
                }
            }
            /* Requesting transition from Loaded to Loaded */
            else if(eState == OMX_StateLoaded)
            {
                DEBUG_PRINT_ERROR("ERROR::send_command_proxy(): Loaded-->Loaded\n");
                post_event(OMX_EventError,OMX_ErrorSameState,\
                    OMX_COMPONENT_GENERATE_EVENT);
                eRet = OMX_ErrorSameState;
            }
            /* Requesting transition from Loaded to WaitForResources */
            else if(eState == OMX_StateWaitForResources)
            {
                /* Since error is None , we will post an event
                at the end of this function definition */
                DEBUG_PRINT_LOW("send_command_proxy(): Loaded-->WaitForResources\n");
            }
            /* Requesting transition from Loaded to Executing */
            else if(eState == OMX_StateExecuting)
            {
                DEBUG_PRINT_ERROR("ERROR::send_command_proxy(): Loaded-->Executing\n");
                post_event(OMX_EventError,OMX_ErrorIncorrectStateTransition,\
                    OMX_COMPONENT_GENERATE_EVENT);
                eRet = OMX_ErrorIncorrectStateTransition;
            }
            /* Requesting transition from Loaded to Pause */
            else if(eState == OMX_StatePause)
            {
                DEBUG_PRINT_ERROR("ERROR::send_command_proxy(): Loaded-->Pause\n");
                post_event(OMX_EventError,OMX_ErrorIncorrectStateTransition,\
                    OMX_COMPONENT_GENERATE_EVENT);
                eRet = OMX_ErrorIncorrectStateTransition;
            }
            /* Requesting transition from Loaded to Invalid */
            else if(eState == OMX_StateInvalid)
            {
                DEBUG_PRINT_ERROR("ERROR::send_command_proxy(): Loaded-->Invalid\n");
                post_event(OMX_EventError,eState,OMX_COMPONENT_GENERATE_EVENT);
                eRet = OMX_ErrorInvalidState;
            }
            else
            {
                DEBUG_PRINT_ERROR("ERROR::send_command_proxy(): Loaded-->Invalid(%d Not Handled)\n",\
                    eState);
                eRet = OMX_ErrorBadParameter;
            }
        }

        /***************************/
        /* Current State is IDLE */
        /***************************/
        else if(m_state == OMX_StateIdle)
        {
            if(eState == OMX_StateLoaded)
            {
                if(release_done())
                {
                    /*
                    Since error is None , we will post an event at the end
                    of this function definition
                    */
                    if (m_pSwVdec)
                    {
                        SwVdec_Stop(m_pSwVdec);
                    }
                    DEBUG_PRINT_LOW("send_command_proxy(): Idle-->Loaded\n");
                }
                else
                {
                    DEBUG_PRINT_LOW("send_command_proxy(): Idle-->Loaded-Pending\n");
                    BITMASK_SET(&m_flags, OMX_COMPONENT_LOADING_PENDING);
                    // Skip the event notification
                    bFlag = 0;
                }
            }
            /* Requesting transition from Idle to Executing */
            else if(eState == OMX_StateExecuting)
            {
                DEBUG_PRINT_LOW("send_command_proxy(): Idle-->Executing\n");
                //BITMASK_SET(&m_flags, OMX_COMPONENT_EXECUTE_PENDING);
                bFlag = 1;
                DEBUG_PRINT_LOW("send_command_proxy(): Idle-->Executing\n");
                m_state=OMX_StateExecuting;
                if (m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY)
                {
                    fill_all_buffers_proxy_dsp(hComp);
                }
                DEBUG_PRINT_HIGH("Stream On CAPTURE Was successful\n");
            }
            /* Requesting transition from Idle to Idle */
            else if(eState == OMX_StateIdle)
            {
                DEBUG_PRINT_ERROR("ERROR::send_command_proxy(): Idle-->Idle\n");
                post_event(OMX_EventError,OMX_ErrorSameState,\
                    OMX_COMPONENT_GENERATE_EVENT);
                eRet = OMX_ErrorSameState;
            }
            /* Requesting transition from Idle to WaitForResources */
            else if(eState == OMX_StateWaitForResources)
            {
                DEBUG_PRINT_ERROR("ERROR::send_command_proxy(): Idle-->WaitForResources\n");
                post_event(OMX_EventError,OMX_ErrorIncorrectStateTransition,\
                    OMX_COMPONENT_GENERATE_EVENT);
                eRet = OMX_ErrorIncorrectStateTransition;
            }
            /* Requesting transition from Idle to Pause */
            else if(eState == OMX_StatePause)
            {
                /*To pause the Video core we need to start the driver*/
                if (/*ioctl (drv_ctx.video_driver_fd,VDEC_IOCTL_CMD_START,
                    NULL) < */0)
                {
                    DEBUG_PRINT_ERROR("\n VDEC_IOCTL_CMD_START FAILED");
                    omx_report_error ();
                    eRet = OMX_ErrorHardware;
                }
                else
                {
                    BITMASK_SET(&m_flags,OMX_COMPONENT_PAUSE_PENDING);
                    DEBUG_PRINT_LOW("send_command_proxy(): Idle-->Pause\n");
                    bFlag = 0;
                }
            }
            /* Requesting transition from Idle to Invalid */
            else if(eState == OMX_StateInvalid)
            {
                DEBUG_PRINT_ERROR("ERROR::send_command_proxy(): Idle-->Invalid\n");
                post_event(OMX_EventError,eState,OMX_COMPONENT_GENERATE_EVENT);
                eRet = OMX_ErrorInvalidState;
            }
            else
            {
                DEBUG_PRINT_ERROR("ERROR::send_command_proxy(): Idle --> %d Not Handled\n",eState);
                eRet = OMX_ErrorBadParameter;
            }
        }

        /******************************/
        /* Current State is Executing */
        /******************************/
        else if(m_state == OMX_StateExecuting)
        {
            DEBUG_PRINT_LOW("\n Command Recieved in OMX_StateExecuting");
            /* Requesting transition from Executing to Idle */
            if(eState == OMX_StateIdle)
            {
                /* Since error is None , we will post an event
                at the end of this function definition
                */
                DEBUG_PRINT_LOW("\n send_command_proxy(): Executing --> Idle \n");
                BITMASK_SET(&m_flags,OMX_COMPONENT_IDLE_PENDING);
                if(!sem_posted)
                {
                    sem_posted = 1;
                    sem_post (&m_cmd_lock);
                    execute_omx_flush(OMX_ALL);
                }
                bFlag = 0;
            }
            /* Requesting transition from Executing to Paused */
            else if(eState == OMX_StatePause)
            {
                DEBUG_PRINT_LOW("\n PAUSE Command Issued");
                m_state = OMX_StatePause;
                bFlag = 1;
            }
            /* Requesting transition from Executing to Loaded */
            else if(eState == OMX_StateLoaded)
            {
                DEBUG_PRINT_ERROR("\n send_command_proxy(): Executing --> Loaded \n");
                post_event(OMX_EventError,OMX_ErrorIncorrectStateTransition,\
                    OMX_COMPONENT_GENERATE_EVENT);
                eRet = OMX_ErrorIncorrectStateTransition;
            }
            /* Requesting transition from Executing to WaitForResources */
            else if(eState == OMX_StateWaitForResources)
            {
                DEBUG_PRINT_ERROR("\n send_command_proxy(): Executing --> WaitForResources \n");
                post_event(OMX_EventError,OMX_ErrorIncorrectStateTransition,\
                    OMX_COMPONENT_GENERATE_EVENT);
                eRet = OMX_ErrorIncorrectStateTransition;
            }
            /* Requesting transition from Executing to Executing */
            else if(eState == OMX_StateExecuting)
            {
                DEBUG_PRINT_ERROR("\n send_command_proxy(): Executing --> Executing \n");
                post_event(OMX_EventError,OMX_ErrorSameState,\
                    OMX_COMPONENT_GENERATE_EVENT);
                eRet = OMX_ErrorSameState;
            }
            /* Requesting transition from Executing to Invalid */
            else if(eState == OMX_StateInvalid)
            {
                DEBUG_PRINT_ERROR("\n send_command_proxy(): Executing --> Invalid \n");
                post_event(OMX_EventError,eState,OMX_COMPONENT_GENERATE_EVENT);
                eRet = OMX_ErrorInvalidState;
            }
            else
            {
                DEBUG_PRINT_ERROR("ERROR::send_command_proxy(): Executing --> %d Not Handled\n",eState);
                eRet = OMX_ErrorBadParameter;
            }
        }
        /***************************/
        /* Current State is Pause  */
        /***************************/
        else if(m_state == OMX_StatePause)
        {
            /* Requesting transition from Pause to Executing */
            if(eState == OMX_StateExecuting)
            {
                DEBUG_PRINT_LOW("\n Pause --> Executing \n");
                m_state = OMX_StateExecuting;
                if (m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY)
                {
                    fill_all_buffers_proxy_dsp(hComp);
                }
                bFlag = 1;
            }
            /* Requesting transition from Pause to Idle */
            else if(eState == OMX_StateIdle)
            {
                /* Since error is None , we will post an event
                at the end of this function definition */
                DEBUG_PRINT_LOW("\n Pause --> Idle \n");
                BITMASK_SET(&m_flags,OMX_COMPONENT_IDLE_PENDING);
                if(!sem_posted)
                {
                    sem_posted = 1;
                    sem_post (&m_cmd_lock);
                    execute_omx_flush(OMX_ALL);
                }
                bFlag = 0;
            }
            /* Requesting transition from Pause to loaded */
            else if(eState == OMX_StateLoaded)
            {
                DEBUG_PRINT_ERROR("\n Pause --> loaded \n");
                post_event(OMX_EventError,OMX_ErrorIncorrectStateTransition,\
                    OMX_COMPONENT_GENERATE_EVENT);
                eRet = OMX_ErrorIncorrectStateTransition;
            }
            /* Requesting transition from Pause to WaitForResources */
            else if(eState == OMX_StateWaitForResources)
            {
                DEBUG_PRINT_ERROR("\n Pause --> WaitForResources \n");
                post_event(OMX_EventError,OMX_ErrorIncorrectStateTransition,\
                    OMX_COMPONENT_GENERATE_EVENT);
                eRet = OMX_ErrorIncorrectStateTransition;
            }
            /* Requesting transition from Pause to Pause */
            else if(eState == OMX_StatePause)
            {
                DEBUG_PRINT_ERROR("\n Pause --> Pause \n");
                post_event(OMX_EventError,OMX_ErrorSameState,\
                    OMX_COMPONENT_GENERATE_EVENT);
                eRet = OMX_ErrorSameState;
            }
            /* Requesting transition from Pause to Invalid */
            else if(eState == OMX_StateInvalid)
            {
                DEBUG_PRINT_ERROR("\n Pause --> Invalid \n");
                post_event(OMX_EventError,eState,OMX_COMPONENT_GENERATE_EVENT);
                eRet = OMX_ErrorInvalidState;
            }
            else
            {
                DEBUG_PRINT_ERROR("ERROR::send_command_proxy(): Paused --> %d Not Handled\n",eState);
                eRet = OMX_ErrorBadParameter;
            }
        }
        /***************************/
        /* Current State is WaitForResources  */
        /***************************/
        else if(m_state == OMX_StateWaitForResources)
        {
            /* Requesting transition from WaitForResources to Loaded */
            if(eState == OMX_StateLoaded)
            {
                /* Since error is None , we will post an event
                at the end of this function definition */
                DEBUG_PRINT_LOW("send_command_proxy(): WaitForResources-->Loaded\n");
            }
            /* Requesting transition from WaitForResources to WaitForResources */
            else if (eState == OMX_StateWaitForResources)
            {
                DEBUG_PRINT_ERROR("ERROR::send_command_proxy(): WaitForResources-->WaitForResources\n");
                post_event(OMX_EventError,OMX_ErrorSameState,
                    OMX_COMPONENT_GENERATE_EVENT);
                eRet = OMX_ErrorSameState;
            }
            /* Requesting transition from WaitForResources to Executing */
            else if(eState == OMX_StateExecuting)
            {
                DEBUG_PRINT_ERROR("ERROR::send_command_proxy(): WaitForResources-->Executing\n");
                post_event(OMX_EventError,OMX_ErrorIncorrectStateTransition,\
                    OMX_COMPONENT_GENERATE_EVENT);
                eRet = OMX_ErrorIncorrectStateTransition;
            }
            /* Requesting transition from WaitForResources to Pause */
            else if(eState == OMX_StatePause)
            {
                DEBUG_PRINT_ERROR("ERROR::send_command_proxy(): WaitForResources-->Pause\n");
                post_event(OMX_EventError,OMX_ErrorIncorrectStateTransition,\
                    OMX_COMPONENT_GENERATE_EVENT);
                eRet = OMX_ErrorIncorrectStateTransition;
            }
            /* Requesting transition from WaitForResources to Invalid */
            else if(eState == OMX_StateInvalid)
            {
                DEBUG_PRINT_ERROR("ERROR::send_command_proxy(): WaitForResources-->Invalid\n");
                post_event(OMX_EventError,eState,OMX_COMPONENT_GENERATE_EVENT);
                eRet = OMX_ErrorInvalidState;
            }
            /* Requesting transition from WaitForResources to Loaded -
            is NOT tested by Khronos TS */

        }
        else
        {
            DEBUG_PRINT_ERROR("ERROR::send_command_proxy(): %d --> %d(Not Handled)\n",m_state,eState);
            eRet = OMX_ErrorBadParameter;
        }
    }
    /********************************/
    /* Current State is Invalid */
    /*******************************/
    else if(m_state == OMX_StateInvalid)
    {
        /* State Transition from Inavlid to any state */
        if(eState == (OMX_StateLoaded || OMX_StateWaitForResources
            || OMX_StateIdle || OMX_StateExecuting
            || OMX_StatePause || OMX_StateInvalid))
        {
            DEBUG_PRINT_ERROR("ERROR::send_command_proxy(): Invalid -->Loaded\n");
            post_event(OMX_EventError,OMX_ErrorInvalidState,\
                OMX_COMPONENT_GENERATE_EVENT);
            eRet = OMX_ErrorInvalidState;
        }
    }
    else if (cmd == OMX_CommandFlush)
    {
        DEBUG_PRINT_HIGH("\n send_command_proxy(): OMX_CommandFlush issued"
            "with param1: %lu", param1);
        if(OMX_CORE_INPUT_PORT_INDEX == param1 || OMX_ALL == param1)
        {
            BITMASK_SET(&m_flags, OMX_COMPONENT_INPUT_FLUSH_PENDING);
        }
        if(OMX_CORE_OUTPUT_PORT_INDEX == param1 || OMX_ALL == param1)
        {
            BITMASK_SET(&m_flags, OMX_COMPONENT_OUTPUT_FLUSH_PENDING);
        }
        if (!sem_posted){
            sem_posted = 1;
            DEBUG_PRINT_LOW("\n Set the Semaphore");
            sem_post (&m_cmd_lock);
            execute_omx_flush(param1);
        }
        bFlag = 0;
    }
    else if ( cmd == OMX_CommandPortEnable)
    {
        DEBUG_PRINT_HIGH("\n send_command_proxy(): OMX_CommandPortEnable issued"
            "with param1: %lu", param1);
        if(param1 == OMX_CORE_INPUT_PORT_INDEX || param1 == OMX_ALL)
        {
            m_inp_bEnabled = OMX_TRUE;

            if( (m_state == OMX_StateLoaded &&
                !BITMASK_PRESENT(&m_flags,OMX_COMPONENT_IDLE_PENDING))
                || allocate_input_done())
            {
                post_event(OMX_CommandPortEnable,OMX_CORE_INPUT_PORT_INDEX,
                    OMX_COMPONENT_GENERATE_EVENT);
            }
            else
            {
                DEBUG_PRINT_LOW("send_command_proxy(): Disabled-->Enabled Pending\n");
                BITMASK_SET(&m_flags, OMX_COMPONENT_INPUT_ENABLE_PENDING);
                // Skip the event notification
                bFlag = 0;
            }
        }
        if(param1 == OMX_CORE_OUTPUT_PORT_INDEX || param1 == OMX_ALL)
        {
            DEBUG_PRINT_LOW("\n Enable output Port command recieved");
            m_out_bEnabled = OMX_TRUE;
            if( (m_state == OMX_StateLoaded &&
                !BITMASK_PRESENT(&m_flags,OMX_COMPONENT_IDLE_PENDING))
                || (allocate_output_done()))
            {
                post_event(OMX_CommandPortEnable,OMX_CORE_OUTPUT_PORT_INDEX,
                    OMX_COMPONENT_GENERATE_EVENT);
            }
            else
            {
                DEBUG_PRINT_LOW("send_command_proxy(): Disabled-->Enabled Pending\n");
                BITMASK_SET(&m_flags, OMX_COMPONENT_OUTPUT_ENABLE_PENDING);
                // Skip the event notification
                bFlag = 0;
            }
        }
    }
    else if (cmd == OMX_CommandPortDisable)
    {
        DEBUG_PRINT_HIGH("\n send_command_proxy(): OMX_CommandPortDisable issued"
            "with param1: %lu", param1);
        if(param1 == OMX_CORE_INPUT_PORT_INDEX || param1 == OMX_ALL)
        {
            m_inp_bEnabled = OMX_FALSE;
            if((m_state == OMX_StateLoaded || m_state == OMX_StateIdle)
                && release_input_done())
            {
                post_event(OMX_CommandPortDisable,OMX_CORE_INPUT_PORT_INDEX,
                    OMX_COMPONENT_GENERATE_EVENT);
            }
            else
            {
                BITMASK_SET(&m_flags, OMX_COMPONENT_INPUT_DISABLE_PENDING);
                if(m_state == OMX_StatePause ||m_state == OMX_StateExecuting)
                {
                    if(!sem_posted)
                    {
                        sem_posted = 1;
                        sem_post (&m_cmd_lock);
                    }
                    execute_omx_flush(OMX_CORE_INPUT_PORT_INDEX);
                }

                // Skip the event notification
                bFlag = 0;
            }
        }
        if(param1 == OMX_CORE_OUTPUT_PORT_INDEX || param1 == OMX_ALL)
        {
            m_out_bEnabled = OMX_FALSE;
            DEBUG_PRINT_LOW("\n Disable output Port command recieved");
            if((m_state == OMX_StateLoaded || m_state == OMX_StateIdle)
                && release_output_done())
            {
                post_event(OMX_CommandPortDisable,OMX_CORE_OUTPUT_PORT_INDEX,\
                    OMX_COMPONENT_GENERATE_EVENT);
            }
            else
            {
                BITMASK_SET(&m_flags, OMX_COMPONENT_OUTPUT_DISABLE_PENDING);
                if(m_state == OMX_StatePause ||m_state == OMX_StateExecuting)
                {
                    if (!sem_posted)
                    {
                        sem_posted = 1;
                        sem_post (&m_cmd_lock);
                    }
                    BITMASK_SET(&m_flags, OMX_COMPONENT_OUTPUT_FLUSH_IN_DISABLE_PENDING);
                    execute_omx_flush(OMX_CORE_OUTPUT_PORT_INDEX);
                }
                // Skip the event notification
                bFlag = 0;

            }
        }
    }
    else
    {
        DEBUG_PRINT_ERROR("Error: Invalid Command other than StateSet (%d)\n",cmd);
        eRet = OMX_ErrorNotImplemented;
    }
    if(eRet == OMX_ErrorNone && bFlag)
    {
        post_event(cmd,eState,OMX_COMPONENT_GENERATE_EVENT);
    }
    if(!sem_posted)
    {
        sem_post(&m_cmd_lock);
    }

    return eRet;
}

/* ======================================================================
FUNCTION
omx_vdec::ExecuteOmxFlush

DESCRIPTION
Executes the OMX flush.

PARAMETERS
flushtype - input flush(1)/output flush(0)/ both.

RETURN VALUE
true/false

========================================================================== */
bool omx_vdec::execute_omx_flush(OMX_U32 flushType)
{
    bool bRet = false;
    struct v4l2_plane plane;
    struct v4l2_buffer v4l2_buf;
    struct v4l2_decoder_cmd dec;
    DEBUG_PRINT_LOW("in %s flushType %d", __func__, (int)flushType);
    memset((void *)&v4l2_buf,0,sizeof(v4l2_buf));
    dec.cmd = V4L2_DEC_QCOM_CMD_FLUSH;
    switch (flushType)
    {
    case OMX_CORE_INPUT_PORT_INDEX:
        input_flush_progress = true;
        dec.flags = V4L2_DEC_QCOM_CMD_FLUSH_OUTPUT;
        break;
    case OMX_CORE_OUTPUT_PORT_INDEX:
        output_flush_progress = true;
        dec.flags = V4L2_DEC_QCOM_CMD_FLUSH_CAPTURE;
        if (m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY)
        {
            m_interm_flush_swvdec_progress = true;
            m_interm_flush_dsp_progress = true;
        }
        break;
    default:
        input_flush_progress = true;
        output_flush_progress = true;
        if (m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY)
        {
            m_interm_flush_swvdec_progress = true;
            m_interm_flush_dsp_progress = true;
        }
        dec.flags = V4L2_DEC_QCOM_CMD_FLUSH_OUTPUT |
            V4L2_DEC_QCOM_CMD_FLUSH_CAPTURE;
    }

    if (!m_pSwVdec || m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY)
    {
        DEBUG_PRINT_LOW("flush dsp %d %d %d", dec.flags, V4L2_DEC_QCOM_CMD_FLUSH_OUTPUT, V4L2_DEC_QCOM_CMD_FLUSH_CAPTURE);
        if (ioctl(drv_ctx.video_driver_fd, VIDIOC_DECODER_CMD, &dec))
        {
            DEBUG_PRINT_ERROR("\n Flush dsp Failed ");
            bRet = false;
        }
    }
    if (flushType == OMX_CORE_INPUT_PORT_INDEX)
    {
        // no input flush independently, wait for output flush
        return bRet;
    }
    if (m_swvdec_mode == SWVDEC_MODE_PARSE_DECODE)
    {
        // for hybrid mode, swvdec flush will hapeen when dsp flush done is received
        SWVDEC_BUFFER_FLUSH_TYPE aFlushType = SWVDEC_FLUSH_OUTPUT;
        if (m_interm_flush_swvdec_progress  || input_flush_progress)
        {
            aFlushType = SWVDEC_FLUSH_ALL;
        }
        DEBUG_PRINT_HIGH("\n Flush swvdec type %d", aFlushType);
        if (SwVdec_Flush(m_pSwVdec, aFlushType) != SWVDEC_S_SUCCESS)
        {
            DEBUG_PRINT_ERROR("\n Flush swvdec Failed ");
        }
    }
    return bRet;
}
/*=========================================================================
FUNCTION : execute_output_flush

DESCRIPTION
Executes the OMX flush at OUTPUT PORT.

PARAMETERS
None.

RETURN VALUE
true/false
==========================================================================*/
bool omx_vdec::execute_output_flush()
{
    unsigned      p1 = 0; // Parameter - 1
    unsigned      p2 = 0; // Parameter - 2
    unsigned      ident = 0;
    bool bRet = true;

    /*Generate FBD for all Buffers in the FTBq*/
    pthread_mutex_lock(&m_lock);
    DEBUG_PRINT_LOW("\n Initiate Output Flush");
    while (m_ftb_q.m_size)
    {
        DEBUG_PRINT_LOW("\n Buffer queue size %d pending buf cnt %d",
            m_ftb_q.m_size,pending_output_buffers);
        m_ftb_q.pop_entry(&p1,&p2,&ident);
        DEBUG_PRINT_LOW("\n ID(%x) P1(%x) P2(%x)", ident, p1, p2);
        if(ident == m_fill_output_msg )
        {
            m_cb.FillBufferDone(&m_cmp, m_app_data, (OMX_BUFFERHEADERTYPE *)p2);
        }
        else if (ident == OMX_COMPONENT_GENERATE_FBD)
        {
            fill_buffer_done(&m_cmp,(OMX_BUFFERHEADERTYPE *)p1);
        }
    }
    pthread_mutex_unlock(&m_lock);
    output_flush_progress = false;

    if (arbitrary_bytes)
    {
        prev_ts = LLONG_MAX;
        rst_prev_ts = true;
    }
    DEBUG_PRINT_HIGH("\n OMX flush o/p Port complete PenBuf(%d)", pending_output_buffers);
    return bRet;
}
/*=========================================================================
FUNCTION : execute_input_flush

DESCRIPTION
Executes the OMX flush at INPUT PORT.

PARAMETERS
None.

RETURN VALUE
true/false
==========================================================================*/
bool omx_vdec::execute_input_flush()
{
    unsigned       i =0;
    unsigned      p1 = 0; // Parameter - 1
    unsigned      p2 = 0; // Parameter - 2
    unsigned      ident = 0;
    bool bRet = true;

    /*Generate EBD for all Buffers in the ETBq*/
    DEBUG_PRINT_LOW("\n Initiate Input Flush \n");

    pthread_mutex_lock(&m_lock);
    DEBUG_PRINT_LOW("\n Check if the Queue is empty \n");
    while (m_etb_q.m_size)
    {
        m_etb_q.pop_entry(&p1,&p2,&ident);

        if (ident == OMX_COMPONENT_GENERATE_ETB_ARBITRARY)
        {
            DEBUG_PRINT_LOW("\n Flush Input Heap Buffer %p",(OMX_BUFFERHEADERTYPE *)p2);
            m_cb.EmptyBufferDone(&m_cmp ,m_app_data, (OMX_BUFFERHEADERTYPE *)p2);
        }
        else if(ident == OMX_COMPONENT_GENERATE_ETB)
        {
            pending_input_buffers++;
            DEBUG_PRINT_LOW("\n Flush Input OMX_COMPONENT_GENERATE_ETB %p, pending_input_buffers %d",
                (OMX_BUFFERHEADERTYPE *)p2, pending_input_buffers);
            empty_buffer_done(&m_cmp,(OMX_BUFFERHEADERTYPE *)p2);
        }
        else if (ident == OMX_COMPONENT_GENERATE_EBD)
        {
            DEBUG_PRINT_LOW("\n Flush Input OMX_COMPONENT_GENERATE_EBD %p",
                (OMX_BUFFERHEADERTYPE *)p1);
            empty_buffer_done(&m_cmp,(OMX_BUFFERHEADERTYPE *)p1);
        }
    }
    time_stamp_dts.flush_timestamp();
    /*Check if Heap Buffers are to be flushed*/
    if (arbitrary_bytes && !(codec_config_flag))
    {
        DEBUG_PRINT_LOW("\n Reset all the variables before flusing");
        h264_scratch.nFilledLen = 0;
        nal_count = 0;
        look_ahead_nal = false;
        frame_count = 0;
        h264_last_au_ts = LLONG_MAX;
        h264_last_au_flags = 0;
        memset(m_demux_offsets, 0, ( sizeof(OMX_U32) * 8192) );
        m_demux_entries = 0;
        DEBUG_PRINT_LOW("\n Initialize parser");
        if (m_frame_parser.mutils)
        {
            m_frame_parser.mutils->initialize_frame_checking_environment();
        }

        while (m_input_pending_q.m_size)
        {
            m_input_pending_q.pop_entry(&p1,&p2,&ident);
            m_cb.EmptyBufferDone(&m_cmp ,m_app_data, (OMX_BUFFERHEADERTYPE *)p1);
        }

        if (psource_frame)
        {
            m_cb.EmptyBufferDone(&m_cmp ,m_app_data,psource_frame);
            psource_frame = NULL;
        }

        if (pdest_frame)
        {
            pdest_frame->nFilledLen = 0;
            m_input_free_q.insert_entry((unsigned) pdest_frame, (unsigned int)NULL,
                (unsigned int)NULL);
            pdest_frame = NULL;
        }
        m_frame_parser.flush();
    }
    else if (codec_config_flag)
    {
        DEBUG_PRINT_HIGH("frame_parser flushing skipped due to codec config buffer "
            "is not sent to the driver yet");
    }
    pthread_mutex_unlock(&m_lock);
    input_flush_progress = false;
    if (!arbitrary_bytes)
    {
        prev_ts = LLONG_MAX;
        rst_prev_ts = true;
    }
#ifdef _ANDROID_
    if (m_debug_timestamp)
    {
        m_timestamp_list.reset_ts_list();
    }
#endif
    DEBUG_PRINT_HIGH("\n OMX flush i/p Port complete PenBuf(%d)", pending_input_buffers);
    return bRet;
}


/* ======================================================================
FUNCTION
omx_vdec::SendCommandEvent

DESCRIPTION
Send the event to decoder pipe.  This is needed to generate the callbacks
in decoder thread context.

PARAMETERS
None.

RETURN VALUE
true/false

========================================================================== */
bool omx_vdec::post_event(unsigned int p1,
                          unsigned int p2,
                          unsigned int id)
{
    bool bRet = false;
    OMX_BUFFERHEADERTYPE* bufHdr = NULL;

    pthread_mutex_lock(&m_lock);

    if (id == OMX_COMPONENT_GENERATE_FTB ||
        id == OMX_COMPONENT_GENERATE_FBD)
    {
        m_ftb_q.insert_entry(p1,p2,id);
    }
    else if (id == OMX_COMPONENT_GENERATE_ETB ||
        id == OMX_COMPONENT_GENERATE_EBD ||
        id == OMX_COMPONENT_GENERATE_ETB_ARBITRARY)
    {
        m_etb_q.insert_entry(p1,p2,id);
    }
    else if (id == OMX_COMPONENT_GENERATE_FTB_DSP)
    {
        bufHdr = (OMX_BUFFERHEADERTYPE*)p2;
        m_ftb_q_dsp.insert_entry(p1,p2,id);
    }
    else if (id == OMX_COMPONENT_GENERATE_ETB_SWVDEC)
    {
        bufHdr = (OMX_BUFFERHEADERTYPE*)p2;
        m_etb_q_swvdec.insert_entry(p1,p2,id);
    }
    else if (id == OMX_COMPONENT_GENERATE_FBD_DSP)
    {
        bufHdr = (OMX_BUFFERHEADERTYPE*)p1;
        m_ftb_q_dsp.insert_entry(p1,p2,id);
    }
    else if (id == OMX_COMPONENT_GENERATE_EBD_SWVDEC)
    {
        bufHdr = (OMX_BUFFERHEADERTYPE*)p1;
        m_etb_q_swvdec.insert_entry(p1,p2,id);
    }
    else
    {
        m_cmd_q.insert_entry(p1,p2,id);
    }

    bRet = true;
    post_message(this, id);
    pthread_mutex_unlock(&m_lock);

    return bRet;
}

OMX_ERRORTYPE omx_vdec::get_supported_profile_level_for_1080p(OMX_VIDEO_PARAM_PROFILELEVELTYPE *profileLevelType)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNoMore;
    if(!profileLevelType)
        return OMX_ErrorBadParameter;

    if(profileLevelType->nPortIndex == 0) {
        if (!strncmp(drv_ctx.kind, "OMX.qcom.video.decoder.hevchybrid",OMX_MAX_STRINGNAME_SIZE) ||
            !strncmp(drv_ctx.kind, "OMX.qcom.video.decoder.hevcswvdec",OMX_MAX_STRINGNAME_SIZE) ||
            !strncmp(drv_ctx.kind, "OMX.qcom.video.decoder.hevc",OMX_MAX_STRINGNAME_SIZE) )
        {
            DEBUG_PRINT_LOW("get_parameter: OMX_IndexParamVideoProfileLevelQuerySupported nProfileIndex ret NoMore %d\n",
                (int)profileLevelType->nProfileIndex);
            eRet = OMX_ErrorNoMore;
        }
        else
        {
            DEBUG_PRINT_ERROR("get_parameter: OMX_IndexParamVideoProfileLevelQuerySupported nProfileIndex ret NoMore %lu\n", profileLevelType->nProfileIndex);
            eRet = OMX_ErrorNoMore;
        }
    }
    else
    {
        DEBUG_PRINT_ERROR("get_parameter: OMX_IndexParamVideoProfileLevelQuerySupported should be queries on Input port only %lu\n", profileLevelType->nPortIndex);
        eRet = OMX_ErrorBadPortIndex;
    }
    return eRet;
}

/* ======================================================================
FUNCTION
omx_vdec::GetParameter

DESCRIPTION
OMX Get Parameter method implementation

PARAMETERS
<TBD>.

RETURN VALUE
Error None if successful.

========================================================================== */
OMX_ERRORTYPE  omx_vdec::get_parameter(OMX_IN OMX_HANDLETYPE     hComp,
                                       OMX_IN OMX_INDEXTYPE paramIndex,
                                       OMX_INOUT OMX_PTR     paramData)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;

    DEBUG_PRINT_LOW("get_parameter: \n");
    if(m_state == OMX_StateInvalid)
    {
        DEBUG_PRINT_ERROR("Get Param in Invalid State\n");
        return OMX_ErrorInvalidState;
    }
    if(paramData == NULL)
    {
        DEBUG_PRINT_LOW("Get Param in Invalid paramData \n");
        return OMX_ErrorBadParameter;
    }
    switch((unsigned long)paramIndex)
    {
    case OMX_IndexParamPortDefinition:
        {
            OMX_PARAM_PORTDEFINITIONTYPE *portDefn =
                (OMX_PARAM_PORTDEFINITIONTYPE *) paramData;
            DEBUG_PRINT_LOW("get_parameter: OMX_IndexParamPortDefinition\n");
            eRet = update_portdef(portDefn);
            if (eRet == OMX_ErrorNone)
                m_port_def = *portDefn;
            break;
        }
    case OMX_IndexParamVideoInit:
        {
            OMX_PORT_PARAM_TYPE *portParamType =
                (OMX_PORT_PARAM_TYPE *) paramData;
            DEBUG_PRINT_LOW("get_parameter: OMX_IndexParamVideoInit\n");

            portParamType->nVersion.nVersion = OMX_SPEC_VERSION;
            portParamType->nSize = sizeof(portParamType);
            portParamType->nPorts           = 2;
            portParamType->nStartPortNumber = 0;
            break;
        }
    case OMX_IndexParamVideoPortFormat:
        {
            OMX_VIDEO_PARAM_PORTFORMATTYPE *portFmt =
                (OMX_VIDEO_PARAM_PORTFORMATTYPE *)paramData;
            DEBUG_PRINT_LOW("get_parameter: OMX_IndexParamVideoPortFormat\n");

            portFmt->nVersion.nVersion = OMX_SPEC_VERSION;
            portFmt->nSize             = sizeof(portFmt);

            if (0 == portFmt->nPortIndex)
            {
                if (0 == portFmt->nIndex)
                {
                    portFmt->eColorFormat =  OMX_COLOR_FormatUnused;
                    portFmt->eCompressionFormat = eCompressionFormat;
                }
                else
                {
                    DEBUG_PRINT_ERROR("get_parameter: OMX_IndexParamVideoPortFormat:"\
                        " NoMore compression formats\n");
                    eRet =  OMX_ErrorNoMore;
                }
            }
            else if (1 == portFmt->nPortIndex)
            {
                portFmt->eCompressionFormat =  OMX_VIDEO_CodingUnused;

                if(0 == portFmt->nIndex)
                    portFmt->eColorFormat = (OMX_COLOR_FORMATTYPE)
                    QOMX_COLOR_FORMATYUV420PackedSemiPlanar32m;
                else if (1 == portFmt->nIndex)
                    portFmt->eColorFormat = OMX_COLOR_FormatYUV420Planar;
                else
                {
                    DEBUG_PRINT_LOW("get_parameter: OMX_IndexParamVideoPortFormat:"\
                        " NoMore Color formats\n");
                    eRet =  OMX_ErrorNoMore;
                }
            }
            else
            {
                DEBUG_PRINT_ERROR("get_parameter: Bad port index %d\n",
                    (int)portFmt->nPortIndex);
                eRet = OMX_ErrorBadPortIndex;
            }
            break;
        }
        /*Component should support this port definition*/
    case OMX_IndexParamAudioInit:
        {
            OMX_PORT_PARAM_TYPE *audioPortParamType =
                (OMX_PORT_PARAM_TYPE *) paramData;
            DEBUG_PRINT_LOW("get_parameter: OMX_IndexParamAudioInit\n");
            audioPortParamType->nVersion.nVersion = OMX_SPEC_VERSION;
            audioPortParamType->nSize = sizeof(audioPortParamType);
            audioPortParamType->nPorts           = 0;
            audioPortParamType->nStartPortNumber = 0;
            break;
        }
        /*Component should support this port definition*/
    case OMX_IndexParamImageInit:
        {
            OMX_PORT_PARAM_TYPE *imagePortParamType =
                (OMX_PORT_PARAM_TYPE *) paramData;
            DEBUG_PRINT_LOW("get_parameter: OMX_IndexParamImageInit\n");
            imagePortParamType->nVersion.nVersion = OMX_SPEC_VERSION;
            imagePortParamType->nSize = sizeof(imagePortParamType);
            imagePortParamType->nPorts           = 0;
            imagePortParamType->nStartPortNumber = 0;
            break;

        }
        /*Component should support this port definition*/
    case OMX_IndexParamOtherInit:
        {
            DEBUG_PRINT_ERROR("get_parameter: OMX_IndexParamOtherInit %08x\n",
                paramIndex);
            eRet =OMX_ErrorUnsupportedIndex;
            break;
        }
    case OMX_IndexParamStandardComponentRole:
        {
            OMX_PARAM_COMPONENTROLETYPE *comp_role;
            comp_role = (OMX_PARAM_COMPONENTROLETYPE *) paramData;
            comp_role->nVersion.nVersion = OMX_SPEC_VERSION;
            comp_role->nSize = sizeof(*comp_role);

            DEBUG_PRINT_LOW("Getparameter: OMX_IndexParamStandardComponentRole %d\n",
                paramIndex);
            strlcpy((char*)comp_role->cRole,(const char*)m_cRole,
                OMX_MAX_STRINGNAME_SIZE);
            break;
        }
        /* Added for parameter test */
    case OMX_IndexParamPriorityMgmt:
        {

            OMX_PRIORITYMGMTTYPE *priorityMgmType =
                (OMX_PRIORITYMGMTTYPE *) paramData;
            DEBUG_PRINT_LOW("get_parameter: OMX_IndexParamPriorityMgmt\n");
            priorityMgmType->nVersion.nVersion = OMX_SPEC_VERSION;
            priorityMgmType->nSize = sizeof(priorityMgmType);

            break;
        }
        /* Added for parameter test */
    case OMX_IndexParamCompBufferSupplier:
        {
            OMX_PARAM_BUFFERSUPPLIERTYPE *bufferSupplierType =
                (OMX_PARAM_BUFFERSUPPLIERTYPE*) paramData;
            DEBUG_PRINT_LOW("get_parameter: OMX_IndexParamCompBufferSupplier\n");

            bufferSupplierType->nSize = sizeof(bufferSupplierType);
            bufferSupplierType->nVersion.nVersion = OMX_SPEC_VERSION;
            if(0 == bufferSupplierType->nPortIndex)
                bufferSupplierType->nPortIndex = OMX_BufferSupplyUnspecified;
            else if (1 == bufferSupplierType->nPortIndex)
                bufferSupplierType->nPortIndex = OMX_BufferSupplyUnspecified;
            else
                eRet = OMX_ErrorBadPortIndex;


            break;
        }
    case OMX_IndexParamVideoAvc:
        {
            DEBUG_PRINT_LOW("get_parameter: OMX_IndexParamVideoAvc %08x\n",
                paramIndex);
            break;
        }
    case OMX_IndexParamVideoH263:
        {
            DEBUG_PRINT_LOW("get_parameter: OMX_IndexParamVideoH263 %08x\n",
                paramIndex);
            break;
        }
    case OMX_IndexParamVideoMpeg4:
        {
            DEBUG_PRINT_LOW("get_parameter: OMX_IndexParamVideoMpeg4 %08x\n",
                paramIndex);
            break;
        }
    case OMX_IndexParamVideoMpeg2:
        {
            DEBUG_PRINT_LOW("get_parameter: OMX_IndexParamVideoMpeg2 %08x\n",
                paramIndex);
            break;
        }
    case OMX_IndexParamVideoProfileLevelQuerySupported:
        {
            DEBUG_PRINT_LOW("get_parameter: OMX_IndexParamVideoProfileLevelQuerySupported %08x\n", paramIndex);
            OMX_VIDEO_PARAM_PROFILELEVELTYPE *profileLevelType =
                (OMX_VIDEO_PARAM_PROFILELEVELTYPE *)paramData;
            eRet = get_supported_profile_level_for_1080p(profileLevelType);
            break;
        }
#if defined (_ANDROID_HONEYCOMB_) || defined (_ANDROID_ICS_)
    case OMX_GoogleAndroidIndexGetAndroidNativeBufferUsage:
        {
            DEBUG_PRINT_HIGH("get_parameter: OMX_GoogleAndroidIndexGetAndroidNativeBufferUsage\n");
            GetAndroidNativeBufferUsageParams* nativeBuffersUsage = (GetAndroidNativeBufferUsageParams *) paramData;
            if(nativeBuffersUsage->nPortIndex == OMX_CORE_OUTPUT_PORT_INDEX) {

                if(secure_mode) {
                    nativeBuffersUsage->nUsage = (GRALLOC_USAGE_PRIVATE_MM_HEAP | GRALLOC_USAGE_PROTECTED |
                        GRALLOC_USAGE_PRIVATE_UNCACHED);
                } else {
                    if (!m_pSwVdec) {
#ifdef _HEVC_USE_ADSP_HEAP_
                        nativeBuffersUsage->nUsage = (GRALLOC_USAGE_PRIVATE_ADSP_HEAP | GRALLOC_USAGE_PRIVATE_UNCACHED);
#else
                        nativeBuffersUsage->nUsage = (GRALLOC_USAGE_PRIVATE_IOMMU_HEAP | GRALLOC_USAGE_PRIVATE_UNCACHED);
#endif
                    }
                    else {
                        // for swvdec use cached buffer
                        DEBUG_PRINT_HIGH("get_parameter: OMX_GoogleAndroidIndexGetAndroidNativeBufferUsage using output buffer cached\n");
                        // nativeBuffersUsage->nUsage = (GRALLOC_USAGE_PRIVATE_IOMMU_HEAP | GRALLOC_USAGE_PRIVATE_UNCACHED);
                        nativeBuffersUsage->nUsage = (GRALLOC_USAGE_PRIVATE_IOMMU_HEAP);
                    }
                    DEBUG_PRINT_HIGH("nativeBuffersUsage->nUsage %x\n", (unsigned int)nativeBuffersUsage->nUsage);
                }
            } else {
                DEBUG_PRINT_HIGH("get_parameter: OMX_GoogleAndroidIndexGetAndroidNativeBufferUsage failed!\n");
                eRet = OMX_ErrorBadParameter;
            }
        }
        break;
#endif

    default:
        {
            DEBUG_PRINT_ERROR("get_parameter: unknown param %08x\n", paramIndex);
            eRet =OMX_ErrorUnsupportedIndex;
        }

    }

    DEBUG_PRINT_LOW("\n get_parameter returning WxH(%d x %d) SxSH(%d x %d)\n",
        drv_ctx.video_resolution.frame_width,
        drv_ctx.video_resolution.frame_height,
        drv_ctx.video_resolution.stride,
        drv_ctx.video_resolution.scan_lines);

    return eRet;
}

#if defined (_ANDROID_HONEYCOMB_) || defined (_ANDROID_ICS_)
OMX_ERRORTYPE omx_vdec::use_android_native_buffer(OMX_IN OMX_HANDLETYPE hComp, OMX_PTR data)
{
    DEBUG_PRINT_LOW("Inside use_android_native_buffer");
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    UseAndroidNativeBufferParams *params = (UseAndroidNativeBufferParams *)data;

    if((params == NULL) ||
        (params->nativeBuffer == NULL) ||
        (params->nativeBuffer->handle == NULL) ||
        !m_enable_android_native_buffers)
        return OMX_ErrorBadParameter;
    m_use_android_native_buffers = OMX_TRUE;
    sp<android_native_buffer_t> nBuf = params->nativeBuffer;
    private_handle_t *handle = (private_handle_t *)nBuf->handle;
    if(OMX_CORE_OUTPUT_PORT_INDEX == params->nPortIndex) {  //android native buffers can be used only on Output port
        OMX_U8 *buffer = NULL;
        if(!secure_mode) {
            buffer = (OMX_U8*)mmap(0, handle->size,
                PROT_READ|PROT_WRITE, MAP_SHARED, handle->fd, 0);
            if(buffer == MAP_FAILED) {
                DEBUG_PRINT_ERROR("Failed to mmap pmem with fd = %d, size = %d", handle->fd, handle->size);
                return OMX_ErrorInsufficientResources;
            }
        }
        eRet = use_buffer(hComp,params->bufferHeader,params->nPortIndex,data,handle->size,buffer);
    } else {
        eRet = OMX_ErrorBadParameter;
    }
    return eRet;
}
#endif
/* ======================================================================
FUNCTION
omx_vdec::Setparameter

DESCRIPTION
OMX Set Parameter method implementation.

PARAMETERS
<TBD>.

RETURN VALUE
OMX Error None if successful.

========================================================================== */
OMX_ERRORTYPE  omx_vdec::set_parameter(OMX_IN OMX_HANDLETYPE     hComp,
                                       OMX_IN OMX_INDEXTYPE paramIndex,
                                       OMX_IN OMX_PTR        paramData)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    int ret=0;
    struct v4l2_format fmt;
    if(m_state == OMX_StateInvalid)
    {
        DEBUG_PRINT_ERROR("Set Param in Invalid State\n");
        return OMX_ErrorInvalidState;
    }
    if(paramData == NULL)
    {
        DEBUG_PRINT_ERROR("Get Param in Invalid paramData \n");
        return OMX_ErrorBadParameter;
    }
    if((m_state != OMX_StateLoaded) &&
        BITMASK_ABSENT(&m_flags,OMX_COMPONENT_OUTPUT_ENABLE_PENDING) &&
        (m_out_bEnabled == OMX_TRUE) &&
        BITMASK_ABSENT(&m_flags, OMX_COMPONENT_INPUT_ENABLE_PENDING) &&
        (m_inp_bEnabled == OMX_TRUE)) {
            DEBUG_PRINT_ERROR("Set Param in Invalid State \n");
            return OMX_ErrorIncorrectStateOperation;
    }
    switch((unsigned long)paramIndex)
    {
    case OMX_IndexParamPortDefinition:
        {
            OMX_PARAM_PORTDEFINITIONTYPE *portDefn;
            portDefn = (OMX_PARAM_PORTDEFINITIONTYPE *) paramData;
            //TODO: Check if any allocate buffer/use buffer/useNativeBuffer has
            //been called.
            DEBUG_PRINT_LOW("set_parameter: OMX_IndexParamPortDefinition H= %d, W = %d\n",
                (int)portDefn->format.video.nFrameHeight,
                (int)portDefn->format.video.nFrameWidth);

            // for pure dsp mode, if the dimension exceeds 720p, reject it
            // so that the stagefright can try the hybrid component
            if (!m_pSwVdec &&
                (portDefn->format.video.nFrameHeight > 720 ||
                portDefn->format.video.nFrameWidth > 1280))
            {
                DEBUG_PRINT_ERROR("\n Full DSP mode only support up to 720p");
                return OMX_ErrorBadParameter;
            }

            if(OMX_DirOutput == portDefn->eDir)
            {
                DEBUG_PRINT_LOW("set_parameter: OMX_IndexParamPortDefinition OP port\n");
                m_display_id = portDefn->format.video.pNativeWindow;
                unsigned int buffer_size;
                if (!client_buffers.get_buffer_req(buffer_size)) {
                    DEBUG_PRINT_ERROR("\n Error in getting buffer requirements");
                    eRet = OMX_ErrorBadParameter;
                } else {
                    if ( portDefn->nBufferCountActual >= drv_ctx.op_buf.mincount &&
                        portDefn->nBufferSize >=  drv_ctx.op_buf.buffer_size )
                    {
                        drv_ctx.op_buf.actualcount = portDefn->nBufferCountActual;
                        drv_ctx.op_buf.buffer_size = portDefn->nBufferSize;
                        eRet = set_buffer_req_swvdec(&drv_ctx.op_buf);
                        if (eRet == OMX_ErrorNone)
                            m_port_def = *portDefn;
                    }
                    else
                    {
                        DEBUG_PRINT_ERROR("ERROR: OP Requirements(#%d: %u) Requested(#%lu: %lu)\n",
                            drv_ctx.op_buf.mincount, drv_ctx.op_buf.buffer_size,
                            portDefn->nBufferCountActual, portDefn->nBufferSize);
                        eRet = OMX_ErrorBadParameter;
                    }
                }
            }
            else if(OMX_DirInput == portDefn->eDir)
            {
                if((portDefn->format.video.xFramerate >> 16) > 0 &&
                    (portDefn->format.video.xFramerate >> 16) <= MAX_SUPPORTED_FPS)
                {
                    // Frame rate only should be set if this is a "known value" or to
                    // activate ts prediction logic (arbitrary mode only) sending input
                    // timestamps with max value (LLONG_MAX).
                    DEBUG_PRINT_HIGH("set_parameter: frame rate set by omx client : %lu",
                        portDefn->format.video.xFramerate >> 16);
                    Q16ToFraction(portDefn->format.video.xFramerate, drv_ctx.frame_rate.fps_numerator,
                        drv_ctx.frame_rate.fps_denominator);
                    if(!drv_ctx.frame_rate.fps_numerator)
                    {
                        DEBUG_PRINT_ERROR("Numerator is zero setting to 30");
                        drv_ctx.frame_rate.fps_numerator = 30;
                    }
                    if(drv_ctx.frame_rate.fps_denominator)
                        drv_ctx.frame_rate.fps_numerator = (int)
                        drv_ctx.frame_rate.fps_numerator / drv_ctx.frame_rate.fps_denominator;
                    drv_ctx.frame_rate.fps_denominator = 1;
                    frm_int = drv_ctx.frame_rate.fps_denominator * 1e6 /
                        drv_ctx.frame_rate.fps_numerator;
                    DEBUG_PRINT_LOW("set_parameter: frm_int(%u) fps(%.2f)",
                        (unsigned int)frm_int, drv_ctx.frame_rate.fps_numerator /
                        (float)drv_ctx.frame_rate.fps_denominator);
                }
                DEBUG_PRINT_LOW("set_parameter: OMX_IndexParamPortDefinition IP port\n");
                if(drv_ctx.video_resolution.frame_height !=
                    portDefn->format.video.nFrameHeight ||
                    drv_ctx.video_resolution.frame_width  !=
                    portDefn->format.video.nFrameWidth)
                {
                    DEBUG_PRINT_LOW("\n SetParam IP: WxH(%d x %d)\n",
                        (int)portDefn->format.video.nFrameWidth,
                        (int)portDefn->format.video.nFrameHeight);
                    if (portDefn->format.video.nFrameHeight != 0x0 &&
                        portDefn->format.video.nFrameWidth != 0x0)
                    {
                        update_resolution(portDefn->format.video.nFrameWidth,
                            portDefn->format.video.nFrameHeight);

                        if (!m_pSwVdec || m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY)
                        {
                            fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
                            fmt.fmt.pix_mp.height = drv_ctx.video_resolution.frame_height;
                            fmt.fmt.pix_mp.width = drv_ctx.video_resolution.frame_width;
                            fmt.fmt.pix_mp.pixelformat = output_capability;
                            DEBUG_PRINT_LOW("\n fmt.fmt.pix_mp.height = %d , fmt.fmt.pix_mp.width = %d \n",
                                fmt.fmt.pix_mp.height,fmt.fmt.pix_mp.width);
                            ret = ioctl(drv_ctx.video_driver_fd, VIDIOC_S_FMT, &fmt);
                            if (ret)
                            {
                                DEBUG_PRINT_ERROR("\n Set Resolution failed h %d w %d", fmt.fmt.pix_mp.height, fmt.fmt.pix_mp.width);
                                eRet = OMX_ErrorUnsupportedSetting;
                                break;
                            }
                        }
                        if (m_pSwVdec)
                        {
                            SWVDEC_PROP prop;
                            prop.ePropId = SWVDEC_PROP_ID_DIMENSIONS;
                            prop.uProperty.sDimensions.nWidth = portDefn->format.video.nFrameWidth;
                            prop.uProperty.sDimensions.nHeight= portDefn->format.video.nFrameHeight;
                            SwVdec_SetProperty(m_pSwVdec,&prop);
                        }
                        eRet = get_buffer_req_swvdec();
                    }
                }
                else if (portDefn->nBufferCountActual >= drv_ctx.ip_buf.mincount
                    || portDefn->nBufferSize != drv_ctx.ip_buf.buffer_size)
                {
                    vdec_allocatorproperty *buffer_prop = &drv_ctx.ip_buf;
                    drv_ctx.ip_buf.actualcount = portDefn->nBufferCountActual;
                    drv_ctx.ip_buf.buffer_size = (portDefn->nBufferSize + buffer_prop->alignment - 1) &
                        (~(buffer_prop->alignment - 1));
                    eRet = set_buffer_req(buffer_prop);
                }
                else
                {
                    DEBUG_PRINT_ERROR("ERROR: IP Requirements(#%d: %u) Requested(#%lu: %lu)\n",
                        drv_ctx.ip_buf.mincount, drv_ctx.ip_buf.buffer_size,
                        portDefn->nBufferCountActual, portDefn->nBufferSize);
                    eRet = OMX_ErrorBadParameter;
                }
            }
            else if (portDefn->eDir ==  OMX_DirMax)
            {
                DEBUG_PRINT_ERROR(" Set_parameter: Bad Port idx %d",
                    (int)portDefn->nPortIndex);
                eRet = OMX_ErrorBadPortIndex;
            }
        }
        break;
    case OMX_IndexParamVideoPortFormat:
        {
            OMX_VIDEO_PARAM_PORTFORMATTYPE *portFmt =
                (OMX_VIDEO_PARAM_PORTFORMATTYPE *)paramData;
            int ret=0;
            struct v4l2_format fmt;
            DEBUG_PRINT_LOW("set_parameter: OMX_IndexParamVideoPortFormat %d\n",
                portFmt->eColorFormat);

            if(1 == portFmt->nPortIndex)
            {
                fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                fmt.fmt.pix_mp.height = drv_ctx.video_resolution.frame_height;
                fmt.fmt.pix_mp.width = drv_ctx.video_resolution.frame_width;
                fmt.fmt.pix_mp.pixelformat = capture_capability;
                enum vdec_output_fromat op_format;
                if((portFmt->eColorFormat == (OMX_COLOR_FORMATTYPE)
                    QOMX_COLOR_FORMATYUV420PackedSemiPlanar32m) ||
                    (portFmt->eColorFormat == OMX_COLOR_FormatYUV420Planar))
                    op_format = (enum vdec_output_fromat)VDEC_YUV_FORMAT_NV12;
                else if(portFmt->eColorFormat ==
                    (OMX_COLOR_FORMATTYPE)
                    QOMX_COLOR_FormatYUV420PackedSemiPlanar64x32Tile2m8ka)
                    op_format = VDEC_YUV_FORMAT_TILE_4x2;
                else
                    eRet = OMX_ErrorBadParameter;

                if(eRet == OMX_ErrorNone)
                {
                    drv_ctx.output_format = op_format;
                    if (!m_pSwVdec)
                    {
                        ret = ioctl(drv_ctx.video_driver_fd, VIDIOC_S_FMT, &fmt);
                    }
                    // need to set output format to swvdec
                    if(ret)
                    {
                        DEBUG_PRINT_ERROR("\n Set output format failed");
                        eRet = OMX_ErrorUnsupportedSetting;
                        /*TODO: How to handle this case */
                    }
                    else
                    {
                        eRet = get_buffer_req_swvdec();
                    }
                }
                if (eRet == OMX_ErrorNone){
                    if (!client_buffers.set_color_format(portFmt->eColorFormat)) {
                        DEBUG_PRINT_ERROR("\n Set color format failed");
                        eRet = OMX_ErrorBadParameter;
                    }
                }
            }
        }
        break;

    case OMX_QcomIndexPortDefn:
        {
            OMX_QCOM_PARAM_PORTDEFINITIONTYPE *portFmt =
                (OMX_QCOM_PARAM_PORTDEFINITIONTYPE *) paramData;
            DEBUG_PRINT_LOW("set_parameter: OMX_IndexQcomParamPortDefinitionType %d\n",
                (int)portFmt->nFramePackingFormat);

            /* Input port */
            if (portFmt->nPortIndex == 0)
            {
                if (portFmt->nFramePackingFormat == OMX_QCOM_FramePacking_Arbitrary)
                {
                    if(secure_mode) {
                        arbitrary_bytes = false;
                        DEBUG_PRINT_ERROR("setparameter: cannot set to arbitary bytes mode in secure session");
                        eRet = OMX_ErrorUnsupportedSetting;
                    } else {
                        arbitrary_bytes = true;
                    }
                }
                else if (portFmt->nFramePackingFormat ==
                    OMX_QCOM_FramePacking_OnlyOneCompleteFrame)
                {
                    arbitrary_bytes = false;
                }
                else
                {
                    DEBUG_PRINT_ERROR("Setparameter: unknown FramePacking format %lu\n",
                        portFmt->nFramePackingFormat);
                    eRet = OMX_ErrorUnsupportedSetting;
                }
            }
            else if (portFmt->nPortIndex == OMX_CORE_OUTPUT_PORT_INDEX)
            {
                DEBUG_PRINT_HIGH("set_parameter: OMX_IndexQcomParamPortDefinitionType OP Port\n");
                if( (portFmt->nMemRegion > OMX_QCOM_MemRegionInvalid &&
                    portFmt->nMemRegion < OMX_QCOM_MemRegionMax) &&
                    portFmt->nCacheAttr == OMX_QCOM_CacheAttrNone)
                {
                    m_out_mem_region_smi = OMX_TRUE;
                    if ((m_out_mem_region_smi && m_out_pvt_entry_pmem))
                    {
                        DEBUG_PRINT_HIGH("set_parameter: OMX_IndexQcomParamPortDefinitionType OP Port: out pmem set\n");
                        m_use_output_pmem = OMX_TRUE;
                    }
                }
            }
        }
        break;

    case OMX_IndexParamStandardComponentRole:
        {
            OMX_PARAM_COMPONENTROLETYPE *comp_role;
            comp_role = (OMX_PARAM_COMPONENTROLETYPE *) paramData;
            DEBUG_PRINT_LOW("set_parameter: OMX_IndexParamStandardComponentRole %s\n",
                comp_role->cRole);

            if((m_state == OMX_StateLoaded)&&
                !BITMASK_PRESENT(&m_flags,OMX_COMPONENT_IDLE_PENDING))
            {
                DEBUG_PRINT_LOW("Set Parameter called in valid state");
            }
            else
            {
                DEBUG_PRINT_ERROR("Set Parameter called in Invalid State\n");
                return OMX_ErrorIncorrectStateOperation;
            }

            if(!strncmp(drv_ctx.kind, "OMX.qcom.video.decoder.hevchybrid",OMX_MAX_STRINGNAME_SIZE) ||
                !strncmp(drv_ctx.kind, "OMX.qcom.video.decoder.hevcswvdec",OMX_MAX_STRINGNAME_SIZE) ||
                !strncmp(drv_ctx.kind, "OMX.qcom.video.decoder.hevc",OMX_MAX_STRINGNAME_SIZE))
            {
                if(!strncmp((char*)comp_role->cRole,"video_decoder.hevc",OMX_MAX_STRINGNAME_SIZE))
                {
                    strlcpy((char*)m_cRole,"video_decoder.hevc",OMX_MAX_STRINGNAME_SIZE);
                }
                else
                {
                    DEBUG_PRINT_ERROR("Setparameter: unknown Index %s\n", comp_role->cRole);
                    eRet =OMX_ErrorUnsupportedSetting;
                }
            }
            else
            {
                DEBUG_PRINT_ERROR("Setparameter: unknown param %s\n", drv_ctx.kind);
                eRet = OMX_ErrorInvalidComponentName;
            }
            break;
        }

    case OMX_IndexParamPriorityMgmt:
        {
            if(m_state != OMX_StateLoaded)
            {
                DEBUG_PRINT_ERROR("Set Parameter called in Invalid State\n");
                return OMX_ErrorIncorrectStateOperation;
            }
            OMX_PRIORITYMGMTTYPE *priorityMgmtype = (OMX_PRIORITYMGMTTYPE*) paramData;
            DEBUG_PRINT_LOW("set_parameter: OMX_IndexParamPriorityMgmt %d\n",
                (int)priorityMgmtype->nGroupID);

            DEBUG_PRINT_LOW("set_parameter: priorityMgmtype %d\n",
                (int)priorityMgmtype->nGroupPriority);

            m_priority_mgm.nGroupID = priorityMgmtype->nGroupID;
            m_priority_mgm.nGroupPriority = priorityMgmtype->nGroupPriority;

            break;
        }

    case OMX_IndexParamCompBufferSupplier:
        {
            OMX_PARAM_BUFFERSUPPLIERTYPE *bufferSupplierType = (OMX_PARAM_BUFFERSUPPLIERTYPE*) paramData;
            DEBUG_PRINT_LOW("set_parameter: OMX_IndexParamCompBufferSupplier %d\n",
                bufferSupplierType->eBufferSupplier);
            if(bufferSupplierType->nPortIndex == 0 || bufferSupplierType->nPortIndex ==1)
                m_buffer_supplier.eBufferSupplier = bufferSupplierType->eBufferSupplier;

            else

                eRet = OMX_ErrorBadPortIndex;

            break;

        }
    case OMX_IndexParamVideoAvc:
    case OMX_IndexParamVideoH263:
    case OMX_IndexParamVideoMpeg4:
    case OMX_IndexParamVideoMpeg2:
        {
            eRet = OMX_ErrorUnsupportedSetting;
            break;
        }
    case OMX_QcomIndexParamVideoDecoderPictureOrder:
        {
            QOMX_VIDEO_DECODER_PICTURE_ORDER *pictureOrder =
                (QOMX_VIDEO_DECODER_PICTURE_ORDER *)paramData;
            struct v4l2_control control;
            int pic_order,rc=0;
            DEBUG_PRINT_HIGH("set_parameter: OMX_QcomIndexParamVideoDecoderPictureOrder %d\n",
                pictureOrder->eOutputPictureOrder);
            if (pictureOrder->eOutputPictureOrder == QOMX_VIDEO_DISPLAY_ORDER) {
                pic_order = V4L2_MPEG_VIDC_VIDEO_OUTPUT_ORDER_DISPLAY;
            }
            else if (pictureOrder->eOutputPictureOrder == QOMX_VIDEO_DECODE_ORDER){
                pic_order = V4L2_MPEG_VIDC_VIDEO_OUTPUT_ORDER_DECODE;
                time_stamp_dts.set_timestamp_reorder_mode(false);
            }
            else
                eRet = OMX_ErrorBadParameter;
            if (eRet == OMX_ErrorNone)
            {
                if (!m_pSwVdec || m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY) {
                    control.id = V4L2_CID_MPEG_VIDC_VIDEO_OUTPUT_ORDER;
                    control.value = pic_order;
                    rc = ioctl(drv_ctx.video_driver_fd, VIDIOC_S_CTRL, &control);
                    if(rc)
                    {
                        DEBUG_PRINT_ERROR("\n Set picture order failed");
                        eRet = OMX_ErrorUnsupportedSetting;
                    }
                }
                // if (m_swvdec_mode == SWVDEC_MODE_PARSE_DECODE)
                {
                    // TODO
                }
            }
            break;
        }
    case OMX_QcomIndexParamConcealMBMapExtraData:
        if(!secure_mode)
            eRet = enable_extradata(VDEC_EXTRADATA_MB_ERROR_MAP, false,
            ((QOMX_ENABLETYPE *)paramData)->bEnable);
        else {
            DEBUG_PRINT_ERROR("\n secure mode setting not supported");
            eRet = OMX_ErrorUnsupportedSetting;
        }
        break;
    case OMX_QcomIndexParamFrameInfoExtraData:
        {
            if(!secure_mode)
                eRet = enable_extradata(OMX_FRAMEINFO_EXTRADATA, false,
                ((QOMX_ENABLETYPE *)paramData)->bEnable);
            else {
                DEBUG_PRINT_ERROR("\n secure mode setting not supported");
                eRet = OMX_ErrorUnsupportedSetting;
            }
            break;
        }
    case OMX_QcomIndexParamInterlaceExtraData:
        if(!secure_mode)
            eRet = enable_extradata(OMX_INTERLACE_EXTRADATA, false,
            ((QOMX_ENABLETYPE *)paramData)->bEnable);
        else {
            DEBUG_PRINT_ERROR("\n secure mode setting not supported");
            eRet = OMX_ErrorUnsupportedSetting;
        }
        break;
    case OMX_QcomIndexParamH264TimeInfo:
        if(!secure_mode)
            eRet = enable_extradata(OMX_TIMEINFO_EXTRADATA, false,
            ((QOMX_ENABLETYPE *)paramData)->bEnable);
        else {
            DEBUG_PRINT_ERROR("\n secure mode setting not supported");
            eRet = OMX_ErrorUnsupportedSetting;
        }
        break;
    case OMX_QcomIndexParamVideoDivx:
        {
            QOMX_VIDEO_PARAM_DIVXTYPE* divXType = (QOMX_VIDEO_PARAM_DIVXTYPE *) paramData;
        }
        break;
    case OMX_QcomIndexPlatformPvt:
        {
            DEBUG_PRINT_HIGH("set_parameter: OMX_QcomIndexPlatformPvt OP Port\n");
            OMX_QCOM_PLATFORMPRIVATE_EXTN* entryType = (OMX_QCOM_PLATFORMPRIVATE_EXTN *) paramData;
            if (entryType->type != OMX_QCOM_PLATFORM_PRIVATE_PMEM)
            {
                DEBUG_PRINT_HIGH("set_parameter: Platform Private entry type (%d) not supported.", entryType->type);
                eRet = OMX_ErrorUnsupportedSetting;
            }
            else
            {
                m_out_pvt_entry_pmem = OMX_TRUE;
                if ((m_out_mem_region_smi && m_out_pvt_entry_pmem))
                {
                    DEBUG_PRINT_HIGH("set_parameter: OMX_QcomIndexPlatformPvt OP Port: out pmem set\n");
                    m_use_output_pmem = OMX_TRUE;
                }
            }

        }
        break;
    case OMX_QcomIndexParamVideoSyncFrameDecodingMode:
        {
            if (!m_pSwVdec || m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY) {
                DEBUG_PRINT_HIGH("set_parameter: OMX_QcomIndexParamVideoSyncFrameDecodingMode");
                DEBUG_PRINT_HIGH("set idr only decoding for thumbnail mode");
                struct v4l2_control control;
                int rc;
                drv_ctx.idr_only_decoding = 1;
                control.id = V4L2_CID_MPEG_VIDC_VIDEO_OUTPUT_ORDER;
                control.value = V4L2_MPEG_VIDC_VIDEO_OUTPUT_ORDER_DECODE;
                rc = ioctl(drv_ctx.video_driver_fd, VIDIOC_S_CTRL, &control);
                if(rc)
                {
                    DEBUG_PRINT_ERROR("\n Set picture order failed");
                    eRet = OMX_ErrorUnsupportedSetting;
                } else {
                    control.id = V4L2_CID_MPEG_VIDC_VIDEO_SYNC_FRAME_DECODE;
                    control.value = V4L2_MPEG_VIDC_VIDEO_SYNC_FRAME_DECODE_ENABLE;
                    rc = ioctl(drv_ctx.video_driver_fd, VIDIOC_S_CTRL, &control);
                    if(rc)
                    {
                        DEBUG_PRINT_ERROR("\n Sync frame setting failed");
                        eRet = OMX_ErrorUnsupportedSetting;
                    }
                }
                // for TODO swvdec
            }
        }
        break;

    case OMX_QcomIndexParamIndexExtraDataType:
        {
            if(!secure_mode) {
                QOMX_INDEXEXTRADATATYPE *extradataIndexType = (QOMX_INDEXEXTRADATATYPE *) paramData;
                if ((extradataIndexType->nIndex == OMX_IndexParamPortDefinition) &&
                    (extradataIndexType->bEnabled == OMX_TRUE) &&
                    (extradataIndexType->nPortIndex == 1))
                {
                    DEBUG_PRINT_HIGH("set_parameter:  OMX_QcomIndexParamIndexExtraDataType SmoothStreaming\n");
                    eRet = enable_extradata(OMX_PORTDEF_EXTRADATA, false, extradataIndexType->bEnabled);

                }
            }
        }
        break;
    case OMX_QcomIndexParamEnableSmoothStreaming:
        {
            if (!m_pSwVdec || m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY) {
                struct v4l2_control control;
                struct v4l2_format fmt;
                control.id = V4L2_CID_MPEG_VIDC_VIDEO_CONTINUE_DATA_TRANSFER;
                control.value = 1;
                int rc = ioctl(drv_ctx.video_driver_fd, VIDIOC_S_CTRL,&control);
                if(rc < 0) {
                    DEBUG_PRINT_ERROR("Failed to enable Smooth Streaming on driver.");
                    eRet = OMX_ErrorHardware;
                }
            }
            // TODO  for swvdec
        }
        break;
#if defined (_ANDROID_HONEYCOMB_) || defined (_ANDROID_ICS_)
        /* Need to allow following two set_parameters even in Idle
        * state. This is ANDROID architecture which is not in sync
        * with openmax standard. */
    case OMX_GoogleAndroidIndexEnableAndroidNativeBuffers:
        {
            EnableAndroidNativeBuffersParams* enableNativeBuffers = (EnableAndroidNativeBuffersParams *) paramData;
            if(enableNativeBuffers) {
                m_enable_android_native_buffers = enableNativeBuffers->enable;
            }
        }
        break;
    case OMX_GoogleAndroidIndexUseAndroidNativeBuffer:
        {
            eRet = use_android_native_buffer(hComp, paramData);
        }
        break;
#endif
    case OMX_QcomIndexParamEnableTimeStampReorder:
        {
            QOMX_INDEXTIMESTAMPREORDER *reorder = (QOMX_INDEXTIMESTAMPREORDER *)paramData;
            if (drv_ctx.picture_order == (vdec_output_order)QOMX_VIDEO_DISPLAY_ORDER) {
                if (reorder->bEnable == OMX_TRUE) {
                    frm_int =0;
                    time_stamp_dts.set_timestamp_reorder_mode(true);
                }
                else
                    time_stamp_dts.set_timestamp_reorder_mode(false);
            } else {
                time_stamp_dts.set_timestamp_reorder_mode(false);
                if (reorder->bEnable == OMX_TRUE)
                {
                    eRet = OMX_ErrorUnsupportedSetting;
                }
            }
        }
        break;
    default:
        {
            DEBUG_PRINT_ERROR("Setparameter: unknown param %d\n", paramIndex);
            eRet = OMX_ErrorUnsupportedIndex;
        }
    }
    return eRet;
}

/* ======================================================================
FUNCTION
omx_vdec::GetConfig

DESCRIPTION
OMX Get Config Method implementation.

PARAMETERS
<TBD>.

RETURN VALUE
OMX Error None if successful.

========================================================================== */
OMX_ERRORTYPE  omx_vdec::get_config(OMX_IN OMX_HANDLETYPE      hComp,
                                    OMX_IN OMX_INDEXTYPE configIndex,
                                    OMX_INOUT OMX_PTR     configData)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;

    if (m_state == OMX_StateInvalid)
    {
        DEBUG_PRINT_ERROR("Get Config in Invalid State\n");
        return OMX_ErrorInvalidState;
    }

    switch ((unsigned long)configIndex)
    {
    case OMX_QcomIndexConfigInterlaced:
        {
            OMX_QCOM_CONFIG_INTERLACETYPE *configFmt =
                (OMX_QCOM_CONFIG_INTERLACETYPE *) configData;
            if (configFmt->nPortIndex == 1)
            {
                if (configFmt->nIndex == 0)
                {
                    configFmt->eInterlaceType = OMX_QCOM_InterlaceFrameProgressive;
                }
                else if (configFmt->nIndex == 1)
                {
                    configFmt->eInterlaceType =
                        OMX_QCOM_InterlaceInterleaveFrameTopFieldFirst;
                }
                else if (configFmt->nIndex == 2)
                {
                    configFmt->eInterlaceType =
                        OMX_QCOM_InterlaceInterleaveFrameBottomFieldFirst;
                }
                else
                {
                    DEBUG_PRINT_ERROR("get_config: OMX_QcomIndexConfigInterlaced:"
                        " NoMore Interlaced formats\n");
                    eRet = OMX_ErrorNoMore;
                }

            }
            else
            {
                DEBUG_PRINT_ERROR("get_config: Bad port index %d queried on only o/p port\n",
                    (int)configFmt->nPortIndex);
                eRet = OMX_ErrorBadPortIndex;
            }
            break;
        }
    case OMX_QcomIndexQueryNumberOfVideoDecInstance:
        {
            QOMX_VIDEO_QUERY_DECODER_INSTANCES *decoderinstances =
                (QOMX_VIDEO_QUERY_DECODER_INSTANCES*)configData;
            decoderinstances->nNumOfInstances = 16;
            /*TODO: How to handle this case */
            break;
        }
    case OMX_QcomIndexConfigVideoFramePackingArrangement:
        {
            DEBUG_PRINT_ERROR("get_config: Framepack data not supported for non H264 codecs");
            break;
        }
    case OMX_IndexConfigCommonOutputCrop:
        {
            OMX_CONFIG_RECTTYPE *rect = (OMX_CONFIG_RECTTYPE *) configData;
            memcpy(rect, &rectangle, sizeof(OMX_CONFIG_RECTTYPE));
            break;
        }
    default:
        {
            DEBUG_PRINT_ERROR("get_config: unknown param %d\n",configIndex);
            eRet = OMX_ErrorBadParameter;
        }

    }

    return eRet;
}

/* ======================================================================
FUNCTION
omx_vdec::SetConfig

DESCRIPTION
OMX Set Config method implementation

PARAMETERS
<TBD>.

RETURN VALUE
OMX Error None if successful.
========================================================================== */
OMX_ERRORTYPE  omx_vdec::set_config(OMX_IN OMX_HANDLETYPE      hComp,
                                    OMX_IN OMX_INDEXTYPE configIndex,
                                    OMX_IN OMX_PTR        configData)
{
    if(m_state == OMX_StateInvalid)
    {
        DEBUG_PRINT_ERROR("Get Config in Invalid State\n");
        return OMX_ErrorInvalidState;
    }

    OMX_ERRORTYPE ret = OMX_ErrorNone;
    OMX_VIDEO_CONFIG_NALSIZE *pNal;

    DEBUG_PRINT_LOW("\n Set Config Called");

    if (m_state == OMX_StateExecuting)
    {
        DEBUG_PRINT_ERROR("set_config:Ignore in Exe state\n");
        return ret;
    }

    if (configIndex == (OMX_INDEXTYPE)OMX_IndexVendorVideoExtraData)
    {
        OMX_VENDOR_EXTRADATATYPE *config = (OMX_VENDOR_EXTRADATATYPE *) configData;
        DEBUG_PRINT_LOW("\n Index OMX_IndexVendorVideoExtraData called");
        return ret;
    }
    else if (configIndex == OMX_IndexConfigVideoNalSize)
    {

        pNal = reinterpret_cast < OMX_VIDEO_CONFIG_NALSIZE * >(configData);
        nal_length = pNal->nNaluBytes;
        m_frame_parser.init_nal_length(nal_length);
        DEBUG_PRINT_LOW("\n OMX_IndexConfigVideoNalSize called with Size %d",nal_length);
        return ret;
    }

    return OMX_ErrorNotImplemented;
}

/* ======================================================================
FUNCTION
omx_vdec::GetExtensionIndex

DESCRIPTION
OMX GetExtensionIndex method implementaion.  <TBD>

PARAMETERS
<TBD>.

RETURN VALUE
OMX Error None if everything successful.

========================================================================== */
OMX_ERRORTYPE  omx_vdec::get_extension_index(OMX_IN OMX_HANDLETYPE      hComp,
                                             OMX_IN OMX_STRING      paramName,
                                             OMX_OUT OMX_INDEXTYPE* indexType)
{
    if(m_state == OMX_StateInvalid)
    {
        DEBUG_PRINT_ERROR("Get Extension Index in Invalid State\n");
        return OMX_ErrorInvalidState;
    }
    else if (!strncmp(paramName, "OMX.QCOM.index.param.video.SyncFrameDecodingMode",sizeof("OMX.QCOM.index.param.video.SyncFrameDecodingMode") - 1)) {
        *indexType = (OMX_INDEXTYPE)OMX_QcomIndexParamVideoSyncFrameDecodingMode;
    }
    else if (!strncmp(paramName, "OMX.QCOM.index.param.IndexExtraData",sizeof("OMX.QCOM.index.param.IndexExtraData") - 1))
    {
        *indexType = (OMX_INDEXTYPE)OMX_QcomIndexParamIndexExtraDataType;
    }
#if defined (_ANDROID_HONEYCOMB_) || defined (_ANDROID_ICS_)
    else if(!strncmp(paramName,"OMX.google.android.index.enableAndroidNativeBuffers", sizeof("OMX.google.android.index.enableAndroidNativeBuffers") - 1)) {
        *indexType = (OMX_INDEXTYPE)OMX_GoogleAndroidIndexEnableAndroidNativeBuffers;
    }
    else if(!strncmp(paramName,"OMX.google.android.index.useAndroidNativeBuffer2", sizeof("OMX.google.android.index.enableAndroidNativeBuffer2") - 1)) {
        *indexType = (OMX_INDEXTYPE)OMX_GoogleAndroidIndexUseAndroidNativeBuffer2;
    }
    else if(!strncmp(paramName,"OMX.google.android.index.useAndroidNativeBuffer", sizeof("OMX.google.android.index.enableAndroidNativeBuffer") - 1)) {
        DEBUG_PRINT_ERROR("Extension: %s is supported\n", paramName);
        *indexType = (OMX_INDEXTYPE)OMX_GoogleAndroidIndexUseAndroidNativeBuffer;
    }
    else if(!strncmp(paramName,"OMX.google.android.index.getAndroidNativeBufferUsage", sizeof("OMX.google.android.index.getAndroidNativeBufferUsage") - 1)) {
        *indexType = (OMX_INDEXTYPE)OMX_GoogleAndroidIndexGetAndroidNativeBufferUsage;
    }
#endif
    else {
        DEBUG_PRINT_ERROR("Extension: %s not implemented\n", paramName);
        return OMX_ErrorNotImplemented;
    }
    return OMX_ErrorNone;
}

/* ======================================================================
FUNCTION
omx_vdec::GetState

DESCRIPTION
Returns the state information back to the caller.<TBD>

PARAMETERS
<TBD>.

RETURN VALUE
Error None if everything is successful.
========================================================================== */
OMX_ERRORTYPE  omx_vdec::get_state(OMX_IN OMX_HANDLETYPE  hComp,
                                   OMX_OUT OMX_STATETYPE* state)
{
    *state = m_state;
    DEBUG_PRINT_LOW("get_state: Returning the state %d\n",*state);
    return OMX_ErrorNone;
}

/* ======================================================================
FUNCTION
omx_vdec::ComponentTunnelRequest

DESCRIPTION
OMX Component Tunnel Request method implementation. <TBD>

PARAMETERS
None.

RETURN VALUE
OMX Error None if everything successful.

========================================================================== */
OMX_ERRORTYPE  omx_vdec::component_tunnel_request(OMX_IN OMX_HANDLETYPE                hComp,
                                                  OMX_IN OMX_U32                        port,
                                                  OMX_IN OMX_HANDLETYPE        peerComponent,
                                                  OMX_IN OMX_U32                    peerPort,
                                                  OMX_INOUT OMX_TUNNELSETUPTYPE* tunnelSetup)
{
    DEBUG_PRINT_ERROR("Error: component_tunnel_request Not Implemented\n");
    return OMX_ErrorNotImplemented;
}

/* ======================================================================
FUNCTION
omx_vdec::UseOutputBuffer

DESCRIPTION
Helper function for Use buffer in the input pin

PARAMETERS
None.

RETURN VALUE
true/false

========================================================================== */
OMX_ERRORTYPE omx_vdec::allocate_extradata()
{
#ifdef USE_ION
    if (drv_ctx.extradata_info.buffer_size) {
        if (drv_ctx.extradata_info.ion.ion_alloc_data.handle) {
            munmap((void *)drv_ctx.extradata_info.uaddr, drv_ctx.extradata_info.size);
            close(drv_ctx.extradata_info.ion.fd_ion_data.fd);
            free_ion_memory(&drv_ctx.extradata_info.ion);
        }
        drv_ctx.extradata_info.size = (drv_ctx.extradata_info.size + 4095) & (~4095);
        DEBUG_PRINT_HIGH("allocate extradata memory size %d\n", drv_ctx.extradata_info.size);
        int heap = 0;
#ifdef _HEVC_USE_ADSP_HEAP_
        heap = ION_ADSP_HEAP_ID;
#endif
        drv_ctx.extradata_info.ion.ion_device_fd = alloc_map_ion_memory(
            drv_ctx.extradata_info.size, 4096,
            &drv_ctx.extradata_info.ion.ion_alloc_data,
            &drv_ctx.extradata_info.ion.fd_ion_data, 0, heap);
        if (drv_ctx.extradata_info.ion.ion_device_fd < 0) {
            DEBUG_PRINT_ERROR("Failed to alloc extradata memory\n");
            return OMX_ErrorInsufficientResources;
        }
        drv_ctx.extradata_info.uaddr = (char *)mmap(NULL,
            drv_ctx.extradata_info.size,
            PROT_READ|PROT_WRITE, MAP_SHARED,
            drv_ctx.extradata_info.ion.fd_ion_data.fd , 0);
        if (drv_ctx.extradata_info.uaddr == MAP_FAILED) {
            DEBUG_PRINT_ERROR("Failed to map extradata memory\n");
            close(drv_ctx.extradata_info.ion.fd_ion_data.fd);
            free_ion_memory(&drv_ctx.extradata_info.ion);
            return OMX_ErrorInsufficientResources;
        }
        memset(drv_ctx.extradata_info.uaddr, 0, drv_ctx.extradata_info.size);
    }
#endif
    return OMX_ErrorNone;
}

void omx_vdec::free_extradata() {
#ifdef USE_ION
    if (drv_ctx.extradata_info.uaddr) {
        munmap((void *)drv_ctx.extradata_info.uaddr, drv_ctx.extradata_info.size);
        close(drv_ctx.extradata_info.ion.fd_ion_data.fd);
        free_ion_memory(&drv_ctx.extradata_info.ion);
    }
    memset(&drv_ctx.extradata_info, 0, sizeof(drv_ctx.extradata_info));
#endif
}

OMX_ERRORTYPE  omx_vdec::use_output_buffer(
    OMX_IN OMX_HANDLETYPE            hComp,
    OMX_INOUT OMX_BUFFERHEADERTYPE** bufferHdr,
    OMX_IN OMX_U32                   port,
    OMX_IN OMX_PTR                   appData,
    OMX_IN OMX_U32                   bytes,
    OMX_IN OMX_U8*                   buffer)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    OMX_BUFFERHEADERTYPE       *bufHdr= NULL; // buffer header
    unsigned                         i= 0; // Temporary counter
    struct vdec_setbuffer_cmd setbuffers;
    OMX_PTR privateAppData = NULL;
    private_handle_t *handle = NULL;
    OMX_U8 *buff = buffer;

    if (!m_out_mem_ptr) {
        DEBUG_PRINT_HIGH("Use_op_buf:Allocating output headers");
        eRet = allocate_output_headers();
        if (!m_pSwVdec && eRet == OMX_ErrorNone)
            eRet = allocate_extradata();
    }

    if (eRet == OMX_ErrorNone) {
        for(i=0; i< drv_ctx.op_buf.actualcount; i++) {
            if(BITMASK_ABSENT(&m_out_bm_count,i))
            {
                break;
            }
        }
    }

    if(i >= drv_ctx.op_buf.actualcount) {
        DEBUG_PRINT_ERROR("Already using %d o/p buffers\n", drv_ctx.op_buf.actualcount);
        eRet = OMX_ErrorInsufficientResources;
    }

    if (eRet == OMX_ErrorNone) {
#if defined(_ANDROID_HONEYCOMB_) || defined(_ANDROID_ICS_)
        if(m_enable_android_native_buffers) {
            if (m_use_android_native_buffers) {
                UseAndroidNativeBufferParams *params = (UseAndroidNativeBufferParams *)appData;
                sp<android_native_buffer_t> nBuf = params->nativeBuffer;
                handle = (private_handle_t *)nBuf->handle;
                privateAppData = params->pAppPrivate;
            } else {
                handle = (private_handle_t *)buff;
                privateAppData = appData;
            }

            if ((OMX_U32)handle->size < drv_ctx.op_buf.buffer_size) {
                DEBUG_PRINT_ERROR("Insufficient sized buffer given for playback,"
                    " expected %u, got %lu",
                    drv_ctx.op_buf.buffer_size, (OMX_U32)handle->size);
                return OMX_ErrorBadParameter;
            }

            drv_ctx.op_buf.buffer_size = (OMX_U32)handle->size;
            if (!m_use_android_native_buffers) {
                if (!secure_mode) {
                    buff =  (OMX_U8*)mmap(0, handle->size,
                        PROT_READ|PROT_WRITE, MAP_SHARED, handle->fd, 0);
                    if (buff == MAP_FAILED) {
                        DEBUG_PRINT_ERROR("Failed to mmap pmem with fd = %d, size = %d", handle->fd, handle->size);
                        return OMX_ErrorInsufficientResources;
                    }
                }
            }
#if defined(_ANDROID_ICS_)
            native_buffer[i].nativehandle = handle;
            native_buffer[i].privatehandle = handle;
#endif
            if(!handle) {
                DEBUG_PRINT_ERROR("Native Buffer handle is NULL");
                return OMX_ErrorBadParameter;
            }
            drv_ctx.ptr_outputbuffer[i].pmem_fd = handle->fd;
            drv_ctx.ptr_outputbuffer[i].offset = 0;
            drv_ctx.ptr_outputbuffer[i].bufferaddr = buff;
            drv_ctx.ptr_outputbuffer[i].mmaped_size =
                drv_ctx.ptr_outputbuffer[i].buffer_len = drv_ctx.op_buf.buffer_size;
            drv_ctx.op_buf_ion_info[i].fd_ion_data.fd = handle->fd;
            drv_ctx.op_buf_ion_info[i].fd_ion_data.handle = (ion_handle*)handle;
            DEBUG_PRINT_HIGH("Native Buffer vaddr %x, idx %d fd %d len %d", (unsigned int)buff,i, handle->fd , drv_ctx.op_buf.buffer_size);
        } else
#endif

            if (!ouput_egl_buffers && !m_use_output_pmem) {
#ifdef USE_ION
                DEBUG_PRINT_HIGH("allocate output buffer memory size %d\n", drv_ctx.op_buf.buffer_size);
                drv_ctx.op_buf_ion_info[i].ion_device_fd = alloc_map_ion_memory(
                    drv_ctx.op_buf.buffer_size,drv_ctx.op_buf.alignment,
                    &drv_ctx.op_buf_ion_info[i].ion_alloc_data,
                    &drv_ctx.op_buf_ion_info[i].fd_ion_data, secure_mode ? ION_SECURE : 0);
                if(drv_ctx.op_buf_ion_info[i].ion_device_fd < 0) {
                    DEBUG_PRINT_ERROR("ION device fd is bad %d\n", drv_ctx.op_buf_ion_info[i].ion_device_fd);
                    return OMX_ErrorInsufficientResources;
                }
                drv_ctx.ptr_outputbuffer[i].pmem_fd = \
                    drv_ctx.op_buf_ion_info[i].fd_ion_data.fd;
#else
                drv_ctx.ptr_outputbuffer[i].pmem_fd = \
                    open (MEM_DEVICE,O_RDWR);

                if (drv_ctx.ptr_outputbuffer[i].pmem_fd < 0) {
                    DEBUG_PRINT_ERROR("ION/pmem buffer fd is bad %d\n", drv_ctx.ptr_outputbuffer[i].pmem_fd);
                    return OMX_ErrorInsufficientResources;
                }

                /* FIXME: why is this code even here? We already open MEM_DEVICE a few lines above */
                if(drv_ctx.ptr_outputbuffer[i].pmem_fd == 0)
                {
                    drv_ctx.ptr_outputbuffer[i].pmem_fd = \
                        open (MEM_DEVICE,O_RDWR);
                    if (drv_ctx.ptr_outputbuffer[i].pmem_fd < 0) {
                        DEBUG_PRINT_ERROR("ION/pmem buffer fd is bad %d\n", drv_ctx.ptr_outputbuffer[i].pmem_fd);
                        return OMX_ErrorInsufficientResources;
                    }
                }

                if(!align_pmem_buffers(drv_ctx.ptr_outputbuffer[i].pmem_fd,
                    drv_ctx.op_buf.buffer_size,
                    drv_ctx.op_buf.alignment))
                {
                    DEBUG_PRINT_ERROR("\n align_pmem_buffers() failed");
                    close(drv_ctx.ptr_outputbuffer[i].pmem_fd);
                    return OMX_ErrorInsufficientResources;
                }
#endif

                if(!secure_mode) {
                    drv_ctx.ptr_outputbuffer[i].bufferaddr =
                        (unsigned char *)mmap(NULL, drv_ctx.op_buf.buffer_size,
                        PROT_READ|PROT_WRITE, MAP_SHARED,
                        drv_ctx.ptr_outputbuffer[i].pmem_fd,0);
                    if (drv_ctx.ptr_outputbuffer[i].bufferaddr == MAP_FAILED) {
                        close(drv_ctx.ptr_outputbuffer[i].pmem_fd);
#ifdef USE_ION
                        free_ion_memory(&drv_ctx.op_buf_ion_info[i]);
#endif
                        DEBUG_PRINT_ERROR("Unable to mmap output buffer\n");
                        return OMX_ErrorInsufficientResources;
                    }
                }
                drv_ctx.ptr_outputbuffer[i].offset = 0;
                privateAppData = appData;
            }
            else {

                DEBUG_PRINT_HIGH("Use_op_buf: out_pmem=%d",m_use_output_pmem);
                if (!appData || !bytes ) {
                    if(!secure_mode && !buffer) {
                        DEBUG_PRINT_ERROR("\n Bad parameters for use buffer in EGL image case");
                        return OMX_ErrorBadParameter;
                    }
                }

                OMX_QCOM_PLATFORM_PRIVATE_LIST *pmem_list;
                OMX_QCOM_PLATFORM_PRIVATE_PMEM_INFO *pmem_info;
                pmem_list = (OMX_QCOM_PLATFORM_PRIVATE_LIST*) appData;
                if (!pmem_list->entryList || !pmem_list->entryList->entry ||
                    !pmem_list->nEntries ||
                    pmem_list->entryList->type != OMX_QCOM_PLATFORM_PRIVATE_PMEM) {
                        DEBUG_PRINT_ERROR("\n Pmem info not valid in use buffer");
                        return OMX_ErrorBadParameter;
                }
                pmem_info = (OMX_QCOM_PLATFORM_PRIVATE_PMEM_INFO *)
                    pmem_list->entryList->entry;
                DEBUG_PRINT_LOW("vdec: use buf: pmem_fd=0x%x",
                    (unsigned int)pmem_info->pmem_fd);
                drv_ctx.ptr_outputbuffer[i].pmem_fd = pmem_info->pmem_fd;
                drv_ctx.ptr_outputbuffer[i].offset = pmem_info->offset;
                drv_ctx.ptr_outputbuffer[i].bufferaddr = buff;
                drv_ctx.ptr_outputbuffer[i].mmaped_size =
                    drv_ctx.ptr_outputbuffer[i].buffer_len = drv_ctx.op_buf.buffer_size;
                privateAppData = appData;
            }
            m_pmem_info[i].offset = drv_ctx.ptr_outputbuffer[i].offset;
            m_pmem_info[i].pmem_fd = drv_ctx.ptr_outputbuffer[i].pmem_fd;

            *bufferHdr = (m_out_mem_ptr + i );
            if(secure_mode)
                drv_ctx.ptr_outputbuffer[i].bufferaddr = *bufferHdr;
            //setbuffers.buffer_type = VDEC_BUFFER_TYPE_OUTPUT;
            memcpy (&setbuffers.buffer,&drv_ctx.ptr_outputbuffer[i],
                sizeof (vdec_bufferpayload));

            DEBUG_PRINT_HIGH("\n Set the Output Buffer Idx: %d Addr: %p, pmem_fd=0x%x", i,
                drv_ctx.ptr_outputbuffer[i].bufferaddr,
                drv_ctx.ptr_outputbuffer[i].pmem_fd );

            if (m_pSwVdec)
            {
                if (m_pSwVdecOpBuffer == NULL)
                {
                    DEBUG_PRINT_HIGH("\n allocating m_pSwVdecOpBuffer %d", drv_ctx.op_buf.actualcount);
                    m_pSwVdecOpBuffer = (SWVDEC_OPBUFFER*)calloc(sizeof(SWVDEC_OPBUFFER), drv_ctx.op_buf.actualcount);
                }

                // SWVdec memory allocation and set the output buffer
                m_pSwVdecOpBuffer[i].nSize = drv_ctx.ptr_outputbuffer[i].mmaped_size;
                m_pSwVdecOpBuffer[i].pBuffer = (uint8*)drv_ctx.ptr_outputbuffer[i].bufferaddr;
                m_pSwVdecOpBuffer[i].pClientBufferData = (void*)i;
                if (SWVDEC_S_SUCCESS !=SwVdec_SetOutputBuffer(m_pSwVdec, &m_pSwVdecOpBuffer[i]))
                {
                    DEBUG_PRINT_HIGH("SwVdec_SetOutputBuffer failed in use_output_buffer\n ");
                    return OMX_ErrorInsufficientResources;
                }
            }
            else
            {
                struct v4l2_buffer buf;
                struct v4l2_plane plane[VIDEO_MAX_PLANES];
                int extra_idx = 0;
                buf.index = i;
                buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                buf.memory = V4L2_MEMORY_USERPTR;
                plane[0].length = drv_ctx.op_buf.buffer_size;
                plane[0].m.userptr = (unsigned long)drv_ctx.ptr_outputbuffer[i].bufferaddr -
                    (unsigned long)drv_ctx.ptr_outputbuffer[i].offset;
                plane[0].reserved[0] = drv_ctx.ptr_outputbuffer[i].pmem_fd;
                plane[0].reserved[1] = drv_ctx.ptr_outputbuffer[i].offset;
                plane[0].data_offset = 0;
                extra_idx = EXTRADATA_IDX(drv_ctx.num_planes);
                if (extra_idx && (extra_idx < VIDEO_MAX_PLANES)) {
                    plane[extra_idx].length = drv_ctx.extradata_info.buffer_size;
                    plane[extra_idx].m.userptr = (long unsigned int) (drv_ctx.extradata_info.uaddr + i * drv_ctx.extradata_info.buffer_size);
#ifdef USE_ION
                    plane[extra_idx].reserved[0] = drv_ctx.extradata_info.ion.fd_ion_data.fd;
#endif
                    plane[extra_idx].reserved[1] = i * drv_ctx.extradata_info.buffer_size;
                    plane[extra_idx].data_offset = 0;
                } else if  (extra_idx >= VIDEO_MAX_PLANES) {
                    DEBUG_PRINT_ERROR("Extradata index is more than allowed: %d\n", extra_idx);
                    return OMX_ErrorBadParameter;
                }
                buf.m.planes = plane;
                buf.length = drv_ctx.num_planes;

                DEBUG_PRINT_LOW("\n Set the Output Buffer Idx: %d Addr: %x", i, (unsigned int)drv_ctx.ptr_outputbuffer[i].bufferaddr);

                if (ioctl(drv_ctx.video_driver_fd, VIDIOC_PREPARE_BUF, &buf)) {
                    DEBUG_PRINT_ERROR("Failed to prepare bufs\n");
                    /*TODO: How to handle this case */
                    return OMX_ErrorInsufficientResources;
                }

                if (i == (drv_ctx.op_buf.actualcount -1) && !streaming[CAPTURE_PORT]) {
                    enum v4l2_buf_type buf_type;
                    buf_type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                    if (ioctl(drv_ctx.video_driver_fd, VIDIOC_STREAMON,&buf_type)) {
                        return OMX_ErrorInsufficientResources;
                    } else {
                        streaming[CAPTURE_PORT] = true;
                        DEBUG_PRINT_LOW("\n STREAMON Successful \n ");
                    }
                }
            }

            (*bufferHdr)->nAllocLen = drv_ctx.op_buf.buffer_size;
            if (m_enable_android_native_buffers) {
                DEBUG_PRINT_LOW("setting pBuffer to private_handle_t %p", handle);
                (*bufferHdr)->pBuffer = (OMX_U8 *)handle;
            } else {
                (*bufferHdr)->pBuffer = buff;
            }
            (*bufferHdr)->pAppPrivate = privateAppData;
            BITMASK_SET(&m_out_bm_count,i);
    }
    return eRet;
}

/* ======================================================================
FUNCTION
omx_vdec::use_input_heap_buffers

DESCRIPTION
OMX Use Buffer Heap allocation method implementation.

PARAMETERS
<TBD>.

RETURN VALUE
OMX Error None , if everything successful.

========================================================================== */
OMX_ERRORTYPE  omx_vdec::use_input_heap_buffers(
    OMX_IN OMX_HANDLETYPE            hComp,
    OMX_INOUT OMX_BUFFERHEADERTYPE** bufferHdr,
    OMX_IN OMX_U32                   port,
    OMX_IN OMX_PTR                   appData,
    OMX_IN OMX_U32                   bytes,
    OMX_IN OMX_U8*                   buffer)
{
    DEBUG_PRINT_LOW("Inside %s, %p\n", __FUNCTION__, buffer);
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    if(!m_inp_heap_ptr)
        m_inp_heap_ptr = (OMX_BUFFERHEADERTYPE*)
        calloc( (sizeof(OMX_BUFFERHEADERTYPE)),
        drv_ctx.ip_buf.actualcount);
    if(!m_phdr_pmem_ptr)
        m_phdr_pmem_ptr = (OMX_BUFFERHEADERTYPE**)
        calloc( (sizeof(OMX_BUFFERHEADERTYPE*)),
        drv_ctx.ip_buf.actualcount);
    if(!m_inp_heap_ptr || !m_phdr_pmem_ptr)
    {
        DEBUG_PRINT_ERROR("Insufficent memory");
        eRet = OMX_ErrorInsufficientResources;
    }
    else if (m_in_alloc_cnt < drv_ctx.ip_buf.actualcount)
    {
        input_use_buffer = true;
        memset(&m_inp_heap_ptr[m_in_alloc_cnt], 0, sizeof(OMX_BUFFERHEADERTYPE));
        m_inp_heap_ptr[m_in_alloc_cnt].pBuffer = buffer;
        m_inp_heap_ptr[m_in_alloc_cnt].nAllocLen = bytes;
        m_inp_heap_ptr[m_in_alloc_cnt].pAppPrivate = appData;
        m_inp_heap_ptr[m_in_alloc_cnt].nInputPortIndex = (OMX_U32) OMX_DirInput;
        m_inp_heap_ptr[m_in_alloc_cnt].nOutputPortIndex = (OMX_U32) OMX_DirMax;
        *bufferHdr = &m_inp_heap_ptr[m_in_alloc_cnt];
        eRet =
            allocate_input_buffer(hComp, &m_phdr_pmem_ptr[m_in_alloc_cnt], port, appData, bytes);
        DEBUG_PRINT_HIGH("\n Heap buffer(%p) Pmem buffer(%p)", *bufferHdr, m_phdr_pmem_ptr[m_in_alloc_cnt]);
        if (!m_input_free_q.insert_entry((unsigned)m_phdr_pmem_ptr[m_in_alloc_cnt],
            (unsigned)NULL, (unsigned)NULL))
        {
            DEBUG_PRINT_ERROR("\nERROR:Free_q is full");
            return OMX_ErrorInsufficientResources;
        }
        m_in_alloc_cnt++;
    }
    else
    {
        DEBUG_PRINT_ERROR("All i/p buffers have been set!");
        eRet = OMX_ErrorInsufficientResources;
    }
    return eRet;
}

/* ======================================================================
FUNCTION
omx_vdec::UseBuffer

DESCRIPTION
OMX Use Buffer method implementation.

PARAMETERS
<TBD>.

RETURN VALUE
OMX Error None , if everything successful.

========================================================================== */
OMX_ERRORTYPE  omx_vdec::use_buffer(
                                    OMX_IN OMX_HANDLETYPE            hComp,
                                    OMX_INOUT OMX_BUFFERHEADERTYPE** bufferHdr,
                                    OMX_IN OMX_U32                   port,
                                    OMX_IN OMX_PTR                   appData,
                                    OMX_IN OMX_U32                   bytes,
                                    OMX_IN OMX_U8*                   buffer)
{
    OMX_ERRORTYPE error = OMX_ErrorNone;
    struct vdec_setbuffer_cmd setbuffers;

    if (bufferHdr == NULL || bytes == 0)
    {
        if(!secure_mode && buffer == NULL) {
            DEBUG_PRINT_ERROR("bad param 0x%p %ld 0x%p",bufferHdr, bytes, buffer);
            return OMX_ErrorBadParameter;
        }
    }
    if(m_state == OMX_StateInvalid)
    {
        DEBUG_PRINT_ERROR("Use Buffer in Invalid State\n");
        return OMX_ErrorInvalidState;
    }
    if(port == OMX_CORE_INPUT_PORT_INDEX)
        error = use_input_heap_buffers(hComp, bufferHdr, port, appData, bytes, buffer);
    else if(port == OMX_CORE_OUTPUT_PORT_INDEX)
        error = use_output_buffer(hComp,bufferHdr,port,appData,bytes,buffer); //not tested
    else
    {
        DEBUG_PRINT_ERROR("Error: Invalid Port Index received %d\n",(int)port);
        error = OMX_ErrorBadPortIndex;
    }
    DEBUG_PRINT_LOW("Use Buffer: port %u, buffer %p, eRet %d", (unsigned int)port, *bufferHdr, error);
    if(error == OMX_ErrorNone)
    {
        if(allocate_done())
        {
            DEBUG_PRINT_LOW("Use Buffer: allocate_done\n");
            if (BITMASK_PRESENT(&m_flags,OMX_COMPONENT_IDLE_PENDING))
            {
                // Send the callback now
                BITMASK_CLEAR((&m_flags),OMX_COMPONENT_IDLE_PENDING);
                post_event(OMX_CommandStateSet,OMX_StateIdle,
                   OMX_COMPONENT_GENERATE_EVENT);
            }
            if (m_pSwVdec)
            {
                DEBUG_PRINT_LOW("Use Buffer: SwVdec_Start\n");
                SwVdec_Start(m_pSwVdec);
            }
        }
        if(port == OMX_CORE_INPUT_PORT_INDEX && m_inp_bPopulated &&
            BITMASK_PRESENT(&m_flags,OMX_COMPONENT_INPUT_ENABLE_PENDING))
        {
            BITMASK_CLEAR((&m_flags),OMX_COMPONENT_INPUT_ENABLE_PENDING);
            post_event(OMX_CommandPortEnable,
                OMX_CORE_INPUT_PORT_INDEX,
                OMX_COMPONENT_GENERATE_EVENT);
        }
        else if(port == OMX_CORE_OUTPUT_PORT_INDEX && m_out_bPopulated &&
            BITMASK_PRESENT(&m_flags,OMX_COMPONENT_OUTPUT_ENABLE_PENDING))
        {
            BITMASK_CLEAR((&m_flags),OMX_COMPONENT_OUTPUT_ENABLE_PENDING);
            post_event(OMX_CommandPortEnable,
                OMX_CORE_OUTPUT_PORT_INDEX,
                OMX_COMPONENT_GENERATE_EVENT);
        }
    }
    return error;
}

OMX_ERRORTYPE omx_vdec::free_input_buffer(unsigned int bufferindex,
                                          OMX_BUFFERHEADERTYPE *pmem_bufferHdr)
{
    if (m_inp_heap_ptr && !input_use_buffer && arbitrary_bytes)
    {
        if(m_inp_heap_ptr[bufferindex].pBuffer)
            free(m_inp_heap_ptr[bufferindex].pBuffer);
        m_inp_heap_ptr[bufferindex].pBuffer = NULL;
    }
    if (pmem_bufferHdr)
        free_input_buffer(pmem_bufferHdr);
    return OMX_ErrorNone;
}

OMX_ERRORTYPE omx_vdec::free_input_buffer(OMX_BUFFERHEADERTYPE *bufferHdr)
{
    unsigned int index = 0;
    if (bufferHdr == NULL || m_inp_mem_ptr == NULL)
    {
        return OMX_ErrorBadParameter;
    }

    index = bufferHdr - m_inp_mem_ptr;
    DEBUG_PRINT_LOW("\n Free Input Buffer index = %d",index);

    if (index < drv_ctx.ip_buf.actualcount && drv_ctx.ptr_inputbuffer)
    {
        DEBUG_PRINT_LOW("\n Free Input Buffer index = %d",index);
        if (drv_ctx.ptr_inputbuffer[index].pmem_fd > 0)
        {
            struct vdec_setbuffer_cmd setbuffers;
            setbuffers.buffer_type = VDEC_BUFFER_TYPE_INPUT;
            memcpy (&setbuffers.buffer,&drv_ctx.ptr_inputbuffer[index],
                sizeof (vdec_bufferpayload));
            DEBUG_PRINT_LOW("\n unmap the input buffer fd=%d",
                drv_ctx.ptr_inputbuffer[index].pmem_fd);
            DEBUG_PRINT_LOW("\n unmap the input buffer size=%d  address = %x",
                drv_ctx.ptr_inputbuffer[index].mmaped_size,
                (unsigned int)drv_ctx.ptr_inputbuffer[index].bufferaddr);
            munmap (drv_ctx.ptr_inputbuffer[index].bufferaddr,
                drv_ctx.ptr_inputbuffer[index].mmaped_size);
            close (drv_ctx.ptr_inputbuffer[index].pmem_fd);
            drv_ctx.ptr_inputbuffer[index].pmem_fd = -1;
            if (m_desc_buffer_ptr && m_desc_buffer_ptr[index].buf_addr)
            {
                free(m_desc_buffer_ptr[index].buf_addr);
                m_desc_buffer_ptr[index].buf_addr = NULL;
                m_desc_buffer_ptr[index].desc_data_size = 0;
            }
#ifdef USE_ION
            free_ion_memory(&drv_ctx.ip_buf_ion_info[index]);
#endif
        }
    }

    return OMX_ErrorNone;
}

OMX_ERRORTYPE omx_vdec::free_output_buffer(OMX_BUFFERHEADERTYPE *bufferHdr)
{
    unsigned int index = 0;

    if (bufferHdr == NULL || m_out_mem_ptr == NULL)
    {
        return OMX_ErrorBadParameter;
    }

    index = bufferHdr - m_out_mem_ptr;
    DEBUG_PRINT_LOW("\n Free ouput Buffer index = %d",index);

    if (index < drv_ctx.op_buf.actualcount
        && drv_ctx.ptr_outputbuffer)
    {
        DEBUG_PRINT_LOW("\n Free ouput Buffer index = %d addr = %x", index,
            (unsigned int)drv_ctx.ptr_outputbuffer[index].bufferaddr);

        struct vdec_setbuffer_cmd setbuffers;
        setbuffers.buffer_type = VDEC_BUFFER_TYPE_OUTPUT;
        memcpy (&setbuffers.buffer,&drv_ctx.ptr_outputbuffer[index],
            sizeof (vdec_bufferpayload));
#ifdef _ANDROID_
        if(m_enable_android_native_buffers) {
            if(drv_ctx.ptr_outputbuffer[index].pmem_fd > 0) {
                munmap(drv_ctx.ptr_outputbuffer[index].bufferaddr,
                    drv_ctx.ptr_outputbuffer[index].mmaped_size);
            }
            drv_ctx.ptr_outputbuffer[index].pmem_fd = -1;
        } else {
#endif
            if (drv_ctx.ptr_outputbuffer[0].pmem_fd > 0 && !ouput_egl_buffers && !m_use_output_pmem)
            {
                DEBUG_PRINT_LOW("\n unmap the output buffer fd = %d",
                    drv_ctx.ptr_outputbuffer[0].pmem_fd);
                DEBUG_PRINT_LOW("\n unmap the ouput buffer size=%d  address = %x",
                    drv_ctx.ptr_outputbuffer[0].mmaped_size * drv_ctx.op_buf.actualcount,
                    (unsigned int)drv_ctx.ptr_outputbuffer[0].bufferaddr);
                munmap (drv_ctx.ptr_outputbuffer[0].bufferaddr,
                    drv_ctx.ptr_outputbuffer[0].mmaped_size * drv_ctx.op_buf.actualcount);
                close (drv_ctx.ptr_outputbuffer[0].pmem_fd);
                drv_ctx.ptr_outputbuffer[0].pmem_fd = -1;
#ifdef USE_ION
                free_ion_memory(&drv_ctx.op_buf_ion_info[0]);
#endif
            }
#ifdef _ANDROID_
        }
#endif
        if (release_output_done()) {
            free_extradata();
        }
    }

    return OMX_ErrorNone;

}

OMX_ERRORTYPE omx_vdec::allocate_input_heap_buffer(OMX_HANDLETYPE       hComp,
                                                   OMX_BUFFERHEADERTYPE **bufferHdr,
                                                   OMX_U32              port,
                                                   OMX_PTR              appData,
                                                   OMX_U32              bytes)
{
    OMX_BUFFERHEADERTYPE *input = NULL;
    unsigned char *buf_addr = NULL;
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    unsigned   i = 0;

    /* Sanity Check*/
    if (bufferHdr == NULL)
    {
        return OMX_ErrorBadParameter;
    }

    if (m_inp_heap_ptr == NULL)
    {
        m_inp_heap_ptr = (OMX_BUFFERHEADERTYPE*) \
            calloc( (sizeof(OMX_BUFFERHEADERTYPE)),
            drv_ctx.ip_buf.actualcount);
        m_phdr_pmem_ptr = (OMX_BUFFERHEADERTYPE**) \
            calloc( (sizeof(OMX_BUFFERHEADERTYPE*)),
            drv_ctx.ip_buf.actualcount);

        if (m_inp_heap_ptr == NULL)
        {
            DEBUG_PRINT_ERROR("\n m_inp_heap_ptr Allocation failed ");
            return OMX_ErrorInsufficientResources;
        }
    }

    /*Find a Free index*/
    for(i=0; i< drv_ctx.ip_buf.actualcount; i++)
    {
        if(BITMASK_ABSENT(&m_heap_inp_bm_count,i))
        {
            DEBUG_PRINT_LOW("\n Free Input Buffer Index %d",i);
            break;
        }
    }

    if (i < drv_ctx.ip_buf.actualcount)
    {
        buf_addr = (unsigned char *)malloc (drv_ctx.ip_buf.buffer_size);

        if (buf_addr == NULL)
        {
            return OMX_ErrorInsufficientResources;
        }

        *bufferHdr = (m_inp_heap_ptr + i);
        input = *bufferHdr;
        BITMASK_SET(&m_heap_inp_bm_count,i);

        input->pBuffer           = (OMX_U8 *)buf_addr;
        input->nSize             = sizeof(OMX_BUFFERHEADERTYPE);
        input->nVersion.nVersion = OMX_SPEC_VERSION;
        input->nAllocLen         = drv_ctx.ip_buf.buffer_size;
        input->pAppPrivate       = appData;
        input->nInputPortIndex   = OMX_CORE_INPUT_PORT_INDEX;
        DEBUG_PRINT_LOW("\n Address of Heap Buffer %p",*bufferHdr );
        eRet = allocate_input_buffer(hComp,&m_phdr_pmem_ptr [i],port,appData,bytes);
        DEBUG_PRINT_LOW("\n Address of Pmem Buffer %p",m_phdr_pmem_ptr[i]);
        /*Add the Buffers to freeq*/
        if (!m_input_free_q.insert_entry((unsigned)m_phdr_pmem_ptr[i],
            (unsigned)NULL, (unsigned)NULL))
        {
            DEBUG_PRINT_ERROR("\nERROR:Free_q is full");
            return OMX_ErrorInsufficientResources;
        }
    }
    else
    {
        return OMX_ErrorBadParameter;
    }

    return eRet;

}


/* ======================================================================
FUNCTION
omx_vdec::AllocateInputBuffer

DESCRIPTION
Helper function for allocate buffer in the input pin

PARAMETERS
None.

RETURN VALUE
true/false

========================================================================== */
OMX_ERRORTYPE  omx_vdec::allocate_input_buffer(
    OMX_IN OMX_HANDLETYPE            hComp,
    OMX_INOUT OMX_BUFFERHEADERTYPE** bufferHdr,
    OMX_IN OMX_U32                   port,
    OMX_IN OMX_PTR                   appData,
    OMX_IN OMX_U32                   bytes)
{

    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    struct vdec_setbuffer_cmd setbuffers;
    OMX_BUFFERHEADERTYPE *input = NULL;
    unsigned   i = 0;
    unsigned char *buf_addr = NULL;
    int pmem_fd = -1;

    if(bytes != drv_ctx.ip_buf.buffer_size)
    {
        DEBUG_PRINT_LOW("\n Requested Size is wrong %d epected is %d",
            (int)bytes, drv_ctx.ip_buf.buffer_size);
        return OMX_ErrorBadParameter;
    }

    if(!m_inp_mem_ptr)
    {
        DEBUG_PRINT_HIGH("\n Allocate i/p buffer Header: Cnt(%d) Sz(%d)",
            drv_ctx.ip_buf.actualcount,
            drv_ctx.ip_buf.buffer_size);

        m_inp_mem_ptr = (OMX_BUFFERHEADERTYPE*) \
            calloc( (sizeof(OMX_BUFFERHEADERTYPE)), drv_ctx.ip_buf.actualcount);

        if (m_inp_mem_ptr == NULL)
        {
            return OMX_ErrorInsufficientResources;
        }

        drv_ctx.ptr_inputbuffer = (struct vdec_bufferpayload *) \
            calloc ((sizeof (struct vdec_bufferpayload)),drv_ctx.ip_buf.actualcount);

        if (drv_ctx.ptr_inputbuffer == NULL)
        {
            return OMX_ErrorInsufficientResources;
        }
#ifdef USE_ION
        drv_ctx.ip_buf_ion_info = (struct vdec_ion *) \
            calloc ((sizeof (struct vdec_ion)),drv_ctx.ip_buf.actualcount);

        if (drv_ctx.ip_buf_ion_info == NULL)
        {
            return OMX_ErrorInsufficientResources;
        }
#endif

        for (i=0; i < drv_ctx.ip_buf.actualcount; i++)
        {
            drv_ctx.ptr_inputbuffer [i].pmem_fd = -1;
#ifdef USE_ION
            drv_ctx.ip_buf_ion_info[i].ion_device_fd = -1;
#endif
        }

        if (m_swvdec_mode == SWVDEC_MODE_PARSE_DECODE)
        {
            // allocate swvdec input buffers
            m_pSwVdecIpBuffer = (SWVDEC_IPBUFFER *)calloc(sizeof(SWVDEC_IPBUFFER), drv_ctx.ip_buf.actualcount);
            if (m_pSwVdecIpBuffer == NULL) {
                eRet =  OMX_ErrorInsufficientResources;
            }
        }
    }

    for(i=0; i< drv_ctx.ip_buf.actualcount; i++)
    {
        if(BITMASK_ABSENT(&m_inp_bm_count,i))
        {
            DEBUG_PRINT_LOW("\n Free Input Buffer Index %d",i);
            break;
        }
    }

    if(i < drv_ctx.ip_buf.actualcount)
    {
#ifdef USE_ION
        int heap = 0;
#ifdef _HEVC_USE_ADSP_HEAP_
        heap = ION_ADSP_HEAP_ID;
#endif
        DEBUG_PRINT_HIGH("\n Allocate ion input Buffer size %d\n", drv_ctx.ip_buf.buffer_size);
        drv_ctx.ip_buf_ion_info[i].ion_device_fd = alloc_map_ion_memory(
            drv_ctx.ip_buf.buffer_size,drv_ctx.op_buf.alignment,
            &drv_ctx.ip_buf_ion_info[i].ion_alloc_data,
            &drv_ctx.ip_buf_ion_info[i].fd_ion_data, secure_mode ? ION_SECURE : 0, heap);
        if(drv_ctx.ip_buf_ion_info[i].ion_device_fd < 0) {
            return OMX_ErrorInsufficientResources;
        }
        pmem_fd = drv_ctx.ip_buf_ion_info[i].fd_ion_data.fd;
#else
        pmem_fd = open (MEM_DEVICE,O_RDWR);

        if (pmem_fd < 0)
        {
            DEBUG_PRINT_ERROR("\n open failed for pmem/adsp for input buffer");
            return OMX_ErrorInsufficientResources;
        }

        if (pmem_fd == 0)
        {
            pmem_fd = open (MEM_DEVICE,O_RDWR);

            if (pmem_fd < 0)
            {
                DEBUG_PRINT_ERROR("\n open failed for pmem/adsp for input buffer");
                return OMX_ErrorInsufficientResources;
            }
        }

        if(!align_pmem_buffers(pmem_fd, drv_ctx.ip_buf.buffer_size,
            drv_ctx.ip_buf.alignment))
        {
            DEBUG_PRINT_ERROR("\n align_pmem_buffers() failed");
            close(pmem_fd);
            return OMX_ErrorInsufficientResources;
        }
#endif
        if (!secure_mode) {
            buf_addr = (unsigned char *)mmap(NULL,
                drv_ctx.ip_buf.buffer_size,
                PROT_READ|PROT_WRITE, MAP_SHARED, pmem_fd, 0);

            if (buf_addr == MAP_FAILED)
            {
                close(pmem_fd);
#ifdef USE_ION
                free_ion_memory(&drv_ctx.ip_buf_ion_info[i]);
#endif
                DEBUG_PRINT_ERROR("\n Map Failed to allocate input buffer");
                return OMX_ErrorInsufficientResources;
            }
        }
        *bufferHdr = (m_inp_mem_ptr + i);
        if (secure_mode)
            drv_ctx.ptr_inputbuffer [i].bufferaddr = *bufferHdr;
        else
            drv_ctx.ptr_inputbuffer [i].bufferaddr = buf_addr;
        drv_ctx.ptr_inputbuffer [i].pmem_fd = pmem_fd;
        drv_ctx.ptr_inputbuffer [i].buffer_len = drv_ctx.ip_buf.buffer_size;
        drv_ctx.ptr_inputbuffer [i].mmaped_size = drv_ctx.ip_buf.buffer_size;
        drv_ctx.ptr_inputbuffer [i].offset = 0;

        if (!m_pSwVdec || m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY)
        {
            struct v4l2_buffer buf;
            struct v4l2_plane plane;
            int rc;
            buf.index = i;
            buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            buf.memory = V4L2_MEMORY_USERPTR;
            plane.bytesused = 0;
            plane.length = drv_ctx.ptr_inputbuffer [i].mmaped_size;
            plane.m.userptr = (unsigned long)drv_ctx.ptr_inputbuffer[i].bufferaddr;
            plane.reserved[0] =drv_ctx.ptr_inputbuffer [i].pmem_fd;
            plane.reserved[1] = 0;
            plane.data_offset = drv_ctx.ptr_inputbuffer[i].offset;
            buf.m.planes = &plane;
            buf.length = 1;

            DEBUG_PRINT_LOW("\n Set the input Buffer Idx: %d Addr: %x", i, (unsigned int)drv_ctx.ptr_inputbuffer[i].bufferaddr);
            rc = ioctl(drv_ctx.video_driver_fd, VIDIOC_PREPARE_BUF, &buf);
            if (rc) {
                DEBUG_PRINT_ERROR("Failed to prepare bufs\n");
                /*TODO: How to handle this case */
                return OMX_ErrorInsufficientResources;
            }
        }
        else if (m_swvdec_mode == SWVDEC_MODE_PARSE_DECODE)
        {
            m_pSwVdecIpBuffer[i].pBuffer = buf_addr;
            m_pSwVdecIpBuffer[i].pClientBufferData = (void*)i;
        }

        input = *bufferHdr;
        BITMASK_SET(&m_inp_bm_count,i);
        DEBUG_PRINT_LOW("\n Buffer address %p of pmem idx %d",*bufferHdr, i);
        if (secure_mode)
            input->pBuffer = (OMX_U8 *)drv_ctx.ptr_inputbuffer [i].pmem_fd;
        else
            input->pBuffer           = (OMX_U8 *)buf_addr;
        input->nSize             = sizeof(OMX_BUFFERHEADERTYPE);
        input->nVersion.nVersion = OMX_SPEC_VERSION;
        input->nAllocLen         = drv_ctx.ip_buf.buffer_size;
        input->pAppPrivate       = appData;
        input->nInputPortIndex   = OMX_CORE_INPUT_PORT_INDEX;
        input->pInputPortPrivate = (void *)&drv_ctx.ptr_inputbuffer [i];

        if (drv_ctx.disable_dmx)
        {
            eRet = allocate_desc_buffer(i);
        }
    }
    else
    {
        DEBUG_PRINT_ERROR("\nERROR:Input Buffer Index not found");
        eRet = OMX_ErrorInsufficientResources;
    }
    return eRet;
}


/* ======================================================================
FUNCTION
omx_vdec::AllocateOutputBuffer

DESCRIPTION
Helper fn for AllocateBuffer in the output pin

PARAMETERS
<TBD>.

RETURN VALUE
OMX Error None if everything went well.

========================================================================== */
OMX_ERRORTYPE  omx_vdec::allocate_output_buffer(
    OMX_IN OMX_HANDLETYPE            hComp,
    OMX_INOUT OMX_BUFFERHEADERTYPE** bufferHdr,
    OMX_IN OMX_U32                   port,
    OMX_IN OMX_PTR                   appData,
    OMX_IN OMX_U32                   bytes)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    OMX_BUFFERHEADERTYPE       *bufHdr= NULL; // buffer header
    unsigned                         i= 0; // Temporary counter
    struct vdec_setbuffer_cmd setbuffers;
    int extra_idx = 0;
#ifdef USE_ION
    int ion_device_fd =-1;
    struct ion_allocation_data ion_alloc_data;
    struct ion_fd_data fd_ion_data;
#endif
    if(!m_out_mem_ptr)
    {
        DEBUG_PRINT_HIGH("\n Allocate o/p buffer Header: Cnt(%d) Sz(%d)",
            drv_ctx.op_buf.actualcount,
            drv_ctx.op_buf.buffer_size);
        int nBufHdrSize        = 0;
        int nPlatformEntrySize = 0;
        int nPlatformListSize  = 0;
        int nPMEMInfoSize = 0;
        int pmem_fd = -1;
        unsigned char *pmem_baseaddress = NULL;

        OMX_QCOM_PLATFORM_PRIVATE_LIST      *pPlatformList;
        OMX_QCOM_PLATFORM_PRIVATE_ENTRY     *pPlatformEntry;
        OMX_QCOM_PLATFORM_PRIVATE_PMEM_INFO *pPMEMInfo;

        DEBUG_PRINT_LOW("Allocating First Output Buffer(%d)\n",
            drv_ctx.op_buf.actualcount);
        nBufHdrSize        = drv_ctx.op_buf.actualcount *
            sizeof(OMX_BUFFERHEADERTYPE);

        nPMEMInfoSize      = drv_ctx.op_buf.actualcount *
            sizeof(OMX_QCOM_PLATFORM_PRIVATE_PMEM_INFO);
        nPlatformListSize  = drv_ctx.op_buf.actualcount *
            sizeof(OMX_QCOM_PLATFORM_PRIVATE_LIST);
        nPlatformEntrySize = drv_ctx.op_buf.actualcount *
            sizeof(OMX_QCOM_PLATFORM_PRIVATE_ENTRY);

        DEBUG_PRINT_LOW("TotalBufHdr %d BufHdrSize %d PMEM %d PL %d\n",nBufHdrSize,
            sizeof(OMX_BUFFERHEADERTYPE),
            nPMEMInfoSize,
            nPlatformListSize);
        DEBUG_PRINT_LOW("PE %d OutputBuffer Count %d \n",nPlatformEntrySize,
            drv_ctx.op_buf.actualcount);
#ifdef USE_ION
        DEBUG_PRINT_HIGH("allocate outputBuffer size %d \n",drv_ctx.op_buf.buffer_size * drv_ctx.op_buf.actualcount);
        int heap_id = 0;
        int flags = secure_mode ? ION_SECURE : 0;
        if (!m_pSwVdec) {
#ifdef _HEVC_USE_ADSP_HEAP_
            heap_id = ION_ADSP_HEAP_ID;
#endif
        }
        ion_device_fd = alloc_map_ion_memory(
            drv_ctx.op_buf.buffer_size * drv_ctx.op_buf.actualcount,
            drv_ctx.op_buf.alignment,
            &ion_alloc_data, &fd_ion_data,flags, heap_id);
        if (ion_device_fd < 0) {
            return OMX_ErrorInsufficientResources;
        }
        pmem_fd = fd_ion_data.fd;
#else
        pmem_fd = open (MEM_DEVICE,O_RDWR);

        if (pmem_fd < 0)
        {
            DEBUG_PRINT_ERROR("\nERROR:pmem fd for output buffer %d",
                drv_ctx.op_buf.buffer_size);
            return OMX_ErrorInsufficientResources;
        }

        if(pmem_fd == 0)
        {
            pmem_fd = open (MEM_DEVICE,O_RDWR);

            if (pmem_fd < 0)
            {
                DEBUG_PRINT_ERROR("\nERROR:pmem fd for output buffer %d",
                    drv_ctx.op_buf.buffer_size);
                return OMX_ErrorInsufficientResources;
            }
        }

        if(!align_pmem_buffers(pmem_fd, drv_ctx.op_buf.buffer_size *
            drv_ctx.op_buf.actualcount,
            drv_ctx.op_buf.alignment))
        {
            DEBUG_PRINT_ERROR("\n align_pmem_buffers() failed");
            close(pmem_fd);
            return OMX_ErrorInsufficientResources;
        }
#endif
        if (!secure_mode) {
            pmem_baseaddress = (unsigned char *)mmap(NULL,
                (drv_ctx.op_buf.buffer_size *
                drv_ctx.op_buf.actualcount),
                PROT_READ|PROT_WRITE,MAP_SHARED,pmem_fd,0);
            if (pmem_baseaddress == MAP_FAILED)
            {
                DEBUG_PRINT_ERROR("\n MMAP failed for Size %d",
                    drv_ctx.op_buf.buffer_size);
                close(pmem_fd);
#ifdef USE_ION
                free_ion_memory(&drv_ctx.op_buf_ion_info[i]);
#endif
                return OMX_ErrorInsufficientResources;
            }
        }
        m_out_mem_ptr = (OMX_BUFFERHEADERTYPE  *)calloc(nBufHdrSize,1);
        // Alloc mem for platform specific info
        char *pPtr=NULL;
        pPtr = (char*) calloc(nPlatformListSize + nPlatformEntrySize +
            nPMEMInfoSize,1);
        drv_ctx.ptr_outputbuffer = (struct vdec_bufferpayload *)\
            calloc (sizeof(struct vdec_bufferpayload),
            drv_ctx.op_buf.actualcount);
        drv_ctx.ptr_respbuffer = (struct vdec_output_frameinfo  *)\
            calloc (sizeof (struct vdec_output_frameinfo),
            drv_ctx.op_buf.actualcount);
#ifdef USE_ION
        drv_ctx.op_buf_ion_info = (struct vdec_ion *)\
            calloc (sizeof(struct vdec_ion),
            drv_ctx.op_buf.actualcount);
#endif

        if (m_pSwVdec && m_pSwVdecOpBuffer == NULL)
        {
            m_pSwVdecOpBuffer = (SWVDEC_OPBUFFER*)calloc(sizeof(SWVDEC_OPBUFFER), drv_ctx.op_buf.actualcount);
        }
        if(m_out_mem_ptr && pPtr && drv_ctx.ptr_outputbuffer && drv_ctx.ptr_respbuffer
            && ((m_pSwVdec && m_pSwVdecOpBuffer) || (!m_pSwVdec)) )
        {
            drv_ctx.ptr_outputbuffer[0].mmaped_size =
                (drv_ctx.op_buf.buffer_size *
                drv_ctx.op_buf.actualcount);
            bufHdr          =  m_out_mem_ptr;
            m_platform_list = (OMX_QCOM_PLATFORM_PRIVATE_LIST *)(pPtr);
            m_platform_entry= (OMX_QCOM_PLATFORM_PRIVATE_ENTRY *)
                (((char *) m_platform_list)  + nPlatformListSize);
            m_pmem_info     = (OMX_QCOM_PLATFORM_PRIVATE_PMEM_INFO *)
                (((char *) m_platform_entry) + nPlatformEntrySize);
            pPlatformList   = m_platform_list;
            pPlatformEntry  = m_platform_entry;
            pPMEMInfo       = m_pmem_info;

            DEBUG_PRINT_LOW("Memory Allocation Succeeded for OUT port%p\n",m_out_mem_ptr);

            // Settting the entire storage nicely
            DEBUG_PRINT_LOW("bHdr %p OutMem %p PE %p\n",bufHdr, m_out_mem_ptr,pPlatformEntry);
            DEBUG_PRINT_LOW(" Pmem Info = %p \n",pPMEMInfo);
            for(i=0; i < drv_ctx.op_buf.actualcount ; i++)
            {
                bufHdr->nSize              = sizeof(OMX_BUFFERHEADERTYPE);
                bufHdr->nVersion.nVersion  = OMX_SPEC_VERSION;
                // Set the values when we determine the right HxW param
                bufHdr->nAllocLen          = bytes;
                bufHdr->nFilledLen         = 0;
                bufHdr->pAppPrivate        = appData;
                bufHdr->nOutputPortIndex   = OMX_CORE_OUTPUT_PORT_INDEX;
                // Platform specific PMEM Information
                // Initialize the Platform Entry
                //DEBUG_PRINT_LOW("Initializing the Platform Entry for %d\n",i);
                pPlatformEntry->type       = OMX_QCOM_PLATFORM_PRIVATE_PMEM;
                pPlatformEntry->entry      = pPMEMInfo;
                // Initialize the Platform List
                pPlatformList->nEntries    = 1;
                pPlatformList->entryList   = pPlatformEntry;
                // Keep pBuffer NULL till vdec is opened
                bufHdr->pBuffer            = NULL;
                bufHdr->nOffset            = 0;

                pPMEMInfo->offset          =  drv_ctx.op_buf.buffer_size*i;
                pPMEMInfo->pmem_fd = 0;
                bufHdr->pPlatformPrivate = pPlatformList;

                drv_ctx.ptr_outputbuffer[i].pmem_fd = pmem_fd;
                m_pmem_info[i].pmem_fd = pmem_fd;
#ifdef USE_ION
                drv_ctx.op_buf_ion_info[i].ion_device_fd = ion_device_fd;
                drv_ctx.op_buf_ion_info[i].ion_alloc_data = ion_alloc_data;
                drv_ctx.op_buf_ion_info[i].fd_ion_data = fd_ion_data;
#endif

                /*Create a mapping between buffers*/
                bufHdr->pOutputPortPrivate = &drv_ctx.ptr_respbuffer[i];
                drv_ctx.ptr_respbuffer[i].client_data = (void *)\
                    &drv_ctx.ptr_outputbuffer[i];
                drv_ctx.ptr_outputbuffer[i].offset = drv_ctx.op_buf.buffer_size*i;
                drv_ctx.ptr_outputbuffer[i].bufferaddr =
                    pmem_baseaddress + (drv_ctx.op_buf.buffer_size*i);

                DEBUG_PRINT_LOW("\n pmem_fd = %d offset = %d address = %p",
                    pmem_fd, drv_ctx.ptr_outputbuffer[i].offset,
                    drv_ctx.ptr_outputbuffer[i].bufferaddr);
                // Move the buffer and buffer header pointers
                bufHdr++;
                pPMEMInfo++;
                pPlatformEntry++;
                pPlatformList++;
            }
        }
        else
        {
            DEBUG_PRINT_ERROR("Output buf mem alloc failed[0x%p][0x%p]\n",\
                m_out_mem_ptr, pPtr);
            if(m_out_mem_ptr)
            {
                free(m_out_mem_ptr);
                m_out_mem_ptr = NULL;
            }
            if(pPtr)
            {
                free(pPtr);
                pPtr = NULL;
            }
            if(drv_ctx.ptr_outputbuffer)
            {
                free(drv_ctx.ptr_outputbuffer);
                drv_ctx.ptr_outputbuffer = NULL;
            }
            if(drv_ctx.ptr_respbuffer)
            {
                free(drv_ctx.ptr_respbuffer);
                drv_ctx.ptr_respbuffer = NULL;
            }
#ifdef USE_ION
            if (drv_ctx.op_buf_ion_info) {
                DEBUG_PRINT_LOW("\n Free o/p ion context");
                free(drv_ctx.op_buf_ion_info);
                drv_ctx.op_buf_ion_info = NULL;
            }
#endif
            eRet =  OMX_ErrorInsufficientResources;
        }
        if ( (!m_pSwVdec) && (eRet == OMX_ErrorNone) )
            eRet = allocate_extradata();
    }

    for(i=0; i< drv_ctx.op_buf.actualcount; i++)
    {
        if(BITMASK_ABSENT(&m_out_bm_count,i))
        {
            DEBUG_PRINT_LOW("\n Found a Free Output Buffer %d",i);
            break;
        }
    }

    if (eRet == OMX_ErrorNone)
    {
        if(i < drv_ctx.op_buf.actualcount)
        {
            int rc;
            m_pmem_info[i].offset = drv_ctx.ptr_outputbuffer[i].offset;
            drv_ctx.ptr_outputbuffer[i].buffer_len = drv_ctx.op_buf.buffer_size;

            *bufferHdr = (m_out_mem_ptr + i );
            if (secure_mode) {
                drv_ctx.ptr_outputbuffer[i].bufferaddr = *bufferHdr;
            }
            drv_ctx.ptr_outputbuffer[i].mmaped_size = drv_ctx.op_buf.buffer_size;

            if (m_pSwVdec)
            {
                (*bufferHdr)->pBuffer = (OMX_U8*)drv_ctx.ptr_outputbuffer[i].bufferaddr;
                (*bufferHdr)->pAppPrivate = appData;
                m_pSwVdecOpBuffer[i].nSize = drv_ctx.ptr_outputbuffer[i].mmaped_size;
                m_pSwVdecOpBuffer[i].pBuffer = (*bufferHdr)->pBuffer;
                m_pSwVdecOpBuffer[i].pClientBufferData = (void*)i;
                SwVdec_SetOutputBuffer(m_pSwVdec, &m_pSwVdecOpBuffer[i]);
            }
            else
            {
                struct v4l2_buffer buf;
                struct v4l2_plane plane[VIDEO_MAX_PLANES];
                buf.index = i;
                buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                buf.memory = V4L2_MEMORY_USERPTR;
                plane[0].length = drv_ctx.op_buf.buffer_size;
                plane[0].m.userptr = (unsigned long)drv_ctx.ptr_outputbuffer[i].bufferaddr -
                    (unsigned long)drv_ctx.ptr_outputbuffer[i].offset;
#ifdef USE_ION
                plane[0].reserved[0] = drv_ctx.op_buf_ion_info[i].fd_ion_data.fd;
#endif
                plane[0].reserved[1] = drv_ctx.ptr_outputbuffer[i].offset;
                plane[0].data_offset = 0;
                extra_idx = EXTRADATA_IDX(drv_ctx.num_planes);
                if (extra_idx && (extra_idx < VIDEO_MAX_PLANES)) {
                    plane[extra_idx].length = drv_ctx.extradata_info.buffer_size;
                    plane[extra_idx].m.userptr = (long unsigned int) (drv_ctx.extradata_info.uaddr + i * drv_ctx.extradata_info.buffer_size);
#ifdef USE_ION
                    plane[extra_idx].reserved[0] = drv_ctx.extradata_info.ion.fd_ion_data.fd;
#endif
                    plane[extra_idx].reserved[1] = i * drv_ctx.extradata_info.buffer_size;
                    plane[extra_idx].data_offset = 0;
                } else if (extra_idx >= VIDEO_MAX_PLANES) {
                    DEBUG_PRINT_ERROR("Extradata index higher than allowed: %d\n", extra_idx);
                    return OMX_ErrorBadParameter;
                }
                buf.m.planes = plane;
                buf.length = drv_ctx.num_planes;
                DEBUG_PRINT_LOW("\n Set the Output Buffer Idx: %d Addr: %x", i, (unsigned int)drv_ctx.ptr_outputbuffer[i].bufferaddr);
                rc = ioctl(drv_ctx.video_driver_fd, VIDIOC_PREPARE_BUF, &buf);
                if (rc) {
                    /*TODO: How to handle this case */
                    return OMX_ErrorInsufficientResources;
                }

                if (i == (drv_ctx.op_buf.actualcount -1 ) && !streaming[CAPTURE_PORT]) {
                    enum v4l2_buf_type buf_type;
                    buf_type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                    rc=ioctl(drv_ctx.video_driver_fd, VIDIOC_STREAMON,&buf_type);
                    if (rc) {
                        return OMX_ErrorInsufficientResources;
                    } else {
                        streaming[CAPTURE_PORT] = true;
                        DEBUG_PRINT_LOW("\n STREAMON Successful \n ");
                    }
                }
            }
            (*bufferHdr)->pBuffer = (OMX_U8*)drv_ctx.ptr_outputbuffer[i].bufferaddr;
            (*bufferHdr)->pAppPrivate = appData;
            BITMASK_SET(&m_out_bm_count,i);
        }
        else
        {
            DEBUG_PRINT_ERROR("All the Output Buffers have been Allocated ; Returning Insufficient \n");
            eRet = OMX_ErrorInsufficientResources;
        }
    }

    return eRet;
}


// AllocateBuffer  -- API Call
/* ======================================================================
FUNCTION
omx_vdec::AllocateBuffer

DESCRIPTION
Returns zero if all the buffers released..

PARAMETERS
None.

RETURN VALUE
true/false

========================================================================== */
OMX_ERRORTYPE  omx_vdec::allocate_buffer(OMX_IN OMX_HANDLETYPE                hComp,
                                         OMX_INOUT OMX_BUFFERHEADERTYPE** bufferHdr,
                                         OMX_IN OMX_U32                        port,
                                         OMX_IN OMX_PTR                     appData,
                                         OMX_IN OMX_U32                       bytes)
{
    unsigned i = 0;
    OMX_ERRORTYPE eRet = OMX_ErrorNone; // OMX return type

    DEBUG_PRINT_LOW("\n Allocate buffer on port %d \n", (int)port);
    if(m_state == OMX_StateInvalid)
    {
        DEBUG_PRINT_ERROR("Allocate Buf in Invalid State\n");
        return OMX_ErrorInvalidState;
    }

    if(port == OMX_CORE_INPUT_PORT_INDEX)
    {
        if (arbitrary_bytes)
        {
            eRet = allocate_input_heap_buffer (hComp,bufferHdr,port,appData,bytes);
        }
        else
        {
            eRet = allocate_input_buffer(hComp,bufferHdr,port,appData,bytes);
        }
    }
    else if(port == OMX_CORE_OUTPUT_PORT_INDEX)
    {
        eRet = client_buffers.allocate_buffers_color_convert(hComp,bufferHdr,port,
            appData,bytes);
    }
    else
    {
        DEBUG_PRINT_ERROR("Error: Invalid Port Index received %d\n",(int)port);
        eRet = OMX_ErrorBadPortIndex;
    }
    DEBUG_PRINT_LOW("Checking for Output Allocate buffer Done");
    if(eRet == OMX_ErrorNone)
    {
        if(allocate_done())
        {
            if(BITMASK_PRESENT(&m_flags,OMX_COMPONENT_IDLE_PENDING))
            {
                // Send the callback now
                BITMASK_CLEAR((&m_flags),OMX_COMPONENT_IDLE_PENDING);
                post_event(OMX_CommandStateSet,OMX_StateIdle,
                    OMX_COMPONENT_GENERATE_EVENT);
            }
            if (m_pSwVdec)
            {
                DEBUG_PRINT_LOW("allocate_buffer: SwVdec_Start\n");
                SwVdec_Start(m_pSwVdec);
            }
        }
        if(port == OMX_CORE_INPUT_PORT_INDEX && m_inp_bPopulated)
        {
            if(BITMASK_PRESENT(&m_flags,OMX_COMPONENT_INPUT_ENABLE_PENDING))
            {
                BITMASK_CLEAR((&m_flags),OMX_COMPONENT_INPUT_ENABLE_PENDING);
                post_event(OMX_CommandPortEnable,
                    OMX_CORE_INPUT_PORT_INDEX,
                    OMX_COMPONENT_GENERATE_EVENT);
            }
        }
        if(port == OMX_CORE_OUTPUT_PORT_INDEX && m_out_bPopulated)
        {
            if(BITMASK_PRESENT(&m_flags,OMX_COMPONENT_OUTPUT_ENABLE_PENDING))
            {
                BITMASK_CLEAR((&m_flags),OMX_COMPONENT_OUTPUT_ENABLE_PENDING);
                post_event(OMX_CommandPortEnable,
                    OMX_CORE_OUTPUT_PORT_INDEX,
                    OMX_COMPONENT_GENERATE_EVENT);
            }
        }
    }
    DEBUG_PRINT_LOW("Allocate Buffer exit with ret Code %d\n",eRet);
    return eRet;
}

// Free Buffer - API call
/* ======================================================================
FUNCTION
omx_vdec::FreeBuffer

DESCRIPTION

PARAMETERS
None.

RETURN VALUE
true/false

========================================================================== */
OMX_ERRORTYPE  omx_vdec::free_buffer(OMX_IN OMX_HANDLETYPE         hComp,
                                     OMX_IN OMX_U32                 port,
                                     OMX_IN OMX_BUFFERHEADERTYPE* buffer)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    unsigned int nPortIndex;

    if(m_state == OMX_StateIdle &&
        (BITMASK_PRESENT(&m_flags ,OMX_COMPONENT_LOADING_PENDING)))
    {
        DEBUG_PRINT_LOW(" free buffer while Component in Loading pending\n");
    }
    else if((m_inp_bEnabled == OMX_FALSE && port == OMX_CORE_INPUT_PORT_INDEX)||
        (m_out_bEnabled == OMX_FALSE && port == OMX_CORE_OUTPUT_PORT_INDEX))
    {
        DEBUG_PRINT_LOW("Free Buffer while port %d disabled\n", (int)port);
    }
    else if ((port == OMX_CORE_INPUT_PORT_INDEX &&
        BITMASK_PRESENT(&m_flags, OMX_COMPONENT_INPUT_ENABLE_PENDING)) ||
        (port == OMX_CORE_OUTPUT_PORT_INDEX &&
        BITMASK_PRESENT(&m_flags, OMX_COMPONENT_OUTPUT_ENABLE_PENDING)))
    {
        DEBUG_PRINT_LOW("Free Buffer while port %d enable pending\n", (int)port);
    }
    else if(m_state == OMX_StateExecuting || m_state == OMX_StatePause)
    {
        DEBUG_PRINT_ERROR("Invalid state to free buffer,ports need to be disabled\n");
        post_event(OMX_EventError,
            OMX_ErrorPortUnpopulated,
            OMX_COMPONENT_GENERATE_EVENT);

        return OMX_ErrorIncorrectStateOperation;
    }
    else if (m_state != OMX_StateInvalid)
    {
        DEBUG_PRINT_ERROR("Invalid state %d to free buffer,port %d lost Buffers\n", m_state, (int)port);
        post_event(OMX_EventError,
            OMX_ErrorPortUnpopulated,
            OMX_COMPONENT_GENERATE_EVENT);
    }

    if(port == OMX_CORE_INPUT_PORT_INDEX)
    {
        /*Check if arbitrary bytes*/
        if(!arbitrary_bytes && !input_use_buffer)
            nPortIndex = buffer - m_inp_mem_ptr;
        else
            nPortIndex = buffer - m_inp_heap_ptr;

        DEBUG_PRINT_LOW("free_buffer on i/p port - Port idx %d \n", nPortIndex);
        if(nPortIndex < drv_ctx.ip_buf.actualcount)
        {
            // Clear the bit associated with it.
            BITMASK_CLEAR(&m_inp_bm_count,nPortIndex);
            BITMASK_CLEAR(&m_heap_inp_bm_count,nPortIndex);
            if (input_use_buffer == true)
            {

                DEBUG_PRINT_LOW("\n Free pmem Buffer index %d",nPortIndex);
                if(m_phdr_pmem_ptr)
                    free_input_buffer(m_phdr_pmem_ptr[nPortIndex]);
            }
            else
            {
                if (arbitrary_bytes)
                {
                    if(m_phdr_pmem_ptr)
                        free_input_buffer(nPortIndex,m_phdr_pmem_ptr[nPortIndex]);
                    else
                        free_input_buffer(nPortIndex,NULL);
                }
                else
                    free_input_buffer(buffer);
            }
            m_inp_bPopulated = OMX_FALSE;
            /*Free the Buffer Header*/
            if (release_input_done())
            {
                DEBUG_PRINT_HIGH("\n ALL input buffers are freed/released");
                free_input_buffer_header();
            }
        }
        else
        {
            DEBUG_PRINT_ERROR("Error: free_buffer ,Port Index Invalid\n");
            eRet = OMX_ErrorBadPortIndex;
        }

        if(BITMASK_PRESENT((&m_flags),OMX_COMPONENT_INPUT_DISABLE_PENDING)
            && release_input_done())
        {
            DEBUG_PRINT_LOW("MOVING TO INPUT DISABLED STATE \n");
            BITMASK_CLEAR((&m_flags),OMX_COMPONENT_INPUT_DISABLE_PENDING);
            post_event(OMX_CommandPortDisable,
                OMX_CORE_INPUT_PORT_INDEX,
                OMX_COMPONENT_GENERATE_EVENT);
        }
    }
    else if(port == OMX_CORE_OUTPUT_PORT_INDEX)
    {
        // check if the buffer is valid
        nPortIndex = buffer - client_buffers.get_il_buf_hdr();
        if(nPortIndex < drv_ctx.op_buf.actualcount)
        {
            DEBUG_PRINT_LOW("free_buffer on o/p port - Port idx %d \n", nPortIndex);
            // Clear the bit associated with it.
            BITMASK_CLEAR(&m_out_bm_count,nPortIndex);
            m_out_bPopulated = OMX_FALSE;
            client_buffers.free_output_buffer (buffer);

            if (release_output_done())
            {
                free_output_buffer_header();
                if (m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY)
                {
                    DEBUG_PRINT_LOW("release_output_done: start free_interm_buffers");
                    free_interm_buffers();
                }
            }
        }
        else
        {
            DEBUG_PRINT_ERROR("Error: free_buffer , Port Index Invalid\n");
            eRet = OMX_ErrorBadPortIndex;
        }
        if(BITMASK_PRESENT((&m_flags),OMX_COMPONENT_OUTPUT_DISABLE_PENDING)
            && release_output_done())
        {
            DEBUG_PRINT_LOW("MOVING TO OUTPUT DISABLED STATE \n");
            BITMASK_CLEAR((&m_flags),OMX_COMPONENT_OUTPUT_DISABLE_PENDING);
#ifdef _ANDROID_ICS_
            if (m_enable_android_native_buffers)
            {
                DEBUG_PRINT_LOW("FreeBuffer - outport disabled: reset native buffers");
                memset(&native_buffer, 0 ,(sizeof(struct nativebuffer) * MAX_NUM_INPUT_OUTPUT_BUFFERS));
            }
#endif

            post_event(OMX_CommandPortDisable,
                OMX_CORE_OUTPUT_PORT_INDEX,
                OMX_COMPONENT_GENERATE_EVENT);
        }
    }
    else
    {
        eRet = OMX_ErrorBadPortIndex;
    }
    if((eRet == OMX_ErrorNone) &&
        (BITMASK_PRESENT(&m_flags ,OMX_COMPONENT_LOADING_PENDING)))
    {
        if(release_done())
        {
            // Send the callback now
            BITMASK_CLEAR((&m_flags),OMX_COMPONENT_LOADING_PENDING);
            if (m_pSwVdec)
            {
                SwVdec_Stop(m_pSwVdec);
            }
            post_event(OMX_CommandStateSet, OMX_StateLoaded,
                OMX_COMPONENT_GENERATE_EVENT);
        }
    }
    return eRet;
}


/* ======================================================================
FUNCTION
omx_vdec::EmptyThisBuffer

DESCRIPTION
This routine is used to push the encoded video frames to
the video decoder.

PARAMETERS
None.

RETURN VALUE
OMX Error None if everything went successful.

========================================================================== */
OMX_ERRORTYPE  omx_vdec::empty_this_buffer(OMX_IN OMX_HANDLETYPE         hComp,
                                           OMX_IN OMX_BUFFERHEADERTYPE* buffer)
{
    OMX_ERRORTYPE ret1 = OMX_ErrorNone;
    unsigned int nBufferIndex = drv_ctx.ip_buf.actualcount;

    if(m_state == OMX_StateInvalid)
    {
        DEBUG_PRINT_ERROR("Empty this buffer in Invalid State\n");
        return OMX_ErrorInvalidState;
    }

    if (buffer == NULL)
    {
        DEBUG_PRINT_ERROR("\nERROR:ETB Buffer is NULL");
        return OMX_ErrorBadParameter;
    }

    if (!m_inp_bEnabled)
    {
        DEBUG_PRINT_ERROR("\nERROR:ETB incorrect state operation, input port is disabled.");
        return OMX_ErrorIncorrectStateOperation;
    }

    if (buffer->nInputPortIndex != OMX_CORE_INPUT_PORT_INDEX)
    {
        DEBUG_PRINT_ERROR("\nERROR:ETB invalid port in header %lu", buffer->nInputPortIndex);
        return OMX_ErrorBadPortIndex;
    }

    if (buffer->nFlags & OMX_BUFFERFLAG_CODECCONFIG)
    {
        codec_config_flag = true;
        DEBUG_PRINT_LOW("%s: codec_config buffer", __FUNCTION__);
    }
    else
    {
        codec_config_flag = false;
    }

#ifdef _ANDROID_
    if(iDivXDrmDecrypt)
    {
        OMX_ERRORTYPE drmErr = iDivXDrmDecrypt->Decrypt(buffer);
        if(drmErr != OMX_ErrorNone) {
            // this error can be ignored
            DEBUG_PRINT_LOW("\nERROR:iDivXDrmDecrypt->Decrypt %d", drmErr);
        }
    }
#endif //_ANDROID_
    if (perf_flag)
    {
        if (!latency)
        {
            dec_time.stop();
            latency = dec_time.processing_time_us();
            dec_time.start();
        }
    }

    if (arbitrary_bytes)
    {
        nBufferIndex = buffer - m_inp_heap_ptr;
    }
    else
    {
        if (input_use_buffer == true)
        {
            nBufferIndex = buffer - m_inp_heap_ptr;
            m_inp_mem_ptr[nBufferIndex].nFilledLen = m_inp_heap_ptr[nBufferIndex].nFilledLen;
            m_inp_mem_ptr[nBufferIndex].nTimeStamp = m_inp_heap_ptr[nBufferIndex].nTimeStamp;
            m_inp_mem_ptr[nBufferIndex].nFlags = m_inp_heap_ptr[nBufferIndex].nFlags;
            buffer = &m_inp_mem_ptr[nBufferIndex];
            DEBUG_PRINT_LOW("Non-Arbitrary mode - buffer address is: malloc %p, pmem%p in Index %d, buffer %p of size %lu",
                &m_inp_heap_ptr[nBufferIndex], &m_inp_mem_ptr[nBufferIndex],nBufferIndex, buffer, buffer->nFilledLen);
        }
        else{
            nBufferIndex = buffer - m_inp_mem_ptr;
        }
    }

    if (nBufferIndex > drv_ctx.ip_buf.actualcount )
    {
        DEBUG_PRINT_ERROR("\nERROR:ETB nBufferIndex is invalid");
        return OMX_ErrorBadParameter;
    }

    DEBUG_PRINT_LOW("[ETB] BHdr(%p) pBuf(%p) nTS(%lld) nFL(%lu)",
        buffer, buffer->pBuffer, buffer->nTimeStamp, buffer->nFilledLen);
    if (arbitrary_bytes)
    {
        post_event ((unsigned)hComp,(unsigned)buffer,
            OMX_COMPONENT_GENERATE_ETB_ARBITRARY);
    }
    else
    {
        if (!(client_extradata & OMX_TIMEINFO_EXTRADATA))
            set_frame_rate(buffer->nTimeStamp);
        post_event ((unsigned)hComp,(unsigned)buffer,OMX_COMPONENT_GENERATE_ETB);
    }
    return OMX_ErrorNone;
}

/* ======================================================================
FUNCTION
omx_vdec::empty_this_buffer_proxy

DESCRIPTION
This routine is used to push the encoded video frames to
the video decoder.

PARAMETERS
None.

RETURN VALUE
OMX Error None if everything went successful.

========================================================================== */
OMX_ERRORTYPE  omx_vdec::empty_this_buffer_proxy(OMX_IN OMX_HANDLETYPE         hComp,
                                                 OMX_IN OMX_BUFFERHEADERTYPE* buffer)
{
    int push_cnt = 0,i=0;
    unsigned nPortIndex = 0;
    OMX_ERRORTYPE ret = OMX_ErrorNone;
    struct vdec_input_frameinfo frameinfo;
    struct vdec_bufferpayload *temp_buffer;
    struct vdec_seqheader seq_header;
    bool port_setting_changed = true;
    bool not_coded_vop = false;

    /*Should we generate a Aync error event*/
    if (buffer == NULL || buffer->pInputPortPrivate == NULL)
    {
        DEBUG_PRINT_ERROR("\nERROR:empty_this_buffer_proxy is invalid");
        return OMX_ErrorBadParameter;
    }

    nPortIndex = buffer-((OMX_BUFFERHEADERTYPE *)m_inp_mem_ptr);

    if (nPortIndex > drv_ctx.ip_buf.actualcount)
    {
        DEBUG_PRINT_ERROR("\nERROR:empty_this_buffer_proxy invalid nPortIndex[%u]",
            nPortIndex);
        return OMX_ErrorBadParameter;
    }

    if (m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY && m_fill_internal_bufers)
    {
        fill_all_buffers_proxy_dsp(hComp);
    }

    pending_input_buffers++;

    /* return zero length and not an EOS buffer */
    if (!arbitrary_bytes && (buffer->nFilledLen == 0) &&
        ((buffer->nFlags & OMX_BUFFERFLAG_EOS) == 0))
    {
        DEBUG_PRINT_HIGH("\n return zero legth buffer");
        post_event ((unsigned int)buffer,VDEC_S_SUCCESS,
            OMX_COMPONENT_GENERATE_EBD);
        return OMX_ErrorNone;
    }

    if(input_flush_progress == true

        || not_coded_vop

        )
    {
        DEBUG_PRINT_LOW("\n Flush in progress return buffer ");
        post_event ((unsigned int)buffer,VDEC_S_SUCCESS,
            OMX_COMPONENT_GENERATE_EBD);
        return OMX_ErrorNone;
    }

    temp_buffer = (struct vdec_bufferpayload *)buffer->pInputPortPrivate;

    if ((temp_buffer -  drv_ctx.ptr_inputbuffer) > (int)drv_ctx.ip_buf.actualcount)
    {
        return OMX_ErrorBadParameter;
    }

    DEBUG_PRINT_LOW("\n ETBProxy: bufhdr = %p, bufhdr->pBuffer = %p", buffer, buffer->pBuffer);
    /*for use buffer we need to memcpy the data*/
    temp_buffer->buffer_len = buffer->nFilledLen;

    if (input_use_buffer)
    {
        if (buffer->nFilledLen <= temp_buffer->buffer_len)
        {
            if(arbitrary_bytes)
            {
                memcpy (temp_buffer->bufferaddr, (buffer->pBuffer + buffer->nOffset),buffer->nFilledLen);
            }
            else
            {
                memcpy (temp_buffer->bufferaddr, (m_inp_heap_ptr[nPortIndex].pBuffer + m_inp_heap_ptr[nPortIndex].nOffset),
                    buffer->nFilledLen);
            }
        }
        else
        {
            return OMX_ErrorBadParameter;
        }

    }

    frameinfo.bufferaddr = temp_buffer->bufferaddr;
    frameinfo.client_data = (void *) buffer;
    frameinfo.datalen = temp_buffer->buffer_len;
    frameinfo.flags = 0;
    frameinfo.offset = buffer->nOffset;
    frameinfo.pmem_fd = temp_buffer->pmem_fd;
    frameinfo.pmem_offset = temp_buffer->offset;
    frameinfo.timestamp = buffer->nTimeStamp;
    if (drv_ctx.disable_dmx && m_desc_buffer_ptr && m_desc_buffer_ptr[nPortIndex].buf_addr)
    {
        DEBUG_PRINT_LOW("ETB: dmx enabled");
        if (m_demux_entries == 0)
        {
            extract_demux_addr_offsets(buffer);
        }

        DEBUG_PRINT_LOW("ETB: handle_demux_data - entries=%d", (int)m_demux_entries);
        handle_demux_data(buffer);
        frameinfo.desc_addr = (OMX_U8 *)m_desc_buffer_ptr[nPortIndex].buf_addr;
        frameinfo.desc_size = m_desc_buffer_ptr[nPortIndex].desc_data_size;
    }
    else
    {
        frameinfo.desc_addr = NULL;
        frameinfo.desc_size = 0;
    }
    if(!arbitrary_bytes)
    {
        frameinfo.flags |= buffer->nFlags;
    }

#ifdef _ANDROID_
    if (m_debug_timestamp)
    {
        if(arbitrary_bytes)
        {
            DEBUG_PRINT_LOW("\n Inserting TIMESTAMP (%lld) into queue", buffer->nTimeStamp);
            m_timestamp_list.insert_ts(buffer->nTimeStamp);
        }
        else if(!arbitrary_bytes && !(buffer->nFlags & OMX_BUFFERFLAG_CODECCONFIG))
        {
            DEBUG_PRINT_LOW("\n Inserting TIMESTAMP (%lld) into queue", buffer->nTimeStamp);
            m_timestamp_list.insert_ts(buffer->nTimeStamp);
        }
    }
#endif

#ifdef INPUT_BUFFER_LOG
    if (inputBufferFile1)
    {
        fwrite((const char *)temp_buffer->bufferaddr,
            temp_buffer->buffer_len,1,inputBufferFile1);
    }
#endif

    if(buffer->nFlags & QOMX_VIDEO_BUFFERFLAG_EOSEQ)
    {
        frameinfo.flags |= QOMX_VIDEO_BUFFERFLAG_EOSEQ;
        buffer->nFlags &= ~QOMX_VIDEO_BUFFERFLAG_EOSEQ;
    }

    if (temp_buffer->buffer_len == 0 || (buffer->nFlags & OMX_BUFFERFLAG_EOS))
    {
        DEBUG_PRINT_HIGH("\n Rxd i/p EOS, Notify Driver that EOS has been reached");
        frameinfo.flags |= VDEC_BUFFERFLAG_EOS;
        h264_scratch.nFilledLen = 0;
        nal_count = 0;
        look_ahead_nal = false;
        frame_count = 0;
        if (m_frame_parser.mutils)
            m_frame_parser.mutils->initialize_frame_checking_environment();
        m_frame_parser.flush();
        h264_last_au_ts = LLONG_MAX;
        h264_last_au_flags = 0;
        memset(m_demux_offsets, 0, ( sizeof(OMX_U32) * 8192) );
        m_demux_entries = 0;
    }

    if ( (!m_pSwVdec) || (m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY) )
    {
        struct v4l2_buffer buf;
        struct v4l2_plane plane;
        memset( (void *)&buf, 0, sizeof(buf));
        memset( (void *)&plane, 0, sizeof(plane));
        int rc;
        unsigned long  print_count;
        if (temp_buffer->buffer_len == 0 || (buffer->nFlags & OMX_BUFFERFLAG_EOS))
        {
            buf.flags = V4L2_QCOM_BUF_FLAG_EOS;
            DEBUG_PRINT_HIGH("\n  INPUT EOS reached \n") ;
        }
        OMX_ERRORTYPE eRet = OMX_ErrorNone;
        buf.index = nPortIndex;
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_USERPTR;
        plane.bytesused = temp_buffer->buffer_len;
        plane.length = drv_ctx.ip_buf.buffer_size;
        plane.m.userptr = (unsigned long)temp_buffer->bufferaddr -
            (unsigned long)temp_buffer->offset;
        plane.reserved[0] = temp_buffer->pmem_fd;
        plane.reserved[1] = temp_buffer->offset;
        plane.data_offset = 0;
        buf.m.planes = &plane;
        buf.length = 1;
        if (frameinfo.timestamp >= LLONG_MAX) {
            buf.flags |= V4L2_QCOM_BUF_TIMESTAMP_INVALID;
        }
        //assumption is that timestamp is in milliseconds
        buf.timestamp.tv_sec = frameinfo.timestamp / 1000000;
        buf.timestamp.tv_usec = (frameinfo.timestamp % 1000000);
        buf.flags |= (buffer->nFlags & OMX_BUFFERFLAG_CODECCONFIG) ? V4L2_QCOM_BUF_FLAG_CODECCONFIG: 0;

        rc = ioctl(drv_ctx.video_driver_fd, VIDIOC_QBUF, &buf);
        if(rc)
        {
            DEBUG_PRINT_ERROR("Failed to qbuf Input buffer to driver\n");
            return OMX_ErrorHardware;
        }
        if(!streaming[OUTPUT_PORT])
        {
            enum v4l2_buf_type buf_type;
            int ret,r;

            buf_type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            DEBUG_PRINT_LOW("send_command_proxy(): Idle-->Executing\n");
            ret=ioctl(drv_ctx.video_driver_fd, VIDIOC_STREAMON,&buf_type);
            if(!ret) {
                DEBUG_PRINT_HIGH("Streamon on OUTPUT Plane was successful \n");
                streaming[OUTPUT_PORT] = true;
            } else {
                /*TODO: How to handle this case */
                DEBUG_PRINT_ERROR(" \n Failed to call streamon on OUTPUT \n");
                DEBUG_PRINT_LOW("If Stream on failed no buffer should be queued");
                post_event ((unsigned int)buffer,VDEC_S_SUCCESS,
                    OMX_COMPONENT_GENERATE_EBD);
                return OMX_ErrorBadParameter;
            }
        }
    }
    else if (m_swvdec_mode == SWVDEC_MODE_PARSE_DECODE)
    {
        // send this to the swvdec
        DEBUG_PRINT_HIGH("\n empty_this_buffer_proxy bufHdr %p pBuffer %p nFilledLen %lu m_pSwVdecIpBuffer %p, idx %d",
            buffer, buffer->pBuffer, buffer->nFilledLen, m_pSwVdecIpBuffer, nPortIndex);
        m_pSwVdecIpBuffer[nPortIndex].nFlags = buffer->nFlags;
        m_pSwVdecIpBuffer[nPortIndex].nFilledLen = buffer->nFilledLen;
        m_pSwVdecIpBuffer[nPortIndex].nIpTimestamp = buffer->nTimeStamp;
        if (SwVdec_EmptyThisBuffer(m_pSwVdec, &m_pSwVdecIpBuffer[nPortIndex]) != SWVDEC_S_SUCCESS) {
            ret = OMX_ErrorBadParameter;
        }
    }

    DEBUG_PRINT_LOW("[ETBP] pBuf(%p) nTS(%lld) Sz(%d)",
        frameinfo.bufferaddr, frameinfo.timestamp, frameinfo.datalen);
    time_stamp_dts.insert_timestamp(buffer);
    return ret;
}

/* ======================================================================
FUNCTION
omx_vdec::FillThisBuffer

DESCRIPTION
IL client uses this method to release the frame buffer
after displaying them.

PARAMETERS
None.

RETURN VALUE
true/false

========================================================================== */
OMX_ERRORTYPE  omx_vdec::fill_this_buffer(OMX_IN OMX_HANDLETYPE  hComp,
                                          OMX_IN OMX_BUFFERHEADERTYPE* buffer)
{
    if(m_state == OMX_StateInvalid)
    {
        DEBUG_PRINT_ERROR("FTB in Invalid State\n");
        return OMX_ErrorInvalidState;
    }

    if (!m_out_bEnabled)
    {
        DEBUG_PRINT_ERROR("\nERROR:FTB incorrect state operation, output port is disabled.");
        return OMX_ErrorIncorrectStateOperation;
    }

    if (buffer == NULL ||
        ((buffer - client_buffers.get_il_buf_hdr()) >= (int)drv_ctx.op_buf.actualcount))
    {
        return OMX_ErrorBadParameter;
    }

    if (buffer->nOutputPortIndex != OMX_CORE_OUTPUT_PORT_INDEX)
    {
        DEBUG_PRINT_ERROR("\nERROR:FTB invalid port in header %lu", buffer->nOutputPortIndex);
        return OMX_ErrorBadPortIndex;
    }

    DEBUG_PRINT_LOW("[FTB] bufhdr = %p, bufhdr->pBuffer = %p", buffer, buffer->pBuffer);
    post_event((unsigned) hComp, (unsigned)buffer, m_fill_output_msg);
    return OMX_ErrorNone;
}
/* ======================================================================
FUNCTION
omx_vdec::fill_this_buffer_proxy

DESCRIPTION
IL client uses this method to release the frame buffer
after displaying them.

PARAMETERS
None.

RETURN VALUE
true/false

========================================================================== */
OMX_ERRORTYPE  omx_vdec::fill_this_buffer_proxy(
    OMX_IN OMX_HANDLETYPE        hComp,
    OMX_IN OMX_BUFFERHEADERTYPE* bufferAdd)
{
    OMX_ERRORTYPE nRet = OMX_ErrorNone;
    OMX_BUFFERHEADERTYPE *buffer = bufferAdd;
    unsigned nPortIndex = 0;
    struct vdec_fillbuffer_cmd fillbuffer;
    struct vdec_bufferpayload     *ptr_outputbuffer = NULL;
    struct vdec_output_frameinfo  *ptr_respbuffer = NULL;

    nPortIndex = buffer-((OMX_BUFFERHEADERTYPE *)client_buffers.get_il_buf_hdr());

    if (bufferAdd == NULL || nPortIndex > drv_ctx.op_buf.actualcount)
        return OMX_ErrorBadParameter;

    DEBUG_PRINT_LOW("\n FTBProxy: bufhdr = %p, bufhdr->pBuffer = %p",
        bufferAdd, bufferAdd->pBuffer);
    /*Return back the output buffer to client*/
    if(m_out_bEnabled != OMX_TRUE || output_flush_progress == true)
    {
        DEBUG_PRINT_LOW("\n Output Buffers return flush/disable condition");
        buffer->nFilledLen = 0;
        m_cb.FillBufferDone (hComp,m_app_data,buffer);
        return OMX_ErrorNone;
    }
    pending_output_buffers++;
    buffer = client_buffers.get_dr_buf_hdr(bufferAdd);
    ptr_respbuffer = (struct vdec_output_frameinfo*)buffer->pOutputPortPrivate;
    if (ptr_respbuffer)
    {
        ptr_outputbuffer =  (struct vdec_bufferpayload*)ptr_respbuffer->client_data;
    }

    if (ptr_respbuffer == NULL || ptr_outputbuffer == NULL)
    {
        DEBUG_PRINT_ERROR("resp buffer or outputbuffer is NULL");
        buffer->nFilledLen = 0;
        m_cb.FillBufferDone (hComp,m_app_data,buffer);
        pending_output_buffers--;
        return OMX_ErrorBadParameter;
    }

    if (m_pSwVdec)
    {
        DEBUG_PRINT_HIGH("\nSwVdec_FillThisBuffer idx %d, bufHdr %x", nPortIndex, (unsigned int)bufferAdd);
        if (SWVDEC_S_SUCCESS != SwVdec_FillThisBuffer(m_pSwVdec, &m_pSwVdecOpBuffer[nPortIndex]))
        {
            DEBUG_PRINT_ERROR("\nSwVdec_FillThisBuffer failed");
        }
    }
    else
    {
        int rc = 0;
        struct v4l2_buffer buf;
        struct v4l2_plane plane[VIDEO_MAX_PLANES];
        memset( (void *)&buf, 0, sizeof(buf));
        memset( (void *)plane, 0, (sizeof(struct v4l2_plane)*VIDEO_MAX_PLANES));
        int extra_idx = 0;

        buf.index = nPortIndex;
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_USERPTR;
        plane[0].bytesused = buffer->nFilledLen;
        plane[0].length = drv_ctx.op_buf.buffer_size;
        plane[0].m.userptr =
            (unsigned long)drv_ctx.ptr_outputbuffer[nPortIndex].bufferaddr -
            (unsigned long)drv_ctx.ptr_outputbuffer[nPortIndex].offset;
        plane[0].reserved[0] = drv_ctx.ptr_outputbuffer[nPortIndex].pmem_fd;
        plane[0].reserved[1] = drv_ctx.ptr_outputbuffer[nPortIndex].offset;
        plane[0].data_offset = 0;
        extra_idx = EXTRADATA_IDX(drv_ctx.num_planes);
        if (extra_idx && (extra_idx < VIDEO_MAX_PLANES)) {
            plane[extra_idx].bytesused = 0;
            plane[extra_idx].length = drv_ctx.extradata_info.buffer_size;
            plane[extra_idx].m.userptr = (long unsigned int) (drv_ctx.extradata_info.uaddr + nPortIndex * drv_ctx.extradata_info.buffer_size);
#ifdef USE_ION
            plane[extra_idx].reserved[0] = drv_ctx.extradata_info.ion.fd_ion_data.fd;
#endif
            plane[extra_idx].reserved[1] = nPortIndex * drv_ctx.extradata_info.buffer_size;
            plane[extra_idx].data_offset = 0;
        } else if (extra_idx >= VIDEO_MAX_PLANES) {
            DEBUG_PRINT_ERROR("Extradata index higher than expected: %d\n", extra_idx);
            return OMX_ErrorBadParameter;
        }
        buf.m.planes = plane;
        buf.length = drv_ctx.num_planes;
        rc = ioctl(drv_ctx.video_driver_fd, VIDIOC_QBUF, &buf);
        if (rc) {
            /*TODO: How to handle this case */
            DEBUG_PRINT_ERROR("Failed to qbuf to driver");
        }
    }
    return OMX_ErrorNone;
}

/* ======================================================================
FUNCTION
omx_vdec::SetCallbacks

DESCRIPTION
Set the callbacks.

PARAMETERS
None.

RETURN VALUE
OMX Error None if everything successful.

========================================================================== */
OMX_ERRORTYPE  omx_vdec::set_callbacks(OMX_IN OMX_HANDLETYPE        hComp,
                                       OMX_IN OMX_CALLBACKTYPE* callbacks,
                                       OMX_IN OMX_PTR             appData)
{

    m_cb       = *callbacks;
    DEBUG_PRINT_LOW("\n Callbacks Set %p %p %p",m_cb.EmptyBufferDone,\
        m_cb.EventHandler,m_cb.FillBufferDone);
    m_app_data =    appData;
    return OMX_ErrorNotImplemented;
}

/* ======================================================================
FUNCTION
omx_vdec::ComponentDeInit

DESCRIPTION
Destroys the component and release memory allocated to the heap.

PARAMETERS
<TBD>.

RETURN VALUE
OMX Error None if everything successful.

========================================================================== */
OMX_ERRORTYPE  omx_vdec::component_deinit(OMX_IN OMX_HANDLETYPE hComp)
{
#ifdef _ANDROID_
    if(iDivXDrmDecrypt)
    {
        delete iDivXDrmDecrypt;
        iDivXDrmDecrypt=NULL;
    }
#endif //_ANDROID_

    unsigned i = 0;
    if (OMX_StateLoaded != m_state)
    {
        DEBUG_PRINT_ERROR("WARNING:Rxd DeInit,OMX not in LOADED state %d\n",\
            m_state);
        DEBUG_PRINT_ERROR("\nPlayback Ended - FAILED");
    }
    else
    {
        DEBUG_PRINT_HIGH("\n Playback Ended - PASSED");
    }

    /*Check if the output buffers have to be cleaned up*/
    if(m_out_mem_ptr)
    {
        DEBUG_PRINT_LOW("Freeing the Output Memory\n");
        for (i = 0; i < drv_ctx.op_buf.actualcount; i++ )
        {
            free_output_buffer (&m_out_mem_ptr[i]);
        }
#ifdef _ANDROID_ICS_
        memset(&native_buffer, 0, (sizeof(nativebuffer) * MAX_NUM_INPUT_OUTPUT_BUFFERS));
#endif
    }

    /*Check if the input buffers have to be cleaned up*/
    if(m_inp_mem_ptr || m_inp_heap_ptr)
    {
        DEBUG_PRINT_LOW("Freeing the Input Memory\n");
        for (i = 0; i<drv_ctx.ip_buf.actualcount; i++ )
        {
            if (m_inp_mem_ptr)
                free_input_buffer (i,&m_inp_mem_ptr[i]);
            else
                free_input_buffer (i,NULL);
        }
    }
    free_input_buffer_header();
    free_output_buffer_header();
    if(h264_scratch.pBuffer)
    {
        free(h264_scratch.pBuffer);
        h264_scratch.pBuffer = NULL;
    }

    if (h264_parser)
    {
        delete h264_parser;
        h264_parser = NULL;
    }

    if(m_platform_list)
    {
        free(m_platform_list);
        m_platform_list = NULL;
    }
    if(m_vendor_config.pData)
    {
        free(m_vendor_config.pData);
        m_vendor_config.pData = NULL;
    }

    // Reset counters in mesg queues
    m_ftb_q.m_size=0;
    m_cmd_q.m_size=0;
    m_etb_q.m_size=0;
    m_ftb_q.m_read = m_ftb_q.m_write =0;
    m_cmd_q.m_read = m_cmd_q.m_write =0;
    m_etb_q.m_read = m_etb_q.m_write =0;
    m_ftb_q_dsp.m_size=0;
    m_etb_q_swvdec.m_size=0;
    m_ftb_q_dsp.m_read = m_ftb_q_dsp.m_write =0;
    m_etb_q_swvdec.m_read = m_etb_q_swvdec.m_write =0;
#ifdef _ANDROID_
    if (m_debug_timestamp)
    {
        m_timestamp_list.reset_ts_list();
    }
#endif

    DEBUG_PRINT_LOW("\n Calling VDEC_IOCTL_STOP_NEXT_MSG");
    //(void)ioctl(drv_ctx.video_driver_fd, VDEC_IOCTL_STOP_NEXT_MSG,
    // NULL);
    DEBUG_PRINT_HIGH("\n Close the driver instance");

#ifdef INPUT_BUFFER_LOG
    fclose (inputBufferFile1);
#endif
#ifdef OUTPUT_BUFFER_LOG
    if (outputBufferFile1)
        fclose (outputBufferFile1);
#endif
#ifdef OUTPUT_EXTRADATA_LOG
    fclose (outputExtradataFile);
#endif
    if (m_pSwVdec)
    {
        SwVdec_DeInit(m_pSwVdec);
        m_pSwVdec = NULL;
    }
    DEBUG_PRINT_HIGH("\n omx_vdec::component_deinit() complete");
    return OMX_ErrorNone;
}

/* ======================================================================
FUNCTION
omx_vdec::UseEGLImage

DESCRIPTION
OMX Use EGL Image method implementation <TBD>.

PARAMETERS
<TBD>.

RETURN VALUE
Not Implemented error.

========================================================================== */
OMX_ERRORTYPE  omx_vdec::use_EGL_image(OMX_IN OMX_HANDLETYPE                hComp,
                                       OMX_INOUT OMX_BUFFERHEADERTYPE** bufferHdr,
                                       OMX_IN OMX_U32                        port,
                                       OMX_IN OMX_PTR                     appData,
                                       OMX_IN void*                      eglImage)
{
    OMX_QCOM_PLATFORM_PRIVATE_LIST pmem_list;
    OMX_QCOM_PLATFORM_PRIVATE_ENTRY pmem_entry;
    OMX_QCOM_PLATFORM_PRIVATE_PMEM_INFO pmem_info;

#ifdef USE_EGL_IMAGE_GPU
    PFNEGLQUERYIMAGEQUALCOMMPROC egl_queryfunc;
    EGLint fd = -1, offset = 0,pmemPtr = 0;
#else
    int fd = -1, offset = 0;
#endif
    DEBUG_PRINT_HIGH("\nuse EGL image support for decoder");
    if (!bufferHdr || !eglImage|| port != OMX_CORE_OUTPUT_PORT_INDEX) {
        DEBUG_PRINT_ERROR("\n ");
    }
#ifdef USE_EGL_IMAGE_GPU
    if(m_display_id == NULL) {
        DEBUG_PRINT_ERROR("Display ID is not set by IL client \n");
        return OMX_ErrorInsufficientResources;
    }
    egl_queryfunc = (PFNEGLQUERYIMAGEQUALCOMMPROC)
        eglGetProcAddress("eglQueryImageKHR");
    egl_queryfunc(m_display_id, eglImage, EGL_BUFFER_HANDLE_QCOM,&fd);
    egl_queryfunc(m_display_id, eglImage, EGL_BUFFER_OFFSET_QCOM,&offset);
    egl_queryfunc(m_display_id, eglImage, EGL_BITMAP_POINTER_KHR,&pmemPtr);
#else //with OMX test app
    struct temp_egl {
        int pmem_fd;
        int offset;
    };
    struct temp_egl *temp_egl_id = NULL;
    void * pmemPtr = (void *) eglImage;
    temp_egl_id = (struct temp_egl *)eglImage;
    if (temp_egl_id != NULL)
    {
        fd = temp_egl_id->pmem_fd;
        offset = temp_egl_id->offset;
    }
#endif
    if (fd < 0) {
        DEBUG_PRINT_ERROR("Improper pmem fd by EGL client %d  \n",fd);
        return OMX_ErrorInsufficientResources;
    }
    pmem_info.pmem_fd = (OMX_U32) fd;
    pmem_info.offset = (OMX_U32) offset;
    pmem_entry.entry = (void *) &pmem_info;
    pmem_entry.type = OMX_QCOM_PLATFORM_PRIVATE_PMEM;
    pmem_list.entryList = &pmem_entry;
    pmem_list.nEntries = 1;
    ouput_egl_buffers = true;
    if (OMX_ErrorNone != use_buffer(hComp,bufferHdr, port,
        (void *)&pmem_list, drv_ctx.op_buf.buffer_size,
        (OMX_U8 *)pmemPtr)) {
            DEBUG_PRINT_ERROR("use buffer call failed for egl image\n");
            return OMX_ErrorInsufficientResources;
    }
    return OMX_ErrorNone;
}

/* ======================================================================
FUNCTION
omx_vdec::ComponentRoleEnum

DESCRIPTION
OMX Component Role Enum method implementation.

PARAMETERS
<TBD>.

RETURN VALUE
OMX Error None if everything is successful.
========================================================================== */
OMX_ERRORTYPE  omx_vdec::component_role_enum(OMX_IN OMX_HANDLETYPE hComp,
                                             OMX_OUT OMX_U8*        role,
                                             OMX_IN OMX_U32        index)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;

    if(!strncmp(drv_ctx.kind, "OMX.qcom.video.decoder.hevchybrid",OMX_MAX_STRINGNAME_SIZE) ||
        !strncmp(drv_ctx.kind, "OMX.qcom.video.decoder.hevcswvdec",OMX_MAX_STRINGNAME_SIZE) ||
        !strncmp(drv_ctx.kind, "OMX.qcom.video.decoder.hevc",OMX_MAX_STRINGNAME_SIZE))
    {
        if((0 == index) && role)
        {
            strlcpy((char *)role, "video_decoder.hevc",OMX_MAX_STRINGNAME_SIZE);
            DEBUG_PRINT_LOW("component_role_enum: role %s\n",role);
        }
        else
        {
            DEBUG_PRINT_LOW("\n No more roles \n");
            eRet = OMX_ErrorNoMore;
        }
    }
    else
    {
        DEBUG_PRINT_ERROR("\nERROR:Querying Role on Unknown Component\n");
        eRet = OMX_ErrorInvalidComponentName;
    }
    return eRet;
}




/* ======================================================================
FUNCTION
omx_vdec::AllocateDone

DESCRIPTION
Checks if entire buffer pool is allocated by IL Client or not.
Need this to move to IDLE state.

PARAMETERS
None.

RETURN VALUE
true/false.

========================================================================== */
bool omx_vdec::allocate_done(void)
{
    bool bRet = false;
    bool bRet_In = false;
    bool bRet_Out = false;

    bRet_In = allocate_input_done();
    bRet_Out = allocate_output_done();

    if(bRet_In && bRet_Out)
    {
        bRet = true;
        if (m_pSwVdec && m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY &&
            allocate_interm_buffer(drv_ctx.interm_op_buf.buffer_size) != OMX_ErrorNone)
        {
            bRet = false;
        }
    }

    return bRet;
}
/* ======================================================================
FUNCTION
omx_vdec::AllocateInputDone

DESCRIPTION
Checks if I/P buffer pool is allocated by IL Client or not.

PARAMETERS
None.

RETURN VALUE
true/false.

========================================================================== */
bool omx_vdec::allocate_input_done(void)
{
    bool bRet = false;
    unsigned i=0;

    if (m_inp_mem_ptr == NULL)
    {
        return bRet;
    }
    if(m_inp_mem_ptr )
    {
        for(;i<drv_ctx.ip_buf.actualcount;i++)
        {
            if(BITMASK_ABSENT(&m_inp_bm_count,i))
            {
                break;
            }
        }
    }
    if(i == drv_ctx.ip_buf.actualcount)
    {
        bRet = true;
        DEBUG_PRINT_HIGH("Allocate done for all i/p buffers");
    }
    if(i==drv_ctx.ip_buf.actualcount && m_inp_bEnabled)
    {
        m_inp_bPopulated = OMX_TRUE;
    }
    return bRet;
}
/* ======================================================================
FUNCTION
omx_vdec::AllocateOutputDone

DESCRIPTION
Checks if entire O/P buffer pool is allocated by IL Client or not.

PARAMETERS
None.

RETURN VALUE
true/false.

========================================================================== */
bool omx_vdec::allocate_output_done(void)
{
    bool bRet = false;
    unsigned j=0;

    if (m_out_mem_ptr == NULL)
    {
        return bRet;
    }

    if (m_out_mem_ptr)
    {
        for(;j < drv_ctx.op_buf.actualcount;j++)
        {
            if(BITMASK_ABSENT(&m_out_bm_count,j))
            {
                break;
            }
        }
    }

    if(j == drv_ctx.op_buf.actualcount)
    {
        bRet = true;
        DEBUG_PRINT_HIGH("Allocate done for all o/p buffers");
        if(m_out_bEnabled)
            m_out_bPopulated = OMX_TRUE;
    }

    return bRet;
}

/* ======================================================================
FUNCTION
omx_vdec::ReleaseDone

DESCRIPTION
Checks if IL client has released all the buffers.

PARAMETERS
None.

RETURN VALUE
true/false

========================================================================== */
bool omx_vdec::release_done(void)
{
    bool bRet = false;

    if(release_input_done())
    {
        if(release_output_done())
        {
            bRet = true;
        }
    }

    if (bRet && m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY)
    {
        bRet = release_interm_done();
    }
    return bRet;
}

bool omx_vdec::release_interm_done(void)
{
    bool bRet = true;
    unsigned int i=0;

    if (!drv_ctx.ptr_interm_outputbuffer) return bRet;

    pthread_mutex_lock(&m_lock);
    for(;i < 32  && drv_ctx.ptr_interm_outputbuffer[i].pmem_fd ; i++)
    {
        if(m_interm_buf_state[i] != WITH_COMPONENT)
        {
            bRet = false;
            break;
        }
    }
    pthread_mutex_unlock(&m_lock);

    DEBUG_PRINT_LOW("\n release_interm_done %d",bRet);
    return bRet;
}


/* ======================================================================
FUNCTION
omx_vdec::ReleaseOutputDone

DESCRIPTION
Checks if IL client has released all the buffers.

PARAMETERS
None.

RETURN VALUE
true/false

========================================================================== */
bool omx_vdec::release_output_done(void)
{
    bool bRet = false;
    unsigned i=0,j=0;

    DEBUG_PRINT_LOW("\n Value of m_out_mem_ptr %p",m_out_mem_ptr);
    if(m_out_mem_ptr)
    {
        for(;j < drv_ctx.op_buf.actualcount ; j++)
        {
            if(BITMASK_PRESENT(&m_out_bm_count,j))
            {
                break;
            }
        }
        if(j == drv_ctx.op_buf.actualcount)
        {
            m_out_bm_count = 0;
            bRet = true;
        }
    }
    else
    {
        m_out_bm_count = 0;
        bRet = true;
    }
    return bRet;
}
/* ======================================================================
FUNCTION
omx_vdec::ReleaseInputDone

DESCRIPTION
Checks if IL client has released all the buffers.

PARAMETERS
None.

RETURN VALUE
true/false

========================================================================== */
bool omx_vdec::release_input_done(void)
{
    bool bRet = false;
    unsigned i=0,j=0;

    DEBUG_PRINT_LOW("\n Value of m_inp_mem_ptr %p",m_inp_mem_ptr);
    if(m_inp_mem_ptr)
    {
        for(;j<drv_ctx.ip_buf.actualcount;j++)
        {
            if( BITMASK_PRESENT(&m_inp_bm_count,j))
            {
                break;
            }
        }
        if(j==drv_ctx.ip_buf.actualcount)
        {
            bRet = true;
        }
    }
    else
    {
        bRet = true;
    }
    return bRet;
}

OMX_ERRORTYPE omx_vdec::fill_buffer_done(OMX_HANDLETYPE hComp,
                                         OMX_BUFFERHEADERTYPE * buffer)
{
    OMX_QCOM_PLATFORM_PRIVATE_PMEM_INFO *pPMEMInfo = NULL;
    if (!buffer || (buffer - m_out_mem_ptr) >= (int)drv_ctx.op_buf.actualcount)
    {
        DEBUG_PRINT_ERROR("\n [FBD] ERROR in ptr(%p)", buffer);
        return OMX_ErrorBadParameter;
    }
    else if (output_flush_progress)
    {
        DEBUG_PRINT_LOW("FBD: Buffer (%p) flushed", buffer);
        buffer->nFilledLen = 0;
        buffer->nTimeStamp = 0;
        buffer->nFlags &= ~OMX_BUFFERFLAG_EXTRADATA;
        buffer->nFlags &= ~QOMX_VIDEO_BUFFERFLAG_EOSEQ;
        buffer->nFlags &= ~OMX_BUFFERFLAG_DATACORRUPT;
    }

    DEBUG_PRINT_LOW("\n fill_buffer_done: bufhdr = %p, bufhdr->pBuffer = %p idx %d, TS %lld %d",
        buffer, buffer->pBuffer, buffer - m_out_mem_ptr, buffer->nTimeStamp );
    pending_output_buffers --;

    if (buffer->nFlags & OMX_BUFFERFLAG_EOS)
    {
        DEBUG_PRINT_HIGH("\n Output EOS has been reached");
        if (!output_flush_progress)
            post_event((unsigned)NULL, (unsigned)NULL,
            OMX_COMPONENT_GENERATE_EOS_DONE);

        if (psource_frame)
        {
            m_cb.EmptyBufferDone(&m_cmp, m_app_data, psource_frame);
            psource_frame = NULL;
        }
        if (pdest_frame)
        {
            pdest_frame->nFilledLen = 0;
            m_input_free_q.insert_entry((unsigned) pdest_frame,(unsigned)NULL,
                (unsigned)NULL);
            pdest_frame = NULL;
        }
    }

#ifdef OUTPUT_BUFFER_LOG
    if (outputBufferFile1 && buffer->nFilledLen)
    {
        int stride = drv_ctx.video_resolution.stride;
        int scanlines = drv_ctx.video_resolution.scan_lines;
        char *temp = (char *)buffer->pBuffer;
        int i;
        int bytes_written = 0;
        for (i = 0; i < drv_ctx.video_resolution.frame_height; i++) {
            bytes_written = fwrite(temp, drv_ctx.video_resolution.frame_width, 1, outputBufferFile1);
            temp += stride;
        }
        temp = (char *)buffer->pBuffer + stride * scanlines;
        int stride_c = stride;
        for(i = 0; i < drv_ctx.video_resolution.frame_height/2; i++) {
            bytes_written += fwrite(temp, drv_ctx.video_resolution.frame_width, 1, outputBufferFile1);
            temp += stride_c;
        }
    }
#endif

    /* For use buffer we need to copy the data */
    if (!output_flush_progress)
    {
        time_stamp_dts.get_next_timestamp(buffer,
            (drv_ctx.interlace != VDEC_InterlaceFrameProgressive)
            ?true:false);
        if (m_debug_timestamp)
        {
            {
                OMX_TICKS expected_ts = 0;
                m_timestamp_list.pop_min_ts(expected_ts);
                DEBUG_PRINT_LOW("\n Current timestamp (%lld),Popped TIMESTAMP (%lld) from list",
                    buffer->nTimeStamp, expected_ts);

                if (buffer->nTimeStamp != expected_ts)
                {
                    DEBUG_PRINT_ERROR("\n ERROR in omx_vdec::async_message_process timestamp Check");
                }
            }
        }
    }
    if (m_cb.FillBufferDone)
    {
        if (buffer->nFilledLen > 0)
        {
            handle_extradata(buffer);
            if (client_extradata & OMX_TIMEINFO_EXTRADATA)
                // Keep min timestamp interval to handle corrupted bit stream scenario
                set_frame_rate(buffer->nTimeStamp);
            else if (arbitrary_bytes)
                adjust_timestamp(buffer->nTimeStamp);
            if (perf_flag)
            {
                if (!proc_frms)
                {
                    dec_time.stop();
                    latency = dec_time.processing_time_us() - latency;
                    DEBUG_PRINT_HIGH(">>> FBD Metrics: Latency(%.2f)mS", latency / 1e3);
                    dec_time.start();
                    fps_metrics.start();
                }
                proc_frms++;
                if (buffer->nFlags & OMX_BUFFERFLAG_EOS)
                {
                    OMX_U64 proc_time = 0;
                    fps_metrics.stop();
                    proc_time = fps_metrics.processing_time_us();
                    DEBUG_PRINT_HIGH(">>> FBD Metrics: proc_frms(%lu) proc_time(%.2f)S fps(%.2f)",
                        proc_frms, (float)proc_time / 1e6,
                        (float)(1e6 * proc_frms) / proc_time);
                    proc_frms = 0;
                }
            }

#ifdef OUTPUT_EXTRADATA_LOG
            if (outputExtradataFile)
            {

                OMX_OTHER_EXTRADATATYPE *p_extra = NULL;
                p_extra = (OMX_OTHER_EXTRADATATYPE *)
                    ((unsigned)(buffer->pBuffer + buffer->nOffset +
                    buffer->nFilledLen + 3)&(~3));
                while(p_extra &&
                    (OMX_U8*)p_extra < (buffer->pBuffer + buffer->nAllocLen) )
                {
                    DEBUG_PRINT_LOW("\nWRITING extradata, size=%d,type=%d",p_extra->nSize, p_extra->eType);
                    fwrite (p_extra,1,p_extra->nSize,outputExtradataFile);
                    if (p_extra->eType == OMX_ExtraDataNone)
                    {
                        break;
                    }
                    p_extra = (OMX_OTHER_EXTRADATATYPE *) (((OMX_U8 *) p_extra) + p_extra->nSize);
                }
            }
#endif
        }
        if (buffer->nFlags & OMX_BUFFERFLAG_EOS){
            prev_ts = LLONG_MAX;
            rst_prev_ts = true;
        }

        pPMEMInfo = (OMX_QCOM_PLATFORM_PRIVATE_PMEM_INFO *)
            ((OMX_QCOM_PLATFORM_PRIVATE_LIST *)
            buffer->pPlatformPrivate)->entryList->entry;
        DEBUG_PRINT_LOW("\n Before FBD callback Accessed Pmeminfo %lu", pPMEMInfo->pmem_fd);
        OMX_BUFFERHEADERTYPE *il_buffer;
        il_buffer = client_buffers.get_il_buf_hdr(buffer);
        if (il_buffer)
            m_cb.FillBufferDone (hComp,m_app_data,il_buffer);
        else {
            DEBUG_PRINT_ERROR("Invalid buffer address from get_il_buf_hdr");
            return OMX_ErrorBadParameter;
        }
        DEBUG_PRINT_LOW("\n After Fill Buffer Done callback %lu",pPMEMInfo->pmem_fd);
    }
    else
    {
        return OMX_ErrorBadParameter;
    }

    return OMX_ErrorNone;
}

OMX_ERRORTYPE omx_vdec::empty_buffer_done(OMX_HANDLETYPE         hComp,
                                          OMX_BUFFERHEADERTYPE* buffer)
{

    if (buffer == NULL || ((buffer - m_inp_mem_ptr) > (int)drv_ctx.ip_buf.actualcount))
    {
        DEBUG_PRINT_ERROR("\n empty_buffer_done: ERROR bufhdr = %p", buffer);
        return OMX_ErrorBadParameter;
    }

    DEBUG_PRINT_LOW("\n empty_buffer_done: bufhdr = %p, bufhdr->pBuffer = %p",
        buffer, buffer->pBuffer);
    pending_input_buffers--;

    if (arbitrary_bytes)
    {
        if (pdest_frame == NULL && input_flush_progress == false)
        {
            DEBUG_PRINT_LOW("\n Push input from buffer done address of Buffer %p",buffer);
            pdest_frame = buffer;
            buffer->nFilledLen = 0;
            buffer->nTimeStamp = LLONG_MAX;
            push_input_buffer (hComp);
        }
        else
        {
            DEBUG_PRINT_LOW("\n Push buffer into freeq address of Buffer %p",buffer);
            buffer->nFilledLen = 0;
            if (!m_input_free_q.insert_entry((unsigned)buffer,
                (unsigned)NULL, (unsigned)NULL))
            {
                DEBUG_PRINT_ERROR("\nERROR:i/p free Queue is FULL Error");
            }
        }
    }
    else if(m_cb.EmptyBufferDone)
    {
        buffer->nFilledLen = 0;
        if (input_use_buffer == true){
            buffer = &m_inp_heap_ptr[buffer-m_inp_mem_ptr];
        }
        m_cb.EmptyBufferDone(hComp ,m_app_data, buffer);
    }
    return OMX_ErrorNone;
}


void dump_buffer(FILE* pFile, char* buffer, int stride, int scanlines, int width, int height)
{
    if (buffer)
    {
        char *temp = (char *)buffer;
        int i;
        int bytes_written = 0;
        int bytes = 0;

        for (i = 0; i < height; i++) {
            bytes_written = fwrite(temp, width, 1, pFile);
            temp += stride;
            if (bytes_written >0)
                bytes += bytes_written * width;
        }
        temp = (char *)buffer + stride * scanlines;
        int stride_c = stride;
        for(i = 0; i < height/2; i++) {
            bytes_written = fwrite(temp, width, 1, pFile);
            temp += stride_c;
            if (bytes_written >0)
                bytes += bytes_written * width;
        }

        DEBUG_PRINT_ERROR("stride %d, scanlines %d, frame_height %d bytes_written %d \n",
            stride, scanlines, height, bytes);
    }
}

int omx_vdec::async_message_process (void *context, void* message)
{
    omx_vdec* omx = NULL;
    struct vdec_msginfo *vdec_msg = NULL;
    OMX_BUFFERHEADERTYPE* omxhdr = NULL;
    struct v4l2_buffer *v4l2_buf_ptr = NULL;
    struct vdec_output_frameinfo *output_respbuf = NULL;
    int rc=1;
    if (context == NULL || message == NULL)
    {
        DEBUG_PRINT_ERROR("\n FATAL ERROR in omx_vdec::async_message_process NULL Check");
        return -1;
    }
    vdec_msg = (struct vdec_msginfo *)message;

    omx = reinterpret_cast<omx_vdec*>(context);

    switch (vdec_msg->msgcode)
    {

    case VDEC_MSG_EVT_HW_ERROR:
        omx->post_event ((unsigned)NULL, vdec_msg->status_code,\
            OMX_COMPONENT_GENERATE_HARDWARE_ERROR);
        break;

    case VDEC_MSG_RESP_START_DONE:
        omx->post_event ((unsigned)NULL, vdec_msg->status_code,\
            OMX_COMPONENT_GENERATE_START_DONE);
        break;

    case VDEC_MSG_RESP_STOP_DONE:
        omx->post_event ((unsigned)NULL, vdec_msg->status_code,\
            OMX_COMPONENT_GENERATE_STOP_DONE);
        break;

    case VDEC_MSG_RESP_RESUME_DONE:
        omx->post_event ((unsigned)NULL, vdec_msg->status_code,\
            OMX_COMPONENT_GENERATE_RESUME_DONE);
        break;

    case VDEC_MSG_RESP_PAUSE_DONE:
        omx->post_event ((unsigned)NULL, vdec_msg->status_code,\
            OMX_COMPONENT_GENERATE_PAUSE_DONE);
        break;

    case VDEC_MSG_RESP_FLUSH_INPUT_DONE:
        omx->post_event ((unsigned)NULL, vdec_msg->status_code,\
            OMX_COMPONENT_GENERATE_EVENT_INPUT_FLUSH);
        break;
    case VDEC_MSG_RESP_FLUSH_OUTPUT_DONE:
        if (!omx->m_pSwVdec)
        {
            omx->post_event ((unsigned)NULL, vdec_msg->status_code,\
                OMX_COMPONENT_GENERATE_EVENT_OUTPUT_FLUSH);
        }
        else
        {
            omx->post_event ((unsigned)NULL, vdec_msg->status_code,\
                OMX_COMPONENT_GENERATE_EVENT_OUTPUT_FLUSH_DSP);
        }
        break;
    case VDEC_MSG_RESP_INPUT_FLUSHED:
    case VDEC_MSG_RESP_INPUT_BUFFER_DONE:

        /* omxhdr = (OMX_BUFFERHEADERTYPE* )
        vdec_msg->msgdata.input_frame_clientdata; */

        v4l2_buf_ptr = (v4l2_buffer*)vdec_msg->msgdata.input_frame_clientdata;
        omxhdr=omx->m_inp_mem_ptr+v4l2_buf_ptr->index;
        if (omxhdr == NULL ||
            ((omxhdr - omx->m_inp_mem_ptr) > (int)omx->drv_ctx.ip_buf.actualcount) )
        {
            omxhdr = NULL;
            vdec_msg->status_code = VDEC_S_EFATAL;
        }

        omx->post_event ((unsigned int)omxhdr,vdec_msg->status_code,
            OMX_COMPONENT_GENERATE_EBD);
        break;
    case VDEC_MSG_EVT_INFO_FIELD_DROPPED:
        int64_t *timestamp;
        timestamp = (int64_t *) malloc(sizeof(int64_t));
        if (timestamp) {
            *timestamp = vdec_msg->msgdata.output_frame.time_stamp;
            omx->post_event ((unsigned int)timestamp, vdec_msg->status_code,
                OMX_COMPONENT_GENERATE_INFO_FIELD_DROPPED);
            DEBUG_PRINT_HIGH("\nField dropped time stamp is %lld",
                vdec_msg->msgdata.output_frame.time_stamp);
        }
        break;

    case VDEC_MSG_RESP_OUTPUT_FLUSHED:
    case VDEC_MSG_RESP_OUTPUT_BUFFER_DONE:
        {
            int actualcount = omx->drv_ctx.op_buf.actualcount;
            OMX_BUFFERHEADERTYPE* p_mem_ptr  = omx->m_out_mem_ptr;
            vdec_output_frameinfo* ptr_respbuffer = omx->drv_ctx.ptr_respbuffer;
            if (omx->m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY)
            {
                actualcount = omx->drv_ctx.interm_op_buf.actualcount;
                p_mem_ptr  = omx->m_interm_mem_ptr;
                ptr_respbuffer = omx->drv_ctx.ptr_interm_respbuffer;
            }
            v4l2_buf_ptr = (v4l2_buffer*)vdec_msg->msgdata.output_frame.client_data;
            omxhdr=p_mem_ptr+v4l2_buf_ptr->index;
            DEBUG_PRINT_LOW("[RespBufDone] Buf(%p) pBuffer (%p) idx %d Ts(%lld) Pic_type(%u)",
                omxhdr, omxhdr->pBuffer, v4l2_buf_ptr->index, vdec_msg->msgdata.output_frame.time_stamp,
                vdec_msg->msgdata.output_frame.pic_type);

            if (omxhdr && omxhdr->pOutputPortPrivate &&
                ((omxhdr - p_mem_ptr) < actualcount) &&
                (((struct vdec_output_frameinfo *)omxhdr->pOutputPortPrivate
                - ptr_respbuffer) < actualcount))
            {
                if ( vdec_msg->msgdata.output_frame.len <=  omxhdr->nAllocLen)
                {
                    omxhdr->nFilledLen = vdec_msg->msgdata.output_frame.len;
                    omxhdr->nOffset = vdec_msg->msgdata.output_frame.offset;
                    omxhdr->nTimeStamp = vdec_msg->msgdata.output_frame.time_stamp;
                    omxhdr->nFlags = omx->m_out_mem_ptr[v4l2_buf_ptr->index].nFlags;

                    if (v4l2_buf_ptr->flags & V4L2_QCOM_BUF_FLAG_EOS)
                    {
                        omxhdr->nFlags |= OMX_BUFFERFLAG_EOS;
                        //rc = -1;
                    }
                    if (v4l2_buf_ptr->flags & V4L2_QCOM_BUF_FLAG_EOSEQ)
                    {
                        omxhdr->nFlags |= QOMX_VIDEO_BUFFERFLAG_EOSEQ;
                    }
                    vdec_msg->msgdata.output_frame.bufferaddr =
                        omx->drv_ctx.ptr_outputbuffer[v4l2_buf_ptr->index].bufferaddr;
                    if (omxhdr->nFilledLen && (((unsigned)omx->rectangle.nLeft !=
                        vdec_msg->msgdata.output_frame.framesize.left)
                        || ((unsigned)omx->rectangle.nTop != vdec_msg->msgdata.output_frame.framesize.top)
                        || (omx->rectangle.nWidth != vdec_msg->msgdata.output_frame.framesize.right)
                        || (omx->rectangle.nHeight != vdec_msg->msgdata.output_frame.framesize.bottom))) {
                            omx->rectangle.nLeft = vdec_msg->msgdata.output_frame.framesize.left;
                            omx->rectangle.nTop = vdec_msg->msgdata.output_frame.framesize.top;
                            omx->rectangle.nWidth = vdec_msg->msgdata.output_frame.framesize.right;
                            omx->rectangle.nHeight = vdec_msg->msgdata.output_frame.framesize.bottom;
                            DEBUG_PRINT_HIGH("\n Crop information has changed\n");
                            if (!omx->m_pSwVdec)
                            {
                                omx->post_event (OMX_CORE_OUTPUT_PORT_INDEX, OMX_IndexConfigCommonOutputCrop,
                                    OMX_COMPONENT_GENERATE_PORT_RECONFIG);
                            }
                    }
                    output_respbuf = (struct vdec_output_frameinfo *)\
                        omxhdr->pOutputPortPrivate;
                    output_respbuf->len = vdec_msg->msgdata.output_frame.len;
                    output_respbuf->offset = vdec_msg->msgdata.output_frame.offset;
                    if (v4l2_buf_ptr->flags & V4L2_BUF_FLAG_KEYFRAME)
                    {
                        output_respbuf->pic_type = PICTURE_TYPE_I;
                    }
                    if (v4l2_buf_ptr->flags & V4L2_BUF_FLAG_PFRAME)
                    {
                        output_respbuf->pic_type = PICTURE_TYPE_P;
                    }
                    if (v4l2_buf_ptr->flags & V4L2_BUF_FLAG_BFRAME) {
                        output_respbuf->pic_type = PICTURE_TYPE_B;
                    }

                    if (omx->output_use_buffer)
                        memcpy ( omxhdr->pBuffer, (void *)
                        ((unsigned long)vdec_msg->msgdata.output_frame.bufferaddr +
                        (unsigned long)vdec_msg->msgdata.output_frame.offset),
                        vdec_msg->msgdata.output_frame.len);
                }
                else
                    omxhdr->nFilledLen = 0;
                if (!omx->m_pSwVdec)
                {
                    omx->post_event ((unsigned int)omxhdr, vdec_msg->status_code,
                        OMX_COMPONENT_GENERATE_FBD);
                }
                else
                {
                    omx->post_event ((unsigned int)omxhdr, vdec_msg->status_code,
                        OMX_COMPONENT_GENERATE_FBD_DSP);
                }
            }
            else if (vdec_msg->msgdata.output_frame.flags & OMX_BUFFERFLAG_EOS)
                omx->post_event ((unsigned int)NULL, vdec_msg->status_code,
                OMX_COMPONENT_GENERATE_EOS_DONE);
            else
                omx->post_event ((unsigned int)NULL, vdec_msg->status_code,
                OMX_COMPONENT_GENERATE_HARDWARE_ERROR);
        }
        break;
    case VDEC_MSG_EVT_CONFIG_CHANGED:
        if (!omx->m_pSwVdec)
        {
            DEBUG_PRINT_HIGH("\n Port settings changed");
            omx->post_event (OMX_CORE_OUTPUT_PORT_INDEX, OMX_IndexParamPortDefinition,
                OMX_COMPONENT_GENERATE_PORT_RECONFIG);
        }
        break;
    case VDEC_MSG_EVT_INFO_CONFIG_CHANGED:
        {
            DEBUG_PRINT_HIGH("\n Port settings changed info");
            // get_buffer_req and populate port defn structure
            OMX_ERRORTYPE eRet = OMX_ErrorNone;
            struct v4l2_format fmt;
            int ret;
            fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            ret = ioctl(omx->drv_ctx.video_driver_fd, VIDIOC_G_FMT, &fmt);
            omx->update_resolution(fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height);
            omx->drv_ctx.video_resolution.stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
            omx->drv_ctx.video_resolution.scan_lines = fmt.fmt.pix_mp.plane_fmt[0].reserved[0];
            omx->m_port_def.nPortIndex = 1;
            eRet = omx->update_portdef(&(omx->m_port_def));
            omx->post_event ((unsigned)NULL, vdec_msg->status_code,\
                OMX_COMPONENT_GENERATE_INFO_PORT_RECONFIG);
            break;
        }
    default:
        break;
    }
    return rc;
}

OMX_ERRORTYPE omx_vdec::empty_this_buffer_proxy_arbitrary (
    OMX_HANDLETYPE hComp,
    OMX_BUFFERHEADERTYPE *buffer
    )
{
    unsigned address,p2,id;
    DEBUG_PRINT_LOW("\n Empty this arbitrary");

    if (buffer == NULL)
    {
        return OMX_ErrorBadParameter;
    }
    DEBUG_PRINT_LOW("\n ETBProxyArb: bufhdr = %p, bufhdr->pBuffer = %p", buffer, buffer->pBuffer);
    DEBUG_PRINT_LOW("\n ETBProxyArb: nFilledLen %lu, flags %lu, timestamp %u",
        buffer->nFilledLen, buffer->nFlags, (unsigned)buffer->nTimeStamp);

    /* return zero length and not an EOS buffer */
    /* return buffer if input flush in progress */
    if ((input_flush_progress == true) || ((buffer->nFilledLen == 0) &&
        ((buffer->nFlags & OMX_BUFFERFLAG_EOS) == 0)))
    {
        DEBUG_PRINT_HIGH("\n return zero legth buffer or flush in progress");
        m_cb.EmptyBufferDone (hComp,m_app_data,buffer);
        return OMX_ErrorNone;
    }

    if (psource_frame == NULL)
    {
        DEBUG_PRINT_LOW("\n Set Buffer as source Buffer %p time stamp %lld",buffer,buffer->nTimeStamp);
        psource_frame = buffer;
        DEBUG_PRINT_LOW("\n Try to Push One Input Buffer ");
        push_input_buffer (hComp);
    }
    else
    {
        DEBUG_PRINT_LOW("\n Push the source buffer into pendingq %p",buffer);
        if (!m_input_pending_q.insert_entry((unsigned)buffer, (unsigned)NULL,
            (unsigned)NULL))
        {
            return OMX_ErrorBadParameter;
        }
    }


    return OMX_ErrorNone;
}

OMX_ERRORTYPE omx_vdec::push_input_buffer (OMX_HANDLETYPE hComp)
{
    unsigned address,p2,id;
    OMX_ERRORTYPE ret = OMX_ErrorNone;

    if (pdest_frame == NULL || psource_frame == NULL)
    {
        /*Check if we have a destination buffer*/
        if (pdest_frame == NULL)
        {
            DEBUG_PRINT_LOW("\n Get a Destination buffer from the queue");
            if (m_input_free_q.m_size)
            {
                m_input_free_q.pop_entry(&address,&p2,&id);
                pdest_frame = (OMX_BUFFERHEADERTYPE *)address;
                pdest_frame->nFilledLen = 0;
                pdest_frame->nTimeStamp = LLONG_MAX;
                DEBUG_PRINT_LOW("\n Address of Pmem Buffer %p",pdest_frame);
            }
        }

        /*Check if we have a destination buffer*/
        if (psource_frame == NULL)
        {
            DEBUG_PRINT_LOW("\n Get a source buffer from the queue");
            if (m_input_pending_q.m_size)
            {
                m_input_pending_q.pop_entry(&address,&p2,&id);
                psource_frame = (OMX_BUFFERHEADERTYPE *)address;
                DEBUG_PRINT_LOW("\n Next source Buffer %p time stamp %lld",psource_frame,
                    psource_frame->nTimeStamp);
                DEBUG_PRINT_LOW("\n Next source Buffer flag %lu length %lu",
                    psource_frame->nFlags,psource_frame->nFilledLen);

            }
        }

    }

    while ((pdest_frame != NULL) && (psource_frame != NULL))
    {
        switch (codec_type_parse)
        {
        case CODEC_TYPE_HEVC:
            ret = push_input_hevc(hComp);
            break;
        default:
            break;
        }
        if (ret != OMX_ErrorNone)
        {
            DEBUG_PRINT_ERROR("\n Pushing input Buffer Failed");
            omx_report_error ();
            break;
        }
    }

    return ret;
}

OMX_ERRORTYPE copy_buffer(OMX_BUFFERHEADERTYPE* pDst, OMX_BUFFERHEADERTYPE* pSrc)
{
    OMX_ERRORTYPE rc = OMX_ErrorNone;
    if ((pDst->nAllocLen - pDst->nFilledLen) >= pSrc->nFilledLen)
    {
        memcpy ((pDst->pBuffer + pDst->nFilledLen), pSrc->pBuffer, pSrc->nFilledLen);
        if (pDst->nTimeStamp == LLONG_MAX)
        {
            pDst->nTimeStamp = pSrc->nTimeStamp;
            DEBUG_PRINT_LOW("Assign Dst nTimeStamp=%lld", pDst->nTimeStamp);
        }
        pDst->nFilledLen += pSrc->nFilledLen;
        pSrc->nFilledLen = 0;
    }
    else
    {
        DEBUG_PRINT_ERROR("Error: Destination buffer overflow\n");
        rc = OMX_ErrorBadParameter;
    }
    return rc;
}

OMX_ERRORTYPE omx_vdec::push_input_hevc (OMX_HANDLETYPE hComp)
{
    OMX_U32 partial_frame = 1;
    unsigned address,p2,id;
    OMX_BOOL isNewFrame = OMX_FALSE;
    OMX_BOOL generate_ebd = OMX_TRUE;
    OMX_ERRORTYPE rc = OMX_ErrorNone;

    if (h264_scratch.pBuffer == NULL)
    {
        DEBUG_PRINT_ERROR("\nERROR:Hevc Scratch Buffer not allocated");
        return OMX_ErrorBadParameter;
    }


    DEBUG_PRINT_LOW("h264_scratch.nFilledLen %lu has look_ahead_nal %d pdest_frame nFilledLen %lu nTimeStamp %lld\n",
        h264_scratch.nFilledLen, look_ahead_nal, pdest_frame->nFilledLen, pdest_frame->nTimeStamp);

    if (h264_scratch.nFilledLen && look_ahead_nal)
    {
        look_ahead_nal = false;

        // copy the lookahead buffer in the scratch
        rc = copy_buffer(pdest_frame, &h264_scratch);
        if (rc != OMX_ErrorNone)
        {
            return rc;
        }
    }
    if (nal_length == 0)
    {
        if (m_frame_parser.parse_sc_frame(psource_frame,
            &h264_scratch,&partial_frame) == -1)
        {
            DEBUG_PRINT_ERROR("\n Error In Parsing Return Error");
            return OMX_ErrorBadParameter;
        }
    }
    else
    {
        DEBUG_PRINT_LOW("Non-zero NAL length clip, hence parse with NAL size %d\n",nal_length);
        if (m_frame_parser.parse_h264_nallength(psource_frame,
            &h264_scratch,&partial_frame) == -1)
        {
            DEBUG_PRINT_ERROR("\n Error In Parsing NAL size, Return Error");
            return OMX_ErrorBadParameter;
        }
    }

    if (partial_frame == 0)
    {
        if (nal_count == 0 && h264_scratch.nFilledLen == 0)
        {
            DEBUG_PRINT_LOW("\n First NAL with Zero Length, hence Skip");
            nal_count++;
            h264_scratch.nTimeStamp = psource_frame->nTimeStamp;
            h264_scratch.nFlags = psource_frame->nFlags;
        }
        else
        {
            DEBUG_PRINT_LOW("\n Parsed New NAL Length = %lu",h264_scratch.nFilledLen);
            if(h264_scratch.nFilledLen)
            {
                mHEVCutils.isNewFrame(&h264_scratch, 0, isNewFrame);
                nal_count++;
            }

            if (!isNewFrame)
            {
                DEBUG_PRINT_LOW("Not a new frame, copy h264_scratch nFilledLen %lu nTimestamp %lld, pdest_frame nFilledLen %lu nTimestamp %lld\n",
                    h264_scratch.nFilledLen, h264_scratch.nTimeStamp, pdest_frame->nFilledLen, pdest_frame->nTimeStamp);
                rc = copy_buffer(pdest_frame, &h264_scratch);
                if ( rc != OMX_ErrorNone)
                {
                    return rc;
                }
            }
            else
            {
                look_ahead_nal = true;
                if (pdest_frame->nFilledLen == 0)
                {
                    look_ahead_nal = false;
                    DEBUG_PRINT_LOW("dest nation buffer empty, copy scratch buffer\n");
                    rc = copy_buffer(pdest_frame, &h264_scratch);
                    if ( rc != OMX_ErrorNone )
                    {
                        return OMX_ErrorBadParameter;
                    }
                }
                else
                {
                    if(psource_frame->nFilledLen || h264_scratch.nFilledLen)
                    {
                        pdest_frame->nFlags &= ~OMX_BUFFERFLAG_EOS;
                    }

                    DEBUG_PRINT_LOW("FrameDetecetd # %d pdest_frame nFilledLen %lu nTimeStamp %lld, look_ahead_nal in h264_scratch nFilledLen %lu nTimeStamp %lld \n",
                        frame_count++, pdest_frame->nFilledLen, pdest_frame->nTimeStamp, h264_scratch.nFilledLen, h264_scratch.nTimeStamp);
                    if (empty_this_buffer_proxy(hComp,pdest_frame) != OMX_ErrorNone)
                    {
                        return OMX_ErrorBadParameter;
                    }
                    pdest_frame = NULL;
                    if (m_input_free_q.m_size)
                    {
                        m_input_free_q.pop_entry(&address,&p2,&id);
                        pdest_frame = (OMX_BUFFERHEADERTYPE *) address;
                        DEBUG_PRINT_LOW("pop the next pdest_buffer %p\n",pdest_frame);
                        pdest_frame->nFilledLen = 0;
                        pdest_frame->nFlags = 0;
                        pdest_frame->nTimeStamp = LLONG_MAX;
                    }
                }
            }
        }
    }
    else
    {
        DEBUG_PRINT_LOW("psource_frame is partial nFilledLen %lu nTimeStamp %lld, pdest_frame nFilledLen %lu nTimeStamp %lld, h264_scratch nFilledLen %lu nTimeStamp %lld \n",
            psource_frame->nFilledLen, psource_frame->nTimeStamp, pdest_frame->nFilledLen, pdest_frame->nTimeStamp, h264_scratch.nFilledLen, h264_scratch.nTimeStamp);

        /*Check if Destination Buffer is full*/
        if (h264_scratch.nAllocLen ==
            h264_scratch.nFilledLen + h264_scratch.nOffset)
        {
            DEBUG_PRINT_ERROR("\nERROR: Frame Not found though Destination Filled");
            return OMX_ErrorStreamCorrupt;
        }
    }

    if (!psource_frame->nFilledLen)
    {
        DEBUG_PRINT_LOW("Buffer Consumed return source %p back to client\n",psource_frame);

        if (psource_frame->nFlags & OMX_BUFFERFLAG_EOS)
        {
            if (pdest_frame)
            {
                DEBUG_PRINT_LOW("EOS Reached Pass Last Buffer\n");
                rc = copy_buffer(pdest_frame, &h264_scratch);
                if ( rc != OMX_ErrorNone )
                {
                    return rc;
                }
                pdest_frame->nTimeStamp = h264_scratch.nTimeStamp;
                pdest_frame->nFlags = h264_scratch.nFlags | psource_frame->nFlags;


                DEBUG_PRINT_ERROR("Push EOS frame number:%d nFilledLen =%lu TimeStamp = %lld nFlags %lu\n",
                    frame_count++, pdest_frame->nFilledLen,pdest_frame->nTimeStamp, pdest_frame->nFlags);

                /*Push the frame to the Decoder*/
                if (empty_this_buffer_proxy(hComp,pdest_frame) != OMX_ErrorNone)
                {
                    return OMX_ErrorBadParameter;
                }
                pdest_frame = NULL;
            }
            else
            {
                DEBUG_PRINT_LOW("\n Last frame in else dest addr %p size %lu frame_count %d",
                    pdest_frame,h264_scratch.nFilledLen, frame_count);
                generate_ebd = OMX_FALSE;
            }
        }
    }
    if(generate_ebd && !psource_frame->nFilledLen)
    {
        m_cb.EmptyBufferDone (hComp,m_app_data,psource_frame);
        psource_frame = NULL;
        if (m_input_pending_q.m_size)
        {
            m_input_pending_q.pop_entry(&address,&p2,&id);
            psource_frame = (OMX_BUFFERHEADERTYPE *) address;
            DEBUG_PRINT_LOW("Next source Buffer flag %lu nFilledLen %lu, nTimeStamp %lld\n",
                psource_frame->nFlags,psource_frame->nFilledLen, psource_frame->nTimeStamp);
        }
    }
    return OMX_ErrorNone;
}

bool omx_vdec::align_pmem_buffers(int pmem_fd, OMX_U32 buffer_size,
                                  OMX_U32 alignment)
{
    struct pmem_allocation allocation;
    allocation.size = buffer_size;
    allocation.align = clip2(alignment);
    if (allocation.align < 4096)
    {
        allocation.align = 4096;
    }
    if (ioctl(pmem_fd, PMEM_ALLOCATE_ALIGNED, &allocation) < 0)
    {
        DEBUG_PRINT_ERROR("\n Aligment(%u) failed with pmem driver Sz(%lu)",
            allocation.align, allocation.size);
        return false;
    }
    return true;
}
#ifdef USE_ION
int omx_vdec::alloc_map_ion_memory(OMX_U32 buffer_size,
                                   OMX_U32 alignment, struct ion_allocation_data *alloc_data,
struct ion_fd_data *fd_data, int flag, int heap_id)
{
    int fd = -EINVAL;
    int rc = -EINVAL;
    int ion_dev_flag;
    struct vdec_ion ion_buf_info;
    if (!alloc_data || buffer_size <= 0 || !fd_data) {
        DEBUG_PRINT_ERROR("Invalid arguments to alloc_map_ion_memory\n");
        return -EINVAL;
    }
    ion_dev_flag = O_RDONLY;
    fd = open (MEM_DEVICE, ion_dev_flag);
    if (fd < 0) {
        DEBUG_PRINT_ERROR("opening ion device failed with fd = %d\n", fd);
        return fd;
    }
    alloc_data->flags = 0;
    if(!secure_mode && (flag & ION_FLAG_CACHED))
    {
        alloc_data->flags |= ION_FLAG_CACHED;
    }
    alloc_data->len = buffer_size;
    alloc_data->align = clip2(alignment);
    if (alloc_data->align < 4096)
    {
        alloc_data->align = 4096;
    }
    if ((secure_mode) && (flag & ION_SECURE))
        alloc_data->flags |= ION_SECURE;

    alloc_data->heap_mask = ION_HEAP(ION_IOMMU_HEAP_ID);
    if (!secure_mode && heap_id)
        alloc_data->heap_mask = ION_HEAP(heap_id);

    DEBUG_PRINT_LOW("\n ION ALLOC memory heap_id %d mask %0xx size %d align %d",
        heap_id, (unsigned int)alloc_data->heap_mask, alloc_data->len, alloc_data->align);
    rc = ioctl(fd,ION_IOC_ALLOC,alloc_data);
    if (rc || !alloc_data->handle) {
        DEBUG_PRINT_ERROR("\n ION ALLOC memory failed ");
        alloc_data->handle = NULL;
        close(fd);
        fd = -ENOMEM;
        return fd;
    }
    fd_data->handle = alloc_data->handle;
    rc = ioctl(fd,ION_IOC_MAP,fd_data);
    if (rc) {
        DEBUG_PRINT_ERROR("\n ION MAP failed ");
        ion_buf_info.ion_alloc_data = *alloc_data;
        ion_buf_info.ion_device_fd = fd;
        ion_buf_info.fd_ion_data = *fd_data;
        free_ion_memory(&ion_buf_info);
        fd_data->fd =-1;
        close(fd);
        fd = -ENOMEM;
    }

    return fd;
}

void omx_vdec::free_ion_memory(struct vdec_ion *buf_ion_info) {

    if(!buf_ion_info) {
        DEBUG_PRINT_ERROR("\n ION: free called with invalid fd/allocdata");
        return;
    }
    if(ioctl(buf_ion_info->ion_device_fd,ION_IOC_FREE,
        &buf_ion_info->ion_alloc_data.handle)) {
            DEBUG_PRINT_ERROR("\n ION: free failed" );
    }

    close(buf_ion_info->ion_device_fd);
    buf_ion_info->ion_device_fd = -1;
    buf_ion_info->ion_alloc_data.handle = NULL;
    buf_ion_info->fd_ion_data.fd = -1;
}
#endif
void omx_vdec::free_output_buffer_header()
{
    DEBUG_PRINT_HIGH("\n ALL output buffers are freed/released");
    output_use_buffer = false;
    ouput_egl_buffers = false;

    if (m_out_mem_ptr)
    {
        free (m_out_mem_ptr);
        m_out_mem_ptr = NULL;
    }

    if(m_platform_list)
    {
        free(m_platform_list);
        m_platform_list = NULL;
    }

    if (drv_ctx.ptr_respbuffer)
    {
        free (drv_ctx.ptr_respbuffer);
        drv_ctx.ptr_respbuffer = NULL;
    }
    if (drv_ctx.ptr_outputbuffer)
    {
        free (drv_ctx.ptr_outputbuffer);
        drv_ctx.ptr_outputbuffer = NULL;
    }
#ifdef USE_ION
    if (drv_ctx.op_buf_ion_info) {
        DEBUG_PRINT_LOW("\n Free o/p ion context");
        free(drv_ctx.op_buf_ion_info);
        drv_ctx.op_buf_ion_info = NULL;
    }
#endif
}

void omx_vdec::free_input_buffer_header()
{
    input_use_buffer = false;
    if (arbitrary_bytes)
    {
        if (m_frame_parser.mutils)
        {
            DEBUG_PRINT_LOW("\n Free utils parser");
            delete (m_frame_parser.mutils);
            m_frame_parser.mutils = NULL;
        }

        if (m_inp_heap_ptr)
        {
            DEBUG_PRINT_LOW("\n Free input Heap Pointer");
            free (m_inp_heap_ptr);
            m_inp_heap_ptr = NULL;
        }

        if (m_phdr_pmem_ptr)
        {
            DEBUG_PRINT_LOW("\n Free input pmem header Pointer");
            free (m_phdr_pmem_ptr);
            m_phdr_pmem_ptr = NULL;
        }
    }
    if (m_inp_mem_ptr)
    {
        DEBUG_PRINT_LOW("\n Free input pmem Pointer area");
        free (m_inp_mem_ptr);
        m_inp_mem_ptr = NULL;
    }
    if (drv_ctx.ptr_inputbuffer)
    {
        DEBUG_PRINT_LOW("\n Free Driver Context pointer");
        free (drv_ctx.ptr_inputbuffer);
        drv_ctx.ptr_inputbuffer = NULL;
    }
#ifdef USE_ION
    if (drv_ctx.ip_buf_ion_info) {
        DEBUG_PRINT_LOW("\n Free ion context");
        free(drv_ctx.ip_buf_ion_info);
        drv_ctx.ip_buf_ion_info = NULL;
    }
#endif
}

int omx_vdec::stream_off(OMX_U32 port)
{
    enum v4l2_buf_type btype;
    int rc = 0;
    enum v4l2_ports v4l2_port = OUTPUT_PORT;

    if (port == OMX_CORE_INPUT_PORT_INDEX) {
        btype = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        v4l2_port = OUTPUT_PORT;
    } else if (port == OMX_CORE_OUTPUT_PORT_INDEX) {
        btype = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        v4l2_port = CAPTURE_PORT;
    } else if (port == OMX_ALL) {
        int rc_input = stream_off(OMX_CORE_INPUT_PORT_INDEX);
        int rc_output = stream_off(OMX_CORE_OUTPUT_PORT_INDEX);

        if (!rc_input)
            return rc_input;
        else
            return rc_output;
    }

    if (!streaming[v4l2_port]) {
        // already streamed off, warn and move on
        DEBUG_PRINT_HIGH("Warning: Attempting to stream off on %d port,"
            " which is already streamed off", v4l2_port);
        return 0;
    }

    DEBUG_PRINT_HIGH("Streaming off %d port", v4l2_port);

    rc = ioctl(drv_ctx.video_driver_fd, VIDIOC_STREAMOFF, &btype);
    if (rc) {
        /*TODO: How to handle this case */
        DEBUG_PRINT_ERROR("Failed to call streamoff on %d Port \n", v4l2_port);
    } else {
        streaming[v4l2_port] = false;
    }

    return rc;
}

OMX_ERRORTYPE omx_vdec::get_buffer_req(vdec_allocatorproperty *buffer_prop)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    struct v4l2_requestbuffers bufreq;
    unsigned int buf_size = 0, extra_data_size = 0, client_extra_data_size = 0;
    struct v4l2_format fmt;
    int ret = 0;

    bufreq.memory = V4L2_MEMORY_USERPTR;
    bufreq.count = 1;
    if(buffer_prop->buffer_type == VDEC_BUFFER_TYPE_INPUT){
        bufreq.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        fmt.type =V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        fmt.fmt.pix_mp.pixelformat = output_capability;
    }else if (buffer_prop->buffer_type == VDEC_BUFFER_TYPE_OUTPUT){
        bufreq.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        fmt.type =V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        fmt.fmt.pix_mp.pixelformat = capture_capability;
    }else {eRet = OMX_ErrorBadParameter;}
    if(eRet==OMX_ErrorNone){
        ret = ioctl(drv_ctx.video_driver_fd,VIDIOC_REQBUFS, &bufreq);
    }
    if(ret)
    {
        DEBUG_PRINT_ERROR("Requesting buffer requirements failed");
        /*TODO: How to handle this case */
        eRet = OMX_ErrorInsufficientResources;
        return eRet;
    }
    else
    {
        buffer_prop->actualcount = bufreq.count;
        buffer_prop->mincount = bufreq.count;
        DEBUG_PRINT_HIGH("Count = %d \n ",bufreq.count);
    }
    DEBUG_PRINT_HIGH("GetBufReq: ActCnt(%d) Size(%d), BufType(%d)",
        buffer_prop->actualcount, buffer_prop->buffer_size, buffer_prop->buffer_type);

    fmt.fmt.pix_mp.height = drv_ctx.video_resolution.frame_height;
    fmt.fmt.pix_mp.width = drv_ctx.video_resolution.frame_width;

    ret = ioctl(drv_ctx.video_driver_fd, VIDIOC_G_FMT, &fmt);

    update_resolution(fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height);
    if (fmt.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
        drv_ctx.num_planes = fmt.fmt.pix_mp.num_planes;
    DEBUG_PRINT_HIGH("Buffer Size (plane[0].sizeimage) = %d \n ",fmt.fmt.pix_mp.plane_fmt[0].sizeimage);

    if(ret)
    {
        /*TODO: How to handle this case */
        DEBUG_PRINT_ERROR("Requesting buffer requirements failed");
        eRet = OMX_ErrorInsufficientResources;
    }
    else
    {
        int extra_idx = 0;
        buffer_prop->buffer_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
        buf_size = buffer_prop->buffer_size;
        extra_idx = EXTRADATA_IDX(drv_ctx.num_planes);
        if (extra_idx && (extra_idx < VIDEO_MAX_PLANES)) {
            extra_data_size =  fmt.fmt.pix_mp.plane_fmt[extra_idx].sizeimage;
        } else if (extra_idx >= VIDEO_MAX_PLANES) {
            DEBUG_PRINT_ERROR("Extradata index is more than allowed: %d\n", extra_idx);
            return OMX_ErrorBadParameter;
        }
        if (client_extradata & OMX_FRAMEINFO_EXTRADATA)
        {
            DEBUG_PRINT_HIGH("Frame info extra data enabled!");
            client_extra_data_size += OMX_FRAMEINFO_EXTRADATA_SIZE;
        }
        if (client_extradata & OMX_INTERLACE_EXTRADATA)
        {
            client_extra_data_size += OMX_INTERLACE_EXTRADATA_SIZE;
        }
        if (client_extradata & OMX_PORTDEF_EXTRADATA)
        {
            client_extra_data_size += OMX_PORTDEF_EXTRADATA_SIZE;
            DEBUG_PRINT_HIGH("Smooth streaming enabled extra_data_size=%d\n",
                client_extra_data_size);
        }
        if (client_extra_data_size)
        {
            client_extra_data_size += sizeof(OMX_OTHER_EXTRADATATYPE); //Space for terminator
            buf_size = ((buf_size + 3)&(~3)); //Align extradata start address to 64Bit
        }
        drv_ctx.extradata_info.size = buffer_prop->actualcount * extra_data_size;
        drv_ctx.extradata_info.count = buffer_prop->actualcount;
        drv_ctx.extradata_info.buffer_size = extra_data_size;
        buf_size += client_extra_data_size;
        buf_size = (buf_size + buffer_prop->alignment - 1)&(~(buffer_prop->alignment - 1));
        DEBUG_PRINT_HIGH("GetBufReq UPDATE: ActCnt(%d) Size(%d) BufSize(%d) BufType(%d)",
            buffer_prop->actualcount, buffer_prop->buffer_size, buf_size, buffer_prop->buffer_type);
        if (in_reconfig) // BufReq will be set to driver when port is disabled
            buffer_prop->buffer_size = buf_size;
        else if (buf_size != buffer_prop->buffer_size)
        {
            buffer_prop->buffer_size = buf_size;
            eRet = set_buffer_req(buffer_prop);
        }
    }
    DEBUG_PRINT_HIGH("GetBufReq OUT: ActCnt(%d) Size(%d), BufType(%d)\n",
        buffer_prop->actualcount, buffer_prop->buffer_size, buffer_prop->buffer_type);
    return eRet;
}

OMX_ERRORTYPE omx_vdec::set_buffer_req(vdec_allocatorproperty *buffer_prop)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    unsigned buf_size = 0;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers bufreq;
    int ret;
    DEBUG_PRINT_LOW("SetBufReq IN: ActCnt(%d) Size(%d)",
        buffer_prop->actualcount, buffer_prop->buffer_size);
    buf_size = (buffer_prop->buffer_size + buffer_prop->alignment - 1)&(~(buffer_prop->alignment - 1));
    if (buf_size != buffer_prop->buffer_size)
    {
        DEBUG_PRINT_ERROR("Buffer size alignment error: Requested(%d) Required(%d)",
            buffer_prop->buffer_size, buf_size);
        eRet = OMX_ErrorBadParameter;
    }
    else
    {
        fmt.fmt.pix_mp.height = drv_ctx.video_resolution.frame_height;
        fmt.fmt.pix_mp.width = drv_ctx.video_resolution.frame_width;

        if (buffer_prop->buffer_type == VDEC_BUFFER_TYPE_INPUT){
            fmt.type =V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            fmt.fmt.pix_mp.pixelformat = output_capability;
        } else if (buffer_prop->buffer_type == VDEC_BUFFER_TYPE_OUTPUT) {
            fmt.type =V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            fmt.fmt.pix_mp.pixelformat = capture_capability;
        } else {eRet = OMX_ErrorBadParameter;}

        ret = ioctl(drv_ctx.video_driver_fd, VIDIOC_S_FMT, &fmt);
        if (ret)
        {
            /*TODO: How to handle this case */
            DEBUG_PRINT_ERROR("Setting buffer requirements (format) failed %d", ret);
            eRet = OMX_ErrorInsufficientResources;
        }

        bufreq.memory = V4L2_MEMORY_USERPTR;
        bufreq.count = buffer_prop->actualcount;
        if(buffer_prop->buffer_type == VDEC_BUFFER_TYPE_INPUT) {
            bufreq.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        } else if (buffer_prop->buffer_type == VDEC_BUFFER_TYPE_OUTPUT) {
            bufreq.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        } else {eRet = OMX_ErrorBadParameter;}

        if (eRet==OMX_ErrorNone) {
            ret = ioctl(drv_ctx.video_driver_fd,VIDIOC_REQBUFS, &bufreq);
        }

        if (ret)
        {
            DEBUG_PRINT_ERROR("Setting buffer requirements (reqbufs) failed %d", ret);
            /*TODO: How to handle this case */
            eRet = OMX_ErrorInsufficientResources;
        } else if (bufreq.count < buffer_prop->actualcount) {
            DEBUG_PRINT_ERROR("Driver refused to change the number of buffers"
                " on v4l2 port %d to %d (prefers %d)", bufreq.type,
                buffer_prop->actualcount, bufreq.count);
            eRet = OMX_ErrorInsufficientResources;
        } else {
            if (!client_buffers.update_buffer_req()) {
                DEBUG_PRINT_ERROR("Setting c2D buffer requirements failed");
                eRet = OMX_ErrorInsufficientResources;
            }
        }
    }
    if (!eRet && !m_pSwVdec && buffer_prop->buffer_type == VDEC_BUFFER_TYPE_OUTPUT)
    {
        // need to update extradata buffers also in pure dsp mode
        drv_ctx.extradata_info.size = buffer_prop->actualcount * drv_ctx.extradata_info.buffer_size;
        drv_ctx.extradata_info.count = buffer_prop->actualcount;
    }
    return eRet;
}

OMX_ERRORTYPE omx_vdec::update_picture_resolution()
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    return eRet;
}

OMX_ERRORTYPE omx_vdec::update_portdef(OMX_PARAM_PORTDEFINITIONTYPE *portDefn)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    if (!portDefn)
    {
        return OMX_ErrorBadParameter;
    }
    DEBUG_PRINT_LOW("omx_vdec::update_portdef\n");
    portDefn->nVersion.nVersion = OMX_SPEC_VERSION;
    portDefn->nSize = sizeof(portDefn);
    portDefn->eDomain    = OMX_PortDomainVideo;
    if (drv_ctx.frame_rate.fps_denominator > 0)
        portDefn->format.video.xFramerate = drv_ctx.frame_rate.fps_numerator /
        drv_ctx.frame_rate.fps_denominator;
    else {
        DEBUG_PRINT_ERROR("Error: Divide by zero \n");
        return OMX_ErrorBadParameter;
    }
    if (0 == portDefn->nPortIndex)
    {
        portDefn->eDir =  OMX_DirInput;
        portDefn->nBufferCountActual = drv_ctx.ip_buf.actualcount;
        portDefn->nBufferCountMin    = drv_ctx.ip_buf.mincount;
        portDefn->nBufferSize        = drv_ctx.ip_buf.buffer_size;
        portDefn->format.video.eColorFormat = OMX_COLOR_FormatUnused;
        portDefn->format.video.eCompressionFormat = eCompressionFormat;
        portDefn->bEnabled   = m_inp_bEnabled;
        portDefn->bPopulated = m_inp_bPopulated;
    }
    else if (1 == portDefn->nPortIndex)
    {
        unsigned int buf_size = 0;
        if (!client_buffers.update_buffer_req()) {
            DEBUG_PRINT_ERROR("\n client_buffers.update_buffer_req Failed");
            return OMX_ErrorHardware;
        }
        if (!client_buffers.get_buffer_req(buf_size)) {
            DEBUG_PRINT_ERROR("\n update buffer requirements");
            return OMX_ErrorHardware;
        }
        portDefn->nBufferSize = buf_size;
        portDefn->eDir =  OMX_DirOutput;
        portDefn->nBufferCountActual = drv_ctx.op_buf.actualcount;
        portDefn->nBufferCountMin    = drv_ctx.op_buf.mincount;
        portDefn->format.video.eCompressionFormat = OMX_VIDEO_CodingUnused;
        portDefn->bEnabled   = m_out_bEnabled;
        portDefn->bPopulated = m_out_bPopulated;
        if (!client_buffers.get_color_format(portDefn->format.video.eColorFormat)) {
            DEBUG_PRINT_ERROR("\n Error in getting color format");
            return OMX_ErrorHardware;
        }
    }
    else
    {
        portDefn->eDir = OMX_DirMax;
        DEBUG_PRINT_LOW(" get_parameter: Bad Port idx %d",
            (int)portDefn->nPortIndex);
        eRet = OMX_ErrorBadPortIndex;
    }
    portDefn->format.video.nFrameHeight =  drv_ctx.video_resolution.frame_height;
    portDefn->format.video.nFrameWidth  =  drv_ctx.video_resolution.frame_width;
    portDefn->format.video.nStride = drv_ctx.video_resolution.stride;
    portDefn->format.video.nSliceHeight = drv_ctx.video_resolution.scan_lines;
    DEBUG_PRINT_ERROR("update_portdef Width = %lu Height = %lu Stride = %ld"
        " SliceHeight = %lu \n", portDefn->format.video.nFrameWidth,
        portDefn->format.video.nFrameHeight,
        portDefn->format.video.nStride,
        portDefn->format.video.nSliceHeight);
    return eRet;

}

OMX_ERRORTYPE omx_vdec::allocate_output_headers()
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    OMX_BUFFERHEADERTYPE *bufHdr = NULL;
    unsigned i= 0;

    if(!m_out_mem_ptr) {
        DEBUG_PRINT_HIGH("\n Use o/p buffer case - Header List allocation");
        int nBufHdrSize        = 0;
        int nPlatformEntrySize = 0;
        int nPlatformListSize  = 0;
        int nPMEMInfoSize = 0;
        OMX_QCOM_PLATFORM_PRIVATE_LIST      *pPlatformList;
        OMX_QCOM_PLATFORM_PRIVATE_ENTRY     *pPlatformEntry;
        OMX_QCOM_PLATFORM_PRIVATE_PMEM_INFO *pPMEMInfo;

        DEBUG_PRINT_LOW("Setting First Output Buffer(%d)\n",
            drv_ctx.op_buf.actualcount);
        nBufHdrSize        = drv_ctx.op_buf.actualcount *
            sizeof(OMX_BUFFERHEADERTYPE);

        nPMEMInfoSize      = drv_ctx.op_buf.actualcount *
            sizeof(OMX_QCOM_PLATFORM_PRIVATE_PMEM_INFO);
        nPlatformListSize  = drv_ctx.op_buf.actualcount *
            sizeof(OMX_QCOM_PLATFORM_PRIVATE_LIST);
        nPlatformEntrySize = drv_ctx.op_buf.actualcount *
            sizeof(OMX_QCOM_PLATFORM_PRIVATE_ENTRY);

        DEBUG_PRINT_LOW("TotalBufHdr %d BufHdrSize %d PMEM %d PL %d\n",nBufHdrSize,
            sizeof(OMX_BUFFERHEADERTYPE),
            nPMEMInfoSize,
            nPlatformListSize);
        DEBUG_PRINT_LOW("PE %d bmSize %d \n",nPlatformEntrySize,
            m_out_bm_count);
        m_out_mem_ptr = (OMX_BUFFERHEADERTYPE  *)calloc(nBufHdrSize,1);
        // Alloc mem for platform specific info
        char *pPtr=NULL;
        pPtr = (char*) calloc(nPlatformListSize + nPlatformEntrySize +
            nPMEMInfoSize,1);
        drv_ctx.ptr_outputbuffer = (struct vdec_bufferpayload *) \
            calloc (sizeof(struct vdec_bufferpayload),
            drv_ctx.op_buf.actualcount);
        drv_ctx.ptr_respbuffer = (struct vdec_output_frameinfo  *)\
            calloc (sizeof (struct vdec_output_frameinfo),
            drv_ctx.op_buf.actualcount);
#ifdef USE_ION
        drv_ctx.op_buf_ion_info = (struct vdec_ion * ) \
            calloc (sizeof(struct vdec_ion),drv_ctx.op_buf.actualcount);
#endif

        if(m_out_mem_ptr && pPtr && drv_ctx.ptr_outputbuffer
            && drv_ctx.ptr_respbuffer)
        {
            bufHdr          =  m_out_mem_ptr;
            m_platform_list = (OMX_QCOM_PLATFORM_PRIVATE_LIST *)(pPtr);
            m_platform_entry= (OMX_QCOM_PLATFORM_PRIVATE_ENTRY *)
                (((char *) m_platform_list)  + nPlatformListSize);
            m_pmem_info     = (OMX_QCOM_PLATFORM_PRIVATE_PMEM_INFO *)
                (((char *) m_platform_entry) + nPlatformEntrySize);
            pPlatformList   = m_platform_list;
            pPlatformEntry  = m_platform_entry;
            pPMEMInfo       = m_pmem_info;

            DEBUG_PRINT_LOW("Memory Allocation Succeeded for OUT port%p\n",m_out_mem_ptr);

            // Settting the entire storage nicely
            DEBUG_PRINT_LOW("bHdr %p OutMem %p PE %p\n",bufHdr,
                m_out_mem_ptr,pPlatformEntry);
            DEBUG_PRINT_LOW(" Pmem Info = %p \n",pPMEMInfo);
            for(i=0; i < drv_ctx.op_buf.actualcount ; i++)
            {
                bufHdr->nSize              = sizeof(OMX_BUFFERHEADERTYPE);
                bufHdr->nVersion.nVersion  = OMX_SPEC_VERSION;
                // Set the values when we determine the right HxW param
                bufHdr->nAllocLen          = 0;
                bufHdr->nFilledLen         = 0;
                bufHdr->pAppPrivate        = NULL;
                bufHdr->nOutputPortIndex   = OMX_CORE_OUTPUT_PORT_INDEX;
                pPlatformEntry->type       = OMX_QCOM_PLATFORM_PRIVATE_PMEM;
                pPlatformEntry->entry      = pPMEMInfo;
                // Initialize the Platform List
                pPlatformList->nEntries    = 1;
                pPlatformList->entryList   = pPlatformEntry;
                // Keep pBuffer NULL till vdec is opened
                bufHdr->pBuffer            = NULL;
                pPMEMInfo->offset          =  0;
                pPMEMInfo->pmem_fd = 0;
                bufHdr->pPlatformPrivate = pPlatformList;
                drv_ctx.ptr_outputbuffer[i].pmem_fd = -1;
#ifdef USE_ION
                drv_ctx.op_buf_ion_info[i].ion_device_fd =-1;
#endif
                /*Create a mapping between buffers*/
                bufHdr->pOutputPortPrivate = &drv_ctx.ptr_respbuffer[i];
                drv_ctx.ptr_respbuffer[i].client_data = (void *) \
                    &drv_ctx.ptr_outputbuffer[i];
                // Move the buffer and buffer header pointers
                bufHdr++;
                pPMEMInfo++;
                pPlatformEntry++;
                pPlatformList++;
            }
        }
        else
        {
            DEBUG_PRINT_ERROR("Output buf mem alloc failed[0x%p][0x%p]\n",\
                m_out_mem_ptr, pPtr);
            if(m_out_mem_ptr)
            {
                free(m_out_mem_ptr);
                m_out_mem_ptr = NULL;
            }
            if(pPtr)
            {
                free(pPtr);
                pPtr = NULL;
            }
            if(drv_ctx.ptr_outputbuffer)
            {
                free(drv_ctx.ptr_outputbuffer);
                drv_ctx.ptr_outputbuffer = NULL;
            }
            if(drv_ctx.ptr_respbuffer)
            {
                free(drv_ctx.ptr_respbuffer);
                drv_ctx.ptr_respbuffer = NULL;
            }
#ifdef USE_ION
            if (drv_ctx.op_buf_ion_info) {
                DEBUG_PRINT_LOW("\n Free o/p ion context");
                free(drv_ctx.op_buf_ion_info);
                drv_ctx.op_buf_ion_info = NULL;
            }
#endif
            eRet =  OMX_ErrorInsufficientResources;
        }
    } else {
        eRet =  OMX_ErrorInsufficientResources;
    }
    return eRet;
}

void omx_vdec::complete_pending_buffer_done_cbs()
{
    unsigned p1;
    unsigned p2;
    unsigned ident;
    omx_cmd_queue tmp_q, pending_bd_q;
    pthread_mutex_lock(&m_lock);
    // pop all pending GENERATE FDB from ftb queue
    while (m_ftb_q.m_size)
    {
        m_ftb_q.pop_entry(&p1,&p2,&ident);
        if(ident == OMX_COMPONENT_GENERATE_FBD)
        {
            pending_bd_q.insert_entry(p1,p2,ident);
        }
        else
        {
            tmp_q.insert_entry(p1,p2,ident);
        }
    }
    //return all non GENERATE FDB to ftb queue
    while(tmp_q.m_size)
    {
        tmp_q.pop_entry(&p1,&p2,&ident);
        m_ftb_q.insert_entry(p1,p2,ident);
    }
    // pop all pending GENERATE EDB from etb queue
    while (m_etb_q.m_size)
    {
        m_etb_q.pop_entry(&p1,&p2,&ident);
        if(ident == OMX_COMPONENT_GENERATE_EBD)
        {
            pending_bd_q.insert_entry(p1,p2,ident);
        }
        else
        {
            tmp_q.insert_entry(p1,p2,ident);
        }
    }
    //return all non GENERATE FDB to etb queue
    while(tmp_q.m_size)
    {
        tmp_q.pop_entry(&p1,&p2,&ident);
        m_etb_q.insert_entry(p1,p2,ident);
    }
    pthread_mutex_unlock(&m_lock);
    // process all pending buffer dones
    while(pending_bd_q.m_size)
    {
        pending_bd_q.pop_entry(&p1,&p2,&ident);
        switch(ident)
        {
        case OMX_COMPONENT_GENERATE_EBD:
            if(empty_buffer_done(&m_cmp, (OMX_BUFFERHEADERTYPE *)p1) != OMX_ErrorNone)
            {
                DEBUG_PRINT_ERROR("\nERROR: empty_buffer_done() failed!\n");
                omx_report_error ();
            }
            break;

        case OMX_COMPONENT_GENERATE_FBD:
            if(fill_buffer_done(&m_cmp, (OMX_BUFFERHEADERTYPE *)p1) != OMX_ErrorNone )
            {
                DEBUG_PRINT_ERROR("\nERROR: fill_buffer_done() failed!\n");
                omx_report_error ();
            }
            break;
        }
    }
}

void omx_vdec::set_frame_rate(OMX_S64 act_timestamp)
{
    OMX_U32 new_frame_interval = 0;
    if (VALID_TS(act_timestamp) && VALID_TS(prev_ts) && act_timestamp != prev_ts
        && (((act_timestamp > prev_ts )? act_timestamp - prev_ts: prev_ts-act_timestamp)>2000))
    {
        new_frame_interval = (act_timestamp > prev_ts)?
            act_timestamp - prev_ts :
        prev_ts - act_timestamp;
        if (new_frame_interval < frm_int || frm_int == 0)
        {
            frm_int = new_frame_interval;
            if(frm_int)
            {
                drv_ctx.frame_rate.fps_numerator = 1e6;
                drv_ctx.frame_rate.fps_denominator = frm_int;
                DEBUG_PRINT_LOW("set_frame_rate: frm_int(%lu) fps(%f)",
                    frm_int, drv_ctx.frame_rate.fps_numerator /
                    (float)drv_ctx.frame_rate.fps_denominator);
            }
        }
    }
    prev_ts = act_timestamp;
}

void omx_vdec::adjust_timestamp(OMX_S64 &act_timestamp)
{
    if (rst_prev_ts && VALID_TS(act_timestamp))
    {
        prev_ts = act_timestamp;
        rst_prev_ts = false;
    }
    else if (VALID_TS(prev_ts))
    {
        bool codec_cond = (drv_ctx.timestamp_adjust)?
            (!VALID_TS(act_timestamp) || (((act_timestamp > prev_ts)?
            (act_timestamp - prev_ts):(prev_ts - act_timestamp)) <= 2000)):
        (!VALID_TS(act_timestamp) || act_timestamp == prev_ts);
        if(frm_int > 0 && codec_cond)
        {
            DEBUG_PRINT_LOW("adjust_timestamp: original ts[%lld]", act_timestamp);
            act_timestamp = prev_ts + frm_int;
            DEBUG_PRINT_LOW("adjust_timestamp: predicted ts[%lld]", act_timestamp);
            prev_ts = act_timestamp;
        }
        else
            set_frame_rate(act_timestamp);
    }
    else if (frm_int > 0)           // In this case the frame rate was set along
    {                               // with the port definition, start ts with 0
        act_timestamp = prev_ts = 0;  // and correct if a valid ts is received.
        rst_prev_ts = true;
    }
}

void omx_vdec::handle_extradata(OMX_BUFFERHEADERTYPE *p_buf_hdr)
{
    OMX_OTHER_EXTRADATATYPE *p_extra = NULL, *p_sei = NULL, *p_vui = NULL;
    OMX_U32 num_conceal_MB = 0;
    OMX_U32 frame_rate = 0;
    int consumed_len = 0;
    OMX_U32 num_MB_in_frame;
    OMX_U32 recovery_sei_flags = 1;
    int buf_index = p_buf_hdr - m_out_mem_ptr;
    struct msm_vidc_panscan_window_payload *panscan_payload = NULL;
    OMX_U8 *pBuffer = (OMX_U8 *)(drv_ctx.ptr_outputbuffer[buf_index].bufferaddr) +
        p_buf_hdr->nOffset;
    if (!drv_ctx.extradata_info.uaddr) {
        return;
    }
    p_extra = (OMX_OTHER_EXTRADATATYPE *)
        ((unsigned)(pBuffer + p_buf_hdr->nOffset + p_buf_hdr->nFilledLen + 3)&(~3));
    char *p_extradata = drv_ctx.extradata_info.uaddr + buf_index * drv_ctx.extradata_info.buffer_size;
    if ((OMX_U8*)p_extra > (pBuffer + p_buf_hdr->nAllocLen))
        p_extra = NULL;
    OMX_OTHER_EXTRADATATYPE *data = (struct OMX_OTHER_EXTRADATATYPE *)p_extradata;
    if (data) {
        while((consumed_len < drv_ctx.extradata_info.buffer_size)
            && (data->eType != (OMX_EXTRADATATYPE)EXTRADATA_NONE)) {
                if ((consumed_len + data->nSize) > (int)drv_ctx.extradata_info.buffer_size) {
                    DEBUG_PRINT_LOW("Invalid extra data size");
                    break;
                }
                switch((unsigned long)data->eType) {
case EXTRADATA_INTERLACE_VIDEO:
    struct msm_vidc_interlace_payload *payload;
    payload = (struct msm_vidc_interlace_payload *)data->data;
    if (payload->format != INTERLACE_FRAME_PROGRESSIVE) {
        int enable = 1;
        OMX_U32 mbaff = 0;
        mbaff = (h264_parser)? (h264_parser->is_mbaff()): false;
        if ((payload->format == INTERLACE_FRAME_PROGRESSIVE)  && !mbaff)
            drv_ctx.interlace = VDEC_InterlaceFrameProgressive;
        else
            drv_ctx.interlace = VDEC_InterlaceInterleaveFrameTopFieldFirst;
        if(m_enable_android_native_buffers)
            setMetaData((private_handle_t *)native_buffer[buf_index].privatehandle,
            PP_PARAM_INTERLACED, (void*)&enable);
    }
    if (!secure_mode && (client_extradata & OMX_INTERLACE_EXTRADATA)) {
        append_interlace_extradata(p_extra, payload->format);
        p_extra = (OMX_OTHER_EXTRADATATYPE *) (((OMX_U8 *) p_extra) + p_extra->nSize);
    }
    break;
case EXTRADATA_FRAME_RATE:
    struct msm_vidc_framerate_payload *frame_rate_payload;
    frame_rate_payload = (struct msm_vidc_framerate_payload *)data->data;
    frame_rate = frame_rate_payload->frame_rate;
    break;
case EXTRADATA_TIMESTAMP:
    struct msm_vidc_ts_payload *time_stamp_payload;
    time_stamp_payload = (struct msm_vidc_ts_payload *)data->data;
    p_buf_hdr->nTimeStamp = time_stamp_payload->timestamp_lo;
    p_buf_hdr->nTimeStamp |= ((unsigned long long)time_stamp_payload->timestamp_hi << 32);
    break;
case EXTRADATA_NUM_CONCEALED_MB:
    struct msm_vidc_concealmb_payload *conceal_mb_payload;
    conceal_mb_payload = (struct msm_vidc_concealmb_payload *)data->data;
    num_MB_in_frame = ((drv_ctx.video_resolution.frame_width + 15) *
        (drv_ctx.video_resolution.frame_height + 15)) >> 8;
    num_conceal_MB = ((num_MB_in_frame > 0)?(conceal_mb_payload->num_mbs * 100 / num_MB_in_frame) : 0);
    break;
case EXTRADATA_ASPECT_RATIO:
    struct msm_vidc_aspect_ratio_payload *aspect_ratio_payload;
    aspect_ratio_payload = (struct msm_vidc_aspect_ratio_payload *)data->data;
    ((struct vdec_output_frameinfo *)
        p_buf_hdr->pOutputPortPrivate)->aspect_ratio_info.par_width = aspect_ratio_payload->aspect_width;
    ((struct vdec_output_frameinfo *)
        p_buf_hdr->pOutputPortPrivate)->aspect_ratio_info.par_height = aspect_ratio_payload->aspect_height;
    break;
case EXTRADATA_RECOVERY_POINT_SEI:
    struct msm_vidc_recoverysei_payload *recovery_sei_payload;
    recovery_sei_payload = (struct msm_vidc_recoverysei_payload *)data->data;
    recovery_sei_flags = recovery_sei_payload->flags;
    if (recovery_sei_flags != FRAME_RECONSTRUCTION_CORRECT) {
        p_buf_hdr->nFlags |= OMX_BUFFERFLAG_DATACORRUPT;
        DEBUG_PRINT_HIGH("Extradata: OMX_BUFFERFLAG_DATACORRUPT Received\n");
    }
    break;
case EXTRADATA_PANSCAN_WINDOW:
    panscan_payload = (struct msm_vidc_panscan_window_payload *)data->data;
    break;
default:
    goto unrecognized_extradata;
                }
                consumed_len += data->nSize;
                data = (OMX_OTHER_EXTRADATATYPE *)((char *)data + data->nSize);
        }
        if (!secure_mode && (client_extradata & OMX_FRAMEINFO_EXTRADATA)) {
            p_buf_hdr->nFlags |= OMX_BUFFERFLAG_EXTRADATA;
            append_frame_info_extradata(p_extra,
                num_conceal_MB, ((struct vdec_output_frameinfo *)p_buf_hdr->pOutputPortPrivate)->pic_type, frame_rate,
                panscan_payload,&((struct vdec_output_frameinfo *)
                p_buf_hdr->pOutputPortPrivate)->aspect_ratio_info);}
    }
unrecognized_extradata:
    if(!secure_mode && client_extradata)
        append_terminator_extradata(p_extra);
    return;
}

OMX_ERRORTYPE omx_vdec::enable_extradata(OMX_U32 requested_extradata,
                                         bool is_internal, bool enable)
{
    OMX_ERRORTYPE ret = OMX_ErrorNone;
    struct v4l2_control control;
    if(m_state != OMX_StateLoaded)
    {
        DEBUG_PRINT_ERROR("ERROR: enable extradata allowed in Loaded state only");
        return OMX_ErrorIncorrectStateOperation;
    }
    DEBUG_PRINT_ERROR("NOTE: enable_extradata: actual[%lu] requested[%lu] enable[%d], is_internal: %d swvdec mode %d\n",
        client_extradata, requested_extradata, enable, is_internal, m_swvdec_mode);

    if (!is_internal) {
        if (enable)
            client_extradata |= requested_extradata;
        else
            client_extradata = client_extradata & ~requested_extradata;
    }

    if (enable) {
        if (m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY) {
            if (requested_extradata & OMX_INTERLACE_EXTRADATA) {
                control.id = V4L2_CID_MPEG_VIDC_VIDEO_EXTRADATA;
                control.value = V4L2_MPEG_VIDC_EXTRADATA_INTERLACE_VIDEO;
                if(ioctl(drv_ctx.video_driver_fd, VIDIOC_S_CTRL, &control)) {
                    DEBUG_PRINT_HIGH("Failed to set interlaced extradata."
                        " Quality of interlaced clips might be impacted.\n");
                }
            } else if (requested_extradata & OMX_FRAMEINFO_EXTRADATA)
            {
                control.id = V4L2_CID_MPEG_VIDC_VIDEO_EXTRADATA;
                control.value = V4L2_MPEG_VIDC_EXTRADATA_FRAME_RATE;
                if(ioctl(drv_ctx.video_driver_fd, VIDIOC_S_CTRL, &control)) {
                    DEBUG_PRINT_HIGH("Failed to set framerate extradata\n");
                }
                control.id = V4L2_CID_MPEG_VIDC_VIDEO_EXTRADATA;
                control.value = V4L2_MPEG_VIDC_EXTRADATA_NUM_CONCEALED_MB;
                if(ioctl(drv_ctx.video_driver_fd, VIDIOC_S_CTRL, &control)) {
                    DEBUG_PRINT_HIGH("Failed to set concealed MB extradata\n");
                }
                control.id = V4L2_CID_MPEG_VIDC_VIDEO_EXTRADATA;
                control.value = V4L2_MPEG_VIDC_EXTRADATA_RECOVERY_POINT_SEI;
                if(ioctl(drv_ctx.video_driver_fd, VIDIOC_S_CTRL, &control)) {
                    DEBUG_PRINT_HIGH("Failed to set recovery point SEI extradata\n");
                }
                control.id = V4L2_CID_MPEG_VIDC_VIDEO_EXTRADATA;
                control.value = V4L2_MPEG_VIDC_EXTRADATA_PANSCAN_WINDOW;
                if(ioctl(drv_ctx.video_driver_fd, VIDIOC_S_CTRL, &control)) {
                    DEBUG_PRINT_HIGH("Failed to set panscan extradata\n");
                }
                control.id = V4L2_CID_MPEG_VIDC_VIDEO_EXTRADATA;
                control.value = V4L2_MPEG_VIDC_INDEX_EXTRADATA_ASPECT_RATIO;
                if(ioctl(drv_ctx.video_driver_fd, VIDIOC_S_CTRL, &control)) {
                    DEBUG_PRINT_HIGH("Failed to set panscan extradata\n");
                }
            } else if (requested_extradata & OMX_TIMEINFO_EXTRADATA){
                control.id = V4L2_CID_MPEG_VIDC_VIDEO_EXTRADATA;
                control.value = V4L2_MPEG_VIDC_EXTRADATA_TIMESTAMP;
                if(ioctl(drv_ctx.video_driver_fd, VIDIOC_S_CTRL, &control)) {
                    DEBUG_PRINT_HIGH("Failed to set timeinfo extradata\n");
                }
            }
        }
    }
    return ret;
}

OMX_U32 omx_vdec::count_MB_in_extradata(OMX_OTHER_EXTRADATATYPE *extra)
{
    OMX_U32 num_MB = 0, byte_count = 0, num_MB_in_frame = 0;
    OMX_U8 *data_ptr = extra->data, data = 0;
    while (byte_count < extra->nDataSize)
    {
        data = *data_ptr;
        while (data)
        {
            num_MB += (data&0x01);
            data >>= 1;
        }
        data_ptr++;
        byte_count++;
    }
    num_MB_in_frame = ((drv_ctx.video_resolution.frame_width + 15) *
        (drv_ctx.video_resolution.frame_height + 15)) >> 8;
    return ((num_MB_in_frame > 0)?(num_MB * 100 / num_MB_in_frame) : 0);
}

void omx_vdec::print_debug_extradata(OMX_OTHER_EXTRADATATYPE *extra)
{
    if (!m_debug_extradata)
        return;

    DEBUG_PRINT_HIGH(
        "============== Extra Data ==============\n"
        "           Size: %lu \n"
        "        Version: %lu \n"
        "      PortIndex: %lu \n"
        "           Type: %x \n"
        "       DataSize: %lu \n",
        extra->nSize, extra->nVersion.nVersion,
        extra->nPortIndex, extra->eType, extra->nDataSize);

    if (extra->eType == (OMX_EXTRADATATYPE)OMX_ExtraDataInterlaceFormat)
    {
        OMX_STREAMINTERLACEFORMAT *intfmt = (OMX_STREAMINTERLACEFORMAT *)extra->data;
        DEBUG_PRINT_HIGH(
            "------ Interlace Format ------\n"
            "                Size: %lu \n"
            "             Version: %lu \n"
            "           PortIndex: %lu \n"
            " Is Interlace Format: %d \n"
            "   Interlace Formats: %lu \n"
            "=========== End of Interlace ===========\n",
            intfmt->nSize, intfmt->nVersion.nVersion, intfmt->nPortIndex,
            intfmt->bInterlaceFormat, intfmt->nInterlaceFormats);
    }
    else if (extra->eType == (OMX_EXTRADATATYPE)OMX_ExtraDataFrameInfo)
    {
        OMX_QCOM_EXTRADATA_FRAMEINFO *fminfo = (OMX_QCOM_EXTRADATA_FRAMEINFO *)extra->data;

        DEBUG_PRINT_HIGH(
            "-------- Frame Format --------\n"
            "             Picture Type: %d \n"
            "           Interlace Type: %d \n"
            " Pan Scan Total Frame Num: %lu \n"
            "   Concealed Macro Blocks: %lu \n"
            "               frame rate: %lu \n"
            "           Aspect Ratio X: %lu \n"
            "           Aspect Ratio Y: %lu \n",
            fminfo->ePicType,
            fminfo->interlaceType,
            fminfo->panScan.numWindows,
            fminfo->nConcealedMacroblocks,
            fminfo->nFrameRate,
            fminfo->aspectRatio.aspectRatioX,
            fminfo->aspectRatio.aspectRatioY);

        for (OMX_U32 i = 0; i < fminfo->panScan.numWindows; i++)
        {
            DEBUG_PRINT_HIGH(
                "------------------------------\n"
                "     Pan Scan Frame Num: %lu \n"
                "            Rectangle x: %ld \n"
                "            Rectangle y: %ld \n"
                "           Rectangle dx: %ld \n"
                "           Rectangle dy: %ld \n",
                i, fminfo->panScan.window[i].x, fminfo->panScan.window[i].y,
                fminfo->panScan.window[i].dx, fminfo->panScan.window[i].dy);
        }

        DEBUG_PRINT_HIGH("========= End of Frame Format ==========");
    }
    else if (extra->eType == OMX_ExtraDataNone)
    {
        DEBUG_PRINT_HIGH("========== End of Terminator ===========");
    }
    else
    {
        DEBUG_PRINT_HIGH("======= End of Driver Extradata ========");
    }
}

void omx_vdec::append_interlace_extradata(OMX_OTHER_EXTRADATATYPE *extra,
                                          OMX_U32 interlaced_format_type)
{
    OMX_STREAMINTERLACEFORMAT *interlace_format;
    OMX_U32 mbaff = 0;
    if (!(client_extradata & OMX_INTERLACE_EXTRADATA)) {
        return;
    }
    extra->nSize = OMX_INTERLACE_EXTRADATA_SIZE;
    extra->nVersion.nVersion = OMX_SPEC_VERSION;
    extra->nPortIndex = OMX_CORE_OUTPUT_PORT_INDEX;
    extra->eType = (OMX_EXTRADATATYPE)OMX_ExtraDataInterlaceFormat;
    extra->nDataSize = sizeof(OMX_STREAMINTERLACEFORMAT);
    interlace_format = (OMX_STREAMINTERLACEFORMAT *)extra->data;
    interlace_format->nSize = sizeof(OMX_STREAMINTERLACEFORMAT);
    interlace_format->nVersion.nVersion = OMX_SPEC_VERSION;
    interlace_format->nPortIndex = OMX_CORE_OUTPUT_PORT_INDEX;
    mbaff = (h264_parser)? (h264_parser->is_mbaff()): false;
    if ((interlaced_format_type == INTERLACE_FRAME_PROGRESSIVE)  && !mbaff)
    {
        interlace_format->bInterlaceFormat = OMX_FALSE;
        interlace_format->nInterlaceFormats = OMX_InterlaceFrameProgressive;
        drv_ctx.interlace = VDEC_InterlaceFrameProgressive;
    }
    else
    {
        interlace_format->bInterlaceFormat = OMX_TRUE;
        interlace_format->nInterlaceFormats = OMX_InterlaceInterleaveFrameTopFieldFirst;
        drv_ctx.interlace = VDEC_InterlaceInterleaveFrameTopFieldFirst;
    }
    print_debug_extradata(extra);
}

void omx_vdec::fill_aspect_ratio_info(
struct vdec_aspectratioinfo *aspect_ratio_info,
    OMX_QCOM_EXTRADATA_FRAMEINFO *frame_info)
{
    m_extradata = frame_info;
    m_extradata->aspectRatio.aspectRatioX = aspect_ratio_info->par_width;
    m_extradata->aspectRatio.aspectRatioY = aspect_ratio_info->par_height;
    DEBUG_PRINT_LOW("aspectRatioX %lu aspectRatioX %lu", m_extradata->aspectRatio.aspectRatioX,
        m_extradata->aspectRatio.aspectRatioY);
}

void omx_vdec::append_frame_info_extradata(OMX_OTHER_EXTRADATATYPE *extra,
                                           OMX_U32 num_conceal_mb, OMX_U32 picture_type, OMX_U32 frame_rate,
struct msm_vidc_panscan_window_payload *panscan_payload,
struct vdec_aspectratioinfo *aspect_ratio_info)
{
    OMX_QCOM_EXTRADATA_FRAMEINFO *frame_info = NULL;
    struct msm_vidc_panscan_window *panscan_window;
    if (!(client_extradata & OMX_FRAMEINFO_EXTRADATA)) {
        return;
    }
    extra->nSize = OMX_FRAMEINFO_EXTRADATA_SIZE;
    extra->nVersion.nVersion = OMX_SPEC_VERSION;
    extra->nPortIndex = OMX_CORE_OUTPUT_PORT_INDEX;
    extra->eType = (OMX_EXTRADATATYPE)OMX_ExtraDataFrameInfo;
    extra->nDataSize = sizeof(OMX_QCOM_EXTRADATA_FRAMEINFO);
    frame_info = (OMX_QCOM_EXTRADATA_FRAMEINFO *)extra->data;
    switch (picture_type)
    {
    case PICTURE_TYPE_I:
        frame_info->ePicType = OMX_VIDEO_PictureTypeI;
        break;
    case PICTURE_TYPE_P:
        frame_info->ePicType = OMX_VIDEO_PictureTypeP;
        break;
    case PICTURE_TYPE_B:
        frame_info->ePicType = OMX_VIDEO_PictureTypeB;
        break;
    default:
        frame_info->ePicType = (OMX_VIDEO_PICTURETYPE)0;
    }
    if (drv_ctx.interlace == VDEC_InterlaceInterleaveFrameTopFieldFirst)
        frame_info->interlaceType = OMX_QCOM_InterlaceInterleaveFrameTopFieldFirst;
    else if (drv_ctx.interlace == VDEC_InterlaceInterleaveFrameBottomFieldFirst)
        frame_info->interlaceType = OMX_QCOM_InterlaceInterleaveFrameBottomFieldFirst;
    else
        frame_info->interlaceType = OMX_QCOM_InterlaceFrameProgressive;
    memset(&frame_info->aspectRatio, 0, sizeof(frame_info->aspectRatio));
    frame_info->nConcealedMacroblocks = num_conceal_mb;
    frame_info->nFrameRate = frame_rate;
    frame_info->panScan.numWindows = 0;
    if(panscan_payload) {
        frame_info->panScan.numWindows = panscan_payload->num_panscan_windows;
        panscan_window = &panscan_payload->wnd[0];
        for (OMX_U32 i = 0; i < frame_info->panScan.numWindows; i++)
        {
            frame_info->panScan.window[i].x = panscan_window->panscan_window_width;
            frame_info->panScan.window[i].y = panscan_window->panscan_window_height;
            frame_info->panScan.window[i].dx = panscan_window->panscan_width_offset;
            frame_info->panScan.window[i].dy = panscan_window->panscan_height_offset;
            panscan_window++;
        }
    }
    fill_aspect_ratio_info(aspect_ratio_info, frame_info);
    print_debug_extradata(extra);
}

void omx_vdec::append_portdef_extradata(OMX_OTHER_EXTRADATATYPE *extra)
{
    OMX_PARAM_PORTDEFINITIONTYPE *portDefn = NULL;
    extra->nSize = OMX_PORTDEF_EXTRADATA_SIZE;
    extra->nVersion.nVersion = OMX_SPEC_VERSION;
    extra->nPortIndex = OMX_CORE_OUTPUT_PORT_INDEX;
    extra->eType = (OMX_EXTRADATATYPE)OMX_ExtraDataPortDef;
    extra->nDataSize = sizeof(OMX_PARAM_PORTDEFINITIONTYPE);
    portDefn = (OMX_PARAM_PORTDEFINITIONTYPE *)extra->data;
    *portDefn = m_port_def;
    DEBUG_PRINT_LOW("append_portdef_extradata height = %lu width = %lu stride = %lu"
        "sliceheight = %lu \n",portDefn->format.video.nFrameHeight,
        portDefn->format.video.nFrameWidth,
        portDefn->format.video.nStride,
        portDefn->format.video.nSliceHeight);
}

void omx_vdec::append_terminator_extradata(OMX_OTHER_EXTRADATATYPE *extra)
{
    if (!client_extradata) {
        return;
    }
    extra->nSize = sizeof(OMX_OTHER_EXTRADATATYPE);
    extra->nVersion.nVersion = OMX_SPEC_VERSION;
    extra->eType = OMX_ExtraDataNone;
    extra->nDataSize = 0;
    extra->data[0] = 0;

    print_debug_extradata(extra);
}

OMX_ERRORTYPE  omx_vdec::allocate_desc_buffer(OMX_U32 index)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    if (index >= drv_ctx.ip_buf.actualcount)
    {
        DEBUG_PRINT_ERROR("\nERROR:Desc Buffer Index not found");
        return OMX_ErrorInsufficientResources;
    }
    if (m_desc_buffer_ptr == NULL)
    {
        m_desc_buffer_ptr = (desc_buffer_hdr*) \
            calloc( (sizeof(desc_buffer_hdr)),
            drv_ctx.ip_buf.actualcount);
        if (m_desc_buffer_ptr == NULL)
        {
            DEBUG_PRINT_ERROR("\n m_desc_buffer_ptr Allocation failed ");
            return OMX_ErrorInsufficientResources;
        }
    }

    m_desc_buffer_ptr[index].buf_addr = (unsigned char *)malloc (DESC_BUFFER_SIZE * sizeof(OMX_U8));
    if (m_desc_buffer_ptr[index].buf_addr == NULL)
    {
        DEBUG_PRINT_ERROR("\ndesc buffer Allocation failed ");
        return OMX_ErrorInsufficientResources;
    }

    return eRet;
}

void omx_vdec::insert_demux_addr_offset(OMX_U32 address_offset)
{
    DEBUG_PRINT_LOW("Inserting address offset (%lu) at idx (%lu)", address_offset,m_demux_entries);
    if (m_demux_entries < 8192)
    {
        m_demux_offsets[m_demux_entries++] = address_offset;
    }
    return;
}

void omx_vdec::extract_demux_addr_offsets(OMX_BUFFERHEADERTYPE *buf_hdr)
{
    OMX_U32 bytes_to_parse = buf_hdr->nFilledLen;
    OMX_U8 *buf = buf_hdr->pBuffer + buf_hdr->nOffset;
    OMX_U32 index = 0;

    m_demux_entries = 0;

    while (index < bytes_to_parse)
    {
        if ( ((buf[index] == 0x00) && (buf[index+1] == 0x00) &&
            (buf[index+2] == 0x00) && (buf[index+3] == 0x01)) ||
            ((buf[index] == 0x00) && (buf[index+1] == 0x00) &&
            (buf[index+2] == 0x01)) )
        {
            //Found start code, insert address offset
            insert_demux_addr_offset(index);
            if (buf[index+2] == 0x01) // 3 byte start code
                index += 3;
            else                      //4 byte start code
                index += 4;
        }
        else
            index++;
    }
    DEBUG_PRINT_LOW("Extracted (%lu) demux entry offsets",m_demux_entries);
    return;
}

OMX_ERRORTYPE omx_vdec::handle_demux_data(OMX_BUFFERHEADERTYPE *p_buf_hdr)
{
    //fix this, handle 3 byte start code, vc1 terminator entry
    OMX_U8 *p_demux_data = NULL;
    OMX_U32 desc_data = 0;
    OMX_U32 start_addr = 0;
    OMX_U32 nal_size = 0;
    OMX_U32 suffix_byte = 0;
    OMX_U32 demux_index = 0;
    OMX_U32 buffer_index = 0;

    if (m_desc_buffer_ptr == NULL)
    {
        DEBUG_PRINT_ERROR("m_desc_buffer_ptr is NULL. Cannot append demux entries.");
        return OMX_ErrorBadParameter;
    }

    buffer_index = p_buf_hdr - ((OMX_BUFFERHEADERTYPE *)m_inp_mem_ptr);
    if (buffer_index > drv_ctx.ip_buf.actualcount)
    {
        DEBUG_PRINT_ERROR("handle_demux_data:Buffer index is incorrect (%lu)", buffer_index);
        return OMX_ErrorBadParameter;
    }

    p_demux_data = (OMX_U8 *) m_desc_buffer_ptr[buffer_index].buf_addr;

    if ( ((OMX_U8*)p_demux_data == NULL) ||
        ((m_demux_entries * 16) + 1) > DESC_BUFFER_SIZE)
    {
        DEBUG_PRINT_ERROR("Insufficient buffer. Cannot append demux entries.");
        return OMX_ErrorBadParameter;
    }
    else
    {
        for (; demux_index < m_demux_entries; demux_index++)
        {
            desc_data = 0;
            start_addr = m_demux_offsets[demux_index];
            if (p_buf_hdr->pBuffer[m_demux_offsets[demux_index] + 2] == 0x01)
            {
                suffix_byte = p_buf_hdr->pBuffer[m_demux_offsets[demux_index] + 3];
            }
            else
            {
                suffix_byte = p_buf_hdr->pBuffer[m_demux_offsets[demux_index] + 4];
            }
            if (demux_index < (m_demux_entries - 1))
            {
                nal_size = m_demux_offsets[demux_index + 1] - m_demux_offsets[demux_index] - 2;
            }
            else
            {
                nal_size = p_buf_hdr->nFilledLen - m_demux_offsets[demux_index] - 2;
            }
            DEBUG_PRINT_LOW("Start_addr(%p), suffix_byte(0x%x),nal_size(%lu),demux_index(%lu)",
                (void*)start_addr,
                (unsigned int)suffix_byte,
                nal_size,
                demux_index);
            desc_data = (start_addr >> 3) << 1;
            desc_data |= (start_addr & 7) << 21;
            desc_data |= suffix_byte << 24;

            memcpy(p_demux_data, &desc_data, sizeof(OMX_U32));
            memcpy(p_demux_data + 4, &nal_size, sizeof(OMX_U32));
            memset(p_demux_data + 8, 0, sizeof(OMX_U32));
            memset(p_demux_data + 12, 0, sizeof(OMX_U32));

            p_demux_data += 16;
        }
        if (codec_type_parse == CODEC_TYPE_VC1)
        {
            DEBUG_PRINT_LOW("VC1 terminator entry");
            desc_data = 0;
            desc_data = 0x82 << 24;
            memcpy(p_demux_data, &desc_data, sizeof(OMX_U32));
            memset(p_demux_data + 4, 0, sizeof(OMX_U32));
            memset(p_demux_data + 8, 0, sizeof(OMX_U32));
            memset(p_demux_data + 12, 0, sizeof(OMX_U32));
            p_demux_data += 16;
            m_demux_entries++;
        }
        //Add zero word to indicate end of descriptors
        memset(p_demux_data, 0, sizeof(OMX_U32));

        m_desc_buffer_ptr[buffer_index].desc_data_size = (m_demux_entries * 16) + sizeof(OMX_U32);
        DEBUG_PRINT_LOW("desc table data size=%lu", m_desc_buffer_ptr[buffer_index].desc_data_size);
    }
    memset(m_demux_offsets, 0, ( sizeof(OMX_U32) * 8192) );
    m_demux_entries = 0;
    DEBUG_PRINT_LOW("Demux table complete!");
    return OMX_ErrorNone;
}

OMX_ERRORTYPE omx_vdec::createDivxDrmContext()
{
    OMX_ERRORTYPE err = OMX_ErrorNone;
    iDivXDrmDecrypt = DivXDrmDecrypt::Create();
    if (iDivXDrmDecrypt) {
        OMX_ERRORTYPE err = iDivXDrmDecrypt->Init();
        if(err!=OMX_ErrorNone) {
            DEBUG_PRINT_ERROR("\nERROR :iDivXDrmDecrypt->Init %d", err);
            delete iDivXDrmDecrypt;
            iDivXDrmDecrypt = NULL;
        }
    }
    else {
        DEBUG_PRINT_ERROR("\nUnable to Create DIVX DRM");
        err = OMX_ErrorUndefined;
    }
    return err;
}

omx_vdec::allocate_color_convert_buf::allocate_color_convert_buf()
{
    enabled = false;
    omx = NULL;
    init_members();
    ColorFormat = OMX_COLOR_FormatMax;
}

void omx_vdec::allocate_color_convert_buf::set_vdec_client(void *client)
{
    omx = reinterpret_cast<omx_vdec*>(client);
}

void omx_vdec::allocate_color_convert_buf::init_members() {
    allocated_count = 0;
    buffer_size_req = 0;
    buffer_alignment_req = 0;
    memset(m_platform_list_client,0,sizeof(m_platform_list_client));
    memset(m_platform_entry_client,0,sizeof(m_platform_entry_client));
    memset(m_pmem_info_client,0,sizeof(m_pmem_info_client));
    memset(m_out_mem_ptr_client,0,sizeof(m_out_mem_ptr_client));
#ifdef USE_ION
    memset(op_buf_ion_info,0,sizeof(m_platform_entry_client));
#endif
    for (int i = 0; i < MAX_COUNT;i++)
        pmem_fd[i] = -1;
}

omx_vdec::allocate_color_convert_buf::~allocate_color_convert_buf() {
    c2d.destroy();
}

bool omx_vdec::allocate_color_convert_buf::update_buffer_req()
{
    bool status = true;
    unsigned int src_size = 0, destination_size = 0;
    OMX_COLOR_FORMATTYPE drv_color_format;
    if (!omx){
        DEBUG_PRINT_ERROR("\n Invalid client in color convert");
        return false;
    }
    if (!enabled){
        DEBUG_PRINT_ERROR("\n No color conversion required");
        return status;
    }
    pthread_mutex_lock(&omx->c_lock);
    if (omx->drv_ctx.output_format != VDEC_YUV_FORMAT_NV12 &&
        ColorFormat != OMX_COLOR_FormatYUV420Planar) {
            DEBUG_PRINT_ERROR("\nupdate_buffer_req: Unsupported color conversion");
            status = false;
            goto fail_update_buf_req;
    }
    c2d.close();
    status = c2d.open(omx->drv_ctx.video_resolution.frame_height,
        omx->drv_ctx.video_resolution.frame_width,
        NV12_128m,YCbCr420P);
    if (status) {
        status = c2d.get_buffer_size(C2D_INPUT,src_size);
        if (status)
            status = c2d.get_buffer_size(C2D_OUTPUT,destination_size);
    }
    if (status) {
        if (!src_size || src_size > omx->drv_ctx.op_buf.buffer_size ||
            !destination_size) {
                DEBUG_PRINT_ERROR("\nERROR: Size mismatch in C2D src_size %d"
                    "driver size %d destination size %d",
                    src_size,omx->drv_ctx.op_buf.buffer_size,destination_size);
                status = false;
                c2d.close();
                buffer_size_req = 0;
        } else {
            buffer_size_req = destination_size;
            if (buffer_size_req < omx->drv_ctx.op_buf.buffer_size)
                buffer_size_req = omx->drv_ctx.op_buf.buffer_size;
            if (buffer_alignment_req < omx->drv_ctx.op_buf.alignment)
                buffer_alignment_req = omx->drv_ctx.op_buf.alignment;
        }
    }
fail_update_buf_req:
    pthread_mutex_unlock(&omx->c_lock);
    return status;
}

bool omx_vdec::allocate_color_convert_buf::set_color_format(
    OMX_COLOR_FORMATTYPE dest_color_format)
{
    bool status = true;
    OMX_COLOR_FORMATTYPE drv_color_format;
    if (!omx){
        DEBUG_PRINT_ERROR("\n Invalid client in color convert");
        return false;
    }
    pthread_mutex_lock(&omx->c_lock);
    if (omx->drv_ctx.output_format == VDEC_YUV_FORMAT_NV12)
        drv_color_format = (OMX_COLOR_FORMATTYPE)
        QOMX_COLOR_FORMATYUV420PackedSemiPlanar32m;
    else {
        DEBUG_PRINT_ERROR("\n Incorrect color format");
        status = false;
    }
    if (status && (drv_color_format != dest_color_format)) {
        DEBUG_PRINT_LOW("Enabling C2D\n");
        if (dest_color_format != OMX_COLOR_FormatYUV420Planar) {
            DEBUG_PRINT_ERROR("\n Unsupported color format for c2d");
            status = false;
        } else {
            ColorFormat = OMX_COLOR_FormatYUV420Planar;
            if (enabled)
                c2d.destroy();
            enabled = false;
            if (!c2d.init()) {
                DEBUG_PRINT_ERROR("\n open failed for c2d");
                status = false;
            } else
                enabled = true;
        }
    } else {
        if (enabled)
            c2d.destroy();
        enabled = false;
    }
    pthread_mutex_unlock(&omx->c_lock);
    return status;
}

OMX_BUFFERHEADERTYPE* omx_vdec::allocate_color_convert_buf::get_il_buf_hdr()
{
    if (!omx){
        DEBUG_PRINT_ERROR("\n Invalid param get_buf_hdr");
        return NULL;
    }
    if (!enabled)
        return omx->m_out_mem_ptr;
    return m_out_mem_ptr_client;
}

OMX_BUFFERHEADERTYPE* omx_vdec::allocate_color_convert_buf::get_il_buf_hdr
(OMX_BUFFERHEADERTYPE *bufadd)
{
    if (!omx){
        DEBUG_PRINT_ERROR("\n Invalid param get_buf_hdr");
        return NULL;
    }
    if (!enabled)
        return bufadd;

    unsigned index = 0;
    index = bufadd - omx->m_out_mem_ptr;
    if (index < omx->drv_ctx.op_buf.actualcount) {
        m_out_mem_ptr_client[index].nFlags = (bufadd->nFlags & OMX_BUFFERFLAG_EOS);
        m_out_mem_ptr_client[index].nTimeStamp = bufadd->nTimeStamp;
        bool status;
        if (!omx->in_reconfig && !omx->output_flush_progress && bufadd->nFilledLen) {
            pthread_mutex_lock(&omx->c_lock);
            status = c2d.convert(omx->drv_ctx.ptr_outputbuffer[index].pmem_fd,
                omx->m_out_mem_ptr->pBuffer, bufadd->pBuffer,pmem_fd[index],
                pmem_baseaddress[index], pmem_baseaddress[index]);
            pthread_mutex_unlock(&omx->c_lock);
            m_out_mem_ptr_client[index].nFilledLen = buffer_size_req;
            if (!status){
                DEBUG_PRINT_ERROR("\n Failed color conversion %d", status);
                m_out_mem_ptr_client[index].nFilledLen = 0;
                return &m_out_mem_ptr_client[index];
            }
        } else
            m_out_mem_ptr_client[index].nFilledLen = 0;
        return &m_out_mem_ptr_client[index];
    }
    DEBUG_PRINT_ERROR("\n Index messed up in the get_il_buf_hdr");
    return NULL;
}

OMX_BUFFERHEADERTYPE* omx_vdec::allocate_color_convert_buf::get_dr_buf_hdr
(OMX_BUFFERHEADERTYPE *bufadd)
{
    if (!omx){
        DEBUG_PRINT_ERROR("\n Invalid param get_buf_hdr");
        return NULL;
    }
    if (!enabled)
        return bufadd;
    unsigned index = 0;
    index = bufadd - m_out_mem_ptr_client;
    if (index < omx->drv_ctx.op_buf.actualcount) {
        return &omx->m_out_mem_ptr[index];
    }
    DEBUG_PRINT_ERROR("\n Index messed up in the get_dr_buf_hdr");
    return NULL;
}
bool omx_vdec::allocate_color_convert_buf::get_buffer_req
(unsigned int &buffer_size)
{
    bool status = true;
    pthread_mutex_lock(&omx->c_lock);
    if (!enabled)
        buffer_size = omx->drv_ctx.op_buf.buffer_size;
    else {
        if (!c2d.get_buffer_size(C2D_OUTPUT,buffer_size)) {
            DEBUG_PRINT_ERROR("\n Get buffer size failed");
            status = false;
            goto fail_get_buffer_size;
        }
    }
    if (buffer_size < omx->drv_ctx.op_buf.buffer_size)
        buffer_size = omx->drv_ctx.op_buf.buffer_size;
    if (buffer_alignment_req < omx->drv_ctx.op_buf.alignment)
        buffer_alignment_req = omx->drv_ctx.op_buf.alignment;
fail_get_buffer_size:
    pthread_mutex_unlock(&omx->c_lock);
    return status;
}
OMX_ERRORTYPE omx_vdec::allocate_color_convert_buf::free_output_buffer(
    OMX_BUFFERHEADERTYPE *bufhdr) {
        unsigned int index = 0;

        if (!enabled)
            return omx->free_output_buffer(bufhdr);
        if (enabled && omx->is_component_secure())
            return OMX_ErrorNone;
        if (!allocated_count || !bufhdr) {
            DEBUG_PRINT_ERROR("\n Color convert no buffer to be freed %p",bufhdr);
            return OMX_ErrorBadParameter;
        }
        index = bufhdr - m_out_mem_ptr_client;
        if (index >= omx->drv_ctx.op_buf.actualcount){
            DEBUG_PRINT_ERROR("\n Incorrect index color convert free_output_buffer");
            return OMX_ErrorBadParameter;
        }
        if (pmem_fd[index] > 0) {
            munmap(pmem_baseaddress[index], buffer_size_req);
            close(pmem_fd[index]);
        }
        pmem_fd[index] = -1;
#ifdef USE_ION
        omx->free_ion_memory(&op_buf_ion_info[index]);
#endif
        m_heap_ptr[index].video_heap_ptr = NULL;
        if (allocated_count > 0)
            allocated_count--;
        else
            allocated_count = 0;
        if (!allocated_count) {
            pthread_mutex_lock(&omx->c_lock);
            c2d.close();
            init_members();
            pthread_mutex_unlock(&omx->c_lock);
        }
        return omx->free_output_buffer(&omx->m_out_mem_ptr[index]);
}

OMX_ERRORTYPE omx_vdec::allocate_color_convert_buf::allocate_buffers_color_convert(OMX_HANDLETYPE hComp,
                                                                                   OMX_BUFFERHEADERTYPE **bufferHdr,OMX_U32 port,OMX_PTR appData,OMX_U32 bytes)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    if (!enabled){
        eRet = omx->allocate_output_buffer(hComp,bufferHdr,port,appData,bytes);
        return eRet;
    }
    if (enabled && omx->is_component_secure()) {
        DEBUG_PRINT_ERROR("\nNotin color convert mode secure_mode %d",
            omx->is_component_secure());
        return OMX_ErrorUnsupportedSetting;
    }
    if (!bufferHdr || bytes > buffer_size_req) {
        DEBUG_PRINT_ERROR("\n Invalid params allocate_buffers_color_convert %p", bufferHdr);
        DEBUG_PRINT_ERROR("\n color_convert buffer_size_req %d bytes %lu",
            buffer_size_req,bytes);
        return OMX_ErrorBadParameter;
    }
    if (allocated_count >= omx->drv_ctx.op_buf.actualcount) {
        DEBUG_PRINT_ERROR("\n Actual count err in allocate_buffers_color_convert");
        return OMX_ErrorInsufficientResources;
    }
    OMX_BUFFERHEADERTYPE *temp_bufferHdr = NULL;
    eRet = omx->allocate_output_buffer(hComp,&temp_bufferHdr,
        port,appData,omx->drv_ctx.op_buf.buffer_size);
    if (eRet != OMX_ErrorNone || !temp_bufferHdr){
        DEBUG_PRINT_ERROR("\n Buffer allocation failed color_convert");
        return eRet;
    }
    if ((temp_bufferHdr - omx->m_out_mem_ptr) >=
        (int)omx->drv_ctx.op_buf.actualcount) {
            DEBUG_PRINT_ERROR("\n Invalid header index %d",
                (temp_bufferHdr - omx->m_out_mem_ptr));
            return OMX_ErrorUndefined;
    }
    unsigned int i = allocated_count;
#ifdef USE_ION
    op_buf_ion_info[i].ion_device_fd = omx->alloc_map_ion_memory(
        buffer_size_req,buffer_alignment_req,
        &op_buf_ion_info[i].ion_alloc_data,&op_buf_ion_info[i].fd_ion_data,
        0);
    pmem_fd[i] = op_buf_ion_info[i].fd_ion_data.fd;
    if (op_buf_ion_info[i].ion_device_fd < 0) {
        DEBUG_PRINT_ERROR("\n alloc_map_ion failed in color_convert");
        return OMX_ErrorInsufficientResources;
    }
    pmem_baseaddress[i] = (unsigned char *)mmap(NULL,buffer_size_req,
        PROT_READ|PROT_WRITE,MAP_SHARED,pmem_fd[i],0);

    if (pmem_baseaddress[i] == MAP_FAILED) {
        DEBUG_PRINT_ERROR("\n MMAP failed for Size %d",buffer_size_req);
        close(pmem_fd[i]);
        omx->free_ion_memory(&op_buf_ion_info[i]);
        return OMX_ErrorInsufficientResources;
    }
    m_heap_ptr[i].video_heap_ptr = new VideoHeap (
        op_buf_ion_info[i].ion_device_fd,buffer_size_req,
        pmem_baseaddress[i],op_buf_ion_info[i].ion_alloc_data.handle,pmem_fd[i]);
#endif
    m_pmem_info_client[i].pmem_fd = (OMX_U32)m_heap_ptr[i].video_heap_ptr.get();
    m_pmem_info_client[i].offset = 0;
    m_platform_entry_client[i].entry = (void *)&m_pmem_info_client[i];
    m_platform_entry_client[i].type = OMX_QCOM_PLATFORM_PRIVATE_PMEM;
    m_platform_list_client[i].nEntries = 1;
    m_platform_list_client[i].entryList = &m_platform_entry_client[i];
    m_out_mem_ptr_client[i].pOutputPortPrivate = NULL;
    m_out_mem_ptr_client[i].nAllocLen = buffer_size_req;
    m_out_mem_ptr_client[i].nFilledLen = 0;
    m_out_mem_ptr_client[i].nFlags = 0;
    m_out_mem_ptr_client[i].nOutputPortIndex = OMX_CORE_OUTPUT_PORT_INDEX;
    m_out_mem_ptr_client[i].nSize = sizeof(OMX_BUFFERHEADERTYPE);
    m_out_mem_ptr_client[i].nVersion.nVersion = OMX_SPEC_VERSION;
    m_out_mem_ptr_client[i].pPlatformPrivate = &m_platform_list_client[i];
    m_out_mem_ptr_client[i].pBuffer = pmem_baseaddress[i];
    m_out_mem_ptr_client[i].pAppPrivate = appData;
    *bufferHdr = &m_out_mem_ptr_client[i];
    DEBUG_PRINT_ERROR("\n IL client buffer header %p", *bufferHdr);
    allocated_count++;
    return eRet;
}

bool omx_vdec::is_component_secure()
{
    return secure_mode;
}

bool omx_vdec::allocate_color_convert_buf::get_color_format(OMX_COLOR_FORMATTYPE &dest_color_format)
{
    bool status = true;
    if (!enabled) {
        if (omx->drv_ctx.output_format == VDEC_YUV_FORMAT_NV12)
            dest_color_format =  (OMX_COLOR_FORMATTYPE)
            QOMX_COLOR_FORMATYUV420PackedSemiPlanar32m;
        else
            status = false;
    } else {
        if (ColorFormat != OMX_COLOR_FormatYUV420Planar) {
            status = false;
        } else
            dest_color_format = OMX_COLOR_FormatYUV420Planar;
    }
    return status;
}


OMX_ERRORTYPE omx_vdec::get_buffer_req_swvdec()
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;

    if (!m_pSwVdec)
    {
        eRet=get_buffer_req(&drv_ctx.ip_buf);
        eRet=get_buffer_req(&drv_ctx.op_buf);
        return eRet;
    }

    SWVDEC_PROP property;
    if (m_swvdec_mode == SWVDEC_MODE_PARSE_DECODE)
    {
        property.ePropId = SWVDEC_PROP_ID_IPBUFFREQ;
        SWVDEC_STATUS sRet = SwVdec_GetProperty(m_pSwVdec, &property);
        if (sRet != SWVDEC_S_SUCCESS)
        {
            return OMX_ErrorUndefined;
        }
        else
        {
            drv_ctx.ip_buf.buffer_size = property.uProperty.sIpBuffReq.nSize;
            drv_ctx.ip_buf.mincount = property.uProperty.sIpBuffReq.nMinCount;
            drv_ctx.ip_buf.actualcount = property.uProperty.sIpBuffReq.nMinCount;
            DEBUG_PRINT_ERROR("swvdec input buf size %d count %d\n",drv_ctx.op_buf.buffer_size,drv_ctx.op_buf.actualcount);
        }
    }

    property.ePropId = SWVDEC_PROP_ID_OPBUFFREQ;
    SWVDEC_STATUS sRet = SwVdec_GetProperty(m_pSwVdec, &property);
    if (sRet != SWVDEC_S_SUCCESS)
    {
        return OMX_ErrorUndefined;
    }
    else
    {
        drv_ctx.op_buf.buffer_size = property.uProperty.sOpBuffReq.nSize;
        drv_ctx.op_buf.mincount = property.uProperty.sOpBuffReq.nMinCount;
        drv_ctx.op_buf.actualcount = property.uProperty.sOpBuffReq.nMinCount;
        DEBUG_PRINT_ERROR("swvdec output buf size %d count %d\n",drv_ctx.op_buf.buffer_size,drv_ctx.op_buf.actualcount);
    }

    if (m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY)
    {
        return get_buffer_req(&drv_ctx.interm_op_buf);
    }
    return OMX_ErrorNone;
}

OMX_ERRORTYPE omx_vdec::set_buffer_req_swvdec(vdec_allocatorproperty *buffer_prop)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;

    if (!m_pSwVdec)
    {
        eRet = set_buffer_req(buffer_prop);
        return eRet;
    }

    unsigned buf_size = 0;
    SWVDEC_PROP property;
    SWVDEC_STATUS sRet = SWVDEC_S_SUCCESS;

    DEBUG_PRINT_HIGH("set_buffer_req_swvdec IN: ActCnt(%d) Size(%d), buffer type %d",
        buffer_prop->actualcount, buffer_prop->buffer_size, buffer_prop->buffer_type);

    buf_size = (buffer_prop->buffer_size + buffer_prop->alignment - 1)&(~(buffer_prop->alignment - 1));
    if (buf_size != buffer_prop->buffer_size)
    {
        DEBUG_PRINT_ERROR("Buffer size alignment error: Requested(%d) Required(%d)",
            buffer_prop->buffer_size, buf_size);
        eRet = OMX_ErrorBadParameter;
    }
    else
    {
        property.uProperty.sIpBuffReq.nSize = buffer_prop->buffer_size;
        property.uProperty.sIpBuffReq.nMaxCount = buffer_prop->actualcount;
        property.uProperty.sIpBuffReq.nMinCount = buffer_prop->actualcount;

        if(buffer_prop->buffer_type == VDEC_BUFFER_TYPE_INPUT)
        {
            property.ePropId = SWVDEC_PROP_ID_IPBUFFREQ;
            DEBUG_PRINT_HIGH("swvdec input Buffer Size =%d Count = %d\n ",property.uProperty.sIpBuffReq.nSize, buffer_prop->mincount);
        }
        else if (buffer_prop->buffer_type == VDEC_BUFFER_TYPE_OUTPUT)
        {
            property.ePropId = SWVDEC_PROP_ID_OPBUFFREQ;
            DEBUG_PRINT_HIGH("swvdec output Buffer Size =%d and Count = %d\n ",property.uProperty.sOpBuffReq.nSize, buffer_prop->actualcount);
        }
        else
        {
            eRet = OMX_ErrorBadParameter;
        }

        if(eRet==OMX_ErrorNone)
        {
            sRet = SwVdec_SetProperty(m_pSwVdec, &property);
        }

        if (sRet != SWVDEC_S_SUCCESS)
        {
            DEBUG_PRINT_ERROR("Set buffer requirements from ARM codec failed");
            return OMX_ErrorInsufficientResources;
        }
    }

    if (buffer_prop->buffer_type == VDEC_BUFFER_TYPE_OUTPUT)
    {
        // need to update extradata buffers also
        drv_ctx.extradata_info.size = buffer_prop->actualcount * drv_ctx.extradata_info.buffer_size;
        drv_ctx.extradata_info.count = buffer_prop->actualcount;
        eRet = set_buffer_req(&drv_ctx.interm_op_buf);
    }

    return eRet;
}

/* ======================================================================
FUNCTION
omx_vdec::empty_this_buffer_proxy

DESCRIPTION
This routine is used to push the encoded video frames to
the video decoder.

PARAMETERS
None.

RETURN VALUE
OMX Error None if everything went successful.

========================================================================== */
OMX_ERRORTYPE  omx_vdec::empty_this_buffer_proxy_swvdec(OMX_IN OMX_HANDLETYPE hComp,
                                                        OMX_IN OMX_BUFFERHEADERTYPE* buffer)
{
    int push_cnt = 0,i=0;
    unsigned nPortIndex = 0;
    OMX_ERRORTYPE ret = OMX_ErrorNone;
    struct vdec_input_frameinfo frameinfo;
    struct vdec_bufferpayload *temp_buffer;
    struct vdec_seqheader seq_header;
    bool port_setting_changed = true;
    bool not_coded_vop = false;

    /*Should we generate a Aync error event*/
    if (buffer == NULL)
    {
        DEBUG_PRINT_ERROR("\nERROR:empty_this_buffer_proxy is invalid");
        return OMX_ErrorBadParameter;
    }

    nPortIndex = buffer-((OMX_BUFFERHEADERTYPE *)m_interm_mem_ptr);

    if (nPortIndex > drv_ctx.interm_op_buf.actualcount)
    {
        DEBUG_PRINT_ERROR("\nERROR:empty_this_buffer_proxy_swvdec invalid nPortIndex[%u]",
            nPortIndex);
        return OMX_ErrorBadParameter;
    }

    /* return zero length and not an EOS buffer */
    if ( (buffer->nFilledLen == 0) &&
        ((buffer->nFlags & OMX_BUFFERFLAG_EOS) == 0))
    {
        DEBUG_PRINT_HIGH("\n return zero legth buffer");
        post_event ((unsigned int)buffer,VDEC_S_SUCCESS,
            OMX_COMPONENT_GENERATE_EBD_SWVDEC);
        return OMX_ErrorNone;
    }

    if(m_interm_bEnabled != OMX_TRUE || m_interm_flush_swvdec_progress == true)
    {
        DEBUG_PRINT_LOW("\n m_interm_flush_swvdec_progress buf %d", nPortIndex);
        return OMX_ErrorNone;
    }

    // send this buffer to swvdec
    DEBUG_PRINT_LOW("\n empty_this_buffer_proxy_swvdec bufHdr %p pBuffer %p nFilledLen %lu m_pSwVdecIpBuffer %p, idx %d nFlags %x",
        buffer, buffer->pBuffer, buffer->nFilledLen, m_pSwVdecIpBuffer, nPortIndex, (unsigned int)buffer->nFlags);
    m_pSwVdecIpBuffer[nPortIndex].nFlags = buffer->nFlags;
    m_pSwVdecIpBuffer[nPortIndex].nFilledLen = buffer->nFilledLen;
    m_pSwVdecIpBuffer[nPortIndex].nIpTimestamp = buffer->nTimeStamp;
    if (SwVdec_EmptyThisBuffer(m_pSwVdec, &m_pSwVdecIpBuffer[nPortIndex]) != SWVDEC_S_SUCCESS) {
        ret = OMX_ErrorBadParameter;
    }
    pthread_mutex_lock(&m_lock);
    m_interm_buf_state[nPortIndex] = WITH_SWVDEC;
    pthread_mutex_unlock(&m_lock);
    return ret;
}

OMX_ERRORTYPE omx_vdec::empty_buffer_done_swvdec(OMX_HANDLETYPE hComp,
                                                 OMX_BUFFERHEADERTYPE* buffer)
{
    int idx = buffer - m_interm_mem_ptr;
    if (buffer == NULL || idx > (int)drv_ctx.interm_op_buf.actualcount)
    {
        DEBUG_PRINT_ERROR("\n empty_buffer_done_swvdec: ERROR bufhdr = %p", buffer);
        return OMX_ErrorBadParameter;
    }

    DEBUG_PRINT_LOW("\n empty_buffer_done_swvdec: bufhdr = %p, bufhdr->pBuffer = %p idx %d",
        buffer, buffer->pBuffer, idx);

    buffer->nFilledLen = 0;
    pthread_mutex_lock(&m_lock);
    m_interm_buf_state[idx] = WITH_COMPONENT;
    pthread_mutex_unlock(&m_lock);

    // need to hold the buffer if the state is not executing or in flushing
    if(m_interm_bEnabled != OMX_TRUE || m_interm_flush_dsp_progress == true)
    {
        DEBUG_PRINT_LOW("empty_buffer_done_swvdec: Buffer (%p) flushed idx %d", buffer, idx);
        buffer->nFilledLen = 0;
        buffer->nTimeStamp = 0;
        buffer->nFlags &= ~OMX_BUFFERFLAG_EXTRADATA;
        buffer->nFlags &= ~QOMX_VIDEO_BUFFERFLAG_EOSEQ;
        buffer->nFlags &= ~OMX_BUFFERFLAG_DATACORRUPT;
        return OMX_ErrorNone;
    }

    // call DSP FTB for the intermediate buffer. post event to the command queue do it asynchrounously
    if (m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY && in_reconfig != true)
    {
        post_event((unsigned int)&m_cmp, (unsigned int)buffer, OMX_COMPONENT_GENERATE_FTB_DSP);
    }
    else if (m_swvdec_mode == SWVDEC_MODE_PARSE_DECODE)
    {
        post_event((unsigned int)&m_cmp, (unsigned int)buffer, OMX_COMPONENT_GENERATE_EBD);
    }

    return OMX_ErrorNone;
}

OMX_ERRORTYPE omx_vdec::fill_all_buffers_proxy_dsp(OMX_HANDLETYPE hComp)
{
    int idx = 0;
    OMX_ERRORTYPE nRet = OMX_ErrorNone;
    if (m_fill_internal_bufers == OMX_FALSE)
    {
        return nRet;
    }
    m_fill_internal_bufers = OMX_FALSE;

    for (idx=0; idx < (int)drv_ctx.interm_op_buf.actualcount; idx++)
    {
        pthread_mutex_lock(&m_lock);
        if (m_interm_buf_state[idx] == WITH_COMPONENT)
        {
            OMX_BUFFERHEADERTYPE* bufHdr = m_interm_mem_ptr + idx;
            nRet = fill_this_buffer_proxy_dsp(hComp, bufHdr);
            if (nRet != OMX_ErrorNone)
            {
                DEBUG_PRINT_ERROR("fill_this_buffer_proxy_dsp failed for buff %d bufHdr %p pBuffer %p\n",
                    idx, bufHdr, bufHdr->pBuffer);
                break;
            }
        }
        pthread_mutex_unlock(&m_lock);
    }
    return nRet;
}


/* ======================================================================
FUNCTION
omx_vdec::fill_this_buffer_proxy_dsp

DESCRIPTION
IL client uses this method to release the frame buffer
after displaying them.

PARAMETERS
None.

RETURN VALUE
true/false

========================================================================== */
OMX_ERRORTYPE  omx_vdec::fill_this_buffer_proxy_dsp(
    OMX_IN OMX_HANDLETYPE hComp,
    OMX_IN OMX_BUFFERHEADERTYPE* bufferAdd)
{
    OMX_ERRORTYPE nRet = OMX_ErrorNone;
    OMX_BUFFERHEADERTYPE *buffer = bufferAdd;
    unsigned nPortIndex = 0;
    struct vdec_fillbuffer_cmd fillbuffer;
    struct vdec_bufferpayload     *ptr_outputbuffer = NULL;
    struct vdec_output_frameinfo  *ptr_respbuffer = NULL;
    int rc = 0;

    if(m_state == OMX_StateInvalid)
    {
        DEBUG_PRINT_ERROR("FTB in Invalid State\n");
        return OMX_ErrorInvalidState;
    }

    nPortIndex = buffer-((OMX_BUFFERHEADERTYPE *)m_interm_mem_ptr);

    if (bufferAdd == NULL || nPortIndex > drv_ctx.interm_op_buf.actualcount)
        return OMX_ErrorBadParameter;

    DEBUG_PRINT_LOW("\n fill_this_buffer_proxy_dsp: bufhdr = %p,pBuffer = %p, idx %d, state %d",
        bufferAdd, bufferAdd->pBuffer, nPortIndex, m_interm_buf_state[nPortIndex]);

    pthread_mutex_lock(&m_lock);
    if (m_interm_buf_state[nPortIndex] == WITH_DSP)
    {
        DEBUG_PRINT_HIGH("\n fill_this_buffer_proxy_dsp: buffer is with dsp\n");
        pthread_mutex_unlock(&m_lock);
        return OMX_ErrorNone;
    }
    pthread_mutex_unlock(&m_lock);

    ptr_respbuffer = (struct vdec_output_frameinfo*)buffer->pOutputPortPrivate;
    if (ptr_respbuffer)
    {
        ptr_outputbuffer = (struct vdec_bufferpayload*)ptr_respbuffer->client_data;
    }

    if (ptr_respbuffer == NULL || ptr_outputbuffer == NULL)
    {
        DEBUG_PRINT_ERROR("resp buffer or outputbuffer is NULL");
        buffer->nFilledLen = 0;
        return OMX_ErrorBadParameter;
    }

    if(m_interm_bEnabled != OMX_TRUE || m_interm_flush_dsp_progress == true)
    {
        DEBUG_PRINT_LOW("\n interm output buffers return flush/disable condition");
        buffer->nFilledLen = 0;
        return OMX_ErrorNone;
    }

    memcpy (&fillbuffer.buffer,ptr_outputbuffer,
        sizeof(struct vdec_bufferpayload));
    fillbuffer.client_data = bufferAdd;

    struct v4l2_buffer buf;
    struct v4l2_plane plane[VIDEO_MAX_PLANES];
    memset( (void *)&buf, 0, sizeof(buf));
    memset( (void *)plane, 0, (sizeof(struct v4l2_plane)*VIDEO_MAX_PLANES));
    int extra_idx = 0;

    buf.index = nPortIndex;
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_USERPTR;
    plane[0].bytesused = buffer->nFilledLen;
    plane[0].length = drv_ctx.interm_op_buf.buffer_size;
    plane[0].m.userptr =
        (unsigned long)drv_ctx.ptr_interm_outputbuffer[nPortIndex].bufferaddr -
        (unsigned long)drv_ctx.ptr_interm_outputbuffer[nPortIndex].offset;

    plane[0].reserved[0] = drv_ctx.ptr_interm_outputbuffer[nPortIndex].pmem_fd;
    plane[0].reserved[1] = drv_ctx.ptr_interm_outputbuffer[nPortIndex].offset;
    plane[0].data_offset = 0;

    extra_idx = EXTRADATA_IDX(drv_ctx.num_planes);
    if (extra_idx && (extra_idx < VIDEO_MAX_PLANES)) {
        plane[extra_idx].bytesused = 0;
        plane[extra_idx].length = drv_ctx.extradata_info.buffer_size;
        plane[extra_idx].m.userptr = (long unsigned int) (drv_ctx.extradata_info.uaddr + nPortIndex * drv_ctx.extradata_info.buffer_size);
        plane[extra_idx].reserved[0] = drv_ctx.extradata_info.ion.fd_ion_data.fd;
        plane[extra_idx].reserved[1] = nPortIndex * drv_ctx.extradata_info.buffer_size;
        plane[extra_idx].data_offset = 0;
    } else if (extra_idx >= VIDEO_MAX_PLANES) {
        DEBUG_PRINT_ERROR("Extradata index higher than expected: %d\n", extra_idx);
        return OMX_ErrorBadParameter;
    }

    buf.m.planes = plane;
    buf.length = drv_ctx.num_planes;
    rc = ioctl(drv_ctx.video_driver_fd, VIDIOC_QBUF, &buf);
    if (rc) {
        DEBUG_PRINT_ERROR("Failed to qbuf to driver");
        return OMX_ErrorBadParameter;
    }

    pthread_mutex_lock(&m_lock);
    m_interm_buf_state[nPortIndex] = WITH_DSP;
    pthread_mutex_unlock(&m_lock);

    return OMX_ErrorNone;
}

static FILE* dsp_output = NULL;


int dump_dsp_buffer(OMX_BUFFERHEADERTYPE * buffer)
{
    if (dsp_output==NULL)
    {
        dsp_output = fopen ("/data/output.dsp", "wb");
    }
    if (dsp_output==NULL)
    {
        return -1;
    }

    if (buffer->nFilledLen)
    {
        fwrite(&buffer->nFilledLen, sizeof(buffer->nFilledLen), 1, dsp_output);
        fwrite(buffer->pBuffer, sizeof(uint8), buffer->nFilledLen, dsp_output);
    }
    return 0;
}


OMX_ERRORTYPE omx_vdec::fill_buffer_done_dsp(OMX_HANDLETYPE hComp,
                                             OMX_BUFFERHEADERTYPE * buffer)
{
    int idx = buffer - m_interm_mem_ptr;
    if (!buffer || idx >= (int)drv_ctx.interm_op_buf.actualcount)
    {
        DEBUG_PRINT_ERROR("\n [FBD] ERROR in ptr(%p)", buffer);
        return OMX_ErrorBadParameter;
    }

    DEBUG_PRINT_LOW("\n fill_buffer_done_dsp: bufhdr = %p, bufhdr->pBuffer = %p, idx %d nFilledLen %lu nFlags %x",
        buffer, buffer->pBuffer, idx, buffer->nFilledLen, (unsigned int)buffer->nFlags);

    pthread_mutex_lock(&m_lock);
    m_interm_buf_state[idx] = WITH_COMPONENT;
    pthread_mutex_unlock(&m_lock);

    if (m_interm_bEnabled != OMX_TRUE || m_interm_flush_swvdec_progress)
    {
        DEBUG_PRINT_LOW("fill_buffer_done_dsp: Buffer (%p) flushed idx %d", buffer, idx);
        buffer->nFilledLen = 0;
        buffer->nTimeStamp = 0;
        buffer->nFlags &= ~OMX_BUFFERFLAG_EXTRADATA;
        buffer->nFlags &= ~QOMX_VIDEO_BUFFERFLAG_EOSEQ;
        buffer->nFlags &= ~OMX_BUFFERFLAG_DATACORRUPT;
        return OMX_ErrorNone;
    }

    if (buffer->nFlags & OMX_BUFFERFLAG_EOS)
    {
        DEBUG_PRINT_HIGH("\n interm EOS has been reached");

        if (psource_frame)
        {
            m_cb.EmptyBufferDone(&m_cmp, m_app_data, psource_frame);
            psource_frame = NULL;
        }
        if (pdest_frame)
        {
            pdest_frame->nFilledLen = 0;
            m_input_free_q.insert_entry((unsigned) pdest_frame,(unsigned)NULL,
                (unsigned)NULL);
            pdest_frame = NULL;
        }
    }

    // dump_dsp_buffer(buffer);

    post_event((unsigned int)&m_cmp, (unsigned int)buffer, OMX_COMPONENT_GENERATE_ETB_SWVDEC);
    return OMX_ErrorNone;
}

OMX_ERRORTYPE  omx_vdec::allocate_interm_buffer(OMX_U32 bytes)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    OMX_BUFFERHEADERTYPE *bufHdr= NULL; // buffer header
    unsigned                         i= 0; // Temporary counter
    struct vdec_setbuffer_cmd setbuffers;
    int extra_idx = 0;

    int ion_device_fd =-1;
    struct ion_allocation_data ion_alloc_data;
    struct ion_fd_data fd_ion_data;

    if(!m_interm_mem_ptr)
    {
        DEBUG_PRINT_HIGH("\n Allocate interm buffer Header: Cnt(%d) Sz(%d)",
            drv_ctx.interm_op_buf.actualcount,  drv_ctx.interm_op_buf.buffer_size);

        int nBufHdrSize = drv_ctx.interm_op_buf.actualcount * sizeof(OMX_BUFFERHEADERTYPE);
        m_interm_mem_ptr = (OMX_BUFFERHEADERTYPE  *)calloc(nBufHdrSize,1);

        drv_ctx.ptr_interm_outputbuffer = (struct vdec_bufferpayload *)
            calloc (sizeof(struct vdec_bufferpayload), drv_ctx.interm_op_buf.actualcount);
        drv_ctx.ptr_interm_respbuffer = (struct vdec_output_frameinfo  *)
            calloc (sizeof (struct vdec_output_frameinfo), drv_ctx.interm_op_buf.actualcount);
        drv_ctx.interm_op_buf_ion_info = (struct vdec_ion *)
            calloc (sizeof(struct vdec_ion), drv_ctx.interm_op_buf.actualcount);
        m_pSwVdecIpBuffer = (SWVDEC_IPBUFFER *)calloc(sizeof(SWVDEC_IPBUFFER), drv_ctx.interm_op_buf.actualcount);

        if (m_interm_mem_ptr == NULL ||
            drv_ctx.ptr_interm_outputbuffer == NULL ||
            drv_ctx.ptr_interm_respbuffer == NULL ||
            drv_ctx.interm_op_buf_ion_info == NULL ||
            m_pSwVdecIpBuffer == NULL)
        {
            goto clean_up;
        }
    }

    bufHdr = m_interm_mem_ptr;
    for (int i = 0; i < drv_ctx.interm_op_buf.actualcount; i++)
    {
        int pmem_fd = -1;
        unsigned char *pmem_baseaddress = NULL;
        int flags = secure_mode ? ION_SECURE : 0;
        if (m_pSwVdec)
        {
            DEBUG_PRINT_HIGH("\n Allocate cached interm buffers \n");
            flags = ION_FLAG_CACHED;
        }

        DEBUG_PRINT_HIGH("allocate interm output buffer size %d idx %d\n",
            drv_ctx.interm_op_buf.buffer_size, i);
        ion_device_fd = alloc_map_ion_memory(
            drv_ctx.interm_op_buf.buffer_size,
            drv_ctx.interm_op_buf.alignment,
            &ion_alloc_data, &fd_ion_data, flags, ION_ADSP_HEAP_ID);
        if (ion_device_fd < 0) {
            eRet = OMX_ErrorInsufficientResources;
            goto clean_up;
        }
        pmem_fd = fd_ion_data.fd;

        drv_ctx.ptr_interm_outputbuffer[i].pmem_fd = pmem_fd;
        drv_ctx.interm_op_buf_ion_info[i].ion_device_fd = ion_device_fd;
        drv_ctx.interm_op_buf_ion_info[i].ion_alloc_data = ion_alloc_data;
        drv_ctx.interm_op_buf_ion_info[i].fd_ion_data = fd_ion_data;

        if (!secure_mode) {
            pmem_baseaddress = (unsigned char *)mmap(NULL,
                drv_ctx.interm_op_buf.buffer_size,
                PROT_READ|PROT_WRITE,MAP_SHARED, pmem_fd, 0);
            if (pmem_baseaddress == MAP_FAILED)
            {
                DEBUG_PRINT_ERROR("\n MMAP failed for Size %d",
                    drv_ctx.interm_op_buf.buffer_size);
                eRet = OMX_ErrorInsufficientResources;
                goto clean_up;
            }
        }

        bufHdr->nSize              = sizeof(OMX_BUFFERHEADERTYPE);
        bufHdr->nVersion.nVersion  = OMX_SPEC_VERSION;
        bufHdr->nAllocLen          = bytes;
        bufHdr->nFilledLen         = 0;
        bufHdr->pAppPrivate        = this;
        bufHdr->nOutputPortIndex   = OMX_CORE_OUTPUT_PORT_INDEX;
        bufHdr->pBuffer            = pmem_baseaddress;
        bufHdr->nOffset            = 0;

        bufHdr->pOutputPortPrivate = &drv_ctx.ptr_interm_respbuffer[i];
        drv_ctx.ptr_interm_respbuffer[i].client_data = (void *)&drv_ctx.ptr_interm_outputbuffer[i];
        drv_ctx.ptr_interm_outputbuffer[i].offset = 0;
        drv_ctx.ptr_interm_outputbuffer[i].bufferaddr = pmem_baseaddress;
        drv_ctx.ptr_interm_outputbuffer[i].mmaped_size = drv_ctx.interm_op_buf.buffer_size;
        drv_ctx.ptr_interm_outputbuffer[i].buffer_len = drv_ctx.interm_op_buf.buffer_size;

        DEBUG_PRINT_LOW("\n interm pmem_fd = %d offset = %d address = %p, bufHdr %p",
            pmem_fd, drv_ctx.ptr_interm_outputbuffer[i].offset,
            drv_ctx.ptr_interm_outputbuffer[i].bufferaddr, bufHdr);

        m_interm_buf_state[i] = WITH_COMPONENT;
        m_pSwVdecIpBuffer[i].pBuffer = bufHdr->pBuffer;
        m_pSwVdecIpBuffer[i].pClientBufferData = (void*)i;

        // Move the buffer and buffer header pointers
        bufHdr++;
    }

    eRet = allocate_extradata();
    if (eRet != OMX_ErrorNone)
    {
        goto clean_up;
    }

    for(i=0; i<drv_ctx.interm_op_buf.actualcount; i++)
    {
        struct v4l2_buffer buf;
        struct v4l2_plane plane[VIDEO_MAX_PLANES];
        int rc;

        bufHdr = (m_interm_mem_ptr + i );
        if (secure_mode) {
            drv_ctx.ptr_interm_outputbuffer[i].bufferaddr = bufHdr;
        }

        buf.index = i;
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_USERPTR;
        plane[0].length = drv_ctx.interm_op_buf.buffer_size;
        plane[0].m.userptr = (unsigned long)drv_ctx.ptr_interm_outputbuffer[i].bufferaddr -
            (unsigned long)drv_ctx.ptr_interm_outputbuffer[i].offset;
        plane[0].reserved[0] = drv_ctx.interm_op_buf_ion_info[i].fd_ion_data.fd;
        plane[0].reserved[1] = drv_ctx.ptr_interm_outputbuffer[i].offset;
        plane[0].data_offset = 0;
        extra_idx = EXTRADATA_IDX(drv_ctx.num_planes);
        if (extra_idx && (extra_idx < VIDEO_MAX_PLANES)) {
            plane[extra_idx].length = drv_ctx.extradata_info.buffer_size;
            plane[extra_idx].m.userptr = (long unsigned int) (drv_ctx.extradata_info.uaddr
                + i * drv_ctx.extradata_info.buffer_size);
            plane[extra_idx].reserved[0] = drv_ctx.extradata_info.ion.fd_ion_data.fd;
            plane[extra_idx].reserved[1] = i * drv_ctx.extradata_info.buffer_size;
            plane[extra_idx].data_offset = 0;
        } else if (extra_idx >= VIDEO_MAX_PLANES) {
            DEBUG_PRINT_ERROR("Extradata index higher than allowed: %d\n", extra_idx);
            goto clean_up;
        }
        buf.m.planes = plane;
        buf.length = drv_ctx.num_planes;
        DEBUG_PRINT_LOW("\n Set interm Output Buffer Idx: %d Addr: %x", i, (unsigned int)drv_ctx.ptr_interm_outputbuffer[i].bufferaddr);
        rc = ioctl(drv_ctx.video_driver_fd, VIDIOC_PREPARE_BUF, &buf);
        if (rc) {
            DEBUG_PRINT_ERROR("VIDIOC_PREPARE_BUF failed\n");
            goto clean_up;
        }

        if (i == (drv_ctx.interm_op_buf.actualcount -1 ) && !streaming[CAPTURE_PORT]) {
            enum v4l2_buf_type buf_type;
            buf_type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            rc=ioctl(drv_ctx.video_driver_fd, VIDIOC_STREAMON,&buf_type);
            if (rc) {
                return OMX_ErrorInsufficientResources;
            } else {
                streaming[CAPTURE_PORT] = true;
                DEBUG_PRINT_LOW("\n STREAMON Successful \n ");
            }
        }

        bufHdr->pAppPrivate = this;
    }

    m_interm_bEnabled = OMX_TRUE;
    m_interm_bPopulated = OMX_TRUE;

    return OMX_ErrorNone;

clean_up:

    if (drv_ctx.interm_op_buf_ion_info)
    {
        for(i=0; i< drv_ctx.interm_op_buf.actualcount; i++)
        {
            close(drv_ctx.ptr_interm_outputbuffer[i].pmem_fd);
            drv_ctx.ptr_interm_outputbuffer[i].pmem_fd = 0;
            free_ion_memory(&drv_ctx.interm_op_buf_ion_info[i]);
        }
    }

    if(m_interm_mem_ptr)
    {
        free(m_interm_mem_ptr);
        m_interm_mem_ptr = NULL;
    }
    if(drv_ctx.ptr_interm_outputbuffer)
    {
        free(drv_ctx.ptr_interm_outputbuffer);
        drv_ctx.ptr_interm_outputbuffer = NULL;
    }
    if(drv_ctx.ptr_interm_respbuffer)
    {
        free(drv_ctx.ptr_interm_respbuffer);
        drv_ctx.ptr_interm_respbuffer = NULL;
    }
    if (drv_ctx.interm_op_buf_ion_info) {
        DEBUG_PRINT_LOW("\n Free o/p ion context");
        free(drv_ctx.interm_op_buf_ion_info);
        drv_ctx.interm_op_buf_ion_info = NULL;
    }
    return OMX_ErrorInsufficientResources;
}

//callback function used by SWVdec

SWVDEC_STATUS omx_vdec::swvdec_input_buffer_done_cb
(
 SWVDEC_HANDLE pSwDec,
 SWVDEC_IPBUFFER *m_pSwVdecIpBuffer,
 void *pClientHandle
 )
{
    SWVDEC_STATUS eRet = SWVDEC_S_SUCCESS;
    omx_vdec *omx = reinterpret_cast<omx_vdec*>(pClientHandle);

    if (m_pSwVdecIpBuffer == NULL)
    {
        eRet = SWVDEC_S_EFAIL;
    }
    else
    {
        omx->swvdec_input_buffer_done(m_pSwVdecIpBuffer);
    }

    return eRet;
}

void omx_vdec::swvdec_input_buffer_done(SWVDEC_IPBUFFER *m_pSwVdecIpBuffer)
{
    int index = (int)m_pSwVdecIpBuffer->pClientBufferData;

    if (m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY)
    {
        post_event((unsigned int)(m_interm_mem_ptr + index),
            VDEC_S_SUCCESS, OMX_COMPONENT_GENERATE_EBD_SWVDEC);
    }
    else
    {
        post_event((unsigned int)(m_inp_mem_ptr + index),
            VDEC_S_SUCCESS, OMX_COMPONENT_GENERATE_EBD);
    }
}

SWVDEC_STATUS omx_vdec::swvdec_fill_buffer_done_cb
(
 SWVDEC_HANDLE pSwDec,
 SWVDEC_OPBUFFER *m_pSwVdecOpBuffer,
 void *pClientHandle
 )
{
    SWVDEC_STATUS eRet = SWVDEC_S_SUCCESS;
    omx_vdec *omx = reinterpret_cast<omx_vdec*>(pClientHandle);

    if (m_pSwVdecOpBuffer == NULL)
    {
        eRet = SWVDEC_S_EFAIL;
    }
    else
    {
        omx->swvdec_fill_buffer_done(m_pSwVdecOpBuffer);
    }
    return eRet;
}

void omx_vdec::swvdec_fill_buffer_done(SWVDEC_OPBUFFER *m_pSwVdecOpBuffer)
{
    int index = (int)m_pSwVdecOpBuffer->pClientBufferData;
    OMX_BUFFERHEADERTYPE *bufHdr = m_out_mem_ptr + index;

    bufHdr->nFilledLen = m_pSwVdecOpBuffer->nFilledLen;
    bufHdr->nFlags = m_pSwVdecOpBuffer->nFlags;
    bufHdr->nTimeStamp = m_pSwVdecOpBuffer->nOpTimestamp;

    if (bufHdr->nFlags & OMX_BUFFERFLAG_EOS)
    {
        DEBUG_PRINT_ERROR("swvdec output EOS reached\n");
    }

    DEBUG_PRINT_LOW("swvdec_fill_buffer_done bufHdr %p pBuffer %p %p idx %d nFilledLen %d nFlags %x\n",
        bufHdr, bufHdr->pBuffer, m_pSwVdecOpBuffer->pBuffer, index, m_pSwVdecOpBuffer->nFilledLen, m_pSwVdecOpBuffer->nFlags);
    post_event((unsigned int)bufHdr, VDEC_S_SUCCESS, OMX_COMPONENT_GENERATE_FBD);
}

SWVDEC_STATUS omx_vdec::swvdec_handle_event_cb
(
    SWVDEC_HANDLE pSwDec,
    SWVDEC_EVENTHANDLER* pEventHandler,
    void *pClientHandle
)
{
    omx_vdec *omx = reinterpret_cast<omx_vdec*>(pClientHandle);
    omx->swvdec_handle_event(pEventHandler);
    return SWVDEC_S_SUCCESS;
}

void omx_vdec::swvdec_handle_event(SWVDEC_EVENTHANDLER *pEvent)
{
    switch(pEvent->eEvent)
    {
    case SWVDEC_FLUSH_DONE:
        DEBUG_PRINT_ERROR("SWVDEC_FLUSH_DONE input_flush_progress %d output_flush_progress %d\n",
            input_flush_progress, output_flush_progress);
        if (input_flush_progress)
        {
            post_event ((unsigned)NULL, VDEC_S_SUCCESS, OMX_COMPONENT_GENERATE_EVENT_INPUT_FLUSH);
        }
        if (output_flush_progress)
        {
            post_event ((unsigned)NULL, VDEC_S_SUCCESS, OMX_COMPONENT_GENERATE_EVENT_OUTPUT_FLUSH);
        }
        break;

    case SWVDEC_RECONFIG_SUFFICIENT_RESOURCES:
        {
            DEBUG_PRINT_HIGH("\n swvdec port settings changed info");

            // get_buffer_req and populate port defn structure
            SWVDEC_PROP prop;
            prop.ePropId = SWVDEC_PROP_ID_DIMENSIONS;
            SwVdec_GetProperty(m_pSwVdec, &prop);

            update_resolution(prop.uProperty.sDimensions.nWidth, prop.uProperty.sDimensions.nHeight);
            drv_ctx.video_resolution.stride = (prop.uProperty.sDimensions.nWidth + 127) & (~127);
            drv_ctx.video_resolution.scan_lines = (prop.uProperty.sDimensions.nHeight + 31) & (~31);

            m_port_def.nPortIndex = 1;
            update_portdef(&m_port_def);
            post_event ((unsigned)NULL, VDEC_S_SUCCESS, OMX_COMPONENT_GENERATE_INFO_PORT_RECONFIG);
        }
        break;

    case SWVDEC_RECONFIG_INSUFFICIENT_RESOURCES:
        DEBUG_PRINT_HIGH("\n swvdec port settings changed");
        in_reconfig = true;
        post_event (OMX_CORE_OUTPUT_PORT_INDEX, OMX_IndexParamPortDefinition,
            OMX_COMPONENT_GENERATE_PORT_RECONFIG);
        break;

    case SWVDEC_ERROR:
        break;

    default:
        break;
    }

    // put into the event command q
    // m_cmd_q.insert_entry((unsigned int)NULL,
    //                    SWVDEC_S_SUCCESS,
    //                        OMX_COMPONENT_GENERATE_STOP_DONE_SWVDEC);
    //   post_message(this, OMX_COMPONENT_GENERATE_STOP_DONE_SWVDEC);
}

bool omx_vdec::execute_input_flush_swvdec()
{
    unsigned       i =0;
    unsigned      p1 = 0; // Parameter - 1
    unsigned      p2 = 0; // Parameter - 2
    unsigned      ident = 0;
    bool bRet = true;

    DEBUG_PRINT_LOW("\n execute_input_flush_swvdec qsize %d, actual %d",
        m_etb_q_swvdec.m_size, drv_ctx.interm_op_buf.actualcount);

    pthread_mutex_lock(&m_lock);
    while (m_etb_q_swvdec.m_size)
    {
        m_etb_q_swvdec.pop_entry(&p1,&p2,&ident);
    }
    pthread_mutex_unlock(&m_lock);

    for (int idx = 0; idx < (int)drv_ctx.interm_op_buf.actualcount; idx++)
    {
        DEBUG_PRINT_LOW("\n Flush swvdec interm bufq idx %d, state %d", idx, m_interm_buf_state[idx]);
        // m_interm_buf_state[idx] = WITH_COMPONENT;
    }

    m_interm_flush_swvdec_progress = false;
    return true;
}


bool omx_vdec::execute_output_flush_dsp()
{
    unsigned       i =0;
    unsigned      p1 = 0; // Parameter - 1
    unsigned      p2 = 0; // Parameter - 2
    unsigned      ident = 0;
    bool bRet = true;

    DEBUG_PRINT_LOW("\n execute_output_flush_dsp qsize %d, actual %d",
        m_ftb_q_dsp.m_size, drv_ctx.interm_op_buf.actualcount);

    pthread_mutex_lock(&m_lock);
    while (m_ftb_q_dsp.m_size)
    {
        m_ftb_q_dsp.pop_entry(&p1,&p2,&ident);
    }
    m_interm_flush_dsp_progress = false;
    m_fill_internal_bufers = OMX_TRUE;
    pthread_mutex_unlock(&m_lock);

    for (int idx = 0; idx < (int)drv_ctx.interm_op_buf.actualcount; idx++)
    {
        DEBUG_PRINT_LOW("\n Flush dsp interm bufq idx %d, state %d", idx, m_interm_buf_state[idx]);
        // m_interm_buf_state[idx] = WITH_COMPONENT;
    }
    return true;
}

OMX_ERRORTYPE omx_vdec::free_interm_buffers()
{
    free_extradata();

    if (drv_ctx.ptr_interm_outputbuffer)
    {
        for(int i=0; i< drv_ctx.interm_op_buf.actualcount; i++)
        {
            if (drv_ctx.ptr_interm_outputbuffer[i].pmem_fd > 0)
            {
                DEBUG_PRINT_LOW("\n Free interm ouput Buffer index = %d addr = %x", i,
                    (unsigned int)drv_ctx.ptr_interm_outputbuffer[i].bufferaddr);

                munmap (drv_ctx.ptr_interm_outputbuffer[i].bufferaddr,
                    drv_ctx.ptr_interm_outputbuffer[i].mmaped_size);
                close(drv_ctx.ptr_interm_outputbuffer[i].pmem_fd);
                drv_ctx.ptr_interm_outputbuffer[i].pmem_fd = 0;
                free_ion_memory(&drv_ctx.interm_op_buf_ion_info[i]);
            }
        }
    }

    if (m_interm_mem_ptr)
    {
        free(m_interm_mem_ptr);
        m_interm_mem_ptr = NULL;
    }

    if (drv_ctx.ptr_interm_respbuffer)
    {
        free (drv_ctx.ptr_interm_respbuffer);
        drv_ctx.ptr_interm_respbuffer = NULL;
    }

    if (drv_ctx.ptr_interm_outputbuffer)
    {
        free (drv_ctx.ptr_interm_outputbuffer);
        drv_ctx.ptr_interm_outputbuffer = NULL;
    }

    if (drv_ctx.interm_op_buf_ion_info) {
        free(drv_ctx.interm_op_buf_ion_info);
        drv_ctx.interm_op_buf_ion_info = NULL;
    }

    if (!in_reconfig || m_swvdec_mode == SWVDEC_MODE_DECODE_ONLY)
    {
        if (m_pSwVdecIpBuffer)
        {
            free(m_pSwVdecIpBuffer);
            m_pSwVdecIpBuffer = NULL;
        }
    }

    if (m_pSwVdecOpBuffer)
    {
        free(m_pSwVdecOpBuffer);
        m_pSwVdecOpBuffer = NULL;
    }

    m_interm_bEnabled = OMX_FALSE;
    m_interm_bPopulated = OMX_FALSE;
    return OMX_ErrorNone;
}
