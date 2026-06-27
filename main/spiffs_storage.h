#ifndef SPIFFS_STORAGE_H
#define SPIFFS_STORAGE_H

#include "esp_err.h"

esp_err_t init_spiffs(void);
int cmd_ls(int argc, char **argv);

#endif // SPIFFS_STORAGE_H
