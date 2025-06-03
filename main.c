#include "allib/kallok/kallok.h"
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include "app.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <dirent.h>
#include "allib/dynamic_list/dynamic_list.h"
#include "slowdb/inc/slowdb.h"
#include <signal.h>
#include <sys/stat.h>

typedef struct {
    struct {
        uint32_t num_downloads;
    } PACKED pkgdbinfo_v1;
} PACKED pkgdbinfo;

static DynamicList TYPES(char) gen_readme(App* app)
{
    DynamicList TYPES(char) out;
    DynamicList_init(&out, sizeof(char), getLIBCAlloc(), 0);

#define addstr(s) \
    DynamicList_addAll(&out, s, strlen(s), sizeof(char));

    addstr("# T/2 download cache\n");
    addstr("use by adding the url of this to your /usr/src/t2-src/download/Mirror-Cache file\n");
    addstr("\n");
    addstr("you can see all the cached files and statistics in /stats.csv\n");
    addstr("note that that might not contain all packages, since statistic tracking is new\n");

#undef addstr

    return out;
}

static DynamicList TYPES(char) gen_stats(App* app)
{
    DynamicList TYPES(char) out;
    DynamicList_init(&out, sizeof(char), getLIBCAlloc(), 0);

    if (!app->per_packet_db)
        return out;

    pthread_mutex_lock(&app->per_packet_db_lock);

#define addstr(s) \
    DynamicList_addAll(&out, s, strlen(s), sizeof(char));

    addstr("file_name,num_downloads\n");
    addstr(",\n");

    slowdb_iter* iter = slowdb_iter_new(app->per_packet_db);
    while ( slowdb_iter_next(iter) )
    {
        int keylen;
        char* key = (char*) slowdb_iter_get_key(iter, &keylen);
        if (key && keylen < 1000) {
            DynamicList_addAll(&out, key, keylen, sizeof(char));
        } else {
            WARNF("slowdb iter get key failed");
        }
        free(key);

        int vallen;
        char* val = (char*) slowdb_iter_get_val(iter, &vallen);
        if (key && vallen <= sizeof(pkgdbinfo)) {
            pkgdbinfo info = {0};
            memcpy(&info, val, vallen);

            char buf[64];
            sprintf(buf, ",%u\n", info.pkgdbinfo_v1.num_downloads);
            addstr(buf);
        } else {
            WARNF("slowdb iter get val failed");
        }

        free(val);
    }
    slowdb_iter_delete(iter);

#undef addstr

    pthread_mutex_unlock(&app->per_packet_db_lock);

    return out;
}

HttpIterRes downtest_next(void* userptr) {
    static char X100[65] = "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\n";
    size_t rem = (size_t)(intptr_t)userptr;
    HttpIterRes res;
    res.free_ptr = false;
    res.len = 0;
    res.new_userptr = userptr;
    if (rem) {
        res.new_userptr = (void*)(intptr_t)(rem - 1);
        res.len = 64;
        res.ptr = X100;
    }
    return res;
}

static struct HttpResponse serve(struct HttpRequest request, void* userdata)
{
    // TODO: protect against spamming (make configurable)

    App* app = userdata;

    if (app->cfg.verbose) {
        LOGF("t2_mirror_server::serve() called");
    }

    if (!strcmp(request.path, "/")) {
        DynamicList TYPES(char) out = gen_readme(app);

        return (struct HttpResponse) {
            .status = 200,
            .status_msg = "OK",
            .content_type = "text/plain",
            .content_size = out.fixed.len,
            .content_mode = HTTP_CONTENT_BYTES,
            .content_val.bytes = (HttpBytesContent) {
                .content = out.fixed.data,
                .free_after = true,
            }
        };
    }

    if (!strcmp(request.path, "/stats.csv")) {
        DynamicList TYPES(char) out = gen_stats(app);

        return (struct HttpResponse) {
            .status = 200,
            .status_msg = "OK",
            .content_type = "text/plain",
            .content_size = out.fixed.len,
            .content_mode = HTTP_CONTENT_BYTES,
            .content_val.bytes = (HttpBytesContent) {
                .content = out.fixed.data,
                .free_after = true,
            }
        };
    }

