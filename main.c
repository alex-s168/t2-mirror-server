#include <pthread.h>
#define DEF_LOG
#include "C-Http-Server/inc/httpserv.h"
#include "app.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <dirent.h>
#include "allib/dynamic_list/dynamic_list.h"

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
        LOGF("requested: \"%s\" (original: \"%s\")", reqfile, original ? original : "null");

        if (!app->cfg.enable_remoteurl) {
            original = NULL;
        }

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

int main(int argc, char **argv)
{
    AppCfg acfg = {0};
    AppCfg_parse(&acfg, "config.hocon");

    if (acfg.http_threads == 0 || acfg.conc_downloads == 0) {
        ERRF("invalid config (num threads)");
        return 1;
    }

    LOGF("using %zu threads", acfg.http_threads);

    if (acfg.enable_remoteurl) {
        WARNF("enable_remoteurl is enabled, because of that, make sure that this server is not publicly accessible");
    }

    App app = {0};
    app.cfg = acfg;
    pthread_rwlock_init(&app.mirrors_lock, 0);
    pthread_mutex_init(&app.currently_downloading_lock, 0);
    DynamicList_init(&app.currently_downloading, sizeof(AlreadyDownloading*), getLIBCAlloc(), 0);
    sem_init(&app.download_sem, 0, app.cfg.conc_downloads);

    reload_print_mirrors(&app);
    clock_t last_mirrors_reload = clock();

    bool relthr_init = false;
    pthread_t relthr;

    HttpCfg cfg = (HttpCfg) {
        .port = acfg.port,
        .reuse = 1,
        .num_threads = cfg.num_threads,
        .con_sleep_us = 1000 * (/*ms*/ 5),
        .max_enq_con = 128,
        .handler = serve,
    };
    Http* server = http_open(cfg, &app);
    if (server == NULL) {
        ERRF("can't start http server");
        return 1;
    }
    LOGF("launched on %u", acfg.port);
    while (true) {
        http_tick(server);

        if (!app.reloading_mirrors_async)
        {
            if (relthr_init) {
                // idk
                relthr_init = false;
            }
            clock_t now = clock();
            double diff = (double) (now - last_mirrors_reload) / CLOCKS_PER_SEC * 1000;
            if (diff >= app.cfg.mirrors_recache_intvl)
            {
                app.reloading_mirrors_async = true;
                last_mirrors_reload = now;
                pthread_create(&relthr, 0, mirrors_reload_thread, &app);
                relthr_init = true;
            }
        }
    }

	return 0;
}
