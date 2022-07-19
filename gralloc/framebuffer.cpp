/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sys/mman.h>

#include <dlfcn.h>

#include <cutils/ashmem.h>
#include <cutils/log.h>

#include <hardware/hardware.h>
#include <hardware/gralloc.h>

#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>
#include <stdlib.h>

#include <cutils/log.h>
#include <cutils/atomic.h>

#if HAVE_ANDROID_OS
#include <linux/fb.h>
#endif

#ifdef FSL_EPDC_FB
#include <linux/mxcfb.h>
#include "gralloc_epdc.h"
#endif

#include "gralloc_priv.h"
#include "gr.h"

/*****************************************************************************/

// numbers of buffers for page flipping
#define NUM_BUFFERS 1

enum
{
    PAGE_FLIP = 0x00000001,
    LOCKED = 0x00000002
};

struct fb_context_t
{
    framebuffer_device_t device;
#ifdef FSL_EPDC_FB
    // Partial udate feature
    bool rect_update;
    int rect_count;
    int update_mode[MAX_RECT_NUM];
    int partial_left[MAX_RECT_NUM];
    int partial_top[MAX_RECT_NUM];
    int partial_width[MAX_RECT_NUM];
    int partial_height[MAX_RECT_NUM];
#endif
};

#ifdef FSL_EPDC_FB
static void update_to_display(int left, int top, int width, int height, int update_mode, int fb_dev)
{
    static __u32 marker_val = 1;
    struct mxcfb_update_data upd_data;
    int retval;
    bool wait_for_complete;
    int auto_update_mode = AUTO_UPDATE_MODE_REGION_MODE;
    memset(&upd_data, 0, sizeof(mxcfb_update_data));

    if ((update_mode & KINDLE_WAVEFORM_MODE_MASK) == KINDLE_WAVEFORM_MODE_DU)
        upd_data.waveform_mode = WAVEFORM_MODE_DU;
    else if ((update_mode & KINDLE_WAVEFORM_MODE_MASK) == KINDLE_WAVEFORM_MODE_GC4)
        upd_data.waveform_mode = WAVEFORM_MODE_GC16_FAST;
    else if ((update_mode & KINDLE_WAVEFORM_MODE_MASK) == KINDLE_WAVEFORM_MODE_GC16)
        upd_data.waveform_mode = WAVEFORM_MODE_GC16;
    else if ((update_mode & KINDLE_WAVEFORM_MODE_MASK) == KINDLE_WAVEFORM_MODE_A2)
        upd_data.waveform_mode = WAVEFORM_MODE_A2;
    else if ((update_mode & KINDLE_WAVEFORM_MODE_MASK) == KINDLE_WAVEFORM_MODE_AUTO)
        upd_data.waveform_mode = WAVEFORM_MODE_AUTO;
    else if ((update_mode & KINDLE_WAVEFORM_MODE_MASK) == KINDLE_WAVEFORM_MODE_REAGL)
        upd_data.waveform_mode = WAVEFORM_MODE_REAGL;
    else if ((update_mode & KINDLE_WAVEFORM_MODE_MASK) == KINDLE_WAVEFORM_MODE_REAGLD)
        upd_data.waveform_mode = WAVEFORM_MODE_REAGLD;
    else
        ALOGI("Invalid waveform_mode\n");

    if ((update_mode & KINDLE_AUTO_MODE_MASK) == KINDLE_AUTO_MODE_REGIONAL)
        auto_update_mode = AUTO_UPDATE_MODE_REGION_MODE;
    else if ((update_mode & KINDLE_AUTO_MODE_MASK) == KINDLE_AUTO_MODE_AUTOMATIC)
        auto_update_mode = AUTO_UPDATE_MODE_AUTOMATIC_MODE;
    else
        ALOGI("Invalid auto_update_mode\n");

    if ((update_mode & KINDLE_UPDATE_MODE_MASK) == KINDLE_UPDATE_MODE_PARTIAL)
        upd_data.update_mode = UPDATE_MODE_PARTIAL;
    else if ((update_mode & KINDLE_UPDATE_MODE_MASK) == KINDLE_UPDATE_MODE_FULL)
        upd_data.update_mode = UPDATE_MODE_FULL;
    else
        ALOGI("Invalid update_mode\n");

    if ((update_mode & KINDLE_WAIT_MODE_MASK) == KINDLE_WAIT_MODE_NOWAIT)
        wait_for_complete = false;
    else if ((update_mode & KINDLE_WAIT_MODE_MASK) == KINDLE_WAIT_MODE_WAIT)
        wait_for_complete = true;
    else
        ALOGI("Invalid wait_for_complete parameter\n");

    if ((update_mode & KINDLE_INVERT_MODE_MASK) == KINDLE_INVERT_MODE_INVERT)
    {
        upd_data.flags |= EPDC_FLAG_ENABLE_INVERSION;
        ALOGI("invert mode on\n");
    }

    retval = ioctl(fb_dev, MXCFB_SET_AUTO_UPDATE_MODE, &auto_update_mode);
    if (retval < 0)
    {
        ALOGI("Set auto update mode failed. Error = 0x%x", retval);
    }

    upd_data.update_marker   = marker_val;

    // upd_data.flags           = (upd_data.waveform_mode == WAVEFORM_MODE_REAGLD)  ? EPDC_FLAG_USE_REAGLD 
    //             : (upd_data.waveform_mode == WAVEFORM_MODE_A2)  ? EPDC_FLAG_FORCE_MONOCHROME : 0U;
    upd_data.quant_bit       = 0;
    upd_data.hist_bw_waveform_mode = WAVEFORM_MODE_DU;
    upd_data.hist_gray_waveform_mode = WAVEFORM_MODE_GC16_FAST;
    upd_data.dither_mode = EPDC_FLAG_USE_DITHERING_ATKINSON;

    upd_data.temp = TEMP_USE_AMBIENT;
    upd_data.update_region.left = left;
    upd_data.update_region.width = width;
    upd_data.update_region.top = top;
    upd_data.update_region.height = height;

    if (wait_for_complete)
    {
        /* Get unique marker value */
        upd_data.update_marker = marker_val++;
    }
    else
    {
        upd_data.update_marker = 0;
    }

    retval = ioctl(fb_dev, MXCFB_SEND_UPDATE, &upd_data);
    while (retval < 0)
    {
        /* We have limited memory available for updates, so wait and
         * then try again after some updates have completed */
        sleep(1);
        retval = ioctl(fb_dev, MXCFB_SEND_UPDATE, &upd_data);
        ALOGI("MXCFB_SEND_UPDATE  retval = 0x%x try again maybe", retval);
    }

    if (wait_for_complete)
    {
        /* Wait for update to complete */
        retval = ioctl(fb_dev, MXCFB_WAIT_FOR_UPDATE_COMPLETE, &upd_data.update_marker);
        if (retval < 0)
        {
            ALOGI("Wait for update complete failed. Error = 0x%x", retval);
        }
    }
}

