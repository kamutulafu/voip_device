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
#include <unistd.h>

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
    char path[256] = "/spiffs";
    if (argc >= 2) {
        if (argv[1][0] == '/') {
            snprintf(path, sizeof(path), "%s", argv[1]);
        } else {
            snprintf(path, sizeof(path), "/spiffs/%s", argv[1]);
        }
    }
    spiffs_list_files(path);
    return 0;
}

void spiffs_remove_all(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        printf("Failed to open directory %s\n", dir_path);
        return;
    }
    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        struct stat entry_stat;
        if (stat(full_path, &entry_stat) == 0) {
            if (S_ISREG(entry_stat.st_mode)) {
                if (unlink(full_path) == 0) {
                    printf("Deleted: %s\n", entry->d_name);
                    count++;
                } else {
                    printf("Failed to delete: %s\n", entry->d_name);
                }
            }
        }
    }
    closedir(dir);
    printf("Total %d files deleted from %s.\n", count, dir_path);
}

int cmd_rm(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: rm <filename> or rm all\n");
        return 1;
    }

    if (strcmp(argv[1], "all") == 0) {
        printf("Deleting all files in /spiffs...\n");
        spiffs_remove_all("/spiffs");
        return 0;
    }

    char filepath[256];
    if (argv[1][0] == '/') {
        snprintf(filepath, sizeof(filepath), "%s", argv[1]);
    } else {
        snprintf(filepath, sizeof(filepath), "/spiffs/%s", argv[1]);
    }

    struct stat entry_stat;
    if (stat(filepath, &entry_stat) != 0) {
        printf("File %s does not exist.\n", filepath);
        return 1;
    }

    if (unlink(filepath) == 0) {
        printf("File %s deleted successfully.\n", filepath);
        return 0;
    } else {
        printf("Failed to delete file %s.\n", filepath);
        return 1;
    }
}
