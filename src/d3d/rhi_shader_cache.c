/*
 * rhi_shader_cache.c - compiled shaders kept on disk between runs
 *
 * Every shader the renderer makes is HLSL generated at run time and compiled
 * when a draw first needs it: D3DCompile for D3D11, DXC to SPIR-V for
 * Vulkan. Each compile is a hitch of tens of milliseconds on the thread that
 * draws -- OutRun 2 pays about 45 of them a run, ~50 ms each, and pays them
 * again every run, because the in-memory caches are thrown away at exit.
 *
 * So the compiled blob is kept, one file per shader, under
 *   <user dir>\shadercache\<title id>\<backend>\<key>.bin
 * (%APPDATA%\xboxrecomp on Windows). The key is a hash of everything that
 * went into the compile: the HLSL, its macros, entry point, profile and
 * optimisation flag, plus a backend tag that names the compile settings. A
 * change to the generator changes the HLSL and so the key; nothing has to be
 * invalidated by hand. Each file also holds that key text in full and a load
 * compares it byte for byte, so a hash collision reads as a miss, never as
 * the wrong shader.
 *
 * Files are written to a temporary name and renamed into place, since
 * shaders compile on whichever guest thread draws. RECOMP_SHADER_CACHE=0
 * turns it off.
 */

#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif
#include "rhi.h"
#include "rhi_shader_cache.h"
#include "recomp_config.h"
#include "graphics/graphics_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <strings.h>
#  include <unistd.h>
#  include <sys/stat.h>
#  include <errno.h>
typedef long LONG;
static long cache_increment(volatile long *v) { return __atomic_add_fetch(v,1,__ATOMIC_RELAXED); }
#  define InterlockedIncrement cache_increment
#  define _stricmp strcasecmp
#endif

#include "graphics/shader_cache_clear.h"

#define CACHE_MAGIC   0x43535258u   /* "XRSC" */
#define CACHE_VERSION 2u
#define CACHE_MAX_KEY (2u * 1024u * 1024u)
#define CACHE_MAX_BLOB (64u * 1024u * 1024u)

/* Everything the compile depended on, as one byte string. */
typedef struct {
    char  *p;
    size_t n, cap;
} KeyText;

static int key_add(KeyText *k, const void *data, size_t n)
{
    if (n >= CACHE_MAX_KEY || k->n >= CACHE_MAX_KEY - n) return 0;
    if (k->n + n + 1 > k->cap) {
        size_t cap = k->cap ? k->cap * 2 : 4096;
        char *p;

        while (cap < k->n + n + 1)
            cap *= 2;
        p = (char *)realloc(k->p, cap);
        if (!p)
            return 0;
        k->p = p;
        k->cap = cap;
    }
    memcpy(k->p + k->n, data, n);
    k->n += n;
    k->p[k->n++] = 0;               /* a separator no field can contain */
    return 1;
}

static int key_add_str(KeyText *k, const char *s)
{
    return key_add(k, s ? s : "", s ? strlen(s) : 0);
}

static int key_build(KeyText *k, const RhiShaderSource *src, const char *backend)
{
    char opt[8];
    const RhiMacro *m;

    memset(k, 0, sizeof *k);
    snprintf(opt, sizeof opt, "%u", src->optimize ? 1u : 0u);
    if (!key_add_str(k, backend) || !key_add_str(k, src->entry) ||
        !key_add_str(k, src->target) || !key_add_str(k, opt))
        return 0;
    for (m = src->macros; m && m->name; m++)
        if (!key_add_str(k, m->name) || !key_add_str(k, m->value))
            return 0;
    return key_add(k, src->hlsl, src->len);
}

