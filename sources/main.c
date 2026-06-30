#include <switch.h>
#include <Python.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <locale.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <png.h>
#include "video_player.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <malloc.h>

/* -------------------------------------------------------
   Globals
------------------------------------------------------- */

u64 cur_progid = 0;
AccountUid userID = {0};

/* -------------------------------------------------------
   Crash handler. Native crashes (in SDL/GL/Mesa/libnx) produce no Python
   traceback, and Ryujinx's GPU debug log shows only "Success", so dump the
   faulting program counter here. error_desc tells the fault type; PC/LR locate
   it (subtract the NRO load base, then addr2line on renpy-switch.elf). If this
   never fires but the app still dies, it's a hang (deadlock), not a crash.
------------------------------------------------------- */
__attribute__((aligned(16))) u8 __nx_exception_stack[0x1000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump *ctx)
{
    printf("\n[main] *** GUEST CPU EXCEPTION (error_desc=0x%x) ***\n", (unsigned)ctx->error_desc);
    printf("[main]  PC = 0x%016lx\n", (unsigned long)ctx->pc.x);
    printf("[main]  LR = 0x%016lx\n", (unsigned long)ctx->lr.x);
    printf("[main]  FP = 0x%016lx\n", (unsigned long)ctx->fp.x);
    printf("[main]  SP = 0x%016lx\n", (unsigned long)ctx->sp.x);
    fflush(stdout);
}


