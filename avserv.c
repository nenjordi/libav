#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>

#include "cmdutils.h"
#include "libavformat/internal.h"
#include "libavutil/log.h"
#include "libavutil/random_seed.h"
#include "libavformat/rtmp.h"
#include "libavformat/rtmppkt.h"
#include "libavformat/rtsp.h"
#include "libavformat/url.h"

#define DEBUG
#define ACCEPT_TOUT 5
const char program_name[] = "avserv";
const int program_birth_year = 2012;
static const OptionDef options[];

/* Minimum config parameters */
#define DEFAULT_CONFIG_FILENAME "/etc/avserver.conf"
#define DEFAULT_PORT

struct avserv_options {
    char *config_filename;
    in_port_t port;
    uint8_t debug;
} avserv_options = { 0 };

void exit_program(int ret)
{
    exit(ret);
}

static void show_help(void)
{
    printf("usage: avserv [options]\n"
           "Hyper fast multi format Audio/Video streaming server\n");
    printf("\n");
    show_help_options(options, "Main options:\n", 0, 0);
}

static void opt_debug(void)
{
    avserv_options.debug = 1;
    av_log_set_level(AV_LOG_DEBUG);
}

/* Command line options */
static const OptionDef options[] = {
#include "cmdutils_common_opts.h"
//    { "n", OPT_BOOL, {(void *)&no_launch }, "enable no-launch mode" },
    { "d", 0, {(void*)opt_debug}, "enable debug mode" },
//    { "f", HAS_ARG | OPT_STRING, {(void*)&config_filename }, "use configfile instead of /etc/avserver.conf", "configfile" },
    { NULL },
};

struct rtmp_info {
    uint32_t epoch;
    uint32_t my_epoch;
    uint8_t peer_random[RTMP_HANDSHAKE_PACKET_SIZE - 8];
    uint8_t my_random[RTMP_HANDSHAKE_PACKET_SIZE - 8];
};

struct avserv_thread_params {
    AVFormatContext *clientctx;
    struct pollfd pollst;
    struct rtmp_info rtmpctx;
    struct sockaddr_in *clientaddr;
};


static int num_clients;

struct client_context {
    pthread_t *thread;
    struct client_context* next;
} *clients;

static void *rtsp_publish_avserv_thread(void *arg)
{
    struct avserv_thread_params *param = arg;
    AVPacket pkt, *ppkt = &pkt;
    RTSPState *rt = param->clientctx->priv_data;
    av_log(NULL, AV_LOG_INFO, "Publish Thread starts\n");
    while (rt->state != RTSP_STATE_STREAMING) {
        // Wait some time
        av_log(param->clientctx, AV_LOG_INFO,
               "Wait for State Streaming in RTSP\n");
    }
    for (;;) {
        if (av_read_frame(param->clientctx, ppkt) < 0) {
            av_log(param->clientctx, AV_LOG_ERROR, "Unable to read frame\n");
            goto rtsp_read_frame_error;
        }
        av_log(param->clientctx, AV_LOG_INFO,
               "Received packet %d bytes pts %ld\n",
               ppkt->size, ppkt->pts);
    }
rtsp_read_frame_error:
    av_log(NULL, AV_LOG_INFO, "Thread ends\n");
    avformat_close_input(&param->clientctx);
    pthread_exit(0);
    return NULL;
}

static int create_rtsp_publish_thread(AVFormatContext *s)
{
    AVInputFormat *iformat;
    pthread_t avservth;
    pthread_attr_t avservth_attr;
    struct avserv_thread_params *avservth_params = { 0 };
    avservth_params = av_malloc(sizeof(struct avserv_thread_params));
    if (!avservth_params) {
        av_log(NULL, AV_LOG_ERROR, "Unable to allocate thread params "
               "memory\n");
        return AVERROR(ENOMEM);
    }
    avservth_params->clientctx = s;
    iformat = av_find_input_format("rtsp");
    if (!av_dict_get(format_opts, "rtsp_flags", NULL, 0))
        av_dict_set(&format_opts, "rtsp_flags", "listen", 0);
    avformat_open_input(&s, "rtsp://localhost:5554/prueba?accept",
                        iformat, &format_opts);
    if (pthread_attr_init(&avservth_attr))
        av_log(NULL, AV_LOG_ERROR, "Unable to init"
               " thread attributes\n");
    if (pthread_create(&avservth , &avservth_attr,
                       rtsp_publish_avserv_thread, avservth_params))
        av_log(NULL, AV_LOG_ERROR, "Unable to create listen thread\n");
    pthread_detach(avservth); /* Detach thread */
    return 0;
}

static void *rtsp_server_avserv_thread(void *arg)
{
    struct avserv_thread_params *param = arg;
    //RTSPState *rt = param->clientctx->priv_data;
    int ret = 0;
    char c; // ERASEME
    av_log(NULL, AV_LOG_INFO, "Server Thread starts\n");

    for (ret=0; ret < 10; ret++) {
        ffurl_read_complete(param->clientctx->pb->opaque, &c, 1);
        av_log(NULL, AV_LOG_INFO, "Received %c\n", c);
    }
    av_log(NULL, AV_LOG_INFO, "Thread ends\n");
    avformat_close_input(&param->clientctx);
    pthread_exit(0);
    return NULL;
}

