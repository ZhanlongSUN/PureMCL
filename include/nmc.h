#ifndef NMC_H
#define NMC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
#include <stdatomic.h>

#define NMC_PATH 4096
#define NMC_ID 96

typedef enum {
    NMC_OK, NMC_MEMORY, NMC_FILE, NMC_PATH_ERROR, NMC_ACCESS, NMC_DISK_FULL,
    NMC_HTTP, NMC_HASH, NMC_JSON, NMC_ZIP, NMC_JAVA, NMC_AUTH, NMC_PROCESS,
    NMC_CANCELLED, NMC_BUSY, NMC_MISSING, NMC_UNSUPPORTED
} NmcCode;

typedef struct {
    NmcCode code;
    unsigned long detail;
    const wchar_t *where; /* Static diagnostic label. Never holds secrets. */
} NmcError;

typedef struct {
    wchar_t root[NMC_PATH];
    wchar_t versions_dir[NMC_PATH]; /* Derived from the executable in production. */
    wchar_t java[NMC_PATH];
    wchar_t jvm[2048];  /* One argument per line. */
    char instance[NMC_ID], version[NMC_ID], account[33];
    unsigned xms, xmx, jobs, gc;
    bool exit_on_launch;
} NmcConfig;

typedef struct {
    char id[NMC_ID];
    char type[24];
} NmcVersionInfo;

typedef struct {
    char id[33];
    char name[64];
} NmcAccountInfo;

typedef enum {
    NMC_STAGE_CATALOG, NMC_STAGE_FILES, NMC_STAGE_ASSETS, NMC_STAGE_JAVA,
    NMC_STAGE_NATIVES, NMC_STAGE_AUTH, NMC_STAGE_LAUNCH
} NmcStage;

typedef struct {
    atomic_bool cancel;
    atomic_uint mirror_hosts;
    atomic_ullong mirror_retry_after;
    atomic_bool mirror_announced;
    void (*progress)(void *, NmcStage, size_t, size_t);
    void (*device)(void *, const wchar_t *, const wchar_t *);
    void (*mirror)(void *);
    void *user;
} NmcTask;

typedef enum { NMC_INSTALL, NMC_REPAIR, NMC_PLAY } NmcAction;

void nmc_task_reset(NmcTask *);
NmcError nmc_config_load(NmcConfig *);
NmcError nmc_config_save(const NmcConfig *);
NmcError nmc_config_validate(const NmcConfig *);
NmcError nmc_versions(const NmcConfig *, bool refresh, NmcTask *,
                      NmcVersionInfo **, size_t *);
NmcError nmc_instances(const NmcConfig *, NmcVersionInfo **, size_t *);
NmcError nmc_instance_create(const NmcConfig *);
NmcError nmc_instance_remove(const NmcConfig *);
NmcError nmc_instance_read(NmcConfig *, const char *id);
NmcError nmc_run(const NmcConfig *, NmcAction, NmcTask *, unsigned long *pid);
NmcError nmc_sign_in(const NmcConfig *, NmcTask *, NmcAccountInfo *);
NmcError nmc_accounts(const NmcConfig *, NmcAccountInfo **, size_t *);
NmcError nmc_sign_out(const NmcConfig *);
void nmc_diagnostic(const NmcConfig *, NmcError);
const wchar_t *nmc_error_text(NmcCode);

#endif

