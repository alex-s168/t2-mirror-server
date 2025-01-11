#define DEF_LOG
#include "C-Http-Server/inc/httpserv.h"
#include "app.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

static struct HttpResponse serve(struct HttpRequest request, void* userdata) {
    App* app = userdata;

    struct HttpResponse r = (struct HttpResponse) {
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

    if (strlen(request.path) > 3) {
        char const* reqfile = request.path + 3;
        LOGF("requested: %s", reqfile);

        int status = ensure_downloaded(app, reqfile);
        if (status == 0) {
            char* path = get_local_path(reqfile);
            FILE* f = fopen(path, "rb");
            if (f != NULL) {
                fseek(f, 0, SEEK_END);
                size_t len = ftell(f);
                rewind(f);
                char* data = malloc(len);
                if (data) {
                    r.status = 200;
                    r.status_msg = "OK";
                    r.content_type = http_detectMime(reqfile);
                    r.content_size = len;
                    r.content_mode = HTTP_CONTENT_FILE;
                    r.content_val.file = (HttpFileContent) {
                        .fp = f,
                        .close_after = true,
                    };
                    LOGF("served %s", reqfile);
                } else {
                    fclose(f);
                }
            }
        }
        else {
            LOGF("can't source %s", reqfile);
        }
    }

    return r;
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

    App app = {0};
    app.cfg = acfg;
    pthread_rwlock_init(&app.mirrors_lock, 0);

    reload_print_mirrors(&app);
    clock_t last_mirrors_reload = clock();

    bool relthr_init = false;
    pthread_t relthr;

    LOGF("launched on %u", acfg.port);
    HttpCfg cfg = (HttpCfg) {
        .port = acfg.port,
        .reuse = 1,
        .num_threads = 4,
        .con_sleep_us = 1000 * (/*ms*/ 5),
        .max_enq_con = 128,
        .handler = serve,
    };
    Http* server = http_open(cfg, &app);
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
