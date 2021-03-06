#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include "stdafx.h"
#include "ringbuf.h"
#include "codec.h"
#include "x264.h"
#include "log.h"

#include "libavutil/time.h"
#include "libavutil/frame.h"
#include "libswscale/swscale.h"

#define YUV_BUF_NUM    3
#define OUT_BUF_SIZE  (2 * 1024 * 1024)
typedef struct {
    CODEC_INTERFACE_FUNCS

    x264_t  *x264;
    int      iw;
    int      ih;
    int      ow;
    int      oh;

    uint8_t *ibuff[YUV_BUF_NUM];
    int      ihead;
    int      itail;
    int      isize;

    uint8_t  obuff[OUT_BUF_SIZE];
    int      ohead;
    int      otail;
    int      osize;

    #define TS_EXIT        (1 << 0)
    #define TS_START       (1 << 1)
    #define TS_REQUEST_IDR (1 << 2)
    int      status;

    pthread_mutex_t imutex;
    pthread_cond_t  icond;
    pthread_mutex_t omutex;
    pthread_cond_t  ocond;
    pthread_t       thread;

    struct SwsContext *sws_context;
} H264ENC;

static void* venc_encode_thread_proc(void *param)
{
    H264ENC *enc = (H264ENC*)param;
    uint8_t *yuv = NULL;
    x264_picture_t pic_in, pic_out;
    x264_nal_t *nals;
    int32_t len, num, i;

    x264_picture_init(&pic_in );
    x264_picture_init(&pic_out);
    pic_in.img.i_csp   = X264_CSP_I420;
    pic_in.img.i_plane = 3;
    pic_in.img.i_stride[0] = enc->ow;
    pic_in.img.i_stride[1] = enc->ow / 2;
    pic_in.img.i_stride[2] = enc->ow / 2;

    while (!(enc->status & TS_EXIT)) {
        if (!(enc->status & TS_START)) {
            usleep(100*1000); continue;
        }

        pthread_mutex_lock(&enc->imutex);
        while (enc->isize <= 0 && !(enc->status & TS_EXIT)) pthread_cond_wait(&enc->icond, &enc->imutex);
        if (enc->isize > 0) {
            pic_in.img.plane[0] = enc->ibuff[enc->ihead];
            pic_in.img.plane[1] = enc->ibuff[enc->ihead] + enc->ow * enc->oh * 4 / 4;
            pic_in.img.plane[2] = enc->ibuff[enc->ihead] + enc->ow * enc->oh * 5 / 4;
            pic_in.i_type       = 0;
            if (enc->status & TS_REQUEST_IDR) {
                enc->status  &= ~TS_REQUEST_IDR;
                pic_in.i_type =  X264_TYPE_IDR;
            }
            len = x264_encoder_encode(enc->x264, &nals, &num, &pic_in, &pic_out);
            if (++enc->ihead == YUV_BUF_NUM) enc->ihead = 0;
            enc->isize--;
        } else {
            len = 0;
        }
        pthread_mutex_unlock(&enc->imutex);
        if (len <= 0) continue;

        pthread_mutex_lock(&enc->omutex);
        if (sizeof(len) + len <= sizeof(enc->obuff) - enc->osize) {
            uint32_t typelen = 'V' | (len << 8);
            enc->otail = ringbuf_write(enc->obuff, sizeof(enc->obuff), enc->otail, (uint8_t*)&typelen, sizeof(typelen));
            enc->osize+= sizeof(typelen);
            for (i=0; i<num; i++) {
                enc->otail = ringbuf_write(enc->obuff, sizeof(enc->obuff), enc->otail, nals[i].p_payload, nals[i].i_payload);
            }
            enc->osize += len;
            pthread_cond_signal(&enc->ocond);
        } else {
            log_printf("venc drop data %d !\n", len);
        }
        pthread_mutex_unlock(&enc->omutex);
    }
    return NULL;
}

