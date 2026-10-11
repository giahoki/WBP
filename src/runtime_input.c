/* Native input: keys press the game's own actions instead of pad buttons. Details: docs/INPUT.md.
 *
 * The game's FD4 user-input layer turns the pad device's physical buttons into virtual inputs
 * (one per game action and context: the circle button alone feeds eleven, menu back and dodge
 * among them) with converters built from the key assign. The digital converter pass of app
 * 01.09 (0x2A98CD0) walks the converters with an iterator in r14; for each it reads the device's
 * GetButton (vtable slot 0xE8, sub_2A91800 for the PS4 pad) at 0x2A98E63 and sets the
 * converter's virtual input bit in [dev+130h] when the button is down. The pad device's slot is
 * pointed at a thunk that also passes r14 and the return address; called from that site,
 * input_get_button asks the iterator for the converter (sub_2FA4840) and the converter for its
 * virtual input (vtable slot 0x30), and adds the keys bound to that virtual input. Everything
 * after (press, hold and repeat detectors, menus, the character) sees an ordinary button.
 *
 * Which keys press which virtual inputs comes from runtime_pad.c: in native mode the split
 * actions (runtime_pad_native_down) leave the pad buttons alone and are applied here. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* Image offsets (PS4 address - 0x400000) in app 01.09. */
#define PAD_VTABLE_GET_BUTTON 0x52b8598u /* pad device vtable slot 0xE8 */
#define PAD_GET_BUTTON 0x2691800u        /* sub_2A91800 */
#define BUTTON_PASS_RETURN 0x2698e65u    /* after call rbx (GetButton) in the pass at 0x2A98CD0 */
#define ITERATOR_GET 0x2ba4840u          /* sub_2FA4840: the converter at an iterator */

typedef ABI int (*GetButton)(void *dev, int physical);
typedef ABI void **(*IteratorGet)(void *iterator);
typedef ABI int (*VirtualOf)(void *converter);
static GetButton original;
static IteratorGet iterator_get;
static uintptr_t pass_return;
static int debug, active;

/* Virtual inputs of each split action (runtime_pad.h order). The circle button's inputs in
 * app 01.09: 195 197 199 235 239 243 (menus) and 298 299 316 324 336 (the character). */
#define MAX_VIRTUAL 512
static const struct { const char *env; const char *defaults; } split_inputs[NATIVE_ACTION_COUNT]={
    [NATIVE_DODGE]={"BB_NATIVE_DODGE","298,299,316,324,336"},
    [NATIVE_BACK]={"BB_NATIVE_BACK","195,197,199,235,239,243"},
};
static uint8_t owner[MAX_VIRTUAL]; /* NATIVE_* + 1 that presses the virtual input, or 0 */

static ABI int input_get_button(void *dev, int physical, void *iterator, uintptr_t ret) {
    int value=original(dev,physical);
    if (ret!=pass_return) return value;
    void **converter=iterator_get(iterator);
    const int v=((VirtualOf)(*(void ***)converter)[6])(converter);
    if (v<0 || v>=MAX_VIRTUAL) return value;
    if (debug) {
        static uint8_t seen[MAX_VIRTUAL];
        if (seen[v]!=(uint8_t)(physical+1)) {
            seen[v]=(uint8_t)(physical+1);
            printf("Input: virtual %d <- physical %d\n",v,physical);
        }
    }
    if (owner[v] && (runtime_pad_native_down()>>(owner[v]-1)&1u)) value=1;
    /* BB_INPUT_PROBE=a,b+c,...: the number row 1..9,0,- holds one group of virtual inputs
     * each, to find out what they do in the game. */
    static int probe_ready, probe_count;
    static uint32_t probe_keys_of[MAX_VIRTUAL];
    if (!probe_ready) {
        probe_ready=1;
        const char *list=getenv("BB_INPUT_PROBE");
        char buf[512], *save=NULL;
        snprintf(buf,sizeof(buf),"%s",list ? list : "");
        for (char *tok=strtok_r(buf,",",&save);tok && probe_count<11;tok=strtok_r(NULL,",",&save),++probe_count) {
            char *member_save=NULL;
            for (char *m=strtok_r(tok,"+ ",&member_save);m;m=strtok_r(NULL,"+ ",&member_save)) {
                const int pv=atoi(m);
                if (pv>0 && pv<MAX_VIRTUAL) probe_keys_of[pv]|=1u<<probe_count;
            }
        }
        if (probe_count) printf("Input: probe keys for %d groups\n",probe_count);
    }
    if (probe_keys_of[v] && (runtime_pad_probe_keys()&probe_keys_of[v])) value=1;
    return value;
}

