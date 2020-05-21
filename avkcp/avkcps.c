#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <winsock2.h>
#include "adev.h"
#include "vdev.h"
#include "codec.h"
#include "ikcp.h"
#include "avkcps.h"

#define AVKCP_CONV (('A' << 0) | ('V' << 8) | ('K' << 16) | ('C' << 24))

#ifdef WIN32
#include <winsock2.h>
#define usleep(t) Sleep((t) / 1000)
#define get_tick_count GetTickCount
#define snprintf _snprintf
#endif

typedef struct {
    #define TS_EXIT  (1 << 0)
    #define TS_START (1 << 1)
    uint32_t  status;
    pthread_t pthread;

    void     *adev;
    void     *vdev;
    CODEC    *aenc;
    CODEC    *venc;
    ikcpcb   *ikcp;
    uint32_t  tick_kcp_update;
    struct    sockaddr_in server_addr;
    struct    sockaddr_in client_addr;
    int       client_connected;
    SOCKET    server_fd;
    char      avinfostr[256];
    uint8_t   buff[2 * 1024 * 1024];
} AVKCPS;

static int udp_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
    AVKCPS *avkcps = (AVKCPS*)user;
    return sendto(avkcps->server_fd, buf, len, 0, (struct sockaddr*)&avkcps->client_addr, sizeof(avkcps->client_addr));
}

static void avkcps_ikcp_update(AVKCPS *avkcps)
{
    uint32_t tickcur = get_tick_count();
    if (tickcur >= avkcps->tick_kcp_update) {
        ikcp_update(avkcps->ikcp, tickcur);
        avkcps->tick_kcp_update = ikcp_check(avkcps->ikcp, get_tick_count());
    }
}

static void ikcp_send_packet(AVKCPS *avkcps, char type, uint8_t *buf, int len)
{
    int remaining = len + sizeof(int32_t), cursend;
    *(int32_t*)buf = (type << 0) | (len << 8);
    do {
        cursend = remaining < 1024 ? remaining : 1024;
        ikcp_send(avkcps->ikcp, buf, cursend);
        remaining -= cursend;
        avkcps_ikcp_update(avkcps);
    } while (remaining > 0);
}

static void* avkcps_thread_proc(void *argv)
{
    AVKCPS  *avkcps = (AVKCPS*)argv;
    struct   sockaddr_in fromaddr;
    uint8_t  buffer[1500];
    uint32_t tickcur = 0, tickheartbeat = 0;
    int      addrlen = sizeof(fromaddr), ret;
    unsigned long opt;

#ifdef WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed !\n");
        return NULL;
    }