    if (!strcmp(request.path, "/DOWNTEST")) {
        return (struct HttpResponse) {
            .status = 200,
            .status_msg = "OK",
            .content_type = "text/plain",
            .content_size = 16384*64,
            .content_mode = HTTP_CONTENT_ITER,
            .content_val.iter = (HttpIterContent) {
                .userptr = (void*)(intptr_t)(16384),
                .next = downtest_next,
            }
        };
    }

    if (strlen(request.path) > 3 && request.path[0] == '/' && request.path[2] == '/') {
        char const* reqfile = request.path + 3;

        LOGF("requested: \"%s\"", reqfile);

        if ( 0 == ensure_downloaded(app, reqfile) )
        {
            char* path = get_local_path(app, reqfile);
            FILE* f = fopen(path, "rb");
            free(path);
            if (f != NULL) {
                fseek(f, 0, SEEK_END);
                size_t len = ftell(f);
                rewind(f);
                LOGF("serving %s", reqfile);
                if ( app->per_packet_db )
                {
                    pthread_mutex_lock(&app->per_packet_db_lock);
                    pkgdbinfo val = {0};
                    int actual_vallen = -1;
                    unsigned char* valptr = slowdb_get(app->per_packet_db,
                            (unsigned char*) reqfile, strlen(reqfile),
                            &actual_vallen);
                    pthread_mutex_unlock(&app->per_packet_db_lock);

                    if (valptr) {
                        if (actual_vallen > sizeof(val)) {
                            ERRF("statsdb stored invalid value for \"%s\". actual len was %i", reqfile, actual_vallen);
                        } else {
                            memcpy((unsigned char*) &val, valptr, actual_vallen);
                        }
                        free(valptr);
                    } else {
                        LOGF("first time insert into statsdb for \"%s\"", reqfile);
                    }
                    val.pkgdbinfo_v1.num_downloads += 1;

                    pthread_mutex_lock(&app->per_packet_db_lock);
                    int status = slowdb_replaceOrPut(app->per_packet_db,
                            (unsigned char*) reqfile, strlen(reqfile),
                            (unsigned char*) &val, sizeof(val));
                    pthread_mutex_unlock(&app->per_packet_db_lock);
                    if (status != 0) {
                        ERRF("statsdb replaceOrPut failed");
                    }
                }

                return (struct HttpResponse) {
                    .status = 200,
                    .status_msg = "OK",
                    .content_type = http_detectMime(reqfile, "application/octet-stream"),
                    .content_size = len,
                    .content_mode = HTTP_CONTENT_FILE,
                    .content_val.file = (HttpFileContent) {
                        .fp = f,
                        .close_after = true,
                    }
                };
            } else {
                fclose(f);
            }
        }
        else {
            LOGF("can't source %s", reqfile);
        }
    }

    return (struct HttpResponse) {
        .status = 404,
        .status_msg = "Not Found",
        .content_type = "text/plain",
        .content_size = 3,
        .content_mode = HTTP_CONTENT_BYTES,
        .content_val.bytes = (HttpBytesContent) {
            .content = "err",
            .free_after = false,
        },
    };
}

static void* mirrors_reload_thread(void* ptr)
{
    App* app = ptr;
    reload_print_mirrors(app);
    app->reloading_mirrors_async = false;
    return NULL;
}

