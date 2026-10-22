#include <switch.h>
#include <Python.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <locale.h>
#include <errno.h>
#include <math.h>
#include <limits.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <EGL/egl.h>
#include "game_picker.h"
#include "module_declarations.h"

#include <malloc.h>

/* -------------------------------------------------------
   Globals
------------------------------------------------------- */

u64 cur_progid = 0;
AccountUid userID = {0};

/* -------------------------------------------------------
   Crash handler. Native crashes (in SDL/GL/Mesa/libnx) produce no Python
   traceback, so dump the faulting context here. error_desc=0x101 is a data
   abort; ESR encodes the exact exception class (EC = esr>>26) and fault
   status (DFSC = esr&0x3f); FAR is the faulting address. All addresses are
   also printed relative to &main so the ELF offset is resolvable without
   knowing the NRO load base (aarch64-none-elf-addr2line accepts
   "offset" values: (pc - main) + main_offset).
------------------------------------------------------- */
__attribute__((aligned(16))) u8 __nx_exception_stack[0x1000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

/* Referenced by the crash handler for base-independent address resolution. */
int main(int argc, char *argv[]);

/* Defined by libnx's default heap init. */
extern char *fake_heap_start;
extern char *fake_heap_end;

void __libnx_exception_handler(ThreadExceptionDump *ctx)
{
    u32 esr = ctx->esr;
    printf("\n[main] *** GUEST CPU EXCEPTION (error_desc=0x%x) ***\n", (unsigned)ctx->error_desc);
    printf("[main]  esr=0x%08x (EC=0x%02x) far=0x%016lx\n", esr, esr >> 26, (unsigned long)ctx->far.x);
    printf("[main]  PC = 0x%016lx  (main+0x%lx)\n",
           (unsigned long)ctx->pc.x, (unsigned long)(ctx->pc.x - (u64)(uintptr_t)&main));
    printf("[main]  LR = 0x%016lx  (main+0x%lx)\n",
           (unsigned long)ctx->lr.x, (unsigned long)(ctx->lr.x - (u64)(uintptr_t)&main));
    printf("[main]  SP = 0x%016lx  FP = 0x%016lx\n",
           (unsigned long)ctx->sp.x, (unsigned long)ctx->fp.x);
    printf("[main]  heap %p-%p (%u MiB), applet type %u\n",
           (void *)fake_heap_start, (void *)fake_heap_end,
           (unsigned)(((fake_heap_end - fake_heap_start) >> 20) & 0xFFFFFFFFU),
           (unsigned)appletGetAppletType());
    for (int i = 0; i < 29; i += 3)
        printf("[main]  x%-2d=%016lx  x%-2d=%016lx  x%-2d=%016lx\n",
               i, (unsigned long)ctx->cpu_gprs[i].x,
               i + 1, (unsigned long)ctx->cpu_gprs[i + 1].x,
               i + 2, (unsigned long)ctx->cpu_gprs[i + 2].x);
    fflush(stdout);
}


/* -------------------------------------------------------
   _nx module (sleep)
------------------------------------------------------- */

static PyObject* py_nx_sleep(PyObject* self, PyObject* args)
{
    double seconds;
    if (!PyArg_ParseTuple(args, "d", &seconds))
        return NULL;

    if (!isfinite(seconds) || seconds < 0.0) {
        PyErr_SetString(PyExc_ValueError, "sleep duration must be finite and non-negative");
        return NULL;
    }
    if (seconds >= (double)INT64_MAX / 1000000000.0) {
        PyErr_SetString(PyExc_OverflowError, "sleep duration is too large");
        return NULL;
    }
    int64_t ns = (int64_t)(seconds * 1000000000.0);

    Py_BEGIN_ALLOW_THREADS
    svcSleepThread(ns);
    Py_END_ALLOW_THREADS

    Py_RETURN_NONE;
}

static PyMethodDef NxMethods[] = {
    {"sleep", py_nx_sleep, METH_VARARGS, "Sleep using svcSleepThread"},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef nx_module = {
    PyModuleDef_HEAD_INIT,
    "_nx",
    NULL,
    -1,
    NxMethods
};

PyMODINIT_FUNC PyInit__nx(void)
{
    return PyModule_Create(&nx_module);
}

/* -------------------------------------------------------
   _switch_splash module — boot splash + load progress

   Ren'Py's presplash draws to a software SDL surface, which the Switch's GL-only
   window cannot provide. Instead we render the splash image + a progress bar with an
   SDL_Renderer here (the SDL_Renderer path already verified on hardware), driven from Python's
   presplash pump during script load. The window is fully torn down in end() BEFORE
   Ren'Py creates its GL window, so there is never a second window on the single
   nwindow and no SDL_Renderer/GL-context conflict (the reason the old adopt-window
   splash faulted).
------------------------------------------------------- */

static SDL_Window   *g_splash_win = NULL;
static SDL_Renderer *g_splash_ren = NULL;
static SDL_Texture  *g_splash_tex = NULL;
static int g_splash_w = 0, g_splash_h = 0;   /* window size */
static int g_tex_w = 0, g_tex_h = 0;          /* splash texture size */

static void splash_draw(double frac)
{
    if (!g_splash_ren) return;

    SDL_SetRenderDrawColor(g_splash_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_splash_ren);

    /* Splash image: contain-fit, centered (preserve aspect, no distortion). */
    if (g_splash_tex && g_tex_w > 0 && g_tex_h > 0) {
        double sx = (double)g_splash_w / g_tex_w;
        double sy = (double)g_splash_h / g_tex_h;
        double s  = sx < sy ? sx : sy;
        int dw = (int)(g_tex_w * s);
        int dh = (int)(g_tex_h * s);
        SDL_Rect dst = { (g_splash_w - dw) / 2, (g_splash_h - dh) / 2, dw, dh };
        SDL_RenderCopy(g_splash_ren, g_splash_tex, NULL, &dst);
    }

    /* Progress bar near the bottom. */
    int margin = g_splash_w / 8;
    int bar_w  = g_splash_w - 2 * margin;
    int bar_h  = 14;
    int bar_x  = margin;
    int bar_y  = g_splash_h - 60;

    SDL_SetRenderDrawColor(g_splash_ren, 40, 40, 40, 255);   /* track */
    SDL_Rect track = { bar_x, bar_y, bar_w, bar_h };
    SDL_RenderFillRect(g_splash_ren, &track);

    SDL_SetRenderDrawColor(g_splash_ren, 220, 220, 220, 255); /* fill */
    if (frac >= 0.0) {
        if (frac > 1.0) frac = 1.0;
        SDL_Rect fill = { bar_x, bar_y, (int)(bar_w * frac), bar_h };
        SDL_RenderFillRect(g_splash_ren, &fill);
    } else {
        /* Indeterminate: a segment sliding back and forth (0->1->0 over 2s). */
        int seg = bar_w / 4;
        Uint32 t = SDL_GetTicks() % 2000;
        double p = (t < 1000) ? (t / 1000.0) : (2.0 - t / 1000.0);
        SDL_Rect fill = { bar_x + (int)((bar_w - seg) * p), bar_y, seg, bar_h };
        SDL_RenderFillRect(g_splash_ren, &fill);
    }

    SDL_RenderPresent(g_splash_ren);
}

static PyObject* py_splash_start(PyObject* self, PyObject* args)
{
    const char *path = "romfs:/Contents/splash.png";
    if (!PyArg_ParseTuple(args, "|s", &path))
        return NULL;

    if (g_splash_win)
        return PyLong_FromLong(1);            /* already started */

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
        printf("[splash] video init failed: %s\n", SDL_GetError()); fflush(stdout);
        return PyLong_FromLong(0);
    }
    IMG_Init(IMG_INIT_PNG);

    g_splash_win = SDL_CreateWindow("Splash", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    1280, 720, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN);
    if (!g_splash_win) {
        printf("[splash] window failed: %s\n", SDL_GetError()); fflush(stdout);
        return PyLong_FromLong(0);
    }
    g_splash_ren = SDL_CreateRenderer(g_splash_win, -1, SDL_RENDERER_ACCELERATED);
    if (!g_splash_ren) {
        printf("[splash] renderer failed: %s\n", SDL_GetError()); fflush(stdout);
        SDL_DestroyWindow(g_splash_win); g_splash_win = NULL;
        return PyLong_FromLong(0);
    }
    SDL_GetWindowSize(g_splash_win, &g_splash_w, &g_splash_h);

    /* Prefer the selected game's gui/main_menu.png as the boot splash; fall back to
       the default romfs splash.png if the game doesn't ship one. */
    g_splash_tex = load_selected_game_splash(g_splash_ren);
    if (g_splash_tex) {
        printf("[splash] using selected game's gui/main_menu.png\n"); fflush(stdout);
    } else {
        g_splash_tex = IMG_LoadTexture(g_splash_ren, path);
    }
    if (g_splash_tex) {
        SDL_QueryTexture(g_splash_tex, NULL, NULL, &g_tex_w, &g_tex_h);
    } else {
        printf("[splash] no game main_menu.png and no image at %s (%s); showing bar on black\n",
               path, IMG_GetError());
        fflush(stdout);
    }

    splash_draw(0.0);
    printf("[splash] started (%dx%d)\n", g_splash_w, g_splash_h); fflush(stdout);
    return PyLong_FromLong(1);
}

static PyObject* py_splash_pump(PyObject* self, PyObject* args)
{
    double frac = -1.0;
    if (!PyArg_ParseTuple(args, "|d", &frac))
        return NULL;

    /* Drain events so the OS doesn't flag us unresponsive during the long load. */
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) { /* discard */ }

    splash_draw(frac);
    Py_RETURN_NONE;
}

