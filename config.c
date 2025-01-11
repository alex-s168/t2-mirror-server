#include <cjson/cJSON.h>
#include <stdint.h>
#include <stdlib.h>
#include <hocon.h>
#include "app.h"

void AppCfg_parse(AppCfg* cfg, const char * hocon_path)
{
    cJSON* j = hocon_parse_file(hocon_path);
    
    cfg->port = (uint16_t) cJSON_GetNumberValue(cJSON_GetObjectItem(j, "port"));

    cfg->mirrors_recache_intvl = cJSON_GetNumberValue(cJSON_GetObjectItem(j, "mirrors_recache_interval_s"));

    cJSON* arr = cJSON_GetObjectItem(j, "backing_mirrors");
    cfg->upstream_mirrors_len = cJSON_GetArraySize(arr);
    cfg->upstream_mirrors = malloc(sizeof(char*) * cfg->upstream_mirrors_len);
    for (size_t i = 0; i < cfg->upstream_mirrors_len; i ++) {
        cfg->upstream_mirrors[i] = cJSON_GetStringValue(cJSON_GetArrayItem(arr, i));
    }
}
