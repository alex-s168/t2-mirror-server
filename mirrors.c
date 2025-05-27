#include "allib/fixed_list/fixed_list.h"
#include "app.h"
#include <curl/curl.h>
#include <curl/easy.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#define DEF_LOG
#include "C-Http-Server/inc/httpserv.h"

static size_t discard_data(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    (void) ptr;
    (void) userdata;
    return size * nmemb;
}

// TODO: instead measure bandwidth of donwload DOWNTEST
/// 0 means error
static double measure_ping(const char * url)
{
    CURL *curl;
    CURLcode res;
    double total_time = 0.0;

    curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3 * 1000);

    res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total_time);
    } else {
        return 0;
    }

    curl_easy_cleanup(curl);

    return total_time;
}

static int mirror_cmp(void const* aa, void const* bb)
{
    Mirror const* a = aa;
    Mirror const* b = bb;

    if (a->delay > b->delay)
        return 1;
    if (a->delay < b->delay)
        return -1;
    return 0;
}

void reload_print_mirrors(App* app)
{
    pthread_rwlock_wrlock(&app->mirrors_lock);
/*
    free_mirrors(app->mirrors);
    app->mirrors = get_mirrors(&app->cfg);

    qsort(app->mirrors.items, app->mirrors.count, sizeof(Mirror), mirror_cmp); 

    size_t ndiscard = 0;
    for (; ndiscard < app->mirrors.count && 
           app->mirrors.items[ndiscard].delay == 0; 
         ndiscard ++);
    memmove(app->mirrors.items, app->mirrors.items + ndiscard, ndiscard * sizeof(Mirror));
    app->mirrors.count -= ndiscard;

    LOGF("reloading mirror list:");
    for (size_t i = 0; i < app->mirrors.count; i ++)
    {
        Mirror m = app->mirrors.items[i];
        LOGF("  %s\t%f ms", m.url, m.delay * 1000);
    }
*/
    pthread_rwlock_unlock(&app->mirrors_lock);
}

size_t download_write(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

// 0 = ok
static int download(App* app, char const* outpath, char const* url, size_t timeout_ms)
{
    LOGF("trying %s", url);

    FILE* file = fopen(outpath, "wb");
    if (!file) return 1;
    CURL* curl = curl_easy_init();

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, download_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    int status = curl_easy_perform(curl) != CURLE_OK;
    if (status == 0) {
        CURLcode code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        if (code != 200) 
            status = 1;
    }

    fclose(file);
    curl_easy_cleanup(curl);
    return status;
}

static void ensure_prefix_dir(App* app, char prefix)
{
    char df[strlen(app->cfg.files_path) + 7];
    sprintf(df, "%s/%c", app->cfg.files_path, prefix);
    struct stat st = {0};
    if (stat(df, &st) == -1) {
        mkdir(df, 0700);
    }
}

// 0 = ok
static int mirror_download(App* app, char const* filename, char const* outpath, Mirror mirror, size_t timeout_ms)
{
    char* url = malloc(strlen(mirror.url) + 3 + strlen(filename) + 1);
    if (!url) return 1;
    strcpy(url, mirror.url);
    if (mirror.url[strlen(mirror.url)-1] != '/') {
        strcat(url, "/");
    }
    char bf[3] = {0};
    bf[0] = filename[0];
    bf[1] = '/';
    strcat(url, bf);
    strcat(url, filename);

    ensure_prefix_dir(app, filename[0]);

    int ok = download(app, outpath, url, timeout_ms);
    if (ok != 0) {
        remove(outpath);
    }

    free(url);

    return ok;
}

// 0 = ok
static int mirrorgroup_download(App* app, char const* filename, char const* outpath, char const* orig_url, MirrorsBundle const* mirrors)
{
    if (mirrors->is_original) {
        ensure_prefix_dir(app, filename[0]);
        return download(app, outpath, orig_url, mirrors->donwload_timeout_ms);
    }

    for (size_t i = 0; i < mirrors->items.fixed.len; i ++)
    {
        Mirror m = *(Mirror*)FixedList_get(mirrors->items.fixed, i);
        if ( 0 == mirror_download(app, filename, outpath, m, mirrors->donwload_timeout_ms) ) {
            return 0;
        }
    }

    return 1;
}

char* get_local_path(App* app, char const* filename)
{
    char* outpath = malloc(strlen(app->cfg.files_path) + 3 + strlen(filename) + 1);
    if (!outpath)
        return NULL;
    sprintf(outpath, "%s/%c/%s", app->cfg.files_path, filename[0], filename);
    return outpath;
}

int ensure_downloaded(App* app, char const* filename) 
{
    char const* orig_url = NULL;

    pthread_rwlock_rdlock(&app->svn_pkgs_mut);

    for (size_t i = 0; i < app->svn_pkgs.fixed.len; i ++)
    {
        SvnDwPkg* pkg = (SvnDwPkg*)FixedList_get(app->svn_pkgs.fixed, i);
        if (!strcmp(pkg->name, filename)) {
            LOGF("found package url in T2 source: \"%s\" = \"%s\"", filename, pkg->url);
            orig_url = pkg->url;
            break;
        }
    }

    pthread_rwlock_unlock(&app->svn_pkgs_mut);

    StartDonwloadingRes conc = start_currently_downloading(app, filename);
    if (conc.is_already_doing)
        return conc.already_doing_res_status;

    sem_wait(&app->download_sem);

    char* outpath = get_local_path(app, filename);

    int anyok = 0;

    if (access(outpath, F_OK) != 0) {
        if (!orig_url) {
            LOGF("%s doesn't exsit in T2 master", filename);
            return 1;
        }

        LOGF("%s is not yet cached", filename);

        pthread_rwlock_rdlock(&app->mirrors_lock);
        
        for (size_t i = 0; i < app->mirrors.bundles.fixed.len; i ++)
        {
            MirrorsBundle const* bundle =
                (MirrorsBundle const *) FixedList_get(app->mirrors.bundles.fixed, i);
            int status = mirrorgroup_download(app, filename, outpath, orig_url, bundle);
            if (status == 0) {
                LOGF("%s downloaded from download group %zu", filename, i);
                anyok = 1;
                break;
            } else {
                LOGF("could NOT download %s from group %zu", filename, i);
            }
        }

        pthread_rwlock_unlock(&app->mirrors_lock);
    }
    else {
        anyok = 1;
    }

    free(outpath);

    int status = (!anyok) ? 1 : 0;

    sem_post(&app->download_sem);

    remove_currently_downloading(app, conc.dl, status);

    return status;
}
