/*
 * kernel_path.c - Xbox device-path translation
 *
 * Translates Xbox device-style paths to host filesystem paths:
 *   \Device\CdRom0\  -> <game_dir>/
 *   D:\               -> <game_dir>/
 *   T:\               -> <save_dir>/TitleData/
 *   U:\               -> <save_dir>/UserData/
 *   Z:\               -> <save_dir>/Cache/<title id>/
 *
 * The Win32 build emits UTF-16 paths (for CreateFileW); the Linux build
 * emits UTF-8 paths with '/' separators (for open()).
 */

#include "kernel.h"
#include "xbox_watchpoint.h"
#include "recomp_config.h"   /* recomp_config_title_id: per-title cache */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* The profile chooser switches future UDATA opens only. Existing handles,
 * title caches, assets and partition devices keep their original backing. */
#ifdef _WIN32
static SRWLOCK s_profile_lock=SRWLOCK_INIT;
#define PROFILE_LOCK() AcquireSRWLockExclusive(&s_profile_lock)
#define PROFILE_UNLOCK() ReleaseSRWLockExclusive(&s_profile_lock)
#else
#include <pthread.h>
#include <sys/stat.h>
static pthread_mutex_t s_profile_lock=PTHREAD_MUTEX_INITIALIZER;
#define PROFILE_LOCK() pthread_mutex_lock(&s_profile_lock)
#define PROFILE_UNLOCK() pthread_mutex_unlock(&s_profile_lock)
#endif
static char s_profile_root[4096];
int xbox_set_profile_root_utf8(const char *root)
{
    if(!root || strlen(root)>=sizeof s_profile_root)return 0;
    if(root[0]) {
#ifdef _WIN32
        WCHAR wide[MAX_PATH];
        if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,root,-1,wide,MAX_PATH))return 0;
        if(!(wide[0]&&wide[1]==L':'&&(wide[2]==L'/'||wide[2]==L'\\')) &&
           !(wide[0]==L'\\'&&wide[1]==L'\\'))return 0;
        DWORD attr=GetFileAttributesW(wide);
        if(attr==INVALID_FILE_ATTRIBUTES || !(attr&FILE_ATTRIBUTE_DIRECTORY))return 0;
#else
        struct stat st;
        if(strlen(root)>=MAX_PATH || root[0]!='/' || stat(root,&st) || !S_ISDIR(st.st_mode))return 0;
#endif
    }
    PROFILE_LOCK();memcpy(s_profile_root,root,strlen(root)+1);PROFILE_UNLOCK();
    return 1;
}
static int profile_root_copy(char *out,size_t capacity)
{
    PROFILE_LOCK();size_t n=strlen(s_profile_root);
    int ok=n>0&&n<capacity;
    if(ok)memcpy(out,s_profile_root,n+1);
    PROFILE_UNLOCK();return ok;
}

/* ======================================================================== */
#if defined(_WIN32)
/* ======================================================================== */

#include <shlobj.h>
#include <winioctl.h>

static WCHAR s_game_dir[MAX_PATH];
static WCHAR s_save_dir[MAX_PATH];
static BOOL  s_initialized = FALSE;

/*
 * The raw disk device, \Device\Harddisk0\Partition0.
 *
 * A title opens it to read the Xbox partition table at sector 4 (offset 0x800)
 * and find the cache partitions behind X:, Y: and Z: -- XAPI's utility-drive
 * mount does exactly that, and DSTEAL_JP calls HalReturnToFirmware when the
 * read fails. A directory cannot answer a 512-byte read at a file offset, so
 * the device is backed by an image file instead.
 *
 * The table is the standard retail geometry. This is emulating a device that
 * has to be there, not fabricating anything the title owns.
 */
#define XBOX_DISK_IMAGE_NAME   L"Partition0.img"
#define XBOX_PART_TABLE_OFFSET 0x800
#define XBOX_PART_IN_USE       0x80000000u

