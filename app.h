#include <stdint.h>
#include <cjson/cJSON.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <semaphore.h>
#include "allib/dynamic_list/dynamic_list.h"

typedef struct {
    uint16_t port;

    double mirrors_recache_intvl;

    char  ** upstream_mirrors;
    size_t   upstream_mirrors_len;

    bool enable_remoteurl;

    size_t http_threads;
    size_t conc_downloads;
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
    pthread_mutex_t dlding;
    char const* fname;
    _Atomic size_t rc;
    int status;
} AlreadyDownloading;

typedef struct {
    AppCfg cfg;

    atomic_bool reloading_mirrors_async;
    pthread_rwlock_t mirrors_lock;
    Mirrors mirrors;

    pthread_mutex_t currently_downloading_lock;
    DynamicList TYPES(AlreadyDownloading*) currently_downloading;

    sem_t download_sem;
} App;

typedef struct {
    AlreadyDownloading* dl;
    /** if this is true, the download already finished */
    bool is_already_doing;
    int already_doing_res_status;
} StartDonwloadingRes;

StartDonwloadingRes start_currently_downloading(App* app, char const* pkg);
void remove_currently_downloading(App* app, AlreadyDownloading* m, int status);

void reload_print_mirrors(App* app);
char* get_local_path(char const* filename);
int ensure_downloaded(App* app, char const* filename, char const* orig_url);
