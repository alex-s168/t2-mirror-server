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

// 0 means error
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

    res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total_time);
    } else {
        return 0;
    }

    curl_easy_cleanup(curl);

    return total_time;
}

void free_mirrors(Mirrors m)
{
    free(m.items);
}

Mirrors get_mirrors(AppCfg const* cfg)
{
    Mirrors m;
    m.count = cfg->upstream_mirrors_len;
    m.items = malloc(sizeof(Mirror) * m.count);

    for (size_t i = 0; i < cfg->upstream_mirrors_len; i ++)
    {
        m.items[i] = (Mirror) {
            .url = cfg->upstream_mirrors[i],
            .delay = measure_ping(cfg->upstream_mirrors[i]),
        };
    }

    return m;
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

    pthread_rwlock_unlock(&app->mirrors_lock);
}

size_t download_write(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

// 0 = ok
static int download(char const* outpath, char const* url)
{
    LOGF("trying %s", url);

    FILE* file = fopen(outpath, "wb");
    if (!file) return 1;
    CURL* curl = curl_easy_init();

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, download_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
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

static void ensure_prefix_dir(char prefix)
{
    char df[7];
    sprintf(df, "data/%c", prefix);
    struct stat st = {0};
    if (stat(df, &st) == -1) {
        mkdir(df, 0700);
    }
}

// 0 = ok
static int mirror_download(char const* filename, char* outpath, Mirror mirror)
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

    ensure_prefix_dir(filename[0]);

    int ok = download(outpath, url);
    if (ok != 0) {
        remove(outpath);
    }

    free(url);

    return ok;
}

char* get_local_path(char const* filename)
{
    char* outpath = malloc(5 + 2 + strlen(filename) + 1);
    if (!outpath)
        return NULL;
    sprintf(outpath, "data/%c/%s", filename[0], filename);
    return outpath;
}

int ensure_downloaded(App* app, char const* filename, char const* orig_url) 
{
    StartDonwloadingRes conc = start_currently_downloading(app, filename);
    if (conc.is_already_doing)
        return conc.already_doing_res_status;

    sem_wait(&app->download_sem);

    char* outpath = get_local_path(filename);

    int anyok = 0;

    if (access(outpath, F_OK) != 0) {
        LOGF("%s is not yet cache", filename);

        pthread_rwlock_rdlock(&app->mirrors_lock);
        
        for (size_t i = 0; i < app->mirrors.count; i ++)
        {
            Mirror m = app->mirrors.items[i];
            int status = mirror_download(filename, outpath, m);
            if (status == 0) {
                LOGF("%s downloaded from %s", filename, m.url);
                anyok = 1;
                break;
            } else {
                LOGF("could NOT download %s from %s", filename, m.url);
            }
        }

        pthread_rwlock_unlock(&app->mirrors_lock);

        if (!anyok && orig_url != NULL)
        {
            ensure_prefix_dir(filename[0]);
            int ok = download(outpath, orig_url);
            if (ok != 0) {
                remove(outpath);
                LOGF("could NOT download %s from original url %s", filename, orig_url);
            } else {
                LOGF("downloaded %s from original url %s", filename, orig_url);
                anyok = 1;
            }
        }
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