static void xbox_write_partition_table(const WCHAR *path)
{
    /* name[16], flags, lba_start, lba_size, reserved -- 32 bytes each */
    static const struct { const char *name; ULONG start, size; } parts[] = {
        { "XBOX_PART_X",  0x00000400, 0x00177000 },  /* X: cache      */
        { "XBOX_PART_Y",  0x00177400, 0x00177000 },  /* Y: cache      */
        { "XBOX_PART_Z",  0x002EE400, 0x00177000 },  /* Z: cache      */
        { "XBOX_PART_C",  0x00465400, 0x000FA000 },  /* C: system     */
        { "XBOX_PART_E",  0x0055F400, 0x00465400 },  /* E: game/save  */
    };
    unsigned char sector[512];
    HANDLE h;
    DWORD written;
    LARGE_INTEGER off;
    size_t i;

    h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;

    memset(sector, 0, sizeof(sector));
    memcpy(sector, "****PARTINFO****", 16);
    for (i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        unsigned char *e = sector + 48 + i * 32;   /* 16 magic + 32 reserved */
        size_t n = strlen(parts[i].name);
        memset(e, ' ', 16);
        memcpy(e, parts[i].name, n < 16 ? n : 16);
        *(ULONG *)(e + 16) = XBOX_PART_IN_USE;
        *(ULONG *)(e + 20) = parts[i].start;
        *(ULONG *)(e + 24) = parts[i].size;
        *(ULONG *)(e + 28) = 0;
    }

    off.QuadPart = XBOX_PART_TABLE_OFFSET;
    if (SetFilePointerEx(h, off, NULL, FILE_BEGIN))
        WriteFile(h, sector, sizeof(sector), &written, NULL);
    CloseHandle(h);
}

void xbox_path_init(const char* game_dir, const char* save_dir)
{
    WCHAR save_base[MAX_PATH];

    /* The fallbacks used to name Burnout 3 specifically, so any other title
     * that passed NULL silently pointed its game dir and its saves at another
     * game's folders. Generic now -- a caller that wants a title-specific
     * location should pass one. */
    if (game_dir) {
        MultiByteToWideChar(CP_UTF8, 0, game_dir, -1, s_game_dir, MAX_PATH);
    } else {
        /* Neutral default: the CWD's "game" subdirectory, which is the
         * layout the game projects use (see templates/new-game). */
        GetCurrentDirectoryW(MAX_PATH, s_game_dir);
    }

    /* RECOMP_SAVE_DIR moves T:, U:, Z: and the partition images out of the
     * per-user default. The default is shared by every title and by the
     * player's own games, so a scripted or test run that does not set this
     * reads and writes the player's real saves -- which is what happened on
     * 30 Sep 2026, and what also made those runs depend on each other. A
     * caller that passes a save_dir of its own still wins. */
    const char *save_source = save_dir ? "caller" : "default";
    if (!save_dir) {
        const char *env = getenv("RECOMP_SAVE_DIR");
        if (env && env[0]) {
            save_dir = env;
            save_source = "RECOMP_SAVE_DIR";
        }
    }

    if (save_dir) {
        MultiByteToWideChar(CP_UTF8, 0, save_dir, -1, s_save_dir, MAX_PATH);
    } else {
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, save_base))) {
            swprintf_s(s_save_dir, MAX_PATH, L"%s\\xboxrecomp", save_base);
        } else {
            GetCurrentDirectoryW(MAX_PATH, s_save_dir);
            wcscat_s(s_save_dir, MAX_PATH, L"\\SaveData");
        }
    }

    size_t len = wcslen(s_game_dir);
    if (len > 0 && s_game_dir[len - 1] == L'\\')
        s_game_dir[len - 1] = L'\0';

    len = wcslen(s_save_dir);
    if (len > 0 && s_save_dir[len - 1] == L'\\')
        s_save_dir[len - 1] = L'\0';

    /* Create the save-side directories. T:/U:/Z: map into subdirectories of
     * save_dir, and a title that opens a file there with a create disposition
     * fails if the parent does not exist -- which reads as "cannot create save
     * file" and sends the title down its init-failure path. Halo asserts
     * exactly that at saved games/game_state_xbox.c:97 and then unwinds,
     * clearing global_d3d_device on the way out, so a missing directory
     * surfaces as a graphics failure.
     *
     * Cheap and idempotent: SHCreateDirectoryExW builds intermediates and is
     * happy if they already exist. */
    {
        static const WCHAR *subs[] = { L"TitleData", L"UserData", L"Cache",
                                       L"SystemData" };
        WCHAR image[MAX_PATH];
        WCHAR dir[MAX_PATH];
        SHCreateDirectoryExW(NULL, s_save_dir, NULL);
        for (int i = 0; i < (int)(sizeof(subs) / sizeof(subs[0])); i++) {
            swprintf_s(dir, MAX_PATH, L"%s\\%s", s_save_dir, subs[i]);
            SHCreateDirectoryExW(NULL, dir, NULL);
        }
        swprintf_s(image, MAX_PATH, L"%s\\%s", s_save_dir, XBOX_DISK_IMAGE_NAME);
        xbox_write_partition_table(image);
        /* The other partition devices, sized to the same geometry the table
         * above describes, so a title that asks the device how big it is gets
         * an answer consistent with the table it just read. Marked sparse
         * first: the cache partitions are 750 MB each and none of that is
         * touched until something writes to it. */
        {
            static const ULONGLONG part_sectors[6] = {
                0,             /* 0: whole disk, sized below      */
                0x00465400ull, /* 1: E: game and saves            */
                0x000FA000ull, /* 2: C: system                    */
                0x00177000ull, /* 3: X: cache                     */
                0x00177000ull, /* 4: Y: cache                     */
                0x00177000ull, /* 5: Z: cache                     */
            };
            for (int p = 1; p <= 5; p++) {
                HANDLE h;
                DWORD ret;
                LARGE_INTEGER end;

                swprintf_s(image, MAX_PATH, L"%s\\Partition%d.img",
                           s_save_dir, p);
                h = CreateFileW(image, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (h == INVALID_HANDLE_VALUE)
                    continue;
                DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0,
                                &ret, NULL);
                end.QuadPart = (LONGLONG)(part_sectors[p] * 512ull);
                if (SetFilePointerEx(h, end, NULL, FILE_BEGIN))
                    SetEndOfFile(h);
                CloseHandle(h);
            }
        }
    }

    s_initialized = TRUE;
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%S, save=%S", s_game_dir, s_save_dir);
    fprintf(stderr, "[PATH] saves in %ls (%s)\n", s_save_dir, save_source);
    fflush(stderr);
}