static void svn_up_index_file(App* app, FILE* fp, DynamicList TYPES(SvnDwPkg) * out)
{
    char lbuf[512];

    while ( fgets(lbuf, sizeof(lbuf), fp) ) 
    {
        size_t lbuflen = strlen(lbuf);

        if (lbuflen > 4 && !memcmp(lbuf, "[D] ", 4))
        {
            char const* s = lbuf;
            s += 5;
            while (!isspace(*s) && *s) s++;
            if (!*s) continue;
            s ++;

            size_t slen = strlen(s);

            char * ptr = malloc(slen*2);
            if (!ptr) continue;
            memcpy(ptr, s, slen+1);

            {
                char* t = strrchr(ptr, '\n');
                if (t) *t = '\0';
            }

            {
                char* t = strrchr(ptr, '\r');
                if (t) *t = '\0';
            }

            char* space = strchr(ptr, ' ');
            if (!space) {
                free(ptr);
                continue;
            }

            *space = '\0';

	    char* url = strstr(space + 1, "http");
	    if (!url) {
		free(ptr);
		continue;
	    }

	    char url_last = url[strlen(url)-1];
	    if (url_last == '/') {
            	strcat((char*) url, ptr);
	    }

            SvnDwPkg pkg = (SvnDwPkg) {
                .name = ptr,
                .url = url
            };

            DynamicList_add(out, &pkg);
        }
    }
}

static void svn_up(App* app)
{
    {
        char cmd[512];
        sprintf(cmd, "cd \"%s\" && svn up > /dev/null", app->cfg.svn_repo_path);
        int status = system(cmd);
        if ( status == 0 ) {
            LOGF("svn update [1/2] successfull");
        } else {
            ERRF("svn update [1/2] failed");
	    return;
        }
    }

    char packages_dir[512];
    sprintf(packages_dir, "%s/package/", app->cfg.svn_repo_path);

    DIR* pkcats = opendir(packages_dir);
    if ( !pkcats ) {
        ERRF("svn update [2/2] failed");
	return;
    }

    DynamicList TYPES(SvnDwPkg) li;
    DynamicList_init(&li, sizeof(SvnDwPkg), getLIBCAlloc(), 0);

    struct dirent* cat;
    while ( (cat = readdir(pkcats)) )
    {
        if (!strcmp(cat->d_name, ".") || !strcmp(cat->d_name, ".."))
            continue;

        char *catpath = malloc(strlen(packages_dir) + strlen(cat->d_name) + 8);
        if (!catpath)
            continue;
        sprintf(catpath, "%s/%s/", packages_dir, cat->d_name);

        DIR* pkgs = opendir(catpath);
        if ( !pkgs ) {
            free(catpath);
            continue;
        }

        struct dirent* pack;
        while ( (pack = readdir(pkgs)) )
        {
            if (!strcmp(pack->d_name, "."))
                continue;
            if (!strcmp(pack->d_name, ".."))
                continue;

            char* descpath = malloc(strlen(catpath) + strlen(pack->d_name)*2 + 32);
            if (!descpath)
                continue;

            sprintf(descpath, "%s%s/%s.desc", catpath, pack->d_name, pack->d_name);

            FILE* descfile = fopen(descpath, "r");
            if (descfile) {
                svn_up_index_file(app, descfile, &li);
                fclose(descfile);
            }

            free(descpath);
        }

        closedir(pkgs);

        free(catpath);
    }

    closedir(pkcats);

    pthread_rwlock_wrlock(&app->svn_pkgs_mut);
    for (size_t i = 0; i < app->svn_pkgs.fixed.len; i ++)
    {
        SvnDwPkg* pkg = (SvnDwPkg*)FixedList_get(app->svn_pkgs.fixed, i);
        free(pkg->name);
    }
    DynamicList_clear(&app->svn_pkgs);
    app->svn_pkgs = li;
    pthread_rwlock_unlock(&app->svn_pkgs_mut);

    LOGF("svn update [2/2] successfull: found %zu download links", li.fixed.len);
}

static void* svn_up_thread(void* ptr)
{
    App* app = ptr;
    svn_up(app);
    app->reloading_svn_async = false;
    return NULL;
}

