#include <stdint.h>
#include <cjson/cJSON.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <semaphore.h>
#include <time.h>
#include <unistd.h>
#define DEF_LOG
#include "C-Http-Server/inc/httpserv.h"
#include "allib/dynamic_list/dynamic_list.h"
#include "slowdb/inc/slowdb.h"

#ifdef __clang__
# define A_NOT_NULL _Nonnull
# define A_NULL _Null
#else
# define A_NOT_NULL /**/
# define A_NULL /**/
#endif

typedef struct {
    double delay;
    char const* A_NOT_NULL url;
} Mirror;

typedef struct {
    size_t donwload_timeout_ms;

    bool is_original;

    DynamicList TYPES(Mirror) items;
} MirrorsBundle;

typedef struct {
    DynamicList TYPES(MirrorsBundle) bundles;
} Mirrors;

typedef struct {
    char const * A_NOT_NULL bind;

    char const * A_NOT_NULL files_path;

    double svn_up_intvl;
    char const * A_NOT_NULL svn_repo_path;

    size_t http_threads;
    size_t conc_downloads;

    bool enable_package_stats;

    size_t mirror_reping_ms;

    Mirrors mirrors;
} AppCfg;

void AppCfg_parse(AppCfg* cfg);

typedef struct {
    pthread_mutex_t dlding;
    char const * A_NOT_NULL fname;
    _Atomic size_t rc;
    int status;
} AlreadyDownloading;

typedef struct {
    /** allocation base */
    char * A_NOT_NULL name;
    char const * A_NOT_NULL url;
} SvnDwPkg;

// TODO: replace with hashtab eventually
typedef DynamicList TYPES(SvnDwPkg) SvnDwPkgList;

typedef struct {
    AppCfg cfg;

    atomic_bool reloading_mirrors_async;
    pthread_rwlock_t mirrors_lock;
    Mirrors mirrors;

    atomic_bool reloading_svn_async;
    SvnDwPkgList svn_pkgs;
    pthread_rwlock_t svn_pkgs_mut;

    pthread_mutex_t currently_downloading_lock;
    DynamicList TYPES(AlreadyDownloading*) currently_downloading;

    sem_t download_sem;

    slowdb* per_packet_db;
    pthread_mutex_t per_packet_db_lock;
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
char* get_local_path(App* app, char const* filename);
int ensure_downloaded(App* app, char const* filename);

typedef time_t timestamp;

static timestamp time_now() {
    timestamp out;
    time(&out);
    return out;
}

static double time_elapsed_seconds(timestamp begin, timestamp end) {
    return difftime(end, begin);
}