static PyObject* py_splash_end(PyObject* self, PyObject* args)
{
    (void)self; (void)args;
    if (g_splash_tex) { SDL_DestroyTexture(g_splash_tex); g_splash_tex = NULL; }
    if (g_splash_ren) { SDL_DestroyRenderer(g_splash_ren); g_splash_ren = NULL; }
    if (g_splash_win) { SDL_DestroyWindow(g_splash_win);   g_splash_win = NULL; }
    IMG_Quit();
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    svcSleepThread(100000000ULL);             /* 100ms settle before Ren'Py creates its window. */
    printf("[splash] ended\n"); fflush(stdout);
    Py_RETURN_NONE;
}

static PyMethodDef SplashMethods[] = {
    {"start", py_splash_start, METH_VARARGS, "Create the boot splash window and show the image."},
    {"pump",  py_splash_pump,  METH_VARARGS, "Redraw the splash; arg is 0..1 progress, or <0 for indeterminate."},
    {"end",   py_splash_end,   METH_VARARGS, "Tear down the splash window before Ren'Py builds its GL window."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef splash_module = {
    PyModuleDef_HEAD_INIT, "_switch_splash", NULL, -1, SplashMethods
};

PyMODINIT_FUNC PyInit__switch_splash(void)
{
    return PyModule_Create(&splash_module);
}

/* -------------------------------------------------------
   _otrhlibnx module
------------------------------------------------------- */

static PyObject* commitsave(PyObject* self, PyObject* args)
{
    u64 total_size = 0;
    u64 free_size = 0;
    FsFileSystem* FsSave = fsdevGetDeviceFileSystem("save");

    if (!FsSave)
        Py_RETURN_NONE;

    FsSaveDataInfoReader reader;
    FsSaveDataInfo info;
    s64 total_entries = 0;
    Result rc = 0;

    fsdevCommitDevice("save");

    fsFsGetTotalSpace(FsSave, "/", &total_size);
    fsFsGetFreeSpace(FsSave, "/", &free_size);

    if (free_size < 0x800000)
    {
        u64 new_size = total_size + 0x800000;

        fsdevUnmountDevice("save");
        fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_User);

        while (1)
        {
            rc = fsSaveDataInfoReaderRead(&reader, &info, 1, &total_entries);
            if (R_FAILED(rc) || total_entries == 0)
                break;

            if (info.save_data_type == FsSaveDataType_Account &&
                userID.uid[0] == info.uid.uid[0] &&
                userID.uid[1] == info.uid.uid[1] &&
                info.application_id == cur_progid)
            {
                fsExtendSaveDataFileSystem(
                    info.save_data_space_id,
                    info.save_data_id,
                    new_size,
                    0x400000
                );
                break;
            }
        }

        fsSaveDataInfoReaderClose(&reader);
        fsdevMountSaveData("save", cur_progid, userID);
    }

    Py_RETURN_NONE;
}

static PyObject* startboost(PyObject* self, PyObject* args)
{
    appletSetCpuBoostMode(ApmPerformanceMode_Boost);
    Py_RETURN_NONE;
}

static PyObject* disableboost(PyObject* self, PyObject* args)
{
    appletSetCpuBoostMode(ApmPerformanceMode_Normal);
    Py_RETURN_NONE;
}

static PyObject* restartprogram(PyObject* self, PyObject* args)
{
    appletRestartProgram(NULL, 0);
    Py_RETURN_NONE;
}

/* -------------------------------------------------------
   Memory Debug Function
------------------------------------------------------- */

void print_memory_status(void) {
    u64 mem_available = 0;
    u64 mem_used = 0;
    
    // Получаем информацию о системной памяти через SVC
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    
    // Получаем информацию о куче (Heap) newlib
    struct mallinfo mi = mallinfo();
    
    printf("\n=== MEMORY DEBUG INFO ===\n");
    printf("System Total:    %lu MB\n", mem_available / (1024 * 1024));
    printf("System Used:     %lu MB\n", mem_used / (1024 * 1024));
    printf("System Free:     %lu MB\n", (mem_available - mem_used) / (1024 * 1024));
    
    printf("Heap Arena:      %d MB\n", mi.arena / (1024 * 1024));
    printf("Heap In Use:     %d MB\n", mi.uordblks / (1024 * 1024));
    printf("Heap Free:       %d MB\n", mi.fordblks / (1024 * 1024));
    
    // Информация о разделе Save
    FsFileSystem* fs = fsdevGetDeviceFileSystem("save");
    if (fs) {
        u64 save_total = 0;
        u64 save_free = 0;
        fsFsGetTotalSpace(fs, "/", &save_total);
        fsFsGetFreeSpace(fs, "/", &save_free);
        printf("Save Total:      %lu MB\n", save_total / (1024 * 1024));
        printf("Save Free:       %lu MB\n", save_free / (1024 * 1024));
    } else {
        printf("Save FS:         NOT MOUNTED\n");
    }
    printf("==========================\n\n");
}

static PyObject* py_mem_info(PyObject* self, PyObject* args)
{
    print_memory_status();
    Py_RETURN_NONE;
}

static PyMethodDef OtrhMethods[] = {
    {"commitsave", commitsave, METH_NOARGS, NULL},
    {"startboost", startboost, METH_NOARGS, NULL},
    {"disableboost", disableboost, METH_NOARGS, NULL},
    {"restartprogram", restartprogram, METH_NOARGS, NULL},
    { "mem_info", py_mem_info, METH_NOARGS, "Print memory status to log" },
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef otrh_module = {
    PyModuleDef_HEAD_INIT,
    "_otrhlibnx",
    NULL,
    -1,
    OtrhMethods
};

PyMODINIT_FUNC PyInit__otrhlibnx(void)
{
    return PyModule_Create(&otrh_module);
}


/* Native Ren'Py module initializers are declared in build/generated/
   module_declarations.h, generated and validated by tools/generate_modules.py. */


/* -------------------------------------------------------
   Heap override
------------------------------------------------------- */

/* Deliberately NOT named __libnx_initheap, so libnx uses its default heap init.
   The custom override (svcGetInfo/svcSetHeapSize with a hand-computed size) runs
   before any logging exists and hung on real hardware while working under Ryujinx.
   Rename back to __libnx_initheap to re-enable the custom heap sizing. */
void renpyswitch_initheap_unused(void)
{
    void* addr = NULL;
    u64 size = 0;
    u64 mem_available = 0, mem_used = 0;

    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);

    // ИЗМЕНЕНИЕ: Оставляем системе 64МБ (0x4000000) вместо 2МБ.
    // Это даст буфер для работы файловой системы при сохранении.
    if (mem_available > mem_used + 0x4000000)
        size = (mem_available - mem_used - 0x4000000) & ~0x1FFFFF;

    if (size == 0)
        size = 0x2000000*16; // 256 MB fallback

    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc) || addr == NULL)
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));

    extern char* fake_heap_start;
    extern char* fake_heap_end;

    fake_heap_start = (char*)addr;
    fake_heap_end   = (char*)addr + size;
}

