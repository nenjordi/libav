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
#include "libavformat/url.h"

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
} avserv_options = {0};

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
    int socket;
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

enum rtmp_handshake_states {
    RTMP_UNINITIALIZED_HS,
    RTMP_VERSIONSENT_HS,
    RTMP_ACKSENT_HS
};

static int rtmp_handshake(struct avserv_thread_params *param)
{
    //struct pollfd pollst;
    uint8_t buffer[RTMP_HANDSHAKE_PACKET_SIZE];
    int eventnum      = 0;
//    uint32_t epoch;
//    uint32_t my_epoch;
    uint32_t temp;
//    uint8_t peer_random[RTMP_HANDSHAKE_PACKET_SIZE - 8];
//    uint8_t my_random[RTMP_HANDSHAKE_PACKET_SIZE - 8];
    enum rtmp_handshake_states handshake_state = RTMP_UNINITIALIZED_HS;
    int randomidx = 0;
    uint8_t error  = 0;
    ssize_t inoutsize = 0;

    param->pollst.fd     = param->socket;
    param->pollst.events = POLLIN | POLLPRI;
    while ((eventnum = poll(&param->pollst, 1, -1)) > 0 && !error) {
        av_log(NULL, AV_LOG_DEBUG, "Event[%d] %d\n", eventnum, param->pollst.revents);
        if ((param->pollst.revents & POLLIN) || (param->pollst.revents & POLLPRI)) {
            switch(handshake_state) {
            case RTMP_UNINITIALIZED_HS:
                inoutsize = read(param->socket,buffer,1); //Receive C0
                if (inoutsize == 0) return AVERROR(EPROTO);
                if (buffer[0] == 3) /* Check Version */
                    if(!write(param->socket, buffer, 1))       //Send S0
                    {
                        av_log(NULL, AV_LOG_ERROR, "Unable to write answer - RTMP S0\n");
                        return AVERROR(EPROTO);
                    }
                handshake_state = RTMP_VERSIONSENT_HS;
                break;
            case RTMP_VERSIONSENT_HS:
                inoutsize = read(param->socket,buffer,4096);
                if (inoutsize == 0) return AVERROR(EPROTO);
                if (inoutsize != RTMP_HANDSHAKE_PACKET_SIZE)
                {
                    av_log(NULL, AV_LOG_ERROR, "Erroneous C1 Message size %d not following standard\n", inoutsize);
                    return AVERROR(EPROTO);
                }
                memcpy(&temp, buffer, 4);
                param->rtmpctx.epoch = ntohl(temp);
                param->rtmpctx.my_epoch = param->rtmpctx.epoch;
                memcpy(&temp, buffer + 4, 4);
                temp  = ntohl(temp);
                if (temp != 0) {
                    av_log(NULL, AV_LOG_ERROR, "Erroneous C1 Message zero != 0 --> %d\n", temp);
                    return AVERROR(EPROTO);
                }
                memcpy(param->rtmpctx.peer_random, buffer + 8, RTMP_HANDSHAKE_PACKET_SIZE - 8);
                //Send answer
                /*By now same epoch will be send*/
                for (randomidx = 0; randomidx < (RTMP_HANDSHAKE_PACKET_SIZE - 8); randomidx += 4) {
                    temp = av_get_random_seed();
                    memcpy(param->rtmpctx.my_random + randomidx, &temp, 4);
                }
                memcpy(buffer + 8, param->rtmpctx.my_random, RTMP_HANDSHAKE_PACKET_SIZE - 8);
                inoutsize = write(param->socket, buffer, RTMP_HANDSHAKE_PACKET_SIZE);
                if (inoutsize != RTMP_HANDSHAKE_PACKET_SIZE) {
                    av_log(NULL, AV_LOG_ERROR, "Unable to write answer - RTMP S1\n");
                    return AVERROR(EPROTO);
                }
                handshake_state = RTMP_ACKSENT_HS;
                break;
            case RTMP_ACKSENT_HS:
                inoutsize = read(param->socket,buffer,4096);
                if (inoutsize == 0) return AVERROR(EPROTO);
                if (inoutsize != RTMP_HANDSHAKE_PACKET_SIZE)
                {
                    av_log(NULL, AV_LOG_ERROR, "Erroneous C2 Message size %d not following standard\n", inoutsize);
                    return AVERROR(EPROTO);
                }
                memcpy(&temp, buffer, 4);
                temp = ntohl(temp);
                if (temp != param->rtmpctx.my_epoch)
                {
                    av_log(NULL, AV_LOG_ERROR, "Erroneous C2 Message epoch does not match up with C1 epoch\n");
                    return AVERROR(EPROTO);
                }

                /*TODO: Should Time2 be used to synchronise timers?*/
                if (memcmp(buffer + 8, param->rtmpctx.my_random, RTMP_HANDSHAKE_PACKET_SIZE - 8) != 0)
                {
                    av_log(NULL, AV_LOG_ERROR, "Erroneous C2 Message random does not match up\n");
                    return AVERROR(EPROTO);
                }
                /* Send S2 */
                temp = htonl(param->rtmpctx.epoch);
                memcpy(buffer, &temp , 4);
                // TODO: Time2 missing, by now 0
                temp = 0;
                memcpy(buffer + 4, &temp, 4);
                memcpy(buffer + 8, param->rtmpctx.peer_random,
                       RTMP_HANDSHAKE_PACKET_SIZE - 8);
                inoutsize = write(param->socket, buffer,
                                  RTMP_HANDSHAKE_PACKET_SIZE);
                if (inoutsize != RTMP_HANDSHAKE_PACKET_SIZE) {
                    av_log(NULL, AV_LOG_ERROR,
                           "Unable to write answer - RTMP S2\n");
                    return AVERROR(EPROTO);
                }

                /* Handshake successful */
                return 0;
                break;
            }
        }
    }
    return error;

}


