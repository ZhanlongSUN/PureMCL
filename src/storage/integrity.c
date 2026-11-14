#include "internal.h"
#include <bcrypt.h>

NmcError nmc_hash_file(const wchar_t *path, size_t digits, char out[65], NmcTask *task) {
    BCRYPT_ALG_HANDLE alg=NULL; BCRYPT_HASH_HANDLE h=NULL;
    HANDLE f=INVALID_HANDLE_VALUE; uint64_t ignored;
    unsigned char *buf=NULL, hash[32];
    NmcError e=OK; NTSTATUS status;
    if (digits!=40 && digits!=64) return ERR(NMC_HASH,L"digest algorithm");
    if (!nmc_file_size(path,&ignored)) return ERR(NMC_MISSING,L"hash input");
    f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,
                  FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,NULL);
    if (f==INVALID_HANDLE_VALUE) return SYS(NMC_FILE,L"hash input");
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(f,&info) || (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {
        e=ERR(NMC_PATH_ERROR,L"hash input type"); goto done;
    }
    status=BCryptOpenAlgorithmProvider(&alg,digits==40 ? BCRYPT_SHA1_ALGORITHM : BCRYPT_SHA256_ALGORITHM,NULL,0);
    if (status<0) goto hash_error;
    status=BCryptCreateHash(alg,&h,NULL,0,NULL,0,0);
    if (status<0) goto hash_error;
    buf=malloc(65536);
    if (!buf) { e=ERR(NMC_MEMORY,L"hash buffer"); goto done; }
    for (;;) {
        DWORD read=0;
        if (nmc_cancelled(task)) { e=ERR(NMC_CANCELLED,L"hash"); goto done; }
        if (!ReadFile(f,buf,65536,&read,NULL)) { e=SYS(NMC_FILE,L"hash read"); goto done; }
        if (!read) break;
        status=BCryptHashData(h,buf,read,0);
        if (status<0) goto hash_error;
    }
    status=BCryptFinishHash(h,hash,(ULONG)(digits/2),0);
    if (status<0) goto hash_error;
    static const char hex[]="0123456789abcdef";
    for (size_t i=0;i<digits/2;++i) { out[i*2]=hex[hash[i]>>4]; out[i*2+1]=hex[hash[i]&15]; }
    out[digits]=0; goto done;
hash_error:
    e=(NmcError){NMC_HASH,(DWORD)status,L"BCrypt"};
done:
    free(buf);
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg,0);
    CloseHandle(f);
    return e;
}
NmcError nmc_check_file(const NmcFile *f, bool verify, NmcTask *t) {
    uint64_t size;
    if (!nmc_file_size(f->path,&size)) return ERR(NMC_MISSING,L"dependency file");
    if (size!=f->size) return ERR(NMC_HASH,L"dependency size");
    if (verify) {
        char digest[65];
        CHECK(nmc_hash_file(f->path,strlen(f->hash),digest,t));
        if (strcmp(digest,f->hash)) return ERR(NMC_HASH,L"dependency digest");
    }
    return OK;
}

