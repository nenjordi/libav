#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>

#include "cmdutils.h"
#include "libavutil/log.h"

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
}

/* Command line options */
static const OptionDef options[] = {
#include "cmdutils_common_opts.h"
//    { "n", OPT_BOOL, {(void *)&no_launch }, "enable no-launch mode" },
    { "d", 0, {(void*)opt_debug}, "enable debug mode" },
//    { "f", HAS_ARG | OPT_STRING, {(void*)&config_filename }, "use configfile instead of /etc/avserver.conf", "configfile" },
    { NULL },
};

struct avserv_thread_params {
    int socket;
};
static void *avserv_thread(void *arg)
{
    struct inspect_thread_params *param = arg;
    av_log(NULL, AV_LOG_INFO, "Thread starts\n");
    for(;;); /* Thread never ends */
    av_log(NULL, AV_LOG_INFO, "Thread ends\n");
    return NULL;
}

int main(int argc, char *argv[])
{
    int ret;
    int listensd;
    struct sockaddr_in serveraddr = {0};
    //TODO: ADD pthread_t monitor
    /* Argument parsing */
    parse_options(NULL, argc, argv, options, NULL);

    /* Parse config file */
    /* TODO: REWRITE? */
    /* if (parse_ffconfig(config_filename) < 0) { */
    /*     fprintf(stderr, "Incorrect config file - exiting.\n"); */
    /*     exit(1); */
    /* } */

    if ((listensd = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
        av_log(NULL, AV_LOG_ERROR,"Unable to open socket\n"); //TODO: NULL->Context
        return -1;
    }

    serveraddr.sin_family      = AF_UNSPEC; /* Allow IPv4 and IPv6*/
    //serveraddr.sin_addr.s_addr = inet_addr("192.168.0.100");
    serveraddr.sin_addr.s_addr = INADDR_ANY;
    serveraddr.sin_port        = htons(5554);
    if ( (ret = bind(listensd, (struct sockaddr *)&serveraddr, sizeof(struct sockaddr_in))) == -1) {
        close(listensd);
        perror("ERROR\n");
        av_log(NULL, AV_LOG_ERROR, "Unable to bind to address %d\n", ret);  //TODO: NULL->Context
        exit(1);
    }

    listen(listensd, 5); //Queue size to be set, 5 for testing

    for (;;) {
        struct sockaddr_in clientaddr = {0};
        socklen_t clientaddrsize;
        pthread_t avservth;
        pthread_attr_t avservth_attr;
        struct avserv_thread_params *avservth_params = {0};
        int clientsd = accept(listensd, (struct sockaddr *)&clientaddr, &clientaddrsize);
        if( clientsd && clientaddrsize == sizeof(clientaddr)) {
            if(pthread_attr_init(&avservth_attr))
                av_log(NULL, AV_LOG_ERROR, "Unable to init thread attributes\n");
            if(!(avservth_params = av_malloc(sizeof(struct avserv_thread_params))))
                av_log(NULL, AV_LOG_ERROR, "Unable to allocate thread params memory\n");
            avservth_params->socket = clientsd;
            if(pthread_create(&avservth , &avservth_attr, avserv_thread, avservth_params))
                av_log(NULL, AV_LOG_ERROR, "Unable to create listen thread\n");
            pthread_detach(avservth); /* Detach thread */
        }
    }
    return 0;
}