static uint64_t fnv64(const char *p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= (unsigned char)p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

static int cache_enabled(void)
{
    /* Honour the diagnostic override, then the same persisted preference the
     * launcher and game use. Re-read here so toggling takes effect immediately
     * without creating a renderer-to-input-library link dependency. */
    const char *value=getenv("RECOMP_SHADER_CACHE");
    char path[32768];FILE *file;
    TsfpGraphicsOptions options=tsfp_graphics_defaults();
    if(value && (!strcmp(value,"0") || !_stricmp(value,"off")))return 0;
    if(pc_graphics_path(path,sizeof path) && (file=fopen(path,"rb"))!=NULL) {
        tsfp_graphics_read(file,&options);fclose(file);
    }
    return options.shader_cache;
}

/* Cache paths accept only a single backend/file component. */
static int cache_component(const char *s)
{
    if(!s || !*s || strstr(s,".."))return 0;
    for(;*s;s++) if(!((*s>='a' && *s<='z') || (*s>='A' && *s<='Z') ||
        (*s>='0' && *s<='9') || *s=='-' || *s=='_' || *s=='.'))return 0;
    return 1;
}
static void cache_mkdir(const char *path)
{
    char copy[1024];size_t i;
    if(strlen(path)>=sizeof copy)return;
    strcpy(copy,path);
    for(i=1;;i++) if(copy[i]=='/' || copy[i]=='\\' || copy[i]==0) {
        char end=copy[i];copy[i]=0;
#ifdef _WIN32
        if(i!=2 || copy[1]!=':')CreateDirectoryA(copy,NULL);
#else
        mkdir(copy,0700);
#endif
        copy[i]=end;if(!end)break;
    }
}

/* The directory for this backend, made on first use. 0 if there is none. */
static int cache_dir(const char *backend, char *out, size_t n)
{
    char base[600];
    int len;

    if (!cache_component(backend) || !recomp_config_user_dir(base, sizeof base))
        return 0;
    cache_mkdir(base);
    len = snprintf(out, n, "%s/shadercache", base);
    if (len <= 0 || (size_t)len >= n)
        return 0;
    cache_mkdir(out);
    len = snprintf(out, n, "%s/shadercache/%08X", base,
                   (unsigned)recomp_config_title_id());
    if (len <= 0 || (size_t)len >= n)
        return 0;
    cache_mkdir(out);
    len = snprintf(out, n, "%s/shadercache/%08X/%s", base,
                   (unsigned)recomp_config_title_id(), backend);
    if (len <= 0 || (size_t)len >= n)
        return 0;
    cache_mkdir(out);
    return 1;
}

static int cache_path(const char *backend, const KeyText *k, char *out, size_t n)
{
    char dir[800];
    int len;

    if (!cache_dir(backend, dir, sizeof dir))
        return 0;
    len = snprintf(out, n, "%s/%016llX.bin", dir,
                   (unsigned long long)fnv64(k->p, k->n));
    return len > 0 && (size_t)len < n;
}

static volatile LONG g_hits, g_misses, g_writes;

static void note(const char *what, LONG n)
{
    /* 1, 10, 100, ... so a run says the cache is working without a line a
     * shader. */
    LONG p = 1;

    while (p < n && p < 1000000)
        p *= 10;
    if (p == n) {
        fprintf(stderr, "[SHADER-CACHE] %s: %ld\n", what, (long)n);
        fflush(stderr);
    }
}

int rhi_shader_cache_get(const RhiShaderSource *src, const char *backend,
                         void **blob, size_t *bytes)
{
    KeyText k;
    char path[1024];
    FILE *f = NULL;
    uint32_t hdr[4];
    uint64_t checksum;
    char *stored = NULL;
    void *data = NULL;
    int ok = 0;

    *blob = NULL;
    *bytes = 0;
    if (!cache_enabled() || !src || !src->hlsl)
        return 0;
    if (!key_build(&k, src, backend))
        goto done;
    if (!cache_path(backend, &k, path, sizeof path))
        goto done;
    f = fopen(path, "rb");
    if (!f)
        goto done;
    if (fread(hdr, sizeof hdr, 1, f) != 1 || hdr[0] != CACHE_MAGIC ||
        hdr[1] != CACHE_VERSION || hdr[2] != (uint32_t)k.n || !hdr[3] ||
        hdr[3] > CACHE_MAX_BLOB || fread(&checksum,sizeof checksum,1,f)!=1)
        goto done;
    stored = (char *)malloc(k.n);
    data = malloc(hdr[3]);
    if (!stored || !data || fread(stored, 1, k.n, f) != k.n ||
        memcmp(stored, k.p, k.n) != 0 || fread(data, 1, hdr[3], f) != hdr[3] ||
        fnv64((const char *)data,hdr[3]) != checksum || fgetc(f) != EOF || ferror(f))
        goto done;
    *blob = data;
    *bytes = hdr[3];
    data = NULL;
    ok = 1;
done:
    if (f)
        fclose(f);
    free(stored);
    free(data);
    free(k.p);
    if (cache_enabled())
        note(ok ? "loaded from disk" : "not on disk, compiling",
             InterlockedIncrement(ok ? &g_hits : &g_misses));
    return ok;
}

void rhi_shader_cache_put(const RhiShaderSource *src, const char *backend,
                          const void *blob, size_t bytes)
{
    KeyText k;
    char path[1024], tmp[1100];
    FILE *f;
    uint32_t hdr[4];
    uint64_t checksum;
    int ok;

    if (!cache_enabled() || !src || !src->hlsl || !blob || !bytes || bytes > CACHE_MAX_BLOB)
        return;
    if (!key_build(&k, src, backend)) {
        free(k.p);
        return;
    }
    if (!cache_path(backend, &k, path, sizeof path)) {
        free(k.p);
        return;
    }
#ifdef _WIN32
    snprintf(tmp, sizeof tmp, "%s.%lu.%lu.tmp", path,
        (unsigned long)GetCurrentProcessId(), (unsigned long)GetCurrentThreadId());
    f = fopen(tmp, "wb");
#else
    snprintf(tmp,sizeof tmp,"%s.XXXXXX",path);
    int fd=mkstemp(tmp);
    f=fd<0?NULL:fdopen(fd,"wb");
    if(fd>=0 && !f){close(fd);unlink(tmp);}
#endif
    if (!f) {
        free(k.p);
        return;
    }
    hdr[0] = CACHE_MAGIC;
    hdr[1] = CACHE_VERSION;
    hdr[2] = (uint32_t)k.n;
    hdr[3] = (uint32_t)bytes;
    checksum=fnv64((const char *)blob,bytes);
    ok = fwrite(hdr, sizeof hdr, 1, f) == 1 && fwrite(&checksum,sizeof checksum,1,f)==1 && fwrite(k.p, 1, k.n, f) == k.n &&
         fwrite(blob, 1, bytes, f) == bytes;
    ok = (fclose(f) == 0) && ok;
#ifdef _WIN32
    if(ok)ok=MoveFileExA(tmp,path,MOVEFILE_REPLACE_EXISTING)!=0;
#else
    if(ok)ok=rename(tmp,path)==0;
#endif
    if(!ok)remove(tmp);
    else note("written to disk", InterlockedIncrement(&g_writes));
    free(k.p);
}

static volatile LONG g_cache_cleared;
static int cache_was_cleared(void)
{
#ifdef _WIN32
    return InterlockedCompareExchange(&g_cache_cleared,0,0)!=0;
#else
    return __atomic_load_n(&g_cache_cleared,__ATOMIC_ACQUIRE)!=0;
#endif
}
int rhi_shader_cache_clear(void)
{
    char base[600],path[800];int n;
    if(!recomp_config_user_dir(base,sizeof base))return -1;
    n=snprintf(path,sizeof path,"%s/shadercache/%08X",base,(unsigned)recomp_config_title_id());
    if(n<0 || (size_t)n>=sizeof path)return -1;
    /* Do not resurrect the in-memory Vulkan pipeline cache at shutdown. New
     * shaders may still populate fresh files after the user clears the cache. */
    InterlockedIncrement(&g_cache_cleared);
    return tsfp_cache_clear_level(path,0);
}

int rhi_cache_file_path(const char *backend, const char *name, char *out, size_t n)
{
    char dir[800];
    int len;

    if (cache_was_cleared() || !cache_component(name) || !cache_enabled() || !cache_dir(backend, dir, sizeof dir))
        return 0;
    len = snprintf(out, n, "%s/%s", dir, name);
    return len > 0 && (size_t)len < n;
}

