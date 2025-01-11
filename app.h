#include <stdint.h>
#include <cjson/cJSON.h>
#include <pthread.h>
#include <stdatomic.h>

typedef struct {
    uint16_t port;

    double mirrors_recache_intvl;

    char  ** upstream_mirrors;
    size_t   upstream_mirrors_len;
} AppCfg;

void AppCfg_parse(AppCfg* cfg, const char * hocon_path);

typedef struct {
    double delay;
    char * url;
} Mirror;

typedef struct {
    Mirror* items;
    size_t  count;
} Mirrors;

Mirrors get_mirrors(AppCfg const* cfg);
void free_mirrors(Mirrors m);

typedef struct {
    AppCfg cfg;

    atomic_bool reloading_mirrors_async;
    pthread_rwlock_t mirrors_lock;
    Mirrors mirrors;
} App;

void reload_print_mirrors(App* app);
char* get_local_path(char const* filename);
int ensure_downloaded(App* app, char const* filename);