/* -------------------------------------------------------
   Save creation
------------------------------------------------------- */

Result createSaveData(void)
{
    FsSaveDataAttribute attr = {0};
    FsSaveDataCreationInfo crt = {0};
    FsSaveDataMetaInfo meta = {0};

    attr.application_id = cur_progid;
    attr.uid = userID;
    attr.save_data_type = FsSaveDataType_Account;

    crt.save_data_size = 0x800000;   // 8 MB
    crt.journal_size   = 0x400000;   // 4 MB
    crt.available_size = 0x8000;
    crt.save_data_space_id = FsSaveDataSpaceId_User;

    return fsCreateSaveDataFileSystem(&attr, &crt, &meta);
}

/* -------------------------------------------------------
   App init / exit
------------------------------------------------------- */

static bool sockets_ready;
static Result romfs_result;
static bool exit_locked;

void userAppInit(void)
{
    /* libnx owns fs, sdmc, time and applet services. Homebrew must not create
       account savedata under the host title's ProgramId. Saves use the SD card. */
    FILE *log = fopen("sdmc:/renpy_switch.log", "a");
    if (log) {
        int fd = fileno(log);
        if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0)
            fprintf(log, "[boot] log redirection failed: %s\n", strerror(errno));
        fclose(log);
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("\n[boot:1] Ren'Py Switch native entry\n");
    printf("[boot:1] applet type %u, heap %p-%p\n",
           (unsigned)appletGetAppletType(), (void *)fake_heap_start, (void *)fake_heap_end);
    romfs_result = romfsInit();
    if (R_FAILED(romfs_result))
        fprintf(stderr, "[boot] romfsInit failed: 0x%08x\n", romfs_result);
    Result rc = socketInitializeDefault();
    sockets_ready = R_SUCCEEDED(rc);
    if (sockets_ready)
        nxlinkStdio();
    else
        fprintf(stderr, "[boot] socketInitializeDefault failed: 0x%08x\n", rc);
}

