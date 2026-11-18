#include "internal.h"
#include <shellapi.h>
#include <miniz.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return false; } } while (0)
static bool paths(void) {
    wchar_t out[NMC_PATH];
    REQUIRE(nmc_relative("org/example/测试.jar",out));
    REQUIRE(!wcscmp(out,L"org\\example\\测试.jar"));
    static const char *bad[]={"../evil.dll","a/../evil","/evil","c:bad","//server/a","a\\b","x/con.txt",
        "a/com¹.txt","x/NUL .txt","a/trailing.","a/space ","a//b","a/file:stream","a/./b","a/\xff"};
    for (size_t i=0;i<NMC_ARRAY(bad);++i) REQUIRE(!nmc_relative(bad[i],out));
    REQUIRE(nmc_content_url("https://piston-data.mojang.com/a"));
    REQUIRE(!nmc_content_url("http://piston-data.mojang.com/a"));
    REQUIRE(!nmc_content_url("https://piston-data.mojang.com.evil.invalid/a"));
    char mirror[2048];
    REQUIRE(nmc_mirror_url("https://libraries.minecraft.net/org/library.jar",mirror)==2 &&
        !strcmp(mirror,"https://bmclapi2.bangbang93.com/maven/org/library.jar"));
    REQUIRE(nmc_mirror_url("https://resources.download.minecraft.net/a1/hash",mirror)==1 &&
        !strcmp(mirror,"https://bmclapi2.bangbang93.com/assets/a1/hash"));
    REQUIRE(nmc_mirror_url("https://piston-data.mojang.com/v1/objects/hash/client.jar",mirror)==4 &&
        !strcmp(mirror,"https://bmclapi2.bangbang93.com/v1/objects/hash/client.jar"));
    REQUIRE(!nmc_mirror_url(VERSION_MANIFEST,mirror) && !nmc_mirror_url(RUNTIME_MANIFEST,mirror) &&
        !nmc_mirror_url("https://api.minecraftservices.com/entitlements/mcstore",mirror));
    return true;
}
static bool isolation(void) {
    wchar_t expected[NMC_PATH], exe[NMC_PATH], a[NMC_PATH], b[NMC_PATH], cpath[NMC_PATH];
    REQUIRE(GetModuleFileNameW(NULL,exe,NMC_PATH));
    wchar_t *name=wcsrchr(exe,L'\\'); REQUIRE(name);
    wcscpy_s(name+1,NMC_PATH-(size_t)(name+1-exe),L"versions");
    REQUIRE(!nmc_launcher_versions(expected).code && !wcscmp(exe,expected));
    NmcConfig c={.instance="same",.version="1.21.11",.root=L"C:\\unrelated",.xms=512,.xmx=2048,.jobs=4};
    wcscpy_s(c.versions_dir,NMC_PATH,expected);
    REQUIRE(!nmc_game_instance(&c,"1.21.11",a).code && !nmc_game_instance(&c,"26.3",b).code);
    REQUIRE(!wcscmp(a+wcslen(expected),L"\\1.21.11\\instances\\same"));
    REQUIRE(!wcscmp(b+wcslen(expected),L"\\26.3\\instances\\same"));
    strcpy_s(c.instance,sizeof(c.instance),"other");
    REQUIRE(!nmc_game_instance(&c,"26.3",cpath).code &&
        !wcscmp(cpath+wcslen(expected),L"\\26.3\\instances\\other"));
    REQUIRE(nmc_game_instance(&c,"latest",a).code && nmc_game_instance(&c,"..",a).code);
    unsigned long pid=1;
    REQUIRE(nmc_run(&c,NMC_PLAY,NULL,&pid).code==NMC_AUTH && !pid);
    return true;
}
static bool quotes(void) {
    static const wchar_t *const args[]={L"",L"plain",L"前 后",L"a\"b",L"x\\\"y",L"C:\\two words\\",L"\\\\",L"\""};
    NmcArgs a={0}; NmcWide line={0};
    for (size_t i=0;i<NMC_ARRAY(args);++i) REQUIRE(nmc_args_add(&a,args[i]));
    const wchar_t *exe=L"C:\\Program Files\\Java\\bin\\javaw.exe";
    REQUIRE(!nmc_command(exe,&a,&line).code);
    int count=0; wchar_t **v=CommandLineToArgvW(line.data,&count);
    REQUIRE(v && count==(int)a.n+1 && !wcscmp(v[0],exe));
    for (size_t i=0;i<a.n;++i) REQUIRE(!wcscmp(v[i+1],args[i]));
    LocalFree(v); nmc_args_free(&a); free(line.data); return true;
}
static bool rules(void) {
    const char *json="[{\"action\":\"allow\"},{\"action\":\"disallow\",\"os\":{\"name\":\"windows\",\"arch\":\"x86\"}},"
        "{\"action\":\"disallow\",\"features\":{\"is_demo_user\":true}}]";
    yyjson_doc *d=yyjson_read(json,strlen(json),0);
    REQUIRE(d && nmc_rules(yyjson_doc_get_root(d)));
    yyjson_doc_free(d);
    json="[{\"action\":\"allow\",\"os\":{\"name\":\"linux\"}}]";
    d=yyjson_read(json,strlen(json),0);
    REQUIRE(d && !nmc_rules(yyjson_doc_get_root(d)));
    yyjson_doc_free(d); return true;
}
static bool make_dir(wchar_t dir[NMC_PATH]) {
    wchar_t temp[NMC_PATH];
    DWORD n=GetTempPathW(NMC_PATH,temp);
    return n && n<NMC_PATH && swprintf(dir,NMC_PATH,L"%lspuremcl-ctest-%lu-%llu",temp,
           GetCurrentProcessId(),GetTickCount64())>0 && !nmc_mkdir(dir).code;
}
static bool disk(void) {
    wchar_t dir[NMC_PATH],file[NMC_PATH],jar[NMC_PATH]; char hash[65];
    REQUIRE(make_dir(dir));
    REQUIRE(nmc_join(file,dir,L"payload"));
    REQUIRE(!nmc_atomic(file,"abc",3).code);
    REQUIRE(!nmc_hash_file(file,40,hash,NULL).code &&
        !strcmp(hash,"a9993e364706816aba3e25717850c26c9cd0d89d"));
    REQUIRE(!nmc_hash_file(file,64,hash,NULL).code &&
        !strcmp(hash,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    NmcArgs cp={0};
    REQUIRE(nmc_args_add(&cp,L"C:\\游戏 data\\lib;A%# long path to wrap a manifest line\\one.jar"));
    REQUIRE(!nmc_classpath_jar(dir,&cp,jar).code);
    NmcBytes b={0}; mz_zip_archive z={0};
    REQUIRE(!nmc_read(jar,32768,&b).code && mz_zip_reader_init_mem(&z,b.data,b.len,0));
    size_t n=0;
    char *manifest=mz_zip_reader_extract_file_to_heap(&z,"META-INF/MANIFEST.MF",&n,0);
    REQUIRE(manifest && n>23 && !memcmp(manifest,"Manifest-Version: 1.0\r\n",23));
    NmcBytes unfolded={0};
    for (size_t i=0;i<n;++i) {
        if (i+2<n && manifest[i]=='\r' && manifest[i+1]=='\n' && manifest[i+2]==' ') { i+=2; continue; }
        REQUIRE(nmc_bytes_add(&unfolded,manifest+i,1));
    }
    REQUIRE(strstr(unfolded.data,"file:/C:/%E6%B8%B8%E6%88%8F%20data/lib%3BA%25%23%20long%20path"));
    free(unfolded.data); mz_free(manifest); mz_zip_reader_end(&z); free(b.data); nmc_args_free(&cp);
    DeleteFileW(file); DeleteFileW(jar); RemoveDirectoryW(dir);
    return true;
}
static bool arguments(void) {
    const char *json="{\"id\":\"unit\",\"arguments\":{\"jvm\":[\"-cp\",\"\x24{classpath}\"],"
        "\"game\":[\"--username\",\"\x24{auth_player_name}\",{\"rules\":[{\"action\":\"allow\",\"os\":{\"name\":\"linux\"}}],"
        "\"value\":\"--wrong-os\"},\"--gameDir\",\"\x24{game_directory}\"]}}";
    NmcPlan p={.count=1,.main_class="com.example.Main",.asset_id="unit",.type="release",
        .assets=L"C:\\work\\assets",.game=L"C:\\测试 directory",.java_major=21};
    NmcConfig c={.root=L"C:\\work",.xms=512,.xmx=2048};
    NmcSession s={.player={.id="01234567890123456789012345678901",.name="Player"},.token="test-only"};
    p.chain[0]=yyjson_read(json,strlen(json),0);
    REQUIRE(p.chain[0] && nmc_args_add(&p.classpath,L"C:\\test file.jar"));
    NmcArgs a={0};
    REQUIRE(!nmc_args_build(&c,&p,&s,&a).code);
    REQUIRE(a.n==9 && !wcscmp(a.v[3],L"C:\\test file.jar") && !wcscmp(a.v[4],L"com.example.Main") &&
        !wcscmp(a.v[6],L"Player") && !wcscmp(a.v[8],L"C:\\测试 directory"));
    nmc_args_free(&a); nmc_plan_free(&p); return true;
}
static bool native_receipt(void) {
    wchar_t dir[NMC_PATH], output[NMC_PATH], zip_path[NMC_PATH], stamp[NMC_PATH], dll[NMC_PATH];
    REQUIRE(make_dir(dir));
    REQUIRE(nmc_join(output,dir,L"natives") && nmc_join(zip_path,dir,L"natives.jar") &&
        nmc_join(stamp,output,L".pure-natives") && nmc_join(dll,output,L"sample.dll"));
    mz_zip_archive z={0}; void *blob=NULL; size_t len=0;
    REQUIRE(mz_zip_writer_init_heap(&z,0,0) &&
        mz_zip_writer_add_mem(&z,"sample.dll","native",6,0) &&
        mz_zip_writer_finalize_heap_archive(&z,&blob,&len));
    REQUIRE(!nmc_atomic(zip_path,blob,len).code);
    mz_free(blob); mz_zip_writer_end(&z);
    NmcPlan p={0}; NmcFile zip={0};
    wcscpy_s(p.unpacked,NMC_PATH,output); wcscpy_s(zip.path,NMC_PATH,zip_path);
    REQUIRE(!nmc_hash_file(zip.path,40,zip.hash,NULL).code);
    p.native_zips=(NmcFiles){.v=&zip,.n=1,.cap=1};
    REQUIRE(!nmc_natives(&p,false,NULL).code);
    uint64_t size=0;
    REQUIRE(nmc_file_size(dll,&size) && size==6);
    REQUIRE(DeleteFileW(dll));
    REQUIRE(!nmc_natives(&p,false,NULL).code && nmc_file_size(dll,&size) && size==6);
    DeleteFileW(dll); DeleteFileW(stamp); DeleteFileW(zip_path); RemoveDirectoryW(output); RemoveDirectoryW(dir);
    return true;
}
static bool process(void) {
    wchar_t dir[NMC_PATH], exe[NMC_PATH], mark[NMC_PATH]; NmcArgs args={0};
    REQUIRE(make_dir(dir) && nmc_join(mark,dir,L"process-mark"));
    DWORD exe_len=GetModuleFileNameW(NULL,exe,NMC_PATH);
    REQUIRE(exe_len && exe_len<NMC_PATH);
    REQUIRE(nmc_args_add(&args,L"--child") && nmc_args_add(&args,L"测试 \" 空格\\") && nmc_args_add(&args,dir));
    unsigned long pid=0;
    REQUIRE(!nmc_spawn(exe,dir,&args,mark,&pid,NULL).code && pid);
    nmc_args_free(&args);
    HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid);
    REQUIRE(p);
    REQUIRE(nmc_process_idle(mark).code==NMC_BUSY);
    REQUIRE(WaitForSingleObject(p,5000)==WAIT_OBJECT_0);
    DWORD code=1;
    REQUIRE(GetExitCodeProcess(p,&code) && code==0);
    CloseHandle(p);
    REQUIRE(!nmc_process_idle(mark).code);
    DeleteFileW(mark); RemoveDirectoryW(dir); return true;
}
static bool integration(const wchar_t *dir,const char *id,const wchar_t *mode) {
    NmcConfig c={.instance="integration",.xms=512,.xmx=2048,.jobs=12};
    REQUIRE(wcslen(dir)<NMC_ARRAY(c.root) && nmc_copy(c.version,sizeof(c.version),id));
    wcscpy_s(c.root,NMC_ARRAY(c.root),dir);
    REQUIRE(nmc_join(c.versions_dir,c.root,L"versions"));
    NmcVersionInfo *v=NULL; size_t n=0;
    bool offline=!wcscmp(mode,L"local");
    NmcError e=nmc_versions(&c,!offline,NULL,&v,&n); free(v);
    fwprintf(stderr,L"catalog: %zu versions; code=%u detail=%lu %ls\n",n,(unsigned)e.code,e.detail,e.where?e.where:L"");
    REQUIRE(!e.code && n>0);
    NmcPlan p={0};
    e=nmc_plan(&c,!offline,NULL,&p);
    fwprintf(stderr,L"plan: %zu files, Java %u; code=%u detail=%lu %ls\n",p.files.n,p.java_major,(unsigned)e.code,e.detail,e.where?e.where:L"");
    REQUIRE(!e.code && p.files.n>0);
    wchar_t expected[NMC_PATH], game[NMC_PATH];
    REQUIRE(!nmc_game_instance(&c,id,expected).code && nmc_join(game,expected,L"game") &&
        !wcscmp(p.game,game));
    if (offline) {
        wchar_t java[NMC_PATH]; NmcArgs a={0};
        REQUIRE(!nmc_assets_ready(&p).code);
        for (size_t i=0;i<p.files.n;++i) REQUIRE(!nmc_check_file(&p.files.v[i],false,NULL).code);
        REQUIRE(!nmc_natives(&p,false,NULL).code);
        REQUIRE(!nmc_runtime(&c,&p,false,false,NULL,java).code);
        NmcSession test={.player={.id="01234567890123456789012345678901",.name="Fixture"},
            .token="fixture-only",.client="01234567-89ab-cdef-0123-456789abcdef"};
        REQUIRE(!nmc_args_build(&c,&p,&test,&a).code && a.n>10);
        nmc_args_free(&a);
        REQUIRE(nmc_args_add(&a,L"-version"));
        unsigned long pid=0;
        REQUIRE(!nmc_spawn(java,p.game,&a,NULL,&pid,NULL).code && pid);
        nmc_args_free(&a);
    }
    nmc_plan_free(&p);
    if (!wcscmp(mode,L"install") || !wcscmp(mode,L"repair")) {
        e=nmc_run(&c,!wcscmp(mode,L"install")?NMC_INSTALL:NMC_REPAIR,NULL,NULL);
        fwprintf(stderr,L"install: code=%u detail=%lu %ls\n",(unsigned)e.code,e.detail,e.where?e.where:L"");
        REQUIRE(!e.code);
    }
    return true;
}
static bool mirror_seen;
static bool installed_and_uninstall(void) {
    wchar_t dir[NMC_PATH], data[NMC_PATH], instance[NMC_PATH], file[NMC_PATH];
    REQUIRE(make_dir(dir) && nmc_join(data,dir,L"data"));
    NmcConfig c={.instance="one",.version="1.2.3",.xms=512,.xmx=2048,.jobs=6};
    wcscpy_s(c.root,NMC_PATH,data);
    REQUIRE(nmc_join(c.versions_dir,dir,L"versions"));
    REQUIRE(nmc_path(file,c.versions_dir,"1.2.3/1.2.3.jar") && !nmc_atomic(file,"jar",3).code);
    REQUIRE(nmc_path(file,c.versions_dir,"1.2.3/1.2.3.json") && !nmc_atomic(file,"{}",2).code);
    REQUIRE(nmc_join(file,data,L"assets\\shared") && !nmc_atomic(file,"asset",5).code);
    NmcVersionInfo *v=NULL; size_t n=0;
    REQUIRE(!nmc_instance_create(&c).code && !nmc_instances(&c,&v,&n).code && !n);
    free(v);
    for (unsigned i=0;i<2;++i) {
        strcpy_s(c.instance,sizeof(c.instance),i?"two":"one");
        NmcPlan p={.base_version="1.2.3"};
        REQUIRE(!nmc_game_instance(&c,c.version,p.instance).code &&
            nmc_join(p.game,p.instance,L"game") && nmc_join(file,p.game,L"saves\\level.dat"));
        REQUIRE(!nmc_atomic(file,"world",5).code && !nmc_mark_installed(&c,&p).code);
    }
    REQUIRE(!nmc_instances(&c,&v,&n).code && n==2);
    free(v);
    strcpy_s(c.instance,sizeof(c.instance),"one");
    REQUIRE(!nmc_game_instance(&c,c.version,instance).code && !nmc_uninstall(&c,NULL).code);
    REQUIRE(GetFileAttributesW(instance)==INVALID_FILE_ATTRIBUTES);
    REQUIRE(!nmc_instances(&c,&v,&n).code && n==1 && !strcmp(v[0].id,"two"));
    free(v);
    uint64_t size;
    REQUIRE(nmc_path(file,c.versions_dir,"1.2.3/1.2.3.jar") && nmc_file_size(file,&size));
    strcpy_s(c.instance,sizeof(c.instance),"two");
    REQUIRE(!nmc_uninstall(&c,NULL).code && !nmc_file_size(file,&size));
    REQUIRE(nmc_join(file,data,L"assets\\shared") && nmc_file_size(file,&size) && size==5);
    REQUIRE(!nmc_remove_tree(dir,c.root).code && !nmc_remove_tree(dir,c.versions_dir).code);
    REQUIRE(RemoveDirectoryW(dir));
    return true;
}
static bool bad_pack(void) {
    wchar_t dir[NMC_PATH], source[NMC_PATH];
    REQUIRE(make_dir(dir) && nmc_join(source,dir,L"bad.zip"));
    mz_zip_archive z={0}; void *blob=NULL; size_t len=0;
    REQUIRE(mz_zip_writer_init_heap(&z,0,0) &&
        mz_zip_writer_add_mem(&z,"mods/xx/outside.txt","data",4,0) &&
        mz_zip_writer_finalize_heap_archive(&z,&blob,&len));
    char *bytes=blob;
    const char name[]="mods/xx/outside.txt";
    unsigned occurrences=0;
    for (size_t i=0;i+sizeof(name)<=len;++i) if (!memcmp(bytes+i,name,sizeof(name)-1)) {
        ++occurrences; bytes[i+5]='.'; bytes[i+6]='.';
    }
    REQUIRE(occurrences==2 && !nmc_atomic(source,blob,len).code);
    mz_free(blob); mz_zip_writer_end(&z);
    NmcConfig c={.instance="bad-pack",.version="1.2.3",.xms=512,.xmx=2048,.jobs=6};
    REQUIRE(nmc_join(c.root,dir,L"data") && nmc_join(c.versions_dir,dir,L"versions"));
    NmcError e=nmc_import_pack(&c,source,NULL);
    if (e.code!=NMC_PATH_ERROR) fwprintf(stderr,L"bad pack: %u %lu %ls\n",(unsigned)e.code,e.detail,e.where?e.where:L"");
    REQUIRE(e.code==NMC_PATH_ERROR && GetFileAttributesW(c.versions_dir)==INVALID_FILE_ATTRIBUTES);
    REQUIRE(DeleteFileW(source) && RemoveDirectoryW(dir));
    return true;
}
static void report(void *unused,NmcStage stage,size_t current,size_t total) {
    (void)unused;
    if (!current || current==total) fwprintf(stderr,L"stage %u: %zu / %zu\n",(unsigned)stage,current,total);
}
static bool install_extra(int argc,wchar_t **argv) {
    NmcConfig c={.xms=512,.xmx=2048,.jobs=10};
    REQUIRE(wcslen(argv[2])<NMC_PATH);
    wcscpy_s(c.root,NMC_PATH,argv[2]);
    REQUIRE(nmc_join(c.versions_dir,c.root,L"versions") &&
        nmc_utf8(argv[3],c.version,sizeof(c.version)) && nmc_utf8(argv[4],c.instance,sizeof(c.instance)));
    NmcTask task={.progress=report}; nmc_task_reset(&task);
    NmcError e;
    if (!wcscmp(argv[1],L"--import")) e=nmc_import_pack(&c,argv[5],&task);
    else if (!wcscmp(argv[1],L"--check-loader")) e=nmc_instance_read(&c,c.instance);
    else {
        c.loader=(NmcLoader)wcstoul(argv[5],NULL,10);
        if (argc==7) REQUIRE(nmc_utf8(argv[6],c.loader_version,sizeof(c.loader_version)));
        e=nmc_run(&c,NMC_INSTALL,&task,NULL);
    }
    if (e.code) fwprintf(stderr,L"install: %u %lu %ls\n",(unsigned)e.code,e.detail,e.where?e.where:L"");
    REQUIRE(!e.code && !nmc_instance_read(&c,c.instance).code && nmc_installed(&c));
    NmcPlan p={0}; e=nmc_plan(&c,false,&task,&p);
    if (e.code) fwprintf(stderr,L"plan: %u %lu %ls\n",(unsigned)e.code,e.detail,e.where?e.where:L"");
    REQUIRE(!e.code && !nmc_assets_ready(&p).code);
    NmcSession test={.player={.id="01234567890123456789012345678901",.name="Fixture"},
        .token="fixture-only",.client="01234567-89ab-cdef-0123-456789abcdef"};
    NmcArgs args={0}; e=nmc_args_build(&c,&p,&test,&args);
    if (e.code) fwprintf(stderr,L"args: %u %lu %ls\n",(unsigned)e.code,e.detail,e.where?e.where:L"");
    REQUIRE(!e.code && args.n>10);
    printf("%s %s %s; %zu runtime arguments\n",c.version,c.loader_version,p.main_class,args.n);
    if (c.loader!=NMC_VANILLA && p.java_major>=17) {
        wchar_t java[NMC_PATH], main_class[256];
        NmcArgs verify={0};
        REQUIRE(!nmc_runtime(&c,&p,false,false,&task,java).code &&
            nmc_utf16(p.main_class,main_class,NMC_ARRAY(main_class)) && nmc_args_add(&verify,L"--dry-run"));
        bool found=false;
        for (size_t i=0;i<args.n;++i) {
            REQUIRE(nmc_args_add(&verify,args.v[i]));
            if (!wcscmp(args.v[i],main_class)) { found=true; break; }
        }
        REQUIRE(found);
        /* Stop after Java loads the main class, without game arguments or an account. */
        e=nmc_run_helper(java,p.game,&verify,&task);
        nmc_args_free(&verify);
        if (e.code) fwprintf(stderr,L"JVM dry run: %u %lu %ls\n",(unsigned)e.code,e.detail,e.where?e.where:L"");
        REQUIRE(!e.code);
    }
    nmc_args_free(&args); nmc_plan_free(&p); return true;
}
typedef struct {
    NmcError errors[2];
    unsigned sources;
} RouteFixture;
static NmcError fake_transfer(void *ctx,bool mirror) {
    RouteFixture *f=ctx;
    f->sources=f->sources*10+(mirror?2u:1u);
    return f->errors[mirror];
}
static bool download_routes(void) {
    const char digest[]="b62ca8ec10d07e6bf5ac8dae0c8c1d2e6a1e3356";
    const char storage[]="https://download.mocdn.net/bmclapi/download/b6/"
        "b62ca8ec10d07e6bf5ac8dae0c8c1d2e6a1e3356?X-Amz-Signature=fixture";
    REQUIRE(nmc_bmcl_storage_url(storage,digest));
    REQUIRE(!nmc_bmcl_storage_url(storage,"a62ca8ec10d07e6bf5ac8dae0c8c1d2e6a1e3356"));
    REQUIRE(!nmc_bmcl_storage_url("https://download.mocdn.net/unrelated?X-Amz-Signature=fixture",digest));
    const char mainland[]="{\"country_code\":\"CN\"}";
    REQUIRE(nmc_geo_is_mainland(mainland,sizeof(mainland)-1));
    const char *const other[]={
        "{\"country_code\":\"HK\"}", "{\"country_code\":\"MO\"}", "{\"country_code\":\"TW\"}",
        "{\"country_code\":\"US\"}", "{\"country\":\"CN\"}", "{\"country_code\":\"CN\""
    };
    for (size_t i=0;i<NMC_ARRAY(other);++i) REQUIRE(!nmc_geo_is_mainland(other[i],strlen(other[i])));

    NmcTask task={0}; nmc_task_reset(&task);
    RouteFixture f={0};
    REQUIRE(!nmc_transfer_route(&task,1,NMC_REGION_MAINLAND,fake_transfer,&f).code && f.sources==2);
    f=(RouteFixture){.errors={{NMC_HTTP,12002,L"stalled official download"},OK}};
    REQUIRE(nmc_transfer_route(&task,1,NMC_REGION_OFFICIAL,fake_transfer,&f).detail==12002 && f.sources==1);
    f=(RouteFixture){.errors={OK,{NMC_HTTP,12002,L"stalled mirror download"}}};
    REQUIRE(!nmc_transfer_route(&task,1,NMC_REGION_MAINLAND,fake_transfer,&f).code && f.sources==21);
    f=(RouteFixture){0};
    REQUIRE(!nmc_transfer_route(&task,1,NMC_REGION_MAINLAND,fake_transfer,&f).code && f.sources==1);
    f=(RouteFixture){0};
    REQUIRE(!nmc_transfer_route(&task,2,NMC_REGION_MAINLAND,fake_transfer,&f).code && f.sources==2);
    f=(RouteFixture){0};
    REQUIRE(!nmc_transfer_route(&task,0,NMC_REGION_MAINLAND,fake_transfer,&f).code && f.sources==1);
    return true;
}
typedef struct { size_t seen; unsigned tail; } TailFixture;
static void record_tail(void *ctx,NmcStage stage,size_t done,size_t total) {
    TailFixture *f=ctx;
    if (stage==NMC_STAGE_ASSETS && total==127) {
        if (done>=112 && done==f->seen+1) ++f->tail;
        f->seen=done;
    }
}
static NmcError available_file(void *ctx,size_t i,NmcTask *t) {
    (void)ctx; (void)i; (void)t; return OK;
}
static bool download_progress(void) {
    TailFixture f={0};
    NmcTask task={.progress=record_tail,.user=&f}; nmc_task_reset(&task);
    REQUIRE(!nmc_parallel(127,4,NMC_STAGE_ASSETS,&task,available_file,NULL).code);
    REQUIRE(f.seen==127 && f.tail>=15);
    return true;
}
static void on_mirror(void *unused) { (void)unused; mirror_seen=true; }
static void observe_transfer(void *unused,uint64_t bytes) { (void)unused; (void)bytes; }
static bool check_mirror(const wchar_t *dir,const wchar_t *sha,const wchar_t *size,const wchar_t *url,bool force) {
    NmcFile file={0}; NmcTask task={.mirror=on_mirror,.transfer=observe_transfer}; nmc_task_reset(&task);
    REQUIRE(nmc_utf8(sha,file.hash,sizeof(file.hash)) && nmc_utf8(url,file.url,sizeof(file.url)));
    wchar_t *end=NULL;
    file.size=wcstoull(size,&end,10);
    REQUIRE(end && !*end && file.size>0 && nmc_join(file.path,dir,L"bmclapi-verified-download"));
    char mirror[2048]; unsigned group=nmc_mirror_url(file.url,mirror);
    REQUIRE(group);
    if (force) atomic_store(&task.download_region,NMC_REGION_MAINLAND);
    NmcError e=nmc_http_file(&file,&task);
    if (e.code) fwprintf(stderr,L"mirror transfer: %u %lu %ls\n",(unsigned)e.code,e.detail,e.where?e.where:L"");
    REQUIRE(!e.code && !nmc_check_file(&file,true,NULL).code);
    if (force) REQUIRE(mirror_seen);
    if (atomic_load(&task.download_region)==NMC_REGION_OFFICIAL) REQUIRE(!mirror_seen);
    printf("Selected %s; verified official digest; received %llu bytes.\n",
        mirror_seen?"BMCLAPI":"Mojang",atomic_load(&task.received_bytes));
    return true;
}
int wmain(int argc,wchar_t **argv) {
    if (((argc==6 || argc==7) && (!wcscmp(argv[1],L"--loader") || !wcscmp(argv[1],L"--import"))) ||
        (argc==5 && !wcscmp(argv[1],L"--check-loader")))
        return install_extra(argc,argv)?0:1;
    if (argc==6 && (!wcscmp(argv[1],L"--mirror-check") || !wcscmp(argv[1],L"--source-check")))
        return check_mirror(argv[2],argv[3],argv[4],argv[5],!wcscmp(argv[1],L"--mirror-check")) ? 0 : 1;
    if (argc==4 && !wcscmp(argv[1],L"--child")) {
        wchar_t dir[NMC_PATH];
        DWORD len=GetCurrentDirectoryW(NMC_PATH,dir);
        Sleep(250);
        return !len || len>=NMC_PATH || wcscmp(dir,argv[3]) || wcscmp(argv[2],L"测试 \" 空格\\");
    }
    if (argc==3 || argc==4) {
        char id[NMC_ID];
        return nmc_utf8(argv[2],id,sizeof(id)) && integration(argv[1],id,argc==4?argv[3]:L"plan") ? 0 : 1;
    }
    return paths() && isolation() && quotes() && rules() && disk() && arguments() && native_receipt() && process() &&
        installed_and_uninstall() && bad_pack() && download_routes() && download_progress() ? 0 : 1;
}

