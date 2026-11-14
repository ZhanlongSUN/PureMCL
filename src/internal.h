#ifndef NMC_INTERNAL_H
#define NMC_INTERNAL_H

#include "nmc.h"
#include <windows.h>
#include <yyjson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NMC_ARRAY(a) (sizeof(a) / sizeof((a)[0]))
#define OK ((NmcError){0})
#define ERR(c,w) ((NmcError){(c),0,(w)})
#define SYS(c,w) nmc_winerr((c),GetLastError(),(w))
#define CHECK(expr) do { NmcError nmc_e_ = (expr); if (nmc_e_.code) return nmc_e_; } while (0)
#define TRY(expr) do { e = (expr); if (e.code) goto done; } while (0)
#define JSON_LIMIT (32u * 1024u * 1024u)
#define VERSION_MANIFEST "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"
#define RUNTIME_MANIFEST "https://piston-meta.mojang.com/v1/products/java-runtime/2ec0cc96c44e5a76b9c8b7c39df7210883d12871/all.json"

typedef struct { char *data; size_t len, cap; } NmcBytes;
typedef struct { wchar_t *data; size_t len, cap; } NmcWide;
typedef struct { wchar_t **v; size_t n, cap; } NmcArgs;
typedef struct {
    char url[2048], hash[65];
    wchar_t path[NMC_PATH];
    uint64_t size;
} NmcFile;
typedef struct { NmcFile *v; size_t n, cap; } NmcFiles;
typedef struct {
    yyjson_doc *chain[12];
    size_t count;
    char main_class[256], asset_id[NMC_ID], type[24], component[NMC_ID];
    unsigned java_major;
    bool legacy_assets, map_assets;
    wchar_t natives[NMC_PATH], unpacked[NMC_PATH], assets[NMC_PATH], game[NMC_PATH], instance[NMC_PATH];
    NmcFile asset_index;
    NmcFiles files, native_zips;
    NmcArgs classpath;
} NmcPlan;
typedef struct {
    NmcAccountInfo player;
    char token[8192], refresh[8192], xuid[32], client[37];
    uint64_t expires;
} NmcSession;

NmcError nmc_winerr(NmcCode, DWORD, const wchar_t *);
bool nmc_cancelled(NmcTask *);
void nmc_progress(NmcTask *, NmcStage, size_t, size_t);
bool nmc_grow(void **, size_t *, size_t, size_t);
bool nmc_bytes_add(NmcBytes *, const void *, size_t);
bool nmc_wide_add(NmcWide *, const wchar_t *, size_t);
bool nmc_args_add(NmcArgs *, const wchar_t *);
void nmc_args_free(NmcArgs *);
bool nmc_utf16(const char *, wchar_t *, size_t);
bool nmc_utf8(const wchar_t *, char *, size_t);
bool nmc_id(const char *);
bool nmc_hex(const char *, size_t);
bool nmc_relative(const char *, wchar_t [NMC_PATH]);
bool nmc_join(wchar_t [NMC_PATH], const wchar_t *, const wchar_t *);
bool nmc_path(wchar_t [NMC_PATH], const wchar_t *, const char *);
NmcError nmc_home(wchar_t [NMC_PATH]);
NmcError nmc_launcher_versions(wchar_t [NMC_PATH]);
NmcError nmc_mkdir(const wchar_t *);
NmcError nmc_parent(const wchar_t *);
bool nmc_file_size(const wchar_t *, uint64_t *);
NmcError nmc_read(const wchar_t *, size_t, NmcBytes *);
NmcError nmc_atomic(const wchar_t *, const void *, size_t);
NmcError nmc_temp(const wchar_t *, wchar_t [NMC_PATH], HANDLE *);
NmcError nmc_promote(const wchar_t *, const wchar_t *);
NmcError nmc_write(HANDLE, const void *, size_t);
NmcError nmc_json(const wchar_t *, yyjson_doc **);
NmcError nmc_json_mem(NmcBytes *, yyjson_doc **);
const char *nmc_str(yyjson_val *, const char *);
bool nmc_copy(char *, size_t, const char *);
NmcError nmc_lock(const wchar_t *, HANDLE *);
NmcError nmc_instance_dir(const NmcConfig *, wchar_t [NMC_PATH]);
NmcError nmc_game_instance(const NmcConfig *, const char *, wchar_t [NMC_PATH]);
NmcError nmc_hash_file(const wchar_t *, size_t, char [65], NmcTask *);
NmcError nmc_check_file(const NmcFile *, bool, NmcTask *);
bool nmc_content_url(const char *);
unsigned nmc_mirror_url(const char *, char [2048]);
NmcError nmc_http_json(const char *, const wchar_t *, const char *, NmcTask *,
                       unsigned *, yyjson_doc **);
NmcError nmc_http_file(const NmcFile *, NmcTask *);
NmcError nmc_http_meta(const char *, const wchar_t *, const char *, NmcTask *);
NmcError nmc_fetch(const NmcFile *, bool, NmcTask *);
NmcError nmc_batch(NmcFiles *, unsigned, bool, NmcStage, NmcTask *);
NmcError nmc_batch_part(NmcFiles *, unsigned, bool, NmcStage, NmcTask *, size_t, size_t);
NmcError nmc_file_meta(NmcFile *, const wchar_t *, const char *, yyjson_val *);
NmcError nmc_catalog(const NmcConfig *, bool, NmcTask *, yyjson_doc **);
NmcError nmc_plan(const NmcConfig *, bool, NmcTask *, NmcPlan *);
void nmc_plan_free(NmcPlan *);
bool nmc_rules(yyjson_val *);
NmcError nmc_assets(const NmcConfig *, const NmcPlan *, bool, NmcTask *);
NmcError nmc_assets_ready(const NmcPlan *);
NmcError nmc_natives(const NmcPlan *, bool, NmcTask *);
NmcError nmc_runtime(const NmcConfig *, const NmcPlan *, bool, bool, NmcTask *,
                     wchar_t [NMC_PATH]);
NmcError nmc_args_build(const NmcConfig *, const NmcPlan *, const NmcSession *,
                        NmcArgs *);
NmcError nmc_command(const wchar_t *, const NmcArgs *, NmcWide *);
NmcError nmc_spawn(const wchar_t *, const wchar_t *, const NmcArgs *,
                   const wchar_t *, unsigned long *);
NmcError nmc_process_idle(const wchar_t *);
NmcError nmc_classpath_jar(const wchar_t *, const NmcArgs *, wchar_t [NMC_PATH]);
NmcError nmc_auth_configured(void);
NmcError nmc_session(const NmcConfig *, NmcTask *, NmcSession *);

#endif

