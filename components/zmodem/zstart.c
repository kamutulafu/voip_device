/*
 * File      : zstart.c
 * the implemention of zmodem protocol.
 * Change Logs:
 * Date           Author       Notes
 * 2011-03-29     itspy       
 */

#include "zdef.h"

struct zmodemf zmodem;

int cmd_zmodem_send(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: zmodem_send <filepath>\n");
        return -1;
    }
    zs_start(argv[1]);
    return 0;
}

int cmd_zmodem_recv(int argc, char **argv)
{
    const char *path = "/spiffs";
    if (argc >= 2) {
        path = argv[1];
    }
    char *path_copy = malloc(strlen(path) + 1);
    if (path_copy == NULL) {
        printf("Memory allocation failed\n");
        return -1;
    }
    strcpy(path_copy, path);
    zr_start(path_copy);
    return 0;
}
