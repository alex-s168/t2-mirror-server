#include <cjson/cJSON.h>
#include <stdint.h>
#include <stdlib.h>
#include <hocon.h>
#include <assert.h>
#include <ctype.h>
#include "allib/dynamic_list/dynamic_list.h"
#include "app.h"

static cJSON* get_expect(cJSON* obj, char const* name) {
    cJSON* out = cJSON_GetObjectItem(obj, name);
    if (out == NULL) {
        ERRF("no member called \"%s\"", name);
        exit(1);
    }
    return out;
}

static double parse_time(cJSON* obj) {
    char const * str = cJSON_GetStringValue(obj);

    char const * timeunit = str;
    while (*timeunit && isdigit(*timeunit)) {
        timeunit ++;
    }

    unsigned num_len = timeunit - str;

    while (*timeunit && isspace(*timeunit)) {
        timeunit ++;
    }

    static char const * allowed_time_units = "smhDW";
    static double unit_mults[] = { 1, 60, 60*60, 24*60*60, 7*24*60*60 };

    char const * timeunit_pt = strchr(allowed_time_units, *timeunit);

    if (!timeunit_pt) {
        ERRF("please specify a time unit, which has to be one of %s", allowed_time_units);
    }

    double mult = unit_mults[timeunit_pt - allowed_time_units];

    char buf[64];
    memcpy(buf, str, num_len);

    double num;
    sscanf(buf, "%lf", &num);

    return num * mult;
}

static size_t parse_time_ms(cJSON* obj) {
    return (size_t) (parse_time(obj) * 1000);
}

static bool expect_bool(cJSON* j) {
    assert(cJSON_IsBool(j));
    return cJSON_IsTrue(j);
}

void AppCfg_parse(AppCfg* cfg)
{
    char const * cfg_path = "config.hocon";

    if (access(cfg_path, F_OK) != 0) {
        if (access("config.hocon.def", F_OK) == 0) {
            assert(system("cp config.hocon.def config.hocon") == 0);
            LOGF("copied config.hocon.def to config.hocon");
        }
        else {
            cfg_path = "/etc/t2-mirror-server.hocon";
            if (access(cfg_path, F_OK) != 0) {
                ERRF("could not find suitable config! tried: \"config.hocon\", \"config.hocon.def\", \"%s\"", cfg_path);
                exit(1);
            }
        }
    }

    LOGF("config path: %s", cfg_path);
    cJSON* j = hocon_parse_file(cfg_path);
    if (!j) {
        ERRF("syntax error in config");
        exit(1);
    }

    cfg->bind = cJSON_GetStringValue(get_expect(j, "bind"));
    assert(cfg->bind);

    cfg->files_path = cJSON_GetStringValue(get_expect(j, "files_path"));
    LOGF("data directory: \"%s\"", cfg->files_path);

    cfg->http_threads = (size_t) cJSON_GetNumberValue(get_expect(j, "http_threads"));
    cfg->conc_downloads = (size_t) cJSON_GetNumberValue(get_expect(j, "conc_downloads"));
    if (cfg->http_threads == 0 || cfg->conc_downloads == 0) {
        ERRF("invalid config (num threads)");
        exit(1);
    }

    cfg->enable_package_stats = expect_bool(get_expect(j, "enable_package_stats"));

    {
        cJSON* verbose = cJSON_GetObjectItem(j, "verbose");
        if (!verbose) {
            WARNF("missing config directive: verbose  default to false");
            cfg->verbose = 0;
        } else {
            cfg->verbose = expect_bool(verbose);
        }
    }

    {
        cJSON* svn = get_expect(j, "svn");

        cfg->svn_up_intvl = parse_time(get_expect(svn, "up_interval_s"));
        cfg->svn_repo_path = cJSON_GetStringValue(get_expect(svn, "repo_path"));

        LOGF("svn repo path: %s", cfg->svn_repo_path);
        LOGF("will update svn every %f seconds", cfg->svn_up_intvl);
    }

    {
        cJSON* download = get_expect(j, "download");

        cfg->mirror_reping_ms = parse_time_ms(get_expect(download, "reping_interval"));

        cJSON* order = get_expect(download, "try_order");

        DynamicList_init(&cfg->mirrors.bundles, sizeof(MirrorsBundle), getLIBCAlloc(), 0);

        cJSON* elt;
        cJSON_ArrayForEach(elt, order)
        {
            MirrorsBundle bundle;
            bundle.donwload_timeout_ms = parse_time_ms(get_expect(elt, "timeout"));

            DynamicList_init(&bundle.items, sizeof(Mirror), getLIBCAlloc(), 0);

            cJSON* original = cJSON_GetObjectItem(elt, "original");
            if (original && cJSON_IsTrue(original))
            {
                bundle.is_original = true;
            }
            else {
                cJSON* options = get_expect(elt, "options");

                cJSON* opt;
                cJSON_ArrayForEach(opt, options)
                {
                    char const* name = cJSON_GetStringValue(opt);
                    Mirror m = {0};
                    m.url = name;
                    *(Mirror*)DynamicList_addp(&bundle.items) = m;
                }
            }

            *(MirrorsBundle*)DynamicList_addp(&cfg->mirrors.bundles) = bundle;
        }
    }
}
