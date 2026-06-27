#include "spiffs_storage.h"
#include "esp_spiffs.h"
#include "esp_log.h"

static const char *TAG = "spiffs_storage";

esp_err_t init_spiffs(void)
{
    ESP_LOGI(TAG, "Initializing SPIFFS...");
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = true
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", (int)total, (int)used);
    }
    return ESP_OK;
}

#include <dirent.h>
#include <sys/stat.h>

void spiffs_list_files(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        printf("Failed to open directory %s\n", dir_path);
        return;
    }
    printf("Listing files in %s:\n", dir_path);
    printf("%-32s | %s\n", "Filename", "Size (bytes)");
    printf("---------------------------------+--------------\n");
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        struct stat entry_stat;
        if (stat(full_path, &entry_stat) == 0) {
            printf("%-32s | %ld\n", entry->d_name, (long)entry_stat.st_size);
        } else {
            printf("%-32s | %s\n", entry->d_name, "<unknown>");
        }
    }
    closedir(dir);
}

int cmd_ls(int argc, char **argv)
{
    const char *path = "/spiffs";
    if (argc >= 2) {
        path = argv[1];
    }
    spiffs_list_files(path);
    return 0;
}