static void uninit(void *ctxt)
{
    H264ENC *enc = (H264ENC*)ctxt;
    if (!ctxt) return;

    pthread_mutex_lock(&enc->imutex);
    enc->status |= TS_EXIT;
    pthread_cond_signal(&enc->icond);
    pthread_mutex_unlock(&enc->imutex);
    pthread_join(enc->thread, NULL);

    if (enc->x264) x264_encoder_close(enc->x264);
    if (enc->sws_context) sws_freeContext(enc->sws_context);

    pthread_mutex_destroy(&enc->imutex);
    pthread_cond_destroy (&enc->icond );
    pthread_mutex_destroy(&enc->omutex);
    pthread_cond_destroy (&enc->ocond );
    free(enc);
}

static void write(void *ctxt, void *buf[8], int len[8])
{
    H264ENC *enc = (H264ENC*)ctxt;
    if (!ctxt) return;
    pthread_mutex_lock(&enc->imutex);
    if (enc->isize < YUV_BUF_NUM) {
        AVFrame picsrc = {0}, picdst = {0};

        if (enc->iw != len[1] || enc->ih != len[2]) {
            enc->iw  = len[1];
            enc->ih  = len[2];
            if (enc->sws_context) {
                sws_freeContext(enc->sws_context);
                enc->sws_context = NULL;
            }
        }
        if (!enc->sws_context) {
            enc->sws_context = sws_getContext(enc->iw, enc->ih, AV_PIX_FMT_BGRA, enc->ow, enc->oh, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, 0, 0, 0);
        }

        picsrc.data[0]     = buf[0];
        picsrc.linesize[0] = len[3];
        picdst.linesize[0] = enc->ow;
        picdst.linesize[1] = enc->ow / 2;
        picdst.linesize[2] = enc->ow / 2;
        picdst.data[0] = enc->ibuff[enc->itail];
        picdst.data[1] = picdst.data[0] + enc->ow * enc->oh * 4 / 4;
        picdst.data[2] = picdst.data[0] + enc->ow * enc->oh * 5 / 4;

        sws_scale(enc->sws_context, (const uint8_t * const*)picsrc.data, picsrc.linesize, 0, enc->ih, picdst.data, picdst.linesize);
        if (++enc->itail == YUV_BUF_NUM) enc->itail = 0;
        enc->isize++;
    }
    pthread_cond_signal(&enc->icond);
    pthread_mutex_unlock(&enc->imutex);
}

static int read(void *ctxt, void *buf, int len, int *fsize, int timeout)
{
    H264ENC *enc = (H264ENC*)ctxt;
    int32_t framesize = 0, readsize = 0, ret = 0;
    struct  timespec ts;
    if (!ctxt) return 0;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 16*1000*1000;
    ts.tv_sec  += ts.tv_nsec / 1000000000;
    ts.tv_nsec %= 1000000000;

    pthread_mutex_lock(&enc->omutex);
    while (timeout && enc->osize <= 0 && (enc->status & TS_START) && ret != ETIMEDOUT) ret = pthread_cond_timedwait(&enc->ocond, &enc->omutex, &ts);
    if (enc->osize > 0) {
        enc->ohead = ringbuf_read(enc->obuff, sizeof(enc->obuff), enc->ohead, (uint8_t*)&framesize , sizeof(framesize));
        enc->osize-= sizeof(framesize);
        framesize  = ((uint32_t)framesize >> 8);
        readsize   = MIN(len, framesize);
        enc->ohead = ringbuf_read(enc->obuff, sizeof(enc->obuff), enc->ohead,  buf , readsize);
        enc->ohead = ringbuf_read(enc->obuff, sizeof(enc->obuff), enc->ohead,  NULL, framesize - readsize);
        enc->osize-= framesize;
    }
    if (fsize) *fsize = framesize;
    pthread_mutex_unlock(&enc->omutex);
    return readsize;
}