class UpdateHelper
{
public:
    UpdateHelper()
    {
        clear();
        clearGCInterval();
    }
    ~UpdateHelper() {}

public:
    void merge(int left, int top, int width, int height, int mode)
    {
        //ALOGI("merge update request: (%d, %d) -- (%d, %d) -- 0x%x\n", left, top, width, height, mode);
        if (left < left_)
        {
            left_ = left;
        }
        if (top < top_)
        {
            top_ = top;
        }
        int right = left + width;
        if (right > right_)
        {
            right_ = right;
        }
        int bottom = top + height;
        if (bottom > bottom_)
        {
            bottom_ = bottom;
        }

        // check waveform, gc interval, waiting mode
        checkWaveform(mode);
        checkType(mode);
        checkWaiting(mode);
        checkGCInterval(mode);
        //ALOGI("merge result: (%d, %d) -- (%d, %d) -- 0x%x\n", left_, top_, right, bottom, update_mode_);
    }

    void updateScreen(int fb_dev)
    {
        if (gc_interval_ >= 0 /*&& waveform_ == WAVEFORM_MODE_AUTO*/)
        {
            if (gu_count_++ < gc_interval_)
            {
                //ALOGI("kindle_display_update: KINDLE_GU_MODE\n");
                //update_mode_ = KINDLE_GU_MODE;
                update_mode_ = waveform_ | full_ | waiting_;
            }
            else
            {
                //ALOGI("kindle_display_update: KINDLE_GC_MODE\n");
                update_mode_ = KINDLE_GC_MODE;
                gu_count_ = 0;
            }
        }
        else
        {
            //ALOGI("kindle_display_update: waveform: 0x%x, full: 0x%x, waiting: 0x%x\n", waveform_, full_, waiting_);
            update_mode_ = waveform_ | full_ | waiting_;
        }
        //ALOGI("kindle_display_update: (%d, %d) -- (%d, %d) -- 0x%x\n", left_, top_, right_, bottom_, update_mode_);
        update_to_display(left_, top_, right_ - left_, bottom_ - top_, update_mode_, fb_dev);
        clear();
    }

private:
    void clear()
    {
        left_ = top_ = INT_MAX;
        right_ = bottom_ = -1;
        waveform_ = KINDLE_WAVEFORM_MODE_GC16;
        full_ = KINDLE_UPDATE_MODE_PARTIAL;
        waiting_ = KINDLE_WAIT_MODE_NOWAIT;
        update_mode_ = KINDLE_DEFAULT_MODE;
    }

