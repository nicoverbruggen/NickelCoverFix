#ifndef NCF_CONFIG_H
#define NCF_CONFIG_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#if !(defined(NCF_CONFIG_DIR) && defined(NCF_CONFIG_DIR_DISP))
#error "NCF_CONFIG_DIR not set (it should be done by the Makefile)"
#endif

typedef struct ncf_config_t ncf_config_t;

ncf_config_t *ncf_config_parse(void);
const char *ncf_config_get(ncf_config_t *cfg, const char *key);
bool ncf_config_bool(ncf_config_t *cfg, const char *key, bool default_value);
double ncf_config_double(ncf_config_t *cfg, const char *key, double default_value);
void ncf_config_free(ncf_config_t *cfg);
const char *ncf_global_config_get(const char *key);
bool ncf_global_config_bool(const char *key, bool default_value);
double ncf_global_config_double(const char *key, double default_value);

#ifdef __cplusplus
}
#endif
#endif