void userAppExit(void)
{
    /* This is only a HOOK. libnx's default __appExit() (runtime/init.c) calls us
       first and then tears down the standard services ITSELF:
           fsdevUnmountAll(); fsExit(); timeExit(); hidExit(); appletExit(); smExit();
       So we must NOT repeat any of that (doing so double-freed and faulted before).
       We close ONLY what WE opened in userAppInit() that libnx does not.

       This matters more than it looks: hbloader loads our NRO inside its own
       long-lived process and, on exit, we return to it (hbmenu) rather than ending
       the process — so any service session we leave open persists, and the NEXT
       launch collides with it. That was the "reload crashes the console" bug: we
       leaked the bsd socket session, so run 2's socketInitializeDefault() aborted the
       bsdsocket sysmodule (User Break) and took hbloader down. So we MUST:
         - appletUnlockExit(): balance the startup appletLockExit().
         - fsdevCommitDevice("save"): flush saves to NAND before libnx unmounts.
         - socketExit(): release the bsd session (the reload-crash fix). It only
           appeared to hang before because worker threads were still alive — SDL_Quit()
           and stopping the scan threads now join them before we reach here.
         - accountExit() / nsExit(): release the account + ns sessions we opened.
       romfs is unmounted by libnx's fsdevUnmountAll(). */
    if (exit_locked) {
        appletUnlockExit();
        exit_locked = false;
    }

    if (fsdevGetDeviceFileSystem("save"))
        fsdevCommitDevice("save");

    if (sockets_ready)
        socketExit();
}