#endif

    avkcps->server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (avkcps->server_fd < 0) {
        printf("failed to open socket !\n");
        goto _exit;
    }

    if (bind(avkcps->server_fd, (struct sockaddr*)&avkcps->server_addr, sizeof(avkcps->server_addr)) == -1) {
        printf("failed to bind !\n");
        goto _exit;
    }
    opt = 1; ioctlsocket(avkcps->server_fd, FIONBIO, &opt); // setup non-block io mode
    opt = 1; setsockopt(avkcps->server_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(int));

    avkcps->ikcp = ikcp_create(AVKCP_CONV, avkcps);
    if (!avkcps->ikcp) {
        printf("failed to create ikcp !\n");
        goto _exit;
    }

    ikcp_setoutput(avkcps->ikcp, udp_output);
    ikcp_nodelay(avkcps->ikcp, 2, 10, 2, 1);
    ikcp_wndsize(avkcps->ikcp, 1024, 256);
    avkcps->ikcp->interval = 1;
    avkcps->ikcp->rx_minrto = 5;
    avkcps->ikcp->fastresend = 1;
    avkcps->ikcp->stream = 1;

    while (!(avkcps->status & TS_EXIT)) {
        if (!(avkcps->status & TS_START)) { usleep(100*1000); continue; }

        if (avkcps->client_connected && ikcp_waitsnd(avkcps->ikcp) < 2000) {
            int readsize, framesize;
            readsize = codec_read(avkcps->aenc, avkcps->buff + sizeof(int32_t), sizeof(avkcps->buff) - sizeof(int32_t), &framesize, 0);
            if (readsize > 0 && readsize == framesize && readsize <= 0xFFFFFF) {
                ikcp_send_packet(avkcps, 'A', avkcps->buff, framesize);
            }
            readsize = codec_read(avkcps->venc, avkcps->buff + sizeof(int32_t), sizeof(avkcps->buff) - sizeof(int32_t), &framesize, 0);
            if (readsize > 0 && readsize == framesize && readsize <= 0xFFFFFF) {
                ikcp_send_packet(avkcps, 'V', avkcps->buff, framesize);
            }
        }

        while (1) {
            if ((ret = recvfrom(avkcps->server_fd, buffer, sizeof(buffer), 0, (struct sockaddr*)&fromaddr, &addrlen)) <= 0) break;
            if (avkcps->client_connected == 0) {
                avkcps->client_connected = 1;
                memcpy(&avkcps->client_addr, &fromaddr, sizeof(avkcps->client_addr));
                codec_reset(avkcps->aenc, CODEC_RESET_CLEAR_INBUF|CODEC_RESET_CLEAR_OUTBUF|CODEC_RESET_REQUEST_IDR);
                codec_reset(avkcps->venc, CODEC_RESET_CLEAR_INBUF|CODEC_RESET_CLEAR_OUTBUF|CODEC_RESET_REQUEST_IDR);
                codec_start(avkcps->aenc, 1);
                codec_start(avkcps->venc, 1);
                adev_start (avkcps->adev, 1);
                vdev_start (avkcps->vdev, 1);
                tickheartbeat = get_tick_count();
                printf("===ck=== client connected !\n");
                ikcp_send_packet(avkcps, 'I', avkcps->avinfostr, (int)strlen(avkcps->avinfostr) + 1);
            }
            if (memcmp(&avkcps->client_addr, &fromaddr, sizeof(avkcps->client_addr)) == 0) ikcp_input(avkcps->ikcp, buffer, ret);
        }

        if (avkcps->client_connected) {
            ret = ikcp_recv(avkcps->ikcp, buffer, sizeof(buffer));
            if (ret > 0 && strcmp(buffer, "hb") == 0) {
                tickheartbeat = get_tick_count();
                printf("ikcp_waitsnd: %d\n", ikcp_waitsnd(avkcps->ikcp));
            } else {
                if (get_tick_count() > tickheartbeat + 3000) {
                    printf("===ck=== client disconnect !\n");
                    avkcps->client_connected = 0;
                    codec_start(avkcps->aenc, 0);
                    codec_start(avkcps->venc, 0);
                    adev_start (avkcps->adev, 0);
                    vdev_start (avkcps->vdev, 0);
                    memset(&avkcps->client_addr, 0, sizeof(avkcps->client_addr));
                }
            }
        }

        avkcps_ikcp_update(avkcps);
        usleep(1*1000);
    }

_exit:
    if (avkcps->server_fd >= 0) closesocket(avkcps->server_fd);
    if (avkcps->ikcp) ikcp_release(avkcps->ikcp);
#ifdef WIN32
    WSACleanup();
#endif
    return NULL;
}

void* avkcps_init(int port, int channels, int samprate, int width, int height, int frate, void *adev, void *vdev, CODEC *aenc, CODEC *venc)
{
    AVKCPS *avkcps = calloc(1, sizeof(AVKCPS));
    if (!avkcps) {
        printf("failed to allocate memory for avkcps !\n");
        return NULL;
    }

    avkcps->server_addr.sin_family      = AF_INET;
    avkcps->server_addr.sin_port        = htons(port);
    avkcps->server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    avkcps->adev = adev;
    avkcps->vdev = vdev;
    avkcps->aenc = aenc;
    avkcps->venc = venc;
    snprintf(avkcps->avinfostr, sizeof(avkcps->avinfostr), "aenc=%s,channels=%d,samprate=%d;venc=%s,width=%d,height=%d,frate=%d;",
        aenc->name, channels, samprate, venc->name, width, height, frate);

    // create server thread
    pthread_create(&avkcps->pthread, NULL, avkcps_thread_proc, avkcps);
    avkcps_start(avkcps, 1);
    return avkcps;
}

void avkcps_exit(void *ctxt)
{
    AVKCPS *avkcps = ctxt;
    if (!ctxt) return;
    adev_start (avkcps->adev, 0);
    vdev_start (avkcps->vdev, 0);
    codec_start(avkcps->aenc, 0);
    codec_start(avkcps->venc, 0);
    avkcps->status |= TS_EXIT;
    pthread_join(avkcps->pthread, NULL);
    free(ctxt);
}

void avkcps_start(void *ctxt, int start)
{
    AVKCPS *avkcps = ctxt;
    if (!ctxt) return;
    if (start) {
        avkcps->status |= TS_START;
    } else {
        avkcps->status &=~TS_START;
    }
}