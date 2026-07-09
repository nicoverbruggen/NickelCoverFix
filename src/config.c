#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "config.h"
#include "util.h"

typedef struct ncf_config_entry_t {
    char *key;
    char *val;
    struct ncf_config_entry_t *next;
} ncf_config_entry_t;

struct ncf_config_t {
    ncf_config_entry_t *head;
    ncf_config_entry_t *tail;
};

static bool ncf_log_file_enabled = true;

bool ncf_should_log_file(void) {
    return ncf_log_file_enabled;
}

static void ncf_config_append(ncf_config_t *cfg, const char *key, const char *val) {
    ncf_config_entry_t *e = (ncf_config_entry_t*)calloc(1, sizeof(ncf_config_entry_t));
    if (!e || !(e->key = strdup(key)) || !(e->val = strdup(val))) {
        NCF_LOG("warning: out of memory while parsing config, skipping '%s'", key);
        if (e) {
            free(e->key);
            free(e->val);
            free(e);
        }
        return;
    }

    if (cfg->tail)
        cfg->tail->next = e;
    else
        cfg->head = e;
    cfg->tail = e;
}

static void ncf_config_write_default(void) {
    mkdir(NCF_CONFIG_DIR, 0755);

    FILE *src = fopen(NCF_CONFIG_DIR "/default", "r");
    if (!src) {
        NCF_LOG("warning: no default config template at %s/default (%s); leaving config absent", NCF_CONFIG_DIR_DISP, strerror(errno));
        return;
    }

    FILE *dst = fopen(NCF_CONFIG_DIR "/config", "w");
    if (!dst) {
        NCF_LOG("warning: could not write default config to %s/config (%s)", NCF_CONFIG_DIR_DISP, strerror(errno));
        fclose(src);
        return;
    }

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) {
            NCF_LOG("warning: could not fully write default config to %s/config", NCF_CONFIG_DIR_DISP);
            break;
        }
    }

    fclose(src);
    fclose(dst);
    NCF_LOG("wrote default config to %s/config from template", NCF_CONFIG_DIR_DISP);
}

ncf_config_t *ncf_config_parse(void) {
    ncf_config_t *cfg = (ncf_config_t*)calloc(1, sizeof(ncf_config_t));
    if (!cfg)
        return NULL;

    FILE *f = fopen(NCF_CONFIG_DIR "/config", "r");
    if (!f && errno == ENOENT) {
        NCF_LOG("no config file at %s/config; writing a default one", NCF_CONFIG_DIR_DISP);
        ncf_config_write_default();
        f = fopen(NCF_CONFIG_DIR "/config", "r");
    }
    if (!f) {
        NCF_LOG("could not open %s/config (%s); using built-in defaults", NCF_CONFIG_DIR_DISP, strerror(errno));
        return cfg;
    }

    char *buf = NULL;
    size_t bufsz = 0;
    ssize_t len;
    int lineno = 0;
    while ((len = getline(&buf, &bufsz, f)) != -1) {
        (void)len;
        lineno++;

        char *hash = strchr(buf, '#');
        if (hash)
            *hash = '\0';

        char *line = strtrim(buf);
        if (!*line)
            continue;

        char *cur = line;
        char *key = strsep(&cur, ":");
        key = strtrim(key);
        if (!key || !*key) {
            NCF_LOG("warning: %s/config: line %d: expected key, ignoring line", NCF_CONFIG_DIR_DISP, lineno);
            continue;
        }
        if (!cur) {
            NCF_LOG("warning: %s/config: line %d: expected ':' after key '%s', ignoring line", NCF_CONFIG_DIR_DISP, lineno, key);
            continue;
        }

        char *val = strtrim(cur);
        ncf_config_append(cfg, key, val);
        NCF_LOG("config: %s = %s", key, val);
    }

    free(buf);
    fclose(f);
    return cfg;
}

const char *ncf_config_get(ncf_config_t *cfg, const char *key) {
    if (!cfg)
        return NULL;
    for (ncf_config_entry_t *e = cfg->head; e; e = e->next)
        if (!strcmp(e->key, key))
            return e->val;
    return NULL;
}

bool ncf_config_bool(ncf_config_t *cfg, const char *key, bool default_value) {
    const char *val = ncf_config_get(cfg, key);
    if (!val || !*val)
        return default_value;
    if (!strcmp(val, "1") || !strcasecmp(val, "true") || !strcasecmp(val, "yes") || !strcasecmp(val, "on"))
        return true;
    if (!strcmp(val, "0") || !strcasecmp(val, "false") || !strcasecmp(val, "no") || !strcasecmp(val, "off"))
        return false;

    NCF_LOG("warning: invalid boolean for '%s': '%s'; using default %d", key, val, default_value ? 1 : 0);
    return default_value;
}

double ncf_config_double(ncf_config_t *cfg, const char *key, double default_value) {
    const char *val = ncf_config_get(cfg, key);
    if (!val || !*val)
        return default_value;

    char *end = NULL;
    errno = 0;
    double parsed = strtod(val, &end);
    bool trailing = false;
    for (const char *p = end; p && *p; p++)
        if (!isspace((unsigned char)*p)) { trailing = true; break; }
    if (errno || end == val || trailing) {
        NCF_LOG("warning: invalid number for '%s': '%s'; using default %.4f", key, val, default_value);
        return default_value;
    }
    return parsed;
}

void ncf_config_free(ncf_config_t *cfg) {
    if (!cfg)
        return;

    ncf_config_entry_t *e = cfg->head;
    while (e) {
        ncf_config_entry_t *next = e->next;
        free(e->key);
        free(e->val);
        free(e);
        e = next;
    }
    free(cfg);
}

static ncf_config_t *ncf_global_config(void) {
    static ncf_config_t *global = NULL;
    if (!global) {
        global = ncf_config_parse();
        ncf_log_file_enabled = ncf_config_bool(global, "ncf_log", true);
    }
    return global;
}

const char *ncf_global_config_get(const char *key) {
    return ncf_config_get(ncf_global_config(), key);
}

bool ncf_global_config_bool(const char *key, bool default_value) {
    return ncf_config_bool(ncf_global_config(), key, default_value);
}

double ncf_global_config_double(const char *key, double default_value) {
    return ncf_config_double(ncf_global_config(), key, default_value);
}
