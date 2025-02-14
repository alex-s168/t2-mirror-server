#include "allib/kallok/kallok.h"
#include <ctype.h>
#include <pthread.h>
#include "app.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <dirent.h>
#include "allib/dynamic_list/dynamic_list.h"
#include <signal.h>

static DynamicList TYPES(char) gen_readme(App* app)
{
    DynamicList TYPES(char) out;
    DynamicList_init(&out, sizeof(char), getLIBCAlloc(), 0);

#define addstr(s) \
    DynamicList_addAll(&out, s, strlen(s), sizeof(char));

    addstr("# T/2 download cache\n");
    addstr("use by adding the url of this to your /usr/src/t2-src/download/Mirror-Cache file\n");
    if (app->cfg.enable_remoteurl) {
        addstr("warning: enable_remoteurl is enabled, which means that anyone with access to this server can cache files with fake URLs!\n");
    }
    addstr("\n");
    addstr("cached files:\n");

    DIR* prefixes = opendir("data");
    if (prefixes == NULL) {
        ERRF("can't find data dir? WTF");
        return out;
    }

    struct dirent* ent;
    while ((ent = readdir(prefixes)) != NULL)
    {
        if (!strcmp(ent->d_name, "."))
            continue;
        if (!strcmp(ent->d_name, ".."))
            continue;

        if (strlen(ent->d_name) > 1)
            continue;

        char buf[sizeof("data/") + strlen(ent->d_name) + 1];
        sprintf(buf, "data/%s", ent->d_name);

        DIR* inpref = opendir(buf);
        if (!inpref)
            continue;

        struct dirent* pkg;
        while ((pkg = readdir(inpref)) != NULL)
        {
            if (!strcmp(pkg->d_name, "."))
                continue;
            if (!strcmp(pkg->d_name, ".."))
                continue;

            addstr("* ");
            addstr(pkg->d_name);
            addstr("\n");
        }

        closedir(inpref);
    }

    closedir(prefixes);

#undef addstr

    return out;
}

static struct HttpResponse serve(struct HttpRequest request, void* userdata)
{
    // TODO: protect against spamming (make configurable)

    App* app = userdata;

    if (!strcmp(request.path, "/")) {
        // TODO: cache this
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

    if (strlen(request.path) > 3 && request.path[0] == '/' && request.path[2] == '/') {
        char const* reqfile = request.path + 3;
        char const* original = http_header_get(&request, "X-Orig-URL");

        if (!app->cfg.enable_remoteurl) {
            original = NULL;
        }

        LOGF("requested: \"%s\" (original: \"%s\")", reqfile, original ? original : "null");

        int status = ensure_downloaded(app, reqfile, original);
        if (status == 0) {
            char* path = get_local_path(reqfile);
            FILE* f = fopen(path, "rb");
            if (f != NULL) {
                fseek(f, 0, SEEK_END);
                size_t len = ftell(f);
                rewind(f);
                char* data = malloc(len);
                if (data) {
                    LOGF("served %s", reqfile);
                    return (struct HttpResponse) {
                        .status = 200,
                        .status_msg = "OK",
                        .content_type = http_detectMime(reqfile),
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
    int status = system("cd t2-trunk && svn up > /dev/null");

    if ( status == 0 ) {
        LOGF("svn update [1/2] successfull");
    } else {
        ERRF("svn update [1/2] failed");
    }

    DIR* pkcats = opendir("t2-trunk/package/");
    if ( !pkcats ) {
        ERRF("svn update [2/2] failed");
    }

    DynamicList TYPES(SvnDwPkg) li;
    DynamicList_init(&li, sizeof(SvnDwPkg), getLIBCAlloc(), 0);

    struct dirent* cat;
    while ( (cat = readdir(pkcats)) )
    {
        if (!strcmp(cat->d_name, ".") || !strcmp(cat->d_name, ".."))
            continue;

        char *catpath = malloc(strlen(cat->d_name) + 32);
        if (!catpath)
            continue;
        sprintf(catpath, "t2-trunk/package/%s/", cat->d_name);

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

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    App app = {0};

    AppCfg_parse(&app.cfg);

    if (app.cfg.http_threads == 0 || app.cfg.conc_downloads == 0) {
        ERRF("invalid config (num threads)");
        return 1;
    }

    if (app.cfg.svn) {
        if ( system("svn --version > /dev/null") != 0 ) {
            ERRF("could not find svn in PATH");
            return 1;
        }

        if ( system("cd t2-trunk && svn info > /dev/null") != 0 ) {
            ERRF("t2-trunk/ is not a valid svn repository");
            ERRF("consider running: \"svn co https://svn.exactcode.de/t2/trunk t2-trunk\"");
            return 1;
        }
    }

    LOGF("using %zu threads", app.cfg.http_threads);

    if (app.cfg.enable_remoteurl) {
        WARNF("enable_remoteurl is enabled, because of that, make sure that this server is not publicly accessible");
    }

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

    HttpCfg cfg = (HttpCfg) {
        .port = app.cfg.port,
        .reuse = 1,
        .num_threads = app.cfg.http_threads,
        .con_sleep_us = 1000 * (/*ms*/ 5),
        .max_enq_con = 128,
        .handler = serve,
    };
    Http* server = http_open(cfg, &app);
    if (server == NULL) {
        ERRF("can't start http server");
        return 1;
    }
    LOGF("launched on %u", app.cfg.port);
    while (true) {
        http_tick(server);

        timestamp now = time_now();

        if (!app.reloading_mirrors_async)
        {
            double diff = time_elapsed_seconds(last_mirrors_reload, now);
            if (diff >= app.cfg.mirrors_recache_intvl)
            {
                app.reloading_mirrors_async = true;
                last_mirrors_reload = now;
                pthread_create(&relthr, 0, mirrors_reload_thread, &app);
            }
        }

        if (app.cfg.svn && !app.reloading_svn_async)
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