/* -------------------------------------------------------
   Error helper
------------------------------------------------------- */

void show_error(const char* message)
{
    printf("[main] show_error: %s\n", message); fflush(stdout);
    if (Py_IsInitialized() && PyErr_Occurred())
        PyErr_Print();

    ErrorSystemConfig c;
    errorSystemCreate(&c, message, message);
    errorSystemShow(&c);

    exit(EXIT_FAILURE);
}

static AppletHookCookie applet_hook_cookie;
static void on_applet_hook(AppletHookType hook, void *param)
{
   switch (hook)
   {
      case AppletHookType_OnExitRequest:
        fsdevCommitDevice("save");
        svcSleepThread(1500000000ULL);
        appletUnlockExit();
        break;

      default:
         break;
   }
}

/* -------------------------------------------------------
   Boot splash
------------------------------------------------------- */

/* Draw a splash via the SDL_Renderer path (works on hardware, unlike Ren'Py's
   software-surface presplash). We present the image and LEAVE the window up: the
   Switch keeps showing the last presented frame during the ~5-8s Python/Ren'Py
   init (which renders nothing), and Ren'Py's first frame later replaces it.
   No-op if romfs:/Contents/splash.png is absent. */
static void show_splash(void)
{
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
        printf("[splash] SDL video init failed: %s\n", SDL_GetError()); fflush(stdout);
        return;
    }
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);

    SDL_Window *win = SDL_CreateWindow("Splash",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1920, 1080,
        SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN);
    if (!win) { printf("[splash] window failed: %s\n", SDL_GetError()); fflush(stdout); return; }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) { printf("[splash] renderer failed: %s\n", SDL_GetError()); fflush(stdout); SDL_DestroyWindow(win); return; }

    SDL_Texture *tex = IMG_LoadTexture(ren, "romfs:/Contents/splash.png");
    if (!tex) {
        printf("[splash] no splash.png (%s); skipping\n", IMG_GetError()); fflush(stdout);
        SDL_DestroyRenderer(ren); SDL_DestroyWindow(win);
        return;
    }

    int sw = 0, sh = 0;
    SDL_GetRendererOutputSize(ren, &sw, &sh);
    SDL_Rect dst = {0, 0, sw, sh};
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, &dst);
    SDL_RenderPresent(ren);
    SDL_DestroyTexture(tex);
    printf("[splash] shown (%dx%d); window left for Ren'Py to adopt\n", sw, sh); fflush(stdout);

    /* Leave the window + GL context current (do NOT destroy ren/win or quit video)
       and tell Ren'Py's display.pyx to adopt THIS window instead of creating a
       second one on the single nwindow. The presented splash persists on the
       framebuffer through init until Ren'Py's first frame draws over it. */
    setenv("RENPY_ADOPT_GL_WINDOW", "1", 1);
    (void)win; (void)ren;
}

/* -------------------------------------------------------
   Main
------------------------------------------------------- */