static void start(void *ctxt, int start)
{
    H264ENC *enc = (H264ENC*)ctxt;
    if (!ctxt) return;
    if (!ctxt) return;
    if (start) {
        enc->status |= TS_START;
    } else {
        pthread_mutex_lock(&enc->omutex);
        enc->status &= ~TS_START;
        pthread_cond_signal(&enc->ocond);
        pthread_mutex_unlock(&enc->omutex);
    }
}

static void reset(void *ctxt, int type)
{
    H264ENC *enc = (H264ENC*)ctxt;
    if (!ctxt) return;
    if (type & CODEC_RESET_CLEAR_INBUF) {
        pthread_mutex_lock(&enc->imutex);
        enc->ihead = enc->itail = enc->isize = 0;
        pthread_mutex_unlock(&enc->imutex);
    }
    if (type & CODEC_RESET_CLEAR_OUTBUF) {
        pthread_mutex_lock(&enc->omutex);
        enc->ohead = enc->otail = enc->osize = 0;
        pthread_mutex_unlock(&enc->omutex);
    }
    if (type & CODEC_RESET_REQUEST_IDR) {
        enc->status |= TS_REQUEST_IDR;
    }
}

static void obuflock(void *ctxt, uint8_t **pbuf, int *max, int *head, int *tail, int *size)
{
    H264ENC *enc = (H264ENC*)ctxt;
    if (!ctxt) return;
    pthread_mutex_lock(&enc->omutex);
    *pbuf = enc->obuff;
    *max  = sizeof(enc->obuff);
    *head = enc->ohead;
    *tail = enc->otail;
    *size = enc->osize;
}

static void obufunlock(void *ctxt, int head, int tail, int size)
{
    H264ENC *enc = (H264ENC*)ctxt;
    if (!ctxt) return;
    enc->ohead = head;
    enc->otail = tail;
    enc->osize = size;
    pthread_mutex_unlock(&enc->omutex);
}

CODEC* h264enc_init(int frate, int w, int h, int bitrate)
{
    x264_param_t param; int i;
    H264ENC *enc = calloc(1, sizeof(H264ENC) + (w * h * 3 / 2) * YUV_BUF_NUM);
    if (!enc) return NULL;

    strncpy(enc->name, "h264enc", sizeof(enc->name));
    enc->uninit     = uninit;
    enc->write      = write;
    enc->read       = read;
    enc->start      = start;
    enc->reset      = reset;
    enc->obuflock   = obuflock;
    enc->obufunlock = obufunlock;

    // init mutex & cond
    pthread_mutex_init(&enc->imutex, NULL);
    pthread_cond_init (&enc->icond , NULL);
    pthread_mutex_init(&enc->omutex, NULL);
    pthread_cond_init (&enc->ocond , NULL);

    x264_param_default_preset(&param, "ultrafast", "zerolatency");
    x264_param_apply_profile (&param, "baseline");
    param.i_csp            = X264_CSP_I420;
    param.i_width          = w;
    param.i_height         = h;
    param.i_fps_num        = frate;
    param.i_fps_den        = 1;
    param.i_slice_count_max= 1;
    param.i_threads        = 1;
    param.i_keyint_min     = frate * 2;
    param.i_keyint_max     = frate * 5;
    param.rc.i_bitrate     = bitrate;
    param.rc.i_rc_method   = X264_RC_ABR;
    param.i_timebase_num   = 1;
    param.i_timebase_den   = 1000;
    enc->ow   = w;
    enc->oh   = h;
    enc->x264 = x264_encoder_open(&param);
    for (i=0; i<YUV_BUF_NUM; i++) {
        enc->ibuff[i] = (uint8_t*)enc + sizeof(H264ENC) + i * (w * h * 3 / 2);
    }
    pthread_create(&enc->thread, NULL, venc_encode_thread_proc, enc);
    return (CODEC*)enc;
}