void show_logos(void)
{
    printf("=== Showing logos ===\n");
    
    // Инициализируем только то, что нужно для логотипов
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0) {
        printf("SDL_InitSubSystem failed for logos: %s\n", SDL_GetError());
        return;
    }
    
    int img_flags = IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_TIF | IMG_INIT_WEBP;
    if (!(IMG_Init(img_flags) & img_flags)) {
        printf("IMG_Init failed: %s\n", IMG_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
        return;
    }
    
    // Открываем все доступные контроллеры
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            SDL_GameController* controller = SDL_GameControllerOpen(i);
            if (controller) {
                printf("Opened controller %d: %s\n", i, SDL_GameControllerName(controller));
            }
        }
    }
    
    // Создаем окно и рендерер для логотипов
    SDL_Window* logo_window = SDL_CreateWindow("Logos", 
                                          SDL_WINDOWPOS_CENTERED, 
                                          SDL_WINDOWPOS_CENTERED, 
                                          1280, 720, 
                                          SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN);
    
    if (!logo_window) {
        printf("SDL_CreateWindow failed: %s\n", SDL_GetError());
        IMG_Quit();
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
        return;
    }
    
    SDL_Renderer* logo_renderer = SDL_CreateRenderer(logo_window, -1, 
                                                SDL_RENDERER_ACCELERATED | 
                                                SDL_RENDERER_PRESENTVSYNC);
    
    if (!logo_renderer) {
        printf("SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(logo_window);
        IMG_Quit();
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
        return;
    }
    
    // Загружаем логотипы
    SDL_Texture* logo1 = NULL;
    SDL_Texture* logo2 = NULL;
    
    // Пробуем разные пути для логотипов
    const char* possible_paths1[] = {
        "romfs:/nintendologo.png",
        "romfs:/Contents/nintendologo.png",
        "sdmc:/nintendologo.png",
        "nintendologo.png"
    };
    
    const char* possible_paths2[] = {
        "romfs:/startupmovie.gif",
        "romfs:/Contents/startupmovie.gif", 
        "sdmc:/startupmovie.gif",
        "startupmovie.gif"
    };
    
    // Загружаем первый логотип
    for (int i = 0; i < 4; i++) {
        logo1 = IMG_LoadTexture(logo_renderer, possible_paths1[i]);
        if (logo1) {
            printf("Loaded logo1 from: %s\n", possible_paths1[i]);
            break;
        }
    }
    
    // Загружаем второй логотип
    for (int i = 0; i < 4; i++) {
        logo2 = IMG_LoadTexture(logo_renderer, possible_paths2[i]);
        if (logo2) {
            printf("Loaded logo2 from: %s\n", possible_paths2[i]);
            break;
        }
    }
    
    // Если нет логотипов, выходим
    if (!logo1 && !logo2) {
        printf("No logos found, continuing without them\n");
        SDL_DestroyRenderer(logo_renderer);
        SDL_DestroyWindow(logo_window);
        IMG_Quit();
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
        return;
    }
    
    // Получаем размеры экрана
    int display_width, display_height;
    SDL_GetWindowSize(logo_window, &display_width, &display_height);
    printf("Display dimensions: %d x %d\n", display_width, display_height);
    
    // Получаем размеры логотипов
    int logo1_width = 0, logo1_height = 0;
    int logo2_width = 0, logo2_height = 0;
    
    if (logo1) {
        SDL_QueryTexture(logo1, NULL, NULL, &logo1_width, &logo1_height);
        printf("Logo1 dimensions: %d x %d\n", logo1_width, logo1_height);
    }
    
    if (logo2) {
        SDL_QueryTexture(logo2, NULL, NULL, &logo2_width, &logo2_height);
        printf("Logo2 dimensions: %d x %d\n", logo2_width, logo2_height);
    }
    
    // Позиции логотипов
    SDL_Rect logo1_rect = {0, 0, logo1_width, logo1_height}; // Левый верхний угол
    
    SDL_Rect logo2_rect = {
        display_width - logo2_width,    // Правый край
        display_height - logo2_height,  // Нижний край
        logo2_width,
        logo2_height
    };
    
    // Основной цикл отображения логотипов
    Uint32 start_time = SDL_GetTicks();
    bool running = true;
    bool skip_requested = false;
    
    while (running) {
        SDL_Event event;
        
        // Проверяем события
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    running = false;
                    skip_requested = true;
                    break;
                    
                case SDL_KEYDOWN:
                    printf("Key pressed: %s\n", SDL_GetKeyName(event.key.keysym.sym));
                    running = false;
                    skip_requested = true;
                    break;
                    
                case SDL_JOYBUTTONDOWN:
                    printf("Joystick button pressed: joystick=%d, button=%d\n", 
                           event.jbutton.which, event.jbutton.button);
                    running = false;
                    skip_requested = true;
                    break;
                    
                case SDL_CONTROLLERBUTTONDOWN:
                    printf("Controller button pressed: controller=%d, button=%d\n", 
                           event.cbutton.which, event.cbutton.button);
                    running = false;
                    skip_requested = true;
                    break;
                    
                case SDL_JOYDEVICEADDED:
                    if (SDL_IsGameController(event.jdevice.which)) {
                        SDL_GameController* controller = SDL_GameControllerOpen(event.jdevice.which);
                        if (controller) {
                            printf("Controller connected: %s\n", SDL_GameControllerName(controller));
                        }
                    }
                    break;
                    
                case SDL_JOYDEVICEREMOVED:
                    printf("Controller disconnected\n");
                    break;
            }
        }
        
        // Проверяем таймаут (3 секунды)
        if (!skip_requested && SDL_GetTicks() - start_time > 3000) {
            printf("3 seconds elapsed, exiting logos\n");
            running = false;
        }
        
        // Если запрошен пропуск, показываем черный экран и выходим
        if (skip_requested) {
            // Очищаем экран черным
            SDL_SetRenderDrawColor(logo_renderer, 0, 0, 0, 255);
            SDL_RenderClear(logo_renderer);
            SDL_RenderPresent(logo_renderer);
            
            // Короткая задержка для отображения черного экрана
            SDL_Delay(50);
            break;
        }
        
        // Очищаем экран
        SDL_SetRenderDrawColor(logo_renderer, 0, 0, 0, 255);
        SDL_RenderClear(logo_renderer);
        
        // Рисуем логотипы
        if (logo1) {
            SDL_RenderCopy(logo_renderer, logo1, NULL, &logo1_rect);
        }
        
        if (logo2) {
            SDL_RenderCopy(logo_renderer, logo2, NULL, &logo2_rect);
        }
        
        // Обновляем экран
        SDL_RenderPresent(logo_renderer);
        
        // Небольшая задержка для экономии CPU
        SDL_Delay(16);
    }
    
    printf("Logos displayed for %u ms\n", SDL_GetTicks() - start_time);
    
    // Очищаем ресурсы логотипов
    if (logo1) SDL_DestroyTexture(logo1);
    if (logo2) SDL_DestroyTexture(logo2);
    
    // Уничтожаем рендерер и окно логотипов
    SDL_DestroyRenderer(logo_renderer);
    SDL_DestroyWindow(logo_window);
    
    // Закрываем все контроллеры
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            SDL_GameControllerClose(SDL_GameControllerFromInstanceID(i));
        }
    }
    
    // Завершаем только SDL_image
    IMG_Quit();
    
    // Завершаем подсистемы, которые мы инициализировали
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
    
    // Важно: небольшая пауза для очистки
    printf("Pausing to let SDL clean up...\n");
    svcSleepThread(100000000ULL); // 100ms пауза
    
    printf("=== Logos finished ===\n");
}
/* -------------------------------------------------------
   _nx module (sleep)
------------------------------------------------------- */

