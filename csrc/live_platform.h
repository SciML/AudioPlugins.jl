/* Native thread, clock, loader and scheduling primitives. No Julia callbacks. */
#ifndef AP_LIVE_PLATFORM_H
#define AP_LIVE_PLATFORM_H
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <avrt.h>
#include <objbase.h>
typedef DWORD ap_thread_id;
typedef HANDLE ap_thread;
typedef struct { void *(*function)(void *); void *argument; } ap_thread_args;
static DWORD WINAPI ap_thread_entry(void *data) {
    ap_thread_args args = *(ap_thread_args *)data; free(data);
    HRESULT result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    args.function(args.argument);
    if (SUCCEEDED(result)) CoUninitialize();
    return 0;
}
static inline ap_thread_id ap_thread_self(void) { return GetCurrentThreadId(); }
static inline int ap_thread_equal(ap_thread_id a, ap_thread_id b) { return a == b; }
static inline int ap_thread_create(ap_thread *thread, void *(*fn)(void *), void *arg) {
    ap_thread_args *args = malloc(sizeof(*args));
    if (!args) return -1;
    args->function = fn; args->argument = arg;
    *thread = CreateThread(NULL, 0, ap_thread_entry, args, 0, NULL);
    if (!*thread) { free(args); return -1; }
    return 0;
}
static inline void ap_thread_join(ap_thread t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
static inline uint64_t ap_clock_ns(void) {
    LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
    return (uint64_t)(t.QuadPart / f.QuadPart) * 1000000000ULL +
        (uint64_t)(t.QuadPart % f.QuadPart) * 1000000000ULL / (uint64_t)f.QuadPart;
}
static inline void ap_sleep_ns(uint64_t ns) { Sleep((DWORD)((ns + 999999) / 1000000)); }
static inline void *ap_module_open(const char *path) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (!n) return NULL;
    wchar_t *wide = malloc((size_t)n * sizeof(*wide)); if (!wide) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, n);
    void *result = (void *)LoadLibraryExW(wide, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    free(wide); return result;
}
static inline void *ap_module_symbol(void *m, const char *name) { return (void *)(uintptr_t)GetProcAddress((HMODULE)m, name); }
static inline void ap_module_close(void *m) { FreeLibrary((HMODULE)m); }
typedef struct { HANDLE registration; } ap_priority;
static inline int ap_priority_enter(ap_priority *p, uint64_t period) {
    (void)period; DWORD index = 0;
    p->registration = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
    return p->registration ? 0 : -1;
}
static inline int ap_priority_leave(ap_priority *p) {
    if (!p->registration) return 0;
    int result = AvRevertMmThreadCharacteristics(p->registration) ? 0 : -1;
    p->registration = NULL; return result;
}
static inline int ap_control_setup(int *owned) {
    HRESULT result = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    *owned = SUCCEEDED(result);
    return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE ? 0 : -1;
}
static inline void ap_control_cleanup(int owned) { if (owned) CoUninitialize(); }
static inline int ap_control_allowed(void) { return 1; } /* Caller supplies its application main thread. */
static inline void ap_pump_events(void) {
    MSG msg;
    for (int i = 0; i < 64 && PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE); ++i) {
        if (msg.message == WM_QUIT) { PostQuitMessage((int)msg.wParam); break; }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
}
#else
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <dlfcn.h>
#include <sched.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <CoreFoundation/CoreFoundation.h>
#endif
static inline int ap_control_setup(int *owned) { *owned = 0; return 0; }
static inline void ap_control_cleanup(int owned) { (void)owned; }
typedef pthread_t ap_thread_id;
typedef pthread_t ap_thread;
static inline ap_thread_id ap_thread_self(void) { return pthread_self(); }
static inline int ap_thread_equal(ap_thread_id a, ap_thread_id b) { return pthread_equal(a, b); }
static inline int ap_thread_create(ap_thread *t, void *(*fn)(void *), void *arg) { return pthread_create(t, NULL, fn, arg); }
static inline void ap_thread_join(ap_thread t) { pthread_join(t, NULL); }
static inline uint64_t ap_clock_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}
static inline void ap_sleep_ns(uint64_t ns) {
    struct timespec t = { (time_t)(ns / 1000000000ULL), (long)(ns % 1000000000ULL) };
    while (nanosleep(&t, &t) && errno == EINTR) {}
}
static inline void *ap_module_open(const char *path) {
#ifdef __APPLE__
    /* CLAP distributes both bare binaries and macOS bundles. */
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), true);
    CFBundleRef bundle = url ? CFBundleCreate(NULL, url) : NULL;
    CFURLRef executable = bundle ? CFBundleCopyExecutableURL(bundle) : NULL;
    UInt8 binary[4096];
    int resolved = executable && CFURLGetFileSystemRepresentation(executable, true, binary, sizeof(binary));
    void *module = dlopen(resolved ? (const char *)binary : path, RTLD_NOW | RTLD_LOCAL);
    if (executable) CFRelease(executable);
    if (bundle) CFRelease(bundle);
    if (url) CFRelease(url);
    return module;
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}
static inline void *ap_module_symbol(void *m, const char *name) { return dlsym(m, name); }
static inline void ap_module_close(void *m) { dlclose(m); }
#ifdef __APPLE__
typedef struct { thread_time_constraint_policy_data_t old; boolean_t defaults; } ap_priority;
static inline int ap_priority_enter(ap_priority *p, uint64_t period) {
    thread_port_t thread = mach_thread_self();
    mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    p->defaults = FALSE;
    kern_return_t result = thread_policy_get(thread, THREAD_TIME_CONSTRAINT_POLICY,
        (thread_policy_t)&p->old, &count, &p->defaults);
    if (result == KERN_SUCCESS) {
        mach_timebase_info_data_t timebase; mach_timebase_info(&timebase);
        uint64_t ticks = period * timebase.denom / timebase.numer;
        if (ticks > UINT32_MAX) { mach_port_deallocate(mach_task_self(), thread); return -1; }
        thread_time_constraint_policy_data_t policy = {
            (uint32_t)ticks, (uint32_t)(ticks / 2), (uint32_t)ticks, TRUE
        };
        result = thread_policy_set(thread, THREAD_TIME_CONSTRAINT_POLICY,
            (thread_policy_t)&policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    }
    mach_port_deallocate(mach_task_self(), thread);
    return result == KERN_SUCCESS ? 0 : -1;
}
static inline int ap_priority_leave(ap_priority *p) {
    thread_port_t thread = mach_thread_self();
    kern_return_t r;
    if (p->defaults) {
        thread_standard_policy_data_t standard = {0};
        r = thread_policy_set(thread, THREAD_STANDARD_POLICY, (thread_policy_t)&standard, THREAD_STANDARD_POLICY_COUNT);
    } else r = thread_policy_set(thread, THREAD_TIME_CONSTRAINT_POLICY,
        (thread_policy_t)&p->old, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    mach_port_deallocate(mach_task_self(), thread); return r == KERN_SUCCESS ? 0 : -1;
}
static inline int ap_control_allowed(void) { return pthread_main_np(); }
static inline void ap_pump_events(void) {
    for (int i = 0; i < 64; ++i)
        if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true) != kCFRunLoopRunHandledSource) break;
}
#else
typedef struct { int policy; struct sched_param params; } ap_priority;
static inline int ap_priority_enter(ap_priority *p, uint64_t period) {
    (void)period;
    if (pthread_getschedparam(pthread_self(), &p->policy, &p->params)) return -1;
    struct sched_param request = { .sched_priority = sched_get_priority_min(SCHED_FIFO) };
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &request);
}
static inline int ap_priority_leave(ap_priority *p) { return pthread_setschedparam(pthread_self(), p->policy, &p->params); }
static inline int ap_control_allowed(void) { return 1; }
static inline void ap_pump_events(void) {}
#endif
#endif
static inline void ap_sleep_until(uint64_t deadline) {
    uint64_t now = ap_clock_ns();
    if (deadline > now) ap_sleep_ns(deadline - now);
}
#endif