    void clearGCInterval()
    {
        gc_interval_ = -1;
        gu_count_ = 0;
    }

    int priority(int index)
    {
        // init, du, gc16, gc4, a2, auto
        static const int data[] = {5, 2, 0, 1, 3, 4};
        return data[index];
    }

    void checkWaveform(int mode)
    {
        int index = (mode & KINDLE_WAVEFORM_MODE_MASK);
        int current = (waveform_ & KINDLE_WAVEFORM_MODE_MASK);
        int pn = priority(index);
        int pc = priority(current);
        if (pn > pc)
        {
            waveform_ = (mode & KINDLE_WAVEFORM_MODE_MASK);
        }
    }

    // full or partial update.
    void checkType(int mode)
    {
        int value = mode & KINDLE_UPDATE_MODE_MASK;
        if (value > full_)
        {
            full_ = value;
        }
    }

    void checkWaiting(int mode)
    {
        int value = mode & KINDLE_WAIT_MODE_MASK;
        if (value > waiting_)
        {
            waiting_ = value;
        }
    }

    void checkGCInterval(int mode)
    {
        if (mode & KINDLE_GC_MASK)
        {
            gc_interval_ = ((mode & 0x00ff0000) >> 16);
            //gu_count_ = gc_interval_;
            //ALOGI("GCInterval policy: %d\n", gc_interval_);
        }
        else if (mode & KINDLE_AUTO_MASK)
        {
            clearGCInterval();
            //ALOGI("Automatic policy.\n");
        }
    }

private:
    int gu_count_;
    int gc_interval_;
    int left_, top_, right_, bottom_;
    int waveform_;
    int full_;
    int waiting_;
    int update_mode_;
};

// Merge all display update requests
// Merge display region
// Merge update mode
//
static void kindle_display_update(int left, int top, int width, int height, int update_mode, int fb_dev, bool update)
{
    static UpdateHelper helper;

    helper.merge(left, top, width, height, update_mode);
    if (update)
    {
        helper.updateScreen(fb_dev);
    }
}
#endif

/*****************************************************************************/

static int fb_setSwapInterval(struct framebuffer_device_t *dev,
                              int interval)
{
    fb_context_t *ctx = (fb_context_t *)dev;
    if (interval < dev->minSwapInterval || interval > dev->maxSwapInterval)
        return -EINVAL;
    // FIXME: implement fb_setSwapInterval
    return 0;
}

#ifdef FSL_EPDC_FB
static int fb_setUpdateRect(struct framebuffer_device_t *dev,
                            int* left, int* top, int* width, int* height, int* update_mode, int count)
{
    fb_context_t *ctx = (fb_context_t *)dev;
    if(count > MAX_RECT_NUM)
    {
        ALOGE("count > MAX_RECT_NUM in fb_setUpdateRect\n");
        return -EINVAL;
    }

    ctx->rect_update = true;
    ctx->rect_count = 0;
    for(int i=0; i < count; i++)
    {
        if (((width[i]|height[i]) <= 0) || ((left[i]|top[i])<0))  return -EINVAL;
        ctx->update_mode[i]       = update_mode[i];
        ctx->partial_left[i]     = left[i];
        ctx->partial_top[i]      = top[i];
        ctx->partial_width[i]    = width[i];
        ctx->partial_height[i]   = height[i];
        //ALOGI("rect:l: %d, t: %d, w: %d, h: %d\n", left[i], top[i], width[i], height[i]);
    }
    ctx->rect_count = count;
    return 0;
}

#else
static int fb_setUpdateRect(struct framebuffer_device_t* dev,
                            int l, int t, int w, int h)
{
    if (((w|h) <= 0) || ((l|t)<0))
        return -EINVAL;
    return 0;
}
#endif