static PyObject* py_nx_sleep(PyObject* self, PyObject* args)
{
    double seconds;
    if (!PyArg_ParseTuple(args, "d", &seconds))
        return NULL;

    uint64_t ns = (uint64_t)(seconds * 1000000000ULL);

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


PyMODINIT_FUNC PyInit_color(void);
PyMODINIT_FUNC PyInit_controller(void);
PyMODINIT_FUNC PyInit_display(void);
PyMODINIT_FUNC PyInit_draw(void);
PyMODINIT_FUNC PyInit_error(void);
PyMODINIT_FUNC PyInit_event(void);
PyMODINIT_FUNC PyInit_font(void);
PyMODINIT_FUNC PyInit_gfxdraw(void);
PyMODINIT_FUNC PyInit_image(void);
PyMODINIT_FUNC PyInit_joystick(void);
PyMODINIT_FUNC PyInit_key(void);
PyMODINIT_FUNC PyInit_locals(void);
PyMODINIT_FUNC PyInit_mouse(void);
PyMODINIT_FUNC PyInit_power(void);
PyMODINIT_FUNC PyInit_pygame_time(void);
PyMODINIT_FUNC PyInit_rect(void);
PyMODINIT_FUNC PyInit_rwobject(void);
PyMODINIT_FUNC PyInit_scrap(void);
PyMODINIT_FUNC PyInit_transform(void);
PyMODINIT_FUNC PyInit_surface(void);

PyMODINIT_FUNC PyInit_render(void);
PyMODINIT_FUNC PyInit_mixer(void);
PyMODINIT_FUNC PyInit_mixer_music(void);


PyMODINIT_FUNC PyInit__renpy(void);
PyMODINIT_FUNC PyInit__renpybidi(void);

PyMODINIT_FUNC PyInit_renpy_audio_filter(void);
PyMODINIT_FUNC PyInit_renpy_audio_renpysound(void);
PyMODINIT_FUNC PyInit_renpy_encryption(void);

PyMODINIT_FUNC PyInit_renpy_display_accelerator(void);
PyMODINIT_FUNC PyInit_renpy_display_matrix(void);
PyMODINIT_FUNC PyInit_renpy_display_quaternion(void);
PyMODINIT_FUNC PyInit_renpy_display_render(void);

PyMODINIT_FUNC PyInit_renpy_text_ftfont(void);
PyMODINIT_FUNC PyInit_renpy_text_textsupport(void);
PyMODINIT_FUNC PyInit_renpy_text_texwrap(void);

PyMODINIT_FUNC PyInit_renpy_lexersupport(void);
PyMODINIT_FUNC PyInit_renpy_pydict(void);
PyMODINIT_FUNC PyInit_renpy_style(void);

PyMODINIT_FUNC PyInit_renpy_styledata_style_activate_functions(void);
PyMODINIT_FUNC PyInit_renpy_styledata_style_functions(void);
PyMODINIT_FUNC PyInit_renpy_styledata_style_hover_functions(void);
PyMODINIT_FUNC PyInit_renpy_styledata_style_idle_functions(void);
PyMODINIT_FUNC PyInit_renpy_styledata_style_insensitive_functions(void);
PyMODINIT_FUNC PyInit_renpy_styledata_style_selected_activate_functions(void);
PyMODINIT_FUNC PyInit_renpy_styledata_style_selected_functions(void);
PyMODINIT_FUNC PyInit_renpy_styledata_style_selected_hover_functions(void);
PyMODINIT_FUNC PyInit_renpy_styledata_style_selected_idle_functions(void);
PyMODINIT_FUNC PyInit_renpy_styledata_style_selected_insensitive_functions(void);
PyMODINIT_FUNC PyInit_renpy_styledata_styleclass(void);
PyMODINIT_FUNC PyInit_renpy_styledata_stylesets(void);

PyMODINIT_FUNC PyInit_renpy_gl_gldraw(void);
PyMODINIT_FUNC PyInit_renpy_gl_glenviron_shader(void);
PyMODINIT_FUNC PyInit_renpy_gl_glrtt_copy(void);
PyMODINIT_FUNC PyInit_renpy_gl_glrtt_fbo(void);
PyMODINIT_FUNC PyInit_renpy_gl_gltexture(void);

PyMODINIT_FUNC PyInit_renpy_gl2_gl2draw(void);
PyMODINIT_FUNC PyInit_renpy_gl2_gl2mesh(void);
PyMODINIT_FUNC PyInit_renpy_gl2_gl2mesh2(void);
PyMODINIT_FUNC PyInit_renpy_gl2_gl2mesh3(void);
PyMODINIT_FUNC PyInit_renpy_gl2_gl2model(void);
PyMODINIT_FUNC PyInit_renpy_gl2_gl2polygon(void);
PyMODINIT_FUNC PyInit_renpy_gl2_gl2shader(void);
PyMODINIT_FUNC PyInit_renpy_gl2_gl2texture(void);

PyMODINIT_FUNC PyInit_renpy_uguu_gl(void);
PyMODINIT_FUNC PyInit_renpy_uguu_uguu(void);

/* 8.5.3-specific modules (merged pygame, new cython modules) */
PyMODINIT_FUNC PyInit_renpy_astsupport(void);
PyMODINIT_FUNC PyInit_renpy_cslots(void);
PyMODINIT_FUNC PyInit_renpy_gl2_gl2uniform(void);
PyMODINIT_FUNC PyInit_renpy_text_bidi(void);
PyMODINIT_FUNC PyInit_renpy_text_hbfont(void);
PyMODINIT_FUNC PyInit_renpy_pygame_error(void);
PyMODINIT_FUNC PyInit_renpy_pygame_color(void);
PyMODINIT_FUNC PyInit_renpy_pygame_controller(void);
PyMODINIT_FUNC PyInit_renpy_pygame_rect(void);
PyMODINIT_FUNC PyInit_renpy_pygame_rwobject(void);
PyMODINIT_FUNC PyInit_renpy_pygame_surface(void);
PyMODINIT_FUNC PyInit_renpy_pygame_display(void);
PyMODINIT_FUNC PyInit_renpy_pygame_event(void);
PyMODINIT_FUNC PyInit_renpy_pygame_locals(void);
PyMODINIT_FUNC PyInit_renpy_pygame_key(void);
PyMODINIT_FUNC PyInit_renpy_pygame_mouse(void);
PyMODINIT_FUNC PyInit_renpy_pygame_joystick(void);
PyMODINIT_FUNC PyInit_renpy_pygame_power(void);
PyMODINIT_FUNC PyInit_renpy_pygame_pygame_time(void);
PyMODINIT_FUNC PyInit_renpy_pygame_image(void);
PyMODINIT_FUNC PyInit_renpy_pygame_transform(void);
PyMODINIT_FUNC PyInit_renpy_pygame_gfxdraw(void);
PyMODINIT_FUNC PyInit_renpy_pygame_draw(void);
PyMODINIT_FUNC PyInit_renpy_pygame_scrap(void);


/* -------------------------------------------------------
   Heap override
------------------------------------------------------- */

/* DIAGNOSTIC: renamed from __libnx_initheap so libnx uses its default heap init.
   On real hardware the app hangs before any logging exists; the custom override
   (svcGetInfo/svcSetHeapSize with a hand-computed size) is the prime suspect since
   it runs before everything and behaves differently on console vs Ryujinx.
   Rename back to __libnx_initheap to re-enable. */
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

void userAppInit()
{
    fsInitialize();
    nsInitialize();
    fsdevMountSdmc();

    freopen("sdmc:/renpy_switch.log", "a", stdout);
    /* Don't freopen the SAME file twice — opening renpy_switch.log a second time
       for append fails on the Switch FS, leaving fd 2 (stderr) dangling and
       swallowing every Python traceback. Point stderr at stdout's handle (2>&1). */
    dup2(fileno(stdout), fileno(stderr));

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("=== Ren'Py 8 Switch launcher ===\n");
    
    Result rc = 0;
    PselUserSelectionSettings settings;
    
    rc = svcGetInfo(&cur_progid, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0);
    rc = accountInitialize(AccountServiceType_Application);
    rc = accountGetPreselectedUser(&userID);
    
    if (R_FAILED(rc)) {
        s32 count;
        accountGetUserCount(&count);

        if (count > 1) {
            // Показываем селектор пользователей
            rc = pselShowUserSelector(&userID, &settings);
            
            // Если пользователь нажал отмену, завершаем программу
            if (R_FAILED(rc) || !accountUidIsValid(&userID)) {
                printf("User selection cancelled. Exiting...\n");
                fflush(stdout);
                
                // Даем время для записи лога
                svcSleepThread(2000000000ULL); // 2 секунды
                
                // Завершаем программу
                exit(0);
            }
        } else if (count == 1) {
            s32 loadedUsers;
            AccountUid account_ids[1];
            accountListAllUsers(account_ids, 1, &loadedUsers);
            if (count > 0) {
                userID = account_ids[0];
            }
        } else {
            // Нет пользователей - завершаем программу
            printf("No users found. Exiting...\n");
            fflush(stdout);
            svcSleepThread(2000000000ULL);
            exit(0);
        }
    }

    // Дополнительная проверка после всех операций
    if (!accountUidIsValid(&userID)) {
        printf("Invalid user selected. Exiting...\n");
        fflush(stdout);
        svcSleepThread(2000000000ULL);
        exit(0);
    }

    // Если пользователь выбран, продолжаем инициализацию
    rc = fsdevMountSaveData("save", cur_progid, userID);
    if (R_FAILED(rc)) {
        rc = createSaveData();
        rc = fsdevMountSaveData("save", cur_progid, userID);
    }

    romfsInit();
    socketInitializeDefault();

    /* If launched via `nxlink`, redirect stdout/stderr to the nxlink host so we get
       real-time console output over the network. Returns -1 when not launched via
       nxlink, in which case the sdmc:/renpy_switch.log redirect above stays active. */
    nxlinkStdio();
}

void userAppExit(void)
{
    if (fsdevGetDeviceFileSystem("save"))
        fsdevCommitDevice("save");

    fsdevUnmountDevice("save");
    socketExit();
    romfsExit();
    nsExit();
    fsExit(); 
}

/* -------------------------------------------------------
   Error helper
------------------------------------------------------- */

void show_error(const char* message)
{
    printf("[main] show_error: %s\n", message); fflush(stdout);
    PyErr_Print();

    ErrorSystemConfig c;
    errorSystemCreate(&c, message, message);
    errorSystemShow(&c);

    Py_Exit(1);
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
   	// Проверяем, что пользователь выбран
    if (!accountUidIsValid(&userID)) {
        printf("No user selected. Game cannot start.\n");
        fflush(stdout);
        svcSleepThread(2000000000ULL); // Даем время для записи в лог
        return 0;
    }

    video_player_init();
    play_video_file_delay("romfs:/Contents/game/intro.webm", 1, 3.0f);
    video_player_quit();
   
    // Показ логотипов при запуске
    show_logos();
   
    video_player_init();
    play_video_file_delay("romfs:/Contents/game/intro.webm", 1, 3.0f);
    video_player_quit();

    /* Boot splash disabled: the leave-window adoption faults on hardware because SDL
       forbids mixing an SDL_Renderer with a separate GL context on one window. To do
       this cleanly main.c must create a *raw GL* context (no SDL_Renderer) and Ren'Py
       must adopt that same context. Until then, keep the known-good boot. */
    /* show_splash(); */

    printf("[main] video/logos done; configuring Python\n"); fflush(stdout);
    // printf("=== Testing video playback ===\n");
    
    // // Инициализируем видео-плеер
    // video_player_init();
    
    // // 1. Из корня romfs
    // printf("\n=== Test: romfs:/intro.mp4 ===\n");
    // play_video_file_delay("romfs:/intro.mp4", 1, 3.0f);
    
    // // 2. Из папки Contents в romfs
    // printf("\n=== Test: romfs:/Contents/intro.mp4 ===\n");
    // play_video_file_delay("romfs:/Contents/intro.mp4", 1, 1.0f);
    
    // // 3. Из папки game/video в romfs
    // printf("\n=== Test: romfs:/Contents/game/video/intro.mp4 ===\n");
    // play_video_file_delay("romfs:/Contents/game/video/intro.mp4", 1, 1.0f);
    
    // // 4. Относительный путь (будет автоматически разрешен)
    // printf("\n=== Test: intro.mp4 ===\n");
    // play_video_file_delay("intro.mp4", 1, 1.0f);
    
    // // 5. С SD карты
    // printf("\n=== Test: sdmc:/intro.mp4 ===\n");
    // play_video_file_delay("sdmc:/intro.mp4", 1, 1.0f);
    
    // // Деинициализируем видео-плеер
    // video_player_quit();
   
    chdir("romfs:/Contents");
    setlocale(LC_ALL, "C");
    setenv("MESA_NO_ERROR", "1", 1);
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

    appletLockExit();
    appletHook(&applet_hook_cookie, on_applet_hook, NULL);

    Py_NoSiteFlag = 1;
    Py_IgnoreEnvironmentFlag = 1;
    Py_NoUserSiteDirectory = 1;
    Py_DontWriteBytecodeFlag = 1;
    Py_OptimizeFlag = 2;

    PyConfig config;

    PyConfig_InitPythonConfig(&config);

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
    PyWideStringList_Append(&config.module_search_paths, L"romfs:/Contents/lib.zip");
    PyWideStringList_Append(&config.module_search_paths, L"romfs:/Contents");
    PyWideStringList_Append(&config.module_search_paths, L"romfs:/Contents/renpy/common");
   
    /* ---- Critical for Python 3.9 embedded ---- */
    /* Switch has no /dev/urandom and newlib's entropy call fails, which makes
       _Py_HashRandomization_Init abort. Pin a fixed hash seed (0 = randomization
       off) so init never asks the OS for entropy. */
    config.use_hash_seed = 1;
    config.hash_seed = 0;
    /* Unbuffered Python stdio so tracebacks reach the log before we exit. */
    config.buffered_stdio = 0;
    config.isolated = 0;
    config.use_environment = 0;
    config.site_import = 0;
    config.user_site_directory = 0;
    config.write_bytecode = 0;
    config.optimization_level = 2;
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

    /* ---- stdlib: ONLY lib.zip ---- */
    config.module_search_paths_set = 1;

    status = PyWideStringList_Append(
        &config.module_search_paths,
        L"romfs:/Contents/lib.zip"
    );
    if (PyStatus_Exception(status)) goto exception;

    /* ---- argv ---- */
    wchar_t* pyargv[] = {
        L"romfs:/Contents/renpy.py",
        NULL
    };
    status = PyConfig_SetArgv(&config, 1, pyargv);
    if (PyStatus_Exception(status)) goto exception;

   
    Py_SetProgramName(L"RenPy3.8.7");



    /* ---- Builtin modules ---- */
    static struct _inittab builtins[] = {

       //{"pygame_sdl2", PyInit_pygame_sdl2},

        {"_nx", PyInit__nx},
        {"_otrhlibnx", PyInit__otrhlibnx},

        {"renpy.pygame.error", PyInit_renpy_pygame_error},
        {"renpy.pygame.color", PyInit_renpy_pygame_color},
        {"renpy.pygame.controller", PyInit_renpy_pygame_controller},
        {"renpy.pygame.rect", PyInit_renpy_pygame_rect},
        {"renpy.pygame.rwobject", PyInit_renpy_pygame_rwobject},
        {"renpy.pygame.surface", PyInit_renpy_pygame_surface},
        {"renpy.pygame.display", PyInit_renpy_pygame_display},
        {"renpy.pygame.event", PyInit_renpy_pygame_event},
        {"renpy.pygame.locals", PyInit_renpy_pygame_locals},
        {"renpy.pygame.key", PyInit_renpy_pygame_key},
        {"renpy.pygame.mouse", PyInit_renpy_pygame_mouse},
        {"renpy.pygame.joystick", PyInit_renpy_pygame_joystick},
        {"renpy.pygame.power", PyInit_renpy_pygame_power},
        {"renpy.pygame.pygame_time", PyInit_renpy_pygame_pygame_time},
        {"renpy.pygame.image", PyInit_renpy_pygame_image},
        {"renpy.pygame.transform", PyInit_renpy_pygame_transform},
        {"renpy.pygame.gfxdraw", PyInit_renpy_pygame_gfxdraw},
        {"renpy.pygame.draw", PyInit_renpy_pygame_draw},
        {"renpy.pygame.scrap", PyInit_renpy_pygame_scrap},

        {"_renpy", PyInit__renpy},
        {"renpy.astsupport", PyInit_renpy_astsupport},
        {"renpy.cslots", PyInit_renpy_cslots},

        {"renpy.audio.filter", PyInit_renpy_audio_filter},
        {"renpy.audio.renpysound", PyInit_renpy_audio_renpysound},
        {"renpy.display.accelerator", PyInit_renpy_display_accelerator},
        {"renpy.display.matrix", PyInit_renpy_display_matrix},
        {"renpy.display.quaternion", PyInit_renpy_display_quaternion},
        {"renpy.display.render", PyInit_renpy_display_render},
        {"renpy.encryption", PyInit_renpy_encryption},

        {"renpy.text.ftfont", PyInit_renpy_text_ftfont},
        {"renpy.text.textsupport", PyInit_renpy_text_textsupport},
        {"renpy.text.texwrap", PyInit_renpy_text_texwrap},
        {"renpy.text.hbfont", PyInit_renpy_text_hbfont},
        {"renpy.text.bidi", PyInit_renpy_text_bidi},
        {"renpy.pydict", PyInit_renpy_pydict},
        {"renpy.lexersupport", PyInit_renpy_lexersupport},
        {"renpy.style", PyInit_renpy_style},
        {"renpy.styledata.style_activate_functions", PyInit_renpy_styledata_style_activate_functions},
        {"renpy.styledata.style_functions", PyInit_renpy_styledata_style_functions},
        {"renpy.styledata.style_hover_functions", PyInit_renpy_styledata_style_hover_functions},
        {"renpy.styledata.style_idle_functions", PyInit_renpy_styledata_style_idle_functions},
        {"renpy.styledata.style_insensitive_functions", PyInit_renpy_styledata_style_insensitive_functions},
        {"renpy.styledata.style_selected_activate_functions", PyInit_renpy_styledata_style_selected_activate_functions},
        {"renpy.styledata.style_selected_functions", PyInit_renpy_styledata_style_selected_functions},
        {"renpy.styledata.style_selected_hover_functions", PyInit_renpy_styledata_style_selected_hover_functions},
        {"renpy.styledata.style_selected_idle_functions", PyInit_renpy_styledata_style_selected_idle_functions},
        {"renpy.styledata.style_selected_insensitive_functions", PyInit_renpy_styledata_style_selected_insensitive_functions},
        {"renpy.styledata.styleclass", PyInit_renpy_styledata_styleclass},
        {"renpy.styledata.stylesets", PyInit_renpy_styledata_stylesets},

        /* renpy.gl.* (legacy GL renderer) removed in 8.5.3 — gl2 only */

        {"renpy.gl2.gl2draw", PyInit_renpy_gl2_gl2draw},
        {"renpy.gl2.gl2mesh", PyInit_renpy_gl2_gl2mesh},
        {"renpy.gl2.gl2mesh2", PyInit_renpy_gl2_gl2mesh2},
        {"renpy.gl2.gl2mesh3", PyInit_renpy_gl2_gl2mesh3},
        {"renpy.gl2.gl2model", PyInit_renpy_gl2_gl2model},
        {"renpy.gl2.gl2polygon", PyInit_renpy_gl2_gl2polygon},
        {"renpy.gl2.gl2shader", PyInit_renpy_gl2_gl2shader},
        {"renpy.gl2.gl2texture", PyInit_renpy_gl2_gl2texture},
        {"renpy.gl2.gl2uniform", PyInit_renpy_gl2_gl2uniform},

        {"renpy.uguu.gl", PyInit_renpy_uguu_gl},
        {"renpy.uguu.uguu", PyInit_renpy_uguu_uguu},

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

    printf("[main] registering builtins + initializing Python\n"); fflush(stdout);
    PyImport_ExtendInittab(builtins);

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
    printf("[main] Python initialized OK\n"); fflush(stdout);


    /* 8.5.3: pygame_sdl2 is merged into renpy.pygame; renpy.py imports what it needs. */


    int python_result;
    python_result = PyRun_SimpleString(
    "import sys, os\n"
    "sys.path.insert(0, 'romfs:/Contents/lib.zip')\n"
    /* Log the real platform string CPython was built with, before we override it. */
    "os.write(1, ('[main] sys.platform (real) = %r\\n' % (sys.platform,)).encode('utf-8','replace'))\n"
    /* PyNX's CPython doesn't report platform 'horizon', but the whole Switch port
       keys off `sys.platform == 'horizon'` (renpy/__init__.py sets renpy.switch from
       it). Force it here so every Switch conditional fires as designed. */
    "sys.platform = 'horizon'\n"
    "os.write(1, ('[main] sys.platform (now)  = %r\\n' % (sys.platform,)).encode('utf-8','replace'))\n"
    );
    
    if (python_result == -1)
    {
        show_error("Could not set the Python path.\n\nThis is an internal error and should not occur during normal usage.");
    }
   #define x(lib) \
    { \
        if (PyRun_SimpleString("import " lib) == -1) \
        { \
            show_error("Could not import python library " lib ".\n\nPlease ensure that you have extracted the files correctly so that the \"lib\" folder is in the same directory as the nsp file, and that the \"lib\" folder contains the folder \"python3.9\". \nInside that folder, the file \"" lib ".py\" or folder \"" lib "\" needs to exist."); \
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
            rc = -1;
            PyObject *exc = PyErr_GetRaisedException();         /* steals ref, clears error */
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
        } else {
            Py_DECREF(res);
        }
    }
    printf("[main] renpy.py returned %d\n", rc); fflush(stdout);

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