static void ensure_dir(char const * path)
{
    struct stat st = {0};
    if ( stat(path, &st) == 0 )
        return;
    mkdir(path, 0755);
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    App app = {0};
    AppCfg_parse(&app.cfg);
    ensure_dir(app.cfg.files_path);

    app.mirrors = app.cfg.mirrors;

    if ( access(app.cfg.files_path, R_OK) )
    {
        if (errno == ENOENT)
        {
            ERRF("data dir doesn't exist: %s", app.cfg.files_path);
        }
        else {
            ERRF("don't have read permissions on data dir: %s", app.cfg.files_path);
        }
        return 1;
    }

    if ( access(app.cfg.files_path, W_OK) )
    {
        ERRF("don't have write permissions on data dir: %s", app.cfg.files_path);
        return 1;
    }

    if (app.cfg.enable_package_stats) {
        slowdb_open_opts opts;
        slowdb_open_opts_default(&opts);
        opts.index_num_buckets = 256;

        char* db_path = malloc(strlen(app.cfg.files_path) + 128);
        sprintf(db_path, "%s/packages.db", app.cfg.files_path);

        // not explicitly closing the db isn't thaaat much of a problem
        if (!( app.per_packet_db = slowdb_openx(db_path, &opts) ))
        {
            ERRF("could not open/create package stats DB");
            return 1;
        }

        pthread_mutex_init(&app.per_packet_db_lock, 0);
    }

    /* SVN */ {
        if ( system("svn --version > /dev/null") != 0 ) {
            ERRF("could not find svn in PATH");
            return 1;
        }

        char cmd[512];
        sprintf(cmd, "mkdir -p \"%s\"", app.cfg.svn_repo_path);
        (void) system(cmd);

        sprintf(cmd, "cd \"%s\" && svn info > /dev/null", app.cfg.svn_repo_path);

        if ( system(cmd) != 0 ) {
            sprintf(cmd, "svn co https://svn.exactcode.de/t2/trunk \"%s\" > /dev/null", app.cfg.svn_repo_path);
            LOGF("executing: %s", cmd);
            if ( system(cmd) != 0 ) {
                ERRF("failed to checkout svn repository");
                return 1;
            }
        }
    }

    LOGF("using %zu threads", app.cfg.http_threads);

    pthread_rwlock_init(&app.mirrors_lock, 0);
    pthread_mutex_init(&app.currently_downloading_lock, 0);
    DynamicList_init(&app.currently_downloading, sizeof(AlreadyDownloading*), getLIBCAlloc(), 0);
    sem_init(&app.download_sem, 0, app.cfg.conc_downloads);
    pthread_rwlock_init(&app.svn_pkgs_mut, 0);
    DynamicList_init(&app.svn_pkgs, sizeof(SvnDwPkg), getLIBCAlloc(), 0);

    reload_print_mirrors(&app);
    timestamp last_mirrors_reload = time_now();
    pthread_t relthr;

    /** do in first http tick asyncly */
    bool force_svn_up = true;
    timestamp last_svn_up = time_now();
    pthread_t upthrd;

    if (app.cfg.verbose) {
        LOGF("verbose logging enabled");
    }

    HttpCfg cfg = (HttpCfg) {
        .bind = app.cfg.bind,
        .reuse = 1,
        .num_threads = app.cfg.http_threads,
        .con_sleep_us = 1000 * (/*ms*/ 5),
        .max_enq_con = 128,
        .handler = serve,
        .verbose = app.cfg.verbose,
    };
    Http* server = http_open(cfg, &app);
    if (server == NULL) {
        ERRF("can't start http server");
        return 1;
    }
    LOGF("Listening on %s", app.cfg.bind);
    while (true) {
        http_tick(server);

        timestamp now = time_now();

        if (!app.reloading_mirrors_async)
        {
            double diff = time_elapsed_seconds(last_mirrors_reload, now);
            if (diff >= app.cfg.mirror_reping_ms)
            {
                app.reloading_mirrors_async = true;
                last_mirrors_reload = now;
                pthread_create(&relthr, 0, mirrors_reload_thread, &app);
            }
        }

        if ( !app.reloading_svn_async )
        {
            double diff = time_elapsed_seconds(last_svn_up, now);
            if (force_svn_up || diff >= app.cfg.svn_up_intvl)
            {
                force_svn_up = false;
                app.reloading_svn_async = true;
                last_svn_up = now;
                pthread_create(&upthrd, 0, svn_up_thread, &app);
            }
        }
    }

    return 0;
}
