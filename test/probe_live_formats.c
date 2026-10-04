/* Standalone native client for adapter sanitizer/leak and ABI checks.
 * Arguments: format plugin-path plugin-id channels. Uses gain fixtures. */
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "../csrc/clap_live.h"
#include "../csrc/live_platform.h"
#include <assert.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
typedef struct { ap_live *s; _Atomic int command; float samples[64]; } worker;
static void *run(void *ptr) {
    worker *w = ptr;
    assert(!ap_live_device_begin(w->s));
    atomic_store(&w->command, 0);
    for (;;) {
        int command = atomic_load(&w->command);
        if (!command) { ap_sleep_ns(100000); continue; }
        if (command == 2) { assert(!ap_live_device_end(w->s)); return NULL; }
        assert(command == 1 && !ap_live_device_process(w->s,w->samples));
        atomic_store(&w->command,0);
    }
}
int main(int argc,char **argv) {
    assert(argc==5);
    int vst = !strcmp(argv[1],"vst3");
    ap_live_config config={48000,32,(uint32_t)atoi(argv[4]),4,AP_LIVE_DEVICE,0};
    int (*open_format)(const char*,const char*,const ap_live_config*,ap_live**) = vst ? ap_live_open_vst3 : !strcmp(argv[1],"lv2") ? ap_live_open_lv2 : ap_live_open;
    ap_live *s=NULL; assert(!open_format(argv[2],argv[3],&config,&s));
    for(int cycle=0;cycle<5;++cycle) {
        assert(!ap_live_start(s));
        worker w={.s=s}; atomic_init(&w.command,-1);
        ap_thread thread; assert(!ap_thread_create(&thread,run,&w));
        while(atomic_load(&w.command)) ap_sleep_ns(100000);
        for(int block=0;block<32;++block) {
            float input[64]; for(unsigned i=0;i<32*config.channels;++i)input[i]=(float)(block+1);
            ap_live_event event={.type=AP_LIVE_PARAM,.param_id=0,.value=vst?.125:.5};
            assert(!ap_live_try_write(s,input,&event,1,(uint64_t)block));
            atomic_store(&w.command,1);
            while(atomic_load(&w.command))ap_sleep_ns(100000);
            uint64_t tick,sequence; float output[64];
            assert(!ap_live_try_read(s,output,&tick,&sequence)); assert(sequence==(uint64_t)block);
            for(unsigned i=0;i<32*config.channels;++i)assert(output[i]==.5f*(block+1));
        }
        atomic_store(&w.command,2);ap_thread_join(thread); assert(!ap_live_stop(s));
    }
    assert(!ap_live_close(s)); puts("format lifecycle, queues and arithmetic: passed");return 0;
}