static int fb_post(struct framebuffer_device_t *dev, buffer_handle_t buffer)
{
    if (private_handle_t::validate(buffer) < 0)
        return -EINVAL;

    fb_context_t *ctx = (fb_context_t *)dev;

    private_handle_t const *hnd = reinterpret_cast<private_handle_t const *>(buffer);
    private_module_t *m = reinterpret_cast<private_module_t *>(
        dev->common.module);
    if (m->currentBuffer)
    {
        m->base.unlock(&m->base, m->currentBuffer);
        m->currentBuffer = 0;
    }

    if (hnd->flags & private_handle_t::PRIV_FLAGS_FRAMEBUFFER)
    {
        const size_t offset = hnd->base - m->framebuffer->base;
        m->info.activate = FB_ACTIVATE_VBL;
        m->info.yoffset = offset / m->finfo.line_length;
        if (ioctl(m->framebuffer->fd, FBIOPAN_DISPLAY, &m->info) == -1)
        {
            ALOGE("FBIOPUT_VSCREENINFO failed");
            m->base.unlock(&m->base, buffer);
            return -errno;
        }

#ifdef FSL_EPDC_FB
        // if (ctx->rect_update)
        // {
        //     for(int i = 0; i < ctx->rect_count; i++) {
        //         kindle_display_update(ctx->partial_left[i],ctx->partial_top[i],
        //             ctx->partial_width[i],ctx->partial_height[i],
        //             ctx->update_mode[i], m->framebuffer->fd, (i >= ctx->rect_count - 1));
        //     }
        //     ctx->rect_update = false;
        // }
        // else
        // {
            kindle_display_update(0, 0, m->info.xres, m->info.yres, KINDLE_WAVEFORM_MODE_A2 | KINDLE_UPDATE_MODE_PARTIAL | KINDLE_WAIT_MODE_NOWAIT, m->framebuffer->fd, true);
        // }
#endif

        m->currentBuffer = buffer;
    }
    else
    {
        // If we can't do the page_flip, just copy the buffer to the front
        // FIXME: use copybit HAL instead of memcpy
        void *fb_vaddr;
        void *buffer_vaddr;

        m->base.lock(&m->base, m->framebuffer,
                     GRALLOC_USAGE_SW_WRITE_RARELY,
                     0, 0, m->info.xres_virtual, m->info.yres,
                     &fb_vaddr);

        m->base.lock(&m->base, buffer,
                     GRALLOC_USAGE_SW_READ_RARELY,
                     0, 0, m->info.xres_virtual, m->info.yres,
                     &buffer_vaddr);

        memcpy(fb_vaddr, buffer_vaddr, m->finfo.line_length * m->info.yres);

#ifdef FSL_EPDC_FB
        if (ctx->rect_update)
        {
            for(int i = 0; i < ctx->rect_count; i++) {
                kindle_display_update(ctx->partial_left[i],ctx->partial_top[i],
                    ctx->partial_width[i],ctx->partial_height[i],
                    ctx->update_mode[i], m->framebuffer->fd, (i >= ctx->rect_count - 1));
            }
            ctx->rect_update = false;
        }
        else
        {
            kindle_display_update(0, 0, m->info.xres, m->info.yres, KINDLE_WAVEFORM_MODE_REAGL | KINDLE_UPDATE_MODE_PARTIAL | KINDLE_WAIT_MODE_NOWAIT, m->framebuffer->fd, true);
        }
#endif

        m->base.unlock(&m->base, buffer);
        m->base.unlock(&m->base, m->framebuffer);
    }

    return 0;
}

/*****************************************************************************/

int mapFrameBufferLocked(struct private_module_t *module)
{
    // already initialized...
    if (module->framebuffer)
    {
        return 0;
    }

    char const *const device_template[] = {
        "/dev/graphics/fb%u",
        "/dev/fb%u",
        0};

    int fd = -1;
    int i = 0;
    char name[64];

    while ((fd == -1) && device_template[i])
    {
        snprintf(name, 64, device_template[i], 0);
        fd = open(name, O_RDWR, 0);
        i++;
    }
    if (fd < 0)
        return -errno;

    struct fb_fix_screeninfo finfo;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == -1)
        return -errno;

    struct fb_var_screeninfo info;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &info) == -1)
        return -errno;

    info.reserved[0] = 0;
    info.reserved[1] = 0;
    info.reserved[2] = 0;
    info.xoffset = 0;
    info.yoffset = 0;
    info.activate = FB_ACTIVATE_NOW;

    /*
     * Request NUM_BUFFERS screens (at lest 2 for page flipping)
     */
    info.yres_virtual = info.yres * NUM_BUFFERS;

