
/**
 * @file httpserver.h
 */

#define REST_CHECK(a, str, goto_tag, ...)                                              \
    do                                                                                 \
    {                                                                                  \
        if (!(a))                                                                      \
        {                                                                              \
            ESP_LOGE(TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
            goto goto_tag;                                                             \
        }                                                                              \
    } while (0)

#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 128)
#define MAX_FILE_SIZE   (5*1024)
#define MAX_FILE_SIZE_STR "5KB"
#define SCRATCH_BUFSIZE  1024

#define CHECK_FILE_EXTENSION(filename, ext) (strcasecmp(&filename[strlen(filename) - strlen(ext)], ext) == 0)

#ifndef MIN
#define MIN(a,b)            (((a) < (b)) ? (a) : (b))
#endif



esp_err_t start_server(const char *base_path);
