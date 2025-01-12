#include "allib/dynamic_list/dynamic_list.h"
#include "app.h"
#include <pthread.h>
#include <stdlib.h>

static void release(AlreadyDownloading* ptr)
{
    if ((--ptr->rc) == 0) {
        free((char*) ptr->fname);
        free(ptr);
    }
}

StartDonwloadingRes start_currently_downloading(App* app, char const* pkg)
{
    pthread_mutex_lock(&app->currently_downloading_lock);

    AlreadyDownloading* already = false;
    for (size_t i = 0; i < app->currently_downloading.fixed.len; i ++)
    {
        AlreadyDownloading* p = *(AlreadyDownloading**)FixedList_get(app->currently_downloading.fixed, i);
        if (!strcmp(p->fname, pkg))
        {
            p->rc ++;
            already = p;
            break;
        }
    }

    AlreadyDownloading* res = NULL;
    if (!already)
    {
        res = malloc(sizeof(AlreadyDownloading));
        if (!res) {
            already = false;
        } else 
        {
            res->fname = strdup(pkg);
            // TODO: check result
            pthread_mutex_init(&res->dlding, NULL);
            pthread_mutex_lock(&res->dlding);
            res->rc = 1;
            DynamicList_add(&app->currently_downloading, &res);
        }
    }

    pthread_mutex_unlock(&app->currently_downloading_lock);

    int status;
    if (already) {
        pthread_mutex_lock(&already->dlding);
        pthread_mutex_unlock(&already->dlding);
        status = already->status;
        release(already);
    }

    return (StartDonwloadingRes) {
        .is_already_doing = already != NULL,
        .dl = res,
        .already_doing_res_status = status,
    };
}

void remove_currently_downloading(App* app, AlreadyDownloading* m, int status)
{
    m->status = status;

    pthread_mutex_lock(&app->currently_downloading_lock);
    for (size_t i = 0; i < app->currently_downloading.fixed.len; i ++)
    {
        AlreadyDownloading* p = *(AlreadyDownloading**)FixedList_get(app->currently_downloading.fixed, i);
        if (p == m) {
            DynamicList_removeAt(&app->currently_downloading, i);
            break;
        }
    }
    pthread_mutex_unlock(&app->currently_downloading_lock);

    pthread_mutex_unlock(&m->dlding);
    release(m);
}