#ifdef FSL_EPDC_FB
    // info.yres_virtual = info.yres;
    info.xres_virtual = info.xres;
    info.bits_per_pixel = 16;
    info.grayscale = 0;
    info.xoffset = 0;
    info.yoffset = 0;
    info.activate = FB_ACTIVATE_FORCE;
#endif

    uint32_t flags = PAGE_FLIP;
    if (ioctl(fd, FBIOPUT_VSCREENINFO, &info) == -1)
    {
        info.yres_virtual = info.yres;
        flags &= ~PAGE_FLIP;
        ALOGW("FBIOPUT_VSCREENINFO failed, page flipping not supported");
    }

    if (info.yres_virtual < info.yres * 2)
    {
        // we need at least 2 for page-flipping
        info.yres_virtual = info.yres;
        flags &= ~PAGE_FLIP;
        ALOGW("page flipping not supported (yres_virtual=%d, requested=%d)",
              info.yres_virtual, info.yres * 2);
    }

    if (ioctl(fd, FBIOGET_VSCREENINFO, &info) == -1)
        return -errno;

#ifdef FSL_EPDC_FB
    int auto_update_mode = AUTO_UPDATE_MODE_AUTOMATIC_MODE;
    int retval = ioctl(fd, MXCFB_SET_AUTO_UPDATE_MODE, &auto_update_mode);
    if (retval < 0)
    {
        ALOGW("MXCFC_SET_AUTO_UPDATE_MODE failed.");
        return -errno;
    }

    int scheme = UPDATE_SCHEME_QUEUE_AND_MERGE;
    retval = ioctl(fd, MXCFB_SET_UPDATE_SCHEME, &scheme);
    if (retval < 0)
    {
        ALOGW("MXCFC_SET_UPDATE_SCHEME failed.");
        return -errno;
    }

    struct mxcfb_waveform_modes wv_modes;
    wv_modes.mode_init = WAVEFORM_MODE_INIT;
	wv_modes.mode_du = WAVEFORM_MODE_DU;
	wv_modes.mode_a2 = WAVEFORM_MODE_A2;
	wv_modes.mode_gc4 = WAVEFORM_MODE_GC16;
	wv_modes.mode_gc8 = WAVEFORM_MODE_GC16;
	wv_modes.mode_gc16 = WAVEFORM_MODE_GC16;
	wv_modes.mode_gc16_fast = WAVEFORM_MODE_A2;
	wv_modes.mode_gc32 = WAVEFORM_MODE_GC16;
	wv_modes.mode_gl16 = WAVEFORM_MODE_GC16;
	wv_modes.mode_gl16_fast = WAVEFORM_MODE_A2;
	wv_modes.mode_du4 = WAVEFORM_MODE_DU4;
    wv_modes.mode_reagl = WAVEFORM_MODE_REAGL;
	wv_modes.mode_reagld = WAVEFORM_MODE_REAGLD;

    retval = ioctl(fd, MXCFB_SET_WAVEFORM_MODES, &wv_modes);
    if (retval < 0)
    {
        ALOGW("MXCFC_SET_WAVEFORM_MODES failed.");
        return -errno;
    }

#endif
    uint64_t refreshQuotient =
        (uint64_t(info.upper_margin + info.lower_margin + info.yres) * (info.left_margin + info.right_margin + info.xres) * info.pixclock);

    /* Beware, info.pixclock might be 0 under emulation, so avoid a
     * division-by-0 here (SIGFPE on ARM) */
    int refreshRate = refreshQuotient > 0 ? (int)(1000000000000000LLU / refreshQuotient) : 0;

    if (refreshRate == 0)
    {
        // bleagh, bad info from the driver
        refreshRate = 60 * 1000; // 60 Hz
    }

#if 1
    refreshRate = 17 * 1000;
