#include <cjson/cJSON.h>
#include <stdint.h>
#include <stdlib.h>
#include <hocon.h>
#include <assert.h>
#include <ctype.h>
#include "app.h"

#define cfg_path "config.hocon"
#define cfg_def_path "config.hocon.def"

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
    if (access(cfg_path, F_OK) != 0) {
        if (system("cp " cfg_def_path " " cfg_path) != 0) {
	    ERRF("no config.hocon.def and no config.hocon???");
	    exit(1);
	}
	LOGF("copied config.hocon.def to config.hocon");
    }

    cJSON* j = hocon_parse_file(cfg_path);
    if (!j) {
        ERRF("syntax error in config");
	exit(1);
    }

    cfg->port = (uint16_t) cJSON_GetNumberValue(get_expect(j, "port"));

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
        LOGF("will update svn every %f seconds", cfg->svn_up_intvl);
    } else {
        cfg->svn = false;
        cfg->svn_up_intvl = 0;
    }

    cfg->http_threads = (size_t) cJSON_GetNumberValue(get_expect(j, "http_threads"));
    cfg->conc_downloads = (size_t) cJSON_GetNumberValue(get_expect(j, "conc_downloads"));
}
