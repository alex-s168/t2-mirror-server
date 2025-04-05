#include <cjson/cJSON.h>
#include <stdint.h>
#include <stdlib.h>
#include <hocon.h>
#include <assert.h>
#include <ctype.h>
#include <string.h>
#include "app.h"
#include "tpre/tpre.h"

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

    cfg->port = (uint16_t) cJSON_GetNumberValue(get_expect(j, "port"));

    cfg->files_path = cJSON_GetStringValue(get_expect(j, "files_path"));
    LOGF("data directory: \"%s\"", cfg->files_path);

    cfg->mirrors_recache_intvl = parse_time(get_expect(j, "mirrors_recache_interval_s"));
    LOGF("will re-cache mirrors every %f seconds", cfg->mirrors_recache_intvl);

    {
        cJSON* arr = cJSON_GetObjectItem(j, "backing_mirrors");
	if (!arr) {
            cfg->upstream_mirrors_len = 0;
	    cfg->upstream_mirrors = NULL;
	} else {
            cfg->upstream_mirrors_len = cJSON_GetArraySize(arr);
            cfg->upstream_mirrors = malloc(sizeof(char*) * cfg->upstream_mirrors_len);
            for (size_t i = 0; i < cfg->upstream_mirrors_len; i ++) {
                cfg->upstream_mirrors[i] = cJSON_GetStringValue(cJSON_GetArrayItem(arr, i));
            }
	}
    }

    cJSON* enable_remoteurl = get_expect(j, "enable_remoteurl");
    assert(cJSON_IsBool(enable_remoteurl));
    cfg->enable_remoteurl = cJSON_IsTrue(enable_remoteurl);

    cJSON* svn = cJSON_GetObjectItem(j, "svn");
    if (svn) {
        cfg->svn = true;
        cfg->svn_up_intvl = parse_time(get_expect(svn, "up_interval_s"));
	cfg->svn_repo_path = cJSON_GetStringValue(get_expect(svn, "repo_path"));

	LOGF("svn repo path: %s", cfg->svn_repo_path);
        LOGF("will update svn every %f seconds", cfg->svn_up_intvl);
    } else {
        cfg->svn = false;
        cfg->svn_up_intvl = 0;
	cfg->svn_repo_path = NULL;
    }

    cfg->http_threads = (size_t) cJSON_GetNumberValue(get_expect(j, "http_threads"));
    cfg->conc_downloads = (size_t) cJSON_GetNumberValue(get_expect(j, "conc_downloads"));

    if (cfg->http_threads == 0 || cfg->conc_downloads == 0) {
        ERRF("invalid config (num threads)");
        exit(1);
    }

    {
        cJSON* stat = get_expect(j, "static");
        size_t stat_len = cJSON_GetArraySize(stat);
        cfg->static_routes_len = stat_len;
        cfg->static_routes = malloc(sizeof(StaticEnt) * stat_len);
        for (size_t i = 0; i < stat_len; i ++) {
            cJSON* ent = cJSON_GetArrayItem(stat, i);
            char const* matchstr = cJSON_GetStringValue(cJSON_GetArrayItem(ent, 0));
            char const* replace = cJSON_GetStringValue(cJSON_GetArrayItem(ent, 1));
            assert(matchstr);
            assert(replace);

            StaticEnt* se = &cfg->static_routes[i];

            tpre_errs_t errs = {0};
            if (tpre_compile(&se->match, matchstr, &errs)) {
                ERRF("=== regex pattern compile failure for: %s\n", matchstr);
                for (size_t i = 0; i < errs.len; i ++) {
                    tpre_err_t e = errs.items[i];
                    ERRF("at %zu: %s\n", e.pat_byte_loc, e.message);
                }
                ERRF("===\n");
                exit(1);
            }

            DynamicList_init(&se->replace, sizeof(StaticEntRepl), getLIBCAlloc(), 0);

            size_t num_pla = 0;
            do {
                char const* pla = strchr(replace, '$');
                size_t len = pla ? (pla - replace) : strlen(replace);

                if (len > 0) {
                    *(StaticEntRepl*)DynamicList_addp(&se->replace) = (StaticEntRepl) {
                        .str = replace,
                        .strln = len,
                    };
                }

                if (pla) {
                    *(StaticEntRepl*)DynamicList_addp(&se->replace) = (StaticEntRepl) {
                        .str = NULL,
                        .strln = 0,
                    };
                    num_pla ++;

                    replace = pla + 1;
                } else {
                    replace = replace + len;
                }
            } while (replace);

            if (num_pla > se->match.max_group) {
                ERRF("more placeholdrs used than capture groups available (in path for static route) %s", matchstr);
                exit(1);
            }
        }
    }
}