int main(int argc, char* argv[])
{
    if (R_FAILED(romfs_result))
        show_error("Cannot mount the NRO RomFS. See sdmc:/renpy_switch.log.");

    /* No intro video or logo sequence in this build; the splash and
       Ren'Py itself own the display from here on. */

    if (chdir("romfs:/Contents") != 0)
        show_error("Cannot open romfs:/Contents.");
    setlocale(LC_ALL, "C");
    /* Force double-buffering. The Switch BufferQueue here only allows 1 acquired
       buffer (2-slot/double-buffered), but Mesa defaults to triple-buffering and
       tries to keep 2 in flight -> "max acquired buffer count: 2 (max: 1)" and a
       swap deadlock after a few frames. Pin double-buffering to match the queue. */
    setenv("SDL_VIDEO_DOUBLE_BUFFER", "1", 1);
    /* Disable vsync. The GL swap was blocking on a vblank/fence event that never
       returns on the Switch (menu freezes ~1-2s, then the GPU wait times out and
       Ryujinx closes). Without vsync, eglSwapBuffers does not wait on vblank. */
    setenv("vblank_mode", "0", 1);
    setenv("RENPY_GL_VSYNC", "0", 1);

    /* This is the Ren'Py LOADER: it shows NO picker of its own. The game to run is
       chosen by the external game picker, which chains here via envSetNextLoad and
       passes the selection ON THE COMMAND LINE. Environment variables set with
       setenv() do NOT survive the NRO hand-off, so — exactly like the Godot runtime
       is booted with `--main-pack "<pck>"` — the picker invokes us as:

           renpy_loader.nro --archive "sdmc:/renpy/MyGame.zip" --kind zip --root "MyGame/game/"

       We translate those args into the RENPY_SWITCH_GAME_* env vars that
       renpy.loader.find_switch_archive() reads in THIS process (where setenv works). */
    int have_archive = 0;
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--archive") == 0) {
            setenv("RENPY_SWITCH_GAME_ARCHIVE", argv[++i], 1);
            have_archive = 1;
        } else if (strcmp(argv[i], "--kind") == 0) {
            setenv("RENPY_SWITCH_GAME_KIND", argv[++i], 1);
        } else if (strcmp(argv[i], "--root") == 0) {
            setenv("RENPY_SWITCH_GAME_ROOT", argv[++i], 1);
        }
    }
    for (int i = 0; i < argc; i++) { printf("[loader] argv[%d]=%s\n", i, argv[i]); }
    fflush(stdout);

    /* Fallback for a manual/direct hbmenu launch with no args: a hand-written
       sdmc:/renpy/game.txt naming the archive (path / kind / root, one per line). */
    if (!have_archive) {
        FILE *sel = fopen("sdmc:/renpy/game.txt", "r");
        if (sel) {
            static const char *keys[3] = {
                "RENPY_SWITCH_GAME_ARCHIVE", "RENPY_SWITCH_GAME_KIND", "RENPY_SWITCH_GAME_ROOT" };
            char line[600];
            for (int i = 0; i < 3 && fgets(line, sizeof line, sel); i++) {
                line[strcspn(line, "\r\n")] = '\0';
                if (line[0]) {
                    setenv(keys[i], line, 1);
                    printf("[switch] %s=%s\n", keys[i], line);
                }
            }
            fclose(sel);
            fflush(stdout);
        }
    }

    appletLockExit();
    exit_locked = true;
    appletHook(&applet_hook_cookie, on_applet_hook, NULL);

    /* The legacy Py_* global flags (Py_NoSiteFlag, Py_OptimizeFlag, ...) were removed
       here: they're deprecated since 3.12 (slated for removal) and redundant — the
       PyConfig fields set below are their exact equivalents (site_import,
       use_environment, user_site_directory, write_bytecode, optimization_level). */
    PyConfig config;

    PyConfig_InitIsolatedConfig(&config);

     /* ---- Указываем Python'у его местоположение ---- */
    /* Это устранит ошибку "Could not find platform independent libraries" */
    PyStatus status;
    
    status = PyConfig_SetString(&config, &config.home, L"romfs:/Contents");
    if (PyStatus_Exception(status)) goto exception;

    status = PyConfig_SetString(&config, &config.prefix, L"romfs:/Contents");
    if (PyStatus_Exception(status)) goto exception;
    
    status = PyConfig_SetString(&config, &config.exec_prefix, L"romfs:/Contents");
    if (PyStatus_Exception(status)) goto exception;
   
    /* Give the import system the stdlib (inside lib.zip) + Ren'Py BEFORE init, and
       tell Python to use exactly these paths instead of computing from prefix/home.
       Without this, Py_InitializeFromConfig can't import 'encodings' and aborts. */
    config.module_search_paths_set = 1;
    status = PyWideStringList_Append(&config.module_search_paths, L"romfs:/Contents/lib.zip");
    if (PyStatus_Exception(status)) goto exception;
    status = PyWideStringList_Append(&config.module_search_paths, L"romfs:/Contents");
    if (PyStatus_Exception(status)) goto exception;
   
    /* ---- Critical for Python 3.9 embedded ---- */
    /* Switch has no /dev/urandom and newlib's entropy call fails, which makes
       _Py_HashRandomization_Init abort. Pin a fixed hash seed (0 = randomization
       off) so init never asks the OS for entropy. */
    config.use_hash_seed = 0;  /* _getentropy_r is backed by libnx randomGet. */
    /* Unbuffered Python stdio so tracebacks reach the log before we exit. */
    config.buffered_stdio = 0;
    config.isolated = 1;
    config.use_environment = 0;
    config.site_import = 0;
    config.user_site_directory = 0;
    config.write_bytecode = 0;
    config.optimization_level = 0;
    config.verbose = 0;

    /* Filesystem encoding */
    status = PyConfig_SetString(&config,
                                &config.filesystem_encoding,
                                L"utf-8");
    if (PyStatus_Exception(status)) goto exception;

    status = PyConfig_SetString(&config,
                                &config.filesystem_errors,
                                L"surrogateescape");
    if (PyStatus_Exception(status)) goto exception;

    /* ---- argv ---- */
    wchar_t* pyargv[] = {
        L"romfs:/Contents/renpy.py",
        NULL
    };
    status = PyConfig_SetArgv(&config, 1, pyargv);
    if (PyStatus_Exception(status)) goto exception;

   
    /* Replaces the deprecated Py_SetProgramName(); set it on the config instead. */
    status = PyConfig_SetString(&config, &config.program_name, L"RenPy8.5.3");
    if (PyStatus_Exception(status)) goto exception;



    /* ---- Builtin modules ---- */
    static struct _inittab builtins[] = {

        {"_nx", PyInit__nx},
        {"_otrhlibnx", PyInit__otrhlibnx},
        {"_switch_splash", PyInit__switch_splash},

        /* Ren'Py native modules: generated and validated by
           tools/generate_modules.py against setup.py/setuplib.py. */
#include "module_entries.h"

        {NULL, NULL}
    };

    /* ---- Sanity check ---- */
    FILE* libzip = fopen("romfs:/Contents/lib.zip", "rb");
    if (!libzip) {
        show_error("Could not find lib.zip");
    }
    fclose(libzip);

    FILE* renpy_file = fopen("romfs:/Contents/renpy.py", "rb");
    if (!renpy_file) {
        show_error("Could not find renpy.py");
    }   

    /* Preflight the files the bootstrap imports/reads, so a damaged or stale
       RomFS fails here with an actionable message instead of surfacing as a
       generic "could not set the Python path" dialog. */
    FILE* switch_platform = fopen("romfs:/Contents/switch_platform.py", "rb");
    if (!switch_platform) {
        show_error("The packaged runtime is damaged: switch_platform.py is missing from the RomFS.\nRepackage the game with tools/package_game.py.");
    }
    fclose(switch_platform);

    if (!have_archive) {
        FILE* game_json = fopen("romfs:/Contents/switch-game.json", "rb");
        if (!game_json) {
            show_error("The packaged game is damaged: switch-game.json is missing from the RomFS.\nRepackage the game with tools/package_game.py.");
        }
        fclose(game_json);
    }

    printf("[main] registering builtins + initializing Python\n"); fflush(stdout);
    if (PyImport_ExtendInittab(builtins) < 0)
        show_error("Cannot register Python builtin modules (out of memory).");

    /* ---- Initialize Python ---- */
    printf("[main] calling Py_InitializeFromConfig\n"); fflush(stdout);
    status = Py_InitializeFromConfig(&config);
    if (PyStatus_Exception(status)) {
        printf("[main] Py_InitializeFromConfig FAILED: func=%s err_msg=%s\n",
               status.func ? status.func : "(null)",
               status.err_msg ? status.err_msg : "(null)");
        fflush(stdout);
        goto exception;
    }
    PyConfig_Clear(&config);
    printf("[boot:2] Python initialized: %s\n", Py_GetVersion()); fflush(stdout);


    /* 8.5.3: pygame_sdl2 is merged into renpy.pygame; renpy.py imports what it needs. */


    int python_result;
    python_result = PyRun_SimpleString(
        "import sys, os, traceback\n"
        /* Guarded logging: fd 2 first, then the SD log file. Never raises, so a\n"
           broken stdout/stderr cannot masquerade as a bootstrap failure. */
        "def _boot_dump(text):\n"
        "    for sink in (lambda: os.write(2, text.encode('utf-8', 'replace')),\n"
        "                 lambda: open('sdmc:/renpy_switch.log', 'a').write(text)):\n"
        "        try:\n"
        "            sink()\n"
        "        except BaseException:\n"
        "            pass\n"
        "def _boot_fail(step):\n"
        "    _boot_dump('[boot] FAILED at %s\\n' % step)\n"
        "    _boot_dump(''.join(traceback.format_exception(*sys.exc_info())))\n"
        "    raise SystemExit(1)\n"
        "try:\n"
        "    _boot_dump('[boot:2.1] sys.platform (real) = %r; sys.path = %r\\n' % (sys.platform, sys.path))\n"
        "except BaseException:\n"
        "    _boot_fail('log check')\n"
        "try:\n"
        "    sys.platform = 'horizon'\n"
        "except BaseException:\n"
        "    _boot_fail('setting sys.platform')\n"
        "try:\n"
        "    import switch_platform\n"
        "    _boot_dump('[boot:2.2] switch_platform imported\\n')\n"
        "except BaseException:\n"
        "    _boot_fail('import switch_platform')\n"
        "try:\n"
        "    switch_platform.initialize()\n"
        "    _boot_dump('[boot:2.3] switch_platform initialized\\n')\n"
        "except BaseException:\n"
        "    _boot_fail('switch_platform.initialize()')\n"
    );
    
    if (python_result == -1)
    {
        show_error("Could not set the Python path.\n\nDetails were written to sdmc:/renpy_switch.log");
    }
   #define x(lib) \
    { \
        if (PyRun_SimpleString("import " lib) == -1) \
        { \
            show_error("Could not import the Python library \"" lib "\".\n\nThe packaged game is damaged; details were written to sdmc:/renpy_switch.log"); \
        } \
    }

    x("os");
    x("encodings");

    #undef x

    /* ---- Run Ren'Py ---- */
    printf("[main] running renpy.py ...\n"); fflush(stdout);

    /* Run renpy.py in the __main__ namespace so `__name__ == "__main__"` and
       `sys.modules["__main__"]` resolve correctly for Ren'Py's bootstrap. We use
       PyRun_FileEx (not the *Simple* variant) so that on error the exception stays
       SET instead of being auto-printed to sys.stderr — which Ren'Py swaps around.
       We then dump the formatted traceback straight to fd 2 (our log) via os.write,
       which no sys.stderr reassignment can hide. */
    int rc = 0;
    int clean_exit = 0;
    {
        PyObject *main_mod  = PyImport_AddModule("__main__");   /* borrowed */
        PyObject *main_dict = PyModule_GetDict(main_mod);       /* borrowed */
        /* Normal script execution sets __file__; PyRun_FileEx does not, and
           renpy.py references it on non-horizon paths. Set it defensively. */
        {
            PyObject *fn = PyUnicode_FromString("romfs:/Contents/renpy.py");
            PyDict_SetItemString(main_dict, "__file__", fn);
            Py_DECREF(fn);
        }
        PyObject *res = PyRun_FileEx(renpy_file, "romfs:/Contents/renpy.py",
                                     Py_file_input, main_dict, main_dict, 1 /*closeit*/);
        if (res == NULL) {
            PyObject *exc = PyErr_GetRaisedException();         /* steals ref, clears error */

            if (exc && PyErr_GivenExceptionMatches(exc, PyExc_SystemExit)) {
                /* NOT a crash. Ren'Py's Quit action (and config.quit) calls sys.exit(),
                   which reaches us as SystemExit — the same NULL return a real error
                   gives. Treat it as a normal shutdown: don't pop the Switch error
                   applet (show_error), just fall through and return to hbmenu. */
                clean_exit = 1;
                printf("[main] Ren'Py exited normally (SystemExit)\n"); fflush(stdout);
                Py_XDECREF(exc);
            } else {
                rc = -1;
                PyDict_SetItemString(main_dict, "_renpy_exc", exc ? exc : Py_None);
                PyRun_SimpleString(
                    "import os, traceback\n"
                    "try:\n"
                    "    _t = ''.join(traceback.format_exception(_renpy_exc))\n"
                    "except BaseException as _e:\n"
                    "    _t = '[py] could not format exception: %r\\n' % (_e,)\n"
                    "os.write(2, b'[py] === renpy.py traceback ===\\n')\n"
                    "os.write(2, _t.encode('utf-8', 'replace'))\n"
                    "os.write(2, b'[py] === end traceback ===\\n')\n"
                );
                Py_XDECREF(exc);
            }
        } else {
            Py_DECREF(res);
        }
    }
    printf("[main] renpy.py returned %d (clean_exit=%d)\n", rc, clean_exit); fflush(stdout);

    if (clean_exit) {
        /* Exit is mode-aware, because applet mode and application mode need opposite
           handling — and the mode is fixed by HOW hbloader was launched:

           - APPLICATION mode (forwarder NSP / title takeover; AppletType_*Application):
             full memory, and svcExitProcess() is the normal, clean exit to HOME with
             NO error dialog. Each launch is then a fresh process, so relaunch works and
             Mesa re-inits from scratch. This is the recommended way to run the loader.

           - APPLET mode (launched via the album applet; AppletType_LibraryApplet):
             svcExitProcess() there pops the system "closed because an error occurred"
             notice, so instead we return to hbloader (hbmenu) cleanly. Caveat: applet
             mode reuses this process on relaunch, and Mesa/nouveau does not re-init a
             second time in one process (run 2's first GPU alloc faults). So relaunch is
             only reliable in application mode. */
        appletUnlockExit();
        if (fsdevGetDeviceFileSystem("save"))
            fsdevCommitDevice("save");
        fflush(stdout);

        AppletType at = appletGetAppletType();
        if (at == AppletType_Application || at == AppletType_SystemApplication) {
            printf("[main] clean exit (application mode) -> svcExitProcess -> HOME\n"); fflush(stdout);
            svcExitProcess();
        }

        /* Applet mode: clean return to hbmenu, no dialog. Free the GPU while Mesa/EGL
           is still alive (order matters — eglTerminate AFTER SDL_Quit crashed with
           PC=1 via a freed dispatch pointer), then SDL_Quit() to join SDL's threads,
           then _exit() to skip the faulting static-destructor pass. */
        printf("[main] clean exit (applet mode) -> eglTerminate + SDL_Quit + _exit\n"); fflush(stdout);
        {
            EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
            if (dpy != EGL_NO_DISPLAY) {
                eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                eglTerminate(dpy);
            }
            eglReleaseThread();
        }
        SDL_Quit();
        _exit(0);
    }

    if (rc != 0) {
        show_error("Ren'Py execution failed");
    }

    Py_Finalize();
    return 0;

exception:
    PyConfig_Clear(&config);
    if (PyStatus_IsExit(status)) {
        return status.exitcode;
    }
    show_error(status.err_msg);
    Py_ExitStatusException(status);
}