/* ======================================================================== */
#else /* !_WIN32  -- POSIX / Linux */
/* ======================================================================== */

#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>

static char s_game_dir[MAX_PATH];
static char s_save_dir[MAX_PATH];
static BOOL s_initialized = FALSE;

/* Strip a single trailing '/' (but never the root '/'). */
static void strip_trailing_slash(char* s)
{
    size_t len = strlen(s);
    if (len > 1 && s[len - 1] == '/')
        s[len - 1] = '\0';
}

/* Recursively create a directory and all missing parents. */
static void mkdir_p(const char* path)
{
    char tmp[MAX_PATH];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return;
    memcpy(tmp, path, len + 1);

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "mkdir %s: %s", tmp, strerror(errno));
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "mkdir %s: %s", tmp, strerror(errno));
}

/* Same retail disk geometry as the Windows path. Files are sparse and live
 * strictly under the configured save directory. Existing data is preserved. */
static void posix_partition_images(void)
{
    static const uint32_t sectors[6] = {0, 0x465400, 0xFA000, 0x177000, 0x177000, 0x177000};
    static const struct { const char *name; uint32_t start, size; } parts[] = {
        {"XBOX_PART_X", 0x400, 0x177000}, {"XBOX_PART_Y", 0x177400, 0x177000},
        {"XBOX_PART_Z", 0x2EE400, 0x177000}, {"XBOX_PART_C", 0x465400, 0xFA000},
        {"XBOX_PART_E", 0x55F400, 0x465400}
    };
    mkdir_p(s_save_dir);
    for (unsigned p = 0; p < 6; p++) {
        char path[MAX_PATH];
        if (snprintf(path, sizeof path, "%s/Partition%u.img", s_save_dir, p) >= (int)sizeof path) continue;
        int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0) {
            if (errno != EEXIST) fprintf(stderr, "[PATH] Cannot create %s: %s\n", path, strerror(errno));
            continue;
        }
        int ok;
        if (p) {
            ok = ftruncate(fd, (off_t)sectors[p] * 512) == 0;
        } else {
            unsigned char sector[512] = {0};
            memcpy(sector, "****PARTINFO****", 16);
            for (unsigned i = 0; i < 5; i++) {
                unsigned char *e = sector + 48 + i * 32;
                memset(e, ' ', 16); memcpy(e, parts[i].name, strlen(parts[i].name));
                uint32_t fields[4] = {0x80000000u, parts[i].start, parts[i].size, 0};
                /* Xbox and Apple Silicon are little endian; spell out the disk format. */
                for (unsigned f = 0; f < 4; f++) for (unsigned b = 0; b < 4; b++)
                    e[16 + f * 4 + b] = (unsigned char)(fields[f] >> (b * 8));
            }
            ok = pwrite(fd, sector, sizeof sector, 0x800) == sizeof sector;
        }
        if (!ok) fprintf(stderr, "[PATH] Could not initialize %s: %s\n", path, strerror(errno));
        close(fd);
    }
}