static int create_rtsp_server_thread(AVFormatContext *s)
{
    // AVInputFormat *iformat;
    pthread_t avservth;
    pthread_attr_t avservth_attr;
    struct avserv_thread_params *avservth_params = { 0 };
    avservth_params = av_malloc(sizeof(struct avserv_thread_params));
    if (!avservth_params) {
        av_log(NULL, AV_LOG_ERROR, "Unable to allocate thread params "
               "memory\n");
        return AVERROR(ENOMEM);
    }
    avservth_params->clientctx = s;
    /* iformat = av_find_input_format("rtsp"); */
    /* if (!av_dict_get(format_opts, "rtsp_flags", NULL, 0)) */
    /*     av_dict_set(&format_opts, "rtsp_flags", "listen", 0); */
    /* avformat_open_input(&s, "rtsp://localhost:5554/prueba?accept", */
    /*                     iformat, &format_opts); */
    if (pthread_attr_init(&avservth_attr))
        av_log(NULL, AV_LOG_ERROR, "Unable to init"
               " thread attributes\n");
    if (pthread_create(&avservth , &avservth_attr,
                       rtsp_server_avserv_thread, avservth_params))
        av_log(NULL, AV_LOG_ERROR, "Unable to create listen thread\n");
    pthread_detach(avservth); /* Detach thread */
    return 0;
}

int main(int argc, char *argv[])
{
    int ret;
    char tcpname[500];
    AVFormatContext rtsp_publish_fmtctx = { 0 };
    AVFormatContext rtsp_server_fmtctx = { 0 };

    // TODO: ADD pthread_t monitor -- Is it really needed?
    /* Argument parsing */
    parse_options(NULL, argc, argv, options, NULL);

    /* Parse config file */
    /* TODO: REWRITE? */
    /* if (parse_ffconfig(config_filename) < 0) { */
    /*     fprintf(stderr, "Incorrect config file - exiting.\n"); */
    /*     exit(1); */
    /* } */

    av_log(NULL, AV_LOG_INFO, "Server starts...\n");

    num_clients = 0;
    clients = NULL;
    avfilter_register_all();
    av_register_all();
    avformat_network_init();

    /* RTSP Publish */
    ff_url_join(tcpname, sizeof(tcpname), "tcp", NULL, "localhost",
                5554, "?accept");
    av_log(NULL, AV_LOG_INFO, "Opening: %s\n", tcpname);
    if (avio_open(&rtsp_publish_fmtctx.pb, tcpname,
//                  AVIO_FLAG_READ_WRITE | AVIO_FLAG_NONBLOCK)) {
                   AVIO_FLAG_READ_WRITE)) {
        av_log(NULL, AV_LOG_ERROR, "Unable to open TCP\n");
        return AVERROR(EIO);
    }
    if (avio_listen(rtsp_publish_fmtctx.pb, tcpname, AVIO_FLAG_READ_WRITE)) {
        av_log(NULL, AV_LOG_ERROR, "Unable to open TCP for listening\n");
        return AVERROR(EIO);
    }

    /* RTSP Server */
    ff_url_join(tcpname, sizeof(tcpname), "tcp", NULL, "localhost",
                5555, "?accept");
    av_log(NULL, AV_LOG_INFO, "Opening: %s\n", tcpname);
    if (avio_open(&rtsp_server_fmtctx.pb, tcpname,
//                  AVIO_FLAG_READ_WRITE | AVIO_FLAG_NONBLOCK)) {
                  AVIO_FLAG_READ_WRITE)) {
        av_log(NULL, AV_LOG_ERROR, "Unable to open TCP\n");
        return AVERROR(EIO);
    }
    if (avio_listen(rtsp_server_fmtctx.pb, tcpname, AVIO_FLAG_READ_WRITE)) {
        av_log(NULL, AV_LOG_ERROR, "Unable to open TCP for listening\n");
        return AVERROR(EIO);
    }

    for (;;) {
        AVIOContext *avioc = NULL;
        // Accept rtsp publish connections
        if (ret = avio_accept(rtsp_publish_fmtctx.pb, &avioc, ACCEPT_TOUT)) {
            av_log(NULL, AV_LOG_ERROR, "Error in avio_accept\n");
            return ret;
        }
        if (avioc) {
            AVFormatContext *client;
            av_log(NULL, AV_LOG_INFO, "New connection received\n");
            num_clients++;
            client = avformat_alloc_context();
            client->pb = avioc;
            //TODO: clients = av_realloc(clients, sizeof(struct client_context)
            //                     * num_clients);
            if (!create_rtsp_publish_thread(client)) {
                av_log(NULL, AV_LOG_ERROR,
                       "Unable to create rtsp publish thread\n");
            }
        }

        // Accept rtsp server connections
        if (ret = avio_accept(rtsp_server_fmtctx.pb, &avioc, ACCEPT_TOUT)) {
            av_log(NULL, AV_LOG_ERROR, "Error in avio_accept\n");
            return ret;
        }
        if (avioc) {
            AVFormatContext *client;
            av_log(NULL, AV_LOG_INFO, "New connection received\n");
            num_clients++;
            client = avformat_alloc_context();
            client->pb = avioc;
            //TODO: clients = av_realloc(clients, sizeof(struct client_context)
            //                     * num_clients);
            if (!create_rtsp_server_thread(client)) {
                av_log(NULL, AV_LOG_ERROR,
                       "Unable to create rtsp publish thread\n");
            }
        }
    }
    return 0;
}