#endif

    if (int(info.width) <= 0 || int(info.height) <= 0)
    {
        // the driver doesn't return that information
        // default to 160 dpi
        info.width = ((info.xres * 25.4f) / 160.0f + 0.5f);
        info.height = ((info.yres * 25.4f) / 160.0f + 0.5f);
    }

    float xdpi = (info.xres * 25.4f) / info.width;
    float ydpi = (info.yres * 25.4f) / info.height;
    float fps = refreshRate / 1000.0f;

    ALOGI("using (fd=%d)\n"
          "id           = %s\n"
          "xres         = %d px\n"
          "yres         = %d px\n"
          "xres_virtual = %d px\n"
          "yres_virtual = %d px\n"
          "bpp          = %d\n"
          "r            = %2u:%u\n"
          "g            = %2u:%u\n"
          "b            = %2u:%u\n"
          "line_length  = %d\n",
          fd,
          finfo.id,
          info.xres,
          info.yres,
          info.xres_virtual,
          info.yres_virtual,
          info.bits_per_pixel,
          info.red.offset, info.red.length,
          info.green.offset, info.green.length,
          info.blue.offset, info.blue.length,
          finfo.line_length);

    ALOGI("width        = %d mm (%f dpi)\n"
          "height       = %d mm (%f dpi)\n"
          "refresh rate = %.2f Hz\n",
          info.width, xdpi,
          info.height, ydpi,
          fps);

    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == -1)
        return -errno;

    if (finfo.smem_len <= 0)
        return -errno;

    module->flags = flags;
    module->info = info;
    module->finfo = finfo;
    module->xdpi = xdpi;
    module->ydpi = ydpi;
    module->fps = fps;

    /*
     * map the framebuffer
     */

    int err;
    size_t fbSize = roundUpToPageSize(finfo.line_length * info.yres_virtual);
    module->framebuffer = new private_handle_t(dup(fd), fbSize, 0);

    module->numBuffers = NUM_BUFFERS;
    //ALOGI("numBuffers = %d", module->numBuffers);
    module->bufferMask = 0;

    void *vaddr = mmap(0, fbSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (vaddr == MAP_FAILED)
    {
        ALOGE("Error mapping the framebuffer (%s)", strerror(errno));
        return -errno;
    }
    module->framebuffer->base = intptr_t(vaddr);
    // memset(vaddr, 0, fbSize);
    return 0;
}

static int mapFrameBuffer(struct private_module_t *module)
{
    pthread_mutex_lock(&module->lock);
    int err = mapFrameBufferLocked(module);
    pthread_mutex_unlock(&module->lock);
    return err;
}

/*****************************************************************************/

static int fb_close(struct hw_device_t *dev)
{
    fb_context_t *ctx = (fb_context_t *)dev;
    if (ctx)
    {
        free(ctx);
    }
    return 0;
}

int fb_device_open(hw_module_t const *module, const char *name,
                   hw_device_t **device)
{
    int status = -EINVAL;
    if (!strcmp(name, GRALLOC_HARDWARE_FB0))
    {
        /* initialize our state here */
        fb_context_t *dev = (fb_context_t *)malloc(sizeof(*dev));
        memset(dev, 0, sizeof(*dev));

        /* initialize the procs */
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = 0;
        dev->device.common.module = const_cast<hw_module_t *>(module);
        dev->device.common.close = fb_close;
        dev->device.setSwapInterval = fb_setSwapInterval;
        dev->device.post = fb_post;
#ifndef FSL_EPDC_FB
        dev->device.setUpdateRect = 0;
#else
        dev->device.setUpdateRect = fb_setUpdateRect;
#endif

        private_module_t *m = (private_module_t *)module;
        status = mapFrameBuffer(m);
        if (status >= 0)
        {
            int stride = m->finfo.line_length / (m->info.bits_per_pixel >> 3);
            int format = (m->info.bits_per_pixel == 32)
                             ? (m->info.red.offset ? HAL_PIXEL_FORMAT_BGRA_8888 : HAL_PIXEL_FORMAT_RGBX_8888)
                             : HAL_PIXEL_FORMAT_RGB_565;
            const_cast<uint32_t &>(dev->device.flags) = 0;
            const_cast<uint32_t &>(dev->device.width) = m->info.xres;
            const_cast<uint32_t &>(dev->device.height) = m->info.yres;
            const_cast<int &>(dev->device.stride) = stride;
            const_cast<int &>(dev->device.format) = format;
            const_cast<float &>(dev->device.xdpi) = m->xdpi;
            const_cast<float &>(dev->device.ydpi) = m->ydpi;
            const_cast<float &>(dev->device.fps) = m->fps;
            const_cast<int &>(dev->device.minSwapInterval) = 1;
            const_cast<int &>(dev->device.maxSwapInterval) = 1;
            *device = &dev->device.common;
        }
    }
    return status;
}
