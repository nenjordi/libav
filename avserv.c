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
    URLContext *clientctx;
    struct pollfd pollst;
    struct rtmp_info rtmpctx;
    struct sockaddr_in *clientaddr;
};


static int num_clients;

struct client_context {
    pthread_t *thread;
    struct client_context* next;
} *clients;

static void *avserv_thread(void *arg)
{
    struct avserv_thread_params *param = arg;
    int ret = 0;
    char c; // ERASEME
    int eventnum;
    int fd = ffurl_get_file_handle(param->clientctx);
    av_log(NULL, AV_LOG_INFO, "Thread starts\n");
    for (ret=0; ret < 10; ret++) {
        ffurl_read_complete(param->clientctx, &c, 1);
        av_log(NULL, AV_LOG_INFO, "Received %c\n", c);
    }
    av_log(NULL, AV_LOG_INFO, "Thread ends\n");
    ffurl_close(param->clientctx);
    pthread_exit(0);
    return NULL;
}

int main(int argc, char *argv[])
{
    int ret;
    char tcpname[500];
    AVFormatContext fmtctx = { 0};

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
    /* if (!fmtctx) { */
    /*     av_log(NULL, AV_LOG_ERROR, "Error initializing AVFormatContext\n"); */
    /*     return AVERROR(ENOMEM); */
    /* } */
    /* fmtctx->oformat = av_guess_format("tcp", NULL, NULL); */
    av_register_all();
    avformat_network_init();

    ff_url_join(tcpname, sizeof(tcpname), "tcp", NULL, "localhost",
                RTMP_DEFAULT_PORT, "?accept");
    av_log(NULL, AV_LOG_INFO, "Opening: %s\n", tcpname);
    if (avio_open(&fmtctx.pb, tcpname, AVIO_FLAG_READ_WRITE)) {
        av_log(NULL, AV_LOG_ERROR, "Unable to open TCP for listening\n");
        return AVERROR(EIO);
    }
    for (;;) {
        AVFormatContext client = { 0 };
        pthread_t avservth;
        pthread_attr_t avservth_attr;
        struct avserv_thread_params *avservth_params = { 0 };
        avservth_params = av_malloc(sizeof(struct avserv_thread_params));
        if (!avservth_params) {
            av_log(NULL, AV_LOG_ERROR, "Unable to allocate thread params "
                   "memory\n");
        }

        av_log(NULL, AV_LOG_INFO, "Accept\n");
        if (ret = avio_accept(fmtctx.pb, &client.pb)) {
            av_log(NULL, AV_LOG_ERROR, "Error in avio_accept\n");
            return ret;
        }
        avservth_params->clientctx = client.pb->opaque;
        num_clients++;
        clients = av_realloc(clients, sizeof(struct client_context)
                             * num_clients);

        if (pthread_attr_init(&avservth_attr))
            av_log(NULL, AV_LOG_ERROR, "Unable to init"
                   " thread attributes\n");
        if (pthread_create(&avservth , &avservth_attr, avserv_thread,
                          avservth_params))
            av_log(NULL, AV_LOG_ERROR, "Unable to create listen thread\n");
        pthread_detach(avservth); /* Detach thread */
    }
    return 0;
}