void xbox_path_init(const char* game_dir, const char* save_dir)
{
    if (game_dir) {
        snprintf(s_game_dir, sizeof(s_game_dir), "%s", game_dir);
    } else {
        /* Neutral default: CWD/"game", the layout game projects use. */
        char cwd[MAX_PATH];
        if (!getcwd(cwd, sizeof(cwd)))
            snprintf(cwd, sizeof(cwd), ".");
        snprintf(s_game_dir, sizeof(s_game_dir), "%s/game", cwd);
    }

    /* RECOMP_SAVE_DIR moves T:, U:, Z: and the partition images out of the
     * per-user default. The default is shared by every title and by the
     * player's own games, so a scripted or test run that does not set this
     * reads and writes the player's real saves -- which is what happened on
     * 30 Sep 2026, and what also made those runs depend on each other. A
     * caller that passes a save_dir of its own still wins. */
    const char *save_source = save_dir ? "caller" : "default";
    if (!save_dir) {
        const char *env = getenv("RECOMP_SAVE_DIR");
        if (env && env[0]) {
            save_dir = env;
            save_source = "RECOMP_SAVE_DIR";
        }
    }

    if (save_dir) {
        snprintf(s_save_dir, sizeof(s_save_dir), "%s", save_dir);
    } else {
        /* XDG base-directory spec: $XDG_DATA_HOME or ~/.local/share */
        const char* xdg = getenv("XDG_DATA_HOME");
        if (xdg && xdg[0]) {
            snprintf(s_save_dir, sizeof(s_save_dir), "%s/xboxrecomp", xdg);
        } else {
            const char* home = getenv("HOME");
            snprintf(s_save_dir, sizeof(s_save_dir), "%s/.local/share/xboxrecomp",
                     (home && home[0]) ? home : ".");
        }
    }

    strip_trailing_slash(s_game_dir);
    strip_trailing_slash(s_save_dir);

    posix_partition_images();
    s_initialized = TRUE;
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%s, save=%s",
             s_game_dir, s_save_dir);
    fprintf(stderr, "[PATH] saves in %s (%s)\n", s_save_dir, save_source);
    fflush(stderr);
}

#endif /* _WIN32 */

#include "guest_kernel_paths.inc"

/* Read-only host metadata for the save-device UI. Never exposes console paths
 * or changes the guest's synthetic disk capacity. Call after path init. */
int xbox_save_root_utf8(char *out, unsigned capacity)
{
    if(!out || !capacity || !s_initialized)return 0;
#ifdef _WIN32
    return WideCharToMultiByte(CP_UTF8,0,s_save_dir,-1,out,(int)capacity,NULL,NULL)>0;
#else
    size_t length=strlen(s_save_dir);
    if(length>=capacity)return 0;
    memcpy(out,s_save_dir,length+1);
    return 1;
#endif
}