static void *avserv_thread(void *arg)
{
    struct avserv_thread_params *param = arg;
    int ret = 0;
    uint8_t rbuf[4096];
    int eventnum, inoutsize;

    av_log(NULL, AV_LOG_INFO, "Thread starts\n");
    ret = rtmp_handshake(param);
    if (ret != 0)
        av_log(NULL, AV_LOG_ERROR, "Problem with RTMP handshake\n");
    while ((eventnum = poll(&param->pollst, 1, -1)) > 0) {
        av_log(NULL, AV_LOG_DEBUG, "Event[%d] %d\n", eventnum,
               param->pollst.revents);
        if ((param->pollst.revents & POLLIN)
            || (param->pollst.revents & POLLPRI)) {
                inoutsize = read(param->socket, rbuf, 4096);
                if (inoutsize == 0) return AVERROR(EPROTO);
        }
    }
    close(param->socket);
    av_log(NULL, AV_LOG_INFO, "Thread ends\n");
    pthread_exit(0);
    return NULL;
}

int main(int argc, char *argv[])
{
    int ret      = 0;
    char tcpname[500];
    //AVFormatContext *fmtctx = avformat_alloc_context();
    URLContext *serverctx;

    //TODO: ADD pthread_t monitor -- Is it really needed?
    /* Argument parsing */
    parse_options(NULL, argc, argv, options, NULL);

    /* Parse config file */
    /* TODO: REWRITE? */
    /* if (parse_ffconfig(config_filename) < 0) { */
    /*     fprintf(stderr, "Incorrect config file - exiting.\n"); */
    /*     exit(1); */
    /* } */

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
    if (ffurl_open(&serverctx, tcpname, AVIO_FLAG_READ_WRITE,
                   NULL, NULL)) //Warning, no IO interrupt callback set,needed?
    {
        av_log(NULL, AV_LOG_ERROR, "Unable to open TCP for listening\n");
        return AVERROR(EIO);
    }
    for (;;) {
        pthread_t avservth;
        pthread_attr_t avservth_attr;
        struct avserv_thread_params *avservth_params = {0};
        avservth_params = av_malloc(sizeof(struct avserv_thread_params));
        if (!avservth_params) {
            av_log(NULL, AV_LOG_ERROR, "Unable to allocate thread params "
                   "memory\n");
        }

        serverctx->prot->url_accept(serverctx, &avservth_params->clientctx,
                                    -1);
        num_clients++;
        clients = av_realloc(clients, sizeof(struct client_context)
                             * num_clients);

            if (pthread_attr_init(&avservth_attr))
                av_log(NULL, AV_LOG_ERROR, "Unable to init"
                       " thread attributes\n");
            if(!(avservth_params = av_malloc(sizeof(struct avserv_thread_params))))
                av_log(NULL, AV_LOG_ERROR, "Unable to allocate thread"
                       " params memory\n");
            if(pthread_create(&avservth , &avservth_attr, avserv_thread, avservth_params))
                av_log(NULL, AV_LOG_ERROR, "Unable to create listen thread\n");
            pthread_detach(avservth); /* Detach thread */
    }
    return 0;
}