static void load_split(void) {
    for (int a=0;a<NATIVE_ACTION_COUNT;++a) {
        if (!split_inputs[a].env) continue; /* NATIVE_RUN goes out as NATIVE_DODGE (runtime_pad.c) */
        const char *list=getenv(split_inputs[a].env);
        if (!list || !*list) list=split_inputs[a].defaults;
        char buf[256], *save=NULL;
        snprintf(buf,sizeof(buf),"%s",list);
        for (char *tok=strtok_r(buf,", ",&save);tok;tok=strtok_r(NULL,", ",&save)) {
            const int v=atoi(tok);
            if (v>0 && v<MAX_VIRTUAL) owner[v]=(uint8_t)(a+1);
        }
    }
}

/* BB_DODGE_ICON=<texture> (set by the launcher with native input): the tutorial token
 * <?CancelIcon?>, used only by the dodge and run floor messages, shows that key-guide texture
 * instead of the circle's KG_OK, which then shows menu back. sub_1EE7B50 expands the token with
 * the wide string loaded by lea rbx,[KG_OK] at 0x1EE7C70; the lea is pointed at the new name,
 * written into the zero tail of the first segment's last page. */
#define CANCEL_ICON_LEA 0x1ae7c70u
#define ICON_NAME_AT 0x50d9700u
static void redirect_cancel_icon(unsigned char *image) {
    const char *name=getenv("BB_DODGE_ICON");
    if (!name || !*name) return;
    static const unsigned char lea[7]={0x48,0x8d,0x1d,0x91,0x09,0xeb,0x02}; /* lea rbx,[rip+..]: L"KG_OK" */
    const size_t n=strlen(name);
    unsigned char *at=image+ICON_NAME_AT;
    int clear=n<32;
    for (size_t i=0;clear && i<(n+1)*2;++i) clear=!at[i];
    if (memcmp(image+CANCEL_ICON_LEA,lea,sizeof(lea)) || !clear) {
        puts("Input: tutorial icon code differs from app 01.09; the dodge prompt keeps the circle icon");
        return;
    }
    for (size_t i=0;i<n;++i) at[i*2]=(unsigned char)name[i];
    const int32_t disp=(int32_t)(ICON_NAME_AT-(CANCEL_ICON_LEA+7));
    memcpy(image+CANCEL_ICON_LEA+3,&disp,4);
    printf("Input: the dodge tutorials show %s\n",name);
}

/* mov rdx,r14; mov rcx,[rsp]; movabs rax,<fn>; jmp rax */
static const unsigned char thunk_template[]={0x4c,0x89,0xf2,0x48,0x8b,0x0c,0x24,0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xe0};

void runtime_input_install(unsigned char *image) {
    const char *dbg=getenv("BB_INPUT_DEBUG");
    debug=dbg && !strcmp(dbg,"1");
    redirect_cancel_icon(image);
    uintptr_t *slot=(uintptr_t *)(image+PAD_VTABLE_GET_BUTTON);
    static const unsigned char site[]={0xff,0xd3,0x84,0xc0}; /* call rbx; test al,al */
    if (*slot!=(uintptr_t)(image+PAD_GET_BUTTON) || memcmp(image+BUTTON_PASS_RETURN-2,site,sizeof(site))) {
        puts("Input: input code differs from app 01.09; native input off");
        return;
    }
    original=(GetButton)*slot;
    iterator_get=(IteratorGet)(image+ITERATOR_GET);
    pass_return=(uintptr_t)(image+BUTTON_PASS_RETURN);
    load_split();
    unsigned char code[sizeof(thunk_template)];
    memcpy(code,thunk_template,sizeof(code));
    const uintptr_t fn=(uintptr_t)input_get_button;
    memcpy(code+9,&fn,8);
#ifdef _WIN32
    unsigned char *stub=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if (!stub) { puts("Input: no memory for the thunk; native input off"); return; }
    memcpy(stub,code,sizeof(code));
    DWORD old;
    if (!VirtualProtect(stub,4096,PAGE_EXECUTE_READ,&old)) { puts("Input: VirtualProtect failed; native input off"); return; }
    FlushInstructionCache(GetCurrentProcess(),stub,4096);
#else
    unsigned char *stub=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (stub==MAP_FAILED) { puts("Input: no memory for the thunk; native input off"); return; }
    memcpy(stub,code,sizeof(code));
    if (mprotect(stub,4096,PROT_READ|PROT_EXEC)) { puts("Input: mprotect failed; native input off"); return; }
#endif
    *slot=(uintptr_t)stub; /* the image is still writable here */
    active=1;
    printf("Input: native input hooked at 0x2A98E63 (debug %d)\n",debug);
}

int runtime_input_active(void) { return active; }
