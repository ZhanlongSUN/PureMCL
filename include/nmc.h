#ifndef NMC_H
#define NMC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
#include <stdatomic.h>

#define NMC_PATH 4096
#define NMC_ID 96
#define NMC_DOWNLOAD_ROUTES 6

typedef enum { NMC_VANILLA, NMC_FABRIC, NMC_FORGE, NMC_NEOFORGE, NMC_OPTIFINE } NmcLoader;

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
    char loader_version[NMC_ID];
    wchar_t loader_profile[NMC_PATH];
    NmcLoader loader;
    unsigned xms, xmx, jobs, gc;
    bool exit_on_launch, track_logs;
} NmcConfig;

typedef struct {
    char id[NMC_ID];
    char type[NMC_ID+32];
} NmcVersionInfo;

typedef struct {
    char id[33];
    char name[64];
} NmcAccountInfo;

typedef enum {
    NMC_STAGE_CATALOG, NMC_STAGE_FILES, NMC_STAGE_ASSETS, NMC_STAGE_JAVA,
    NMC_STAGE_NATIVES, NMC_STAGE_AUTH, NMC_STAGE_LAUNCH, NMC_STAGE_LOADER, NMC_STAGE_PACK
} NmcStage;

typedef struct {
    atomic_bool cancel, stop_workers;
    atomic_uint download_region;
    atomic_ullong mirror_retry_after[NMC_DOWNLOAD_ROUTES];
    atomic_bool mirror_announced;
    atomic_ullong received_bytes, next_transfer_notice;
    void *http_session; /* Borrowed by download workers for the duration of a batch. */
    NmcError watch_error;
    void (*progress)(void *, NmcStage, size_t, size_t);
    void (*device)(void *, const wchar_t *, const wchar_t *);
    void (*mirror)(void *);
    void (*transfer)(void *, uint64_t);
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
NmcError nmc_uninstall(const NmcConfig *, NmcTask *);
NmcError nmc_import_pack(const NmcConfig *, const wchar_t *, NmcTask *);
NmcError nmc_instance_read(NmcConfig *, const char *id);
NmcError nmc_run(const NmcConfig *, NmcAction, NmcTask *, unsigned long *pid);
NmcError nmc_sign_in(const NmcConfig *, NmcTask *, NmcAccountInfo *);
NmcError nmc_accounts(const NmcConfig *, NmcAccountInfo **, size_t *);
NmcError nmc_sign_out(const NmcConfig *);
void nmc_diagnostic(const NmcConfig *, NmcError);
const wchar_t *nmc_error_text(NmcCode);

#endif

