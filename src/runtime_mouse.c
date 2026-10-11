/* Native mouse camera: mouse motion turns the game's orbit camera directly instead of going
 * through the right stick (its speed curve and smoothing). Details and addresses: docs/ADDRESSES.md.
 *
 * The player camera update (sub_183AC60 in app 01.09, the camera object in r13) settles its two
 * orbit angles by 0x183CF2E: [cam+140h] vertical (clamped to [cam+1F0h]..[cam+1ECh]) and
 * [cam+144h] horizontal (wrapped to [-pi, pi]); the camera is then built from them. Earlier in
 * the frame the game re-derives the angles and eases the vertical one back to [cam+150h], so
 * the mouse is applied at 0x183CF2E: a 14-byte jump runs a stub that saves the caller-saved
 * registers, calls mouse_camera(cam) with the guest's calling convention, restores them, runs
 * the 17 replaced bytes (which reload the vertical angle) and jumps back. mouse_camera also does
 * what the stick branch does on manual input ([150h] = vertical angle, [130h] = 1, [134h] =
 * [1E0h]) so the game does not pull the view back. The window thread adds mouse counts with
 * atomics; mouse_camera takes them with an exchange.
 *
 * The speed is the mouse_sensitivity setting (in-game menu, wbp.ini; BB_MOUSE_SENS), read on
 * every update. BB_MOUSE=0 turns it off, BB_MOUSE_INVERT_Y=1 inverts the vertical axis,
 * BB_MOUSE_AXES="a,b,c,d" sets the raw matrix (radians per count at sensitivity 1):
 * angle140 += dx*a + dy*b, angle144 += dx*c + dy*d. BB_MOUSE_DEBUG=1 logs the angles. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* Image offset of the hook (PS4 address 0x183CF2E, eboot base 0x400000) and its bytes:
 * vmovss xmm0,[r13+140h]; vmovss [rbp-2F0h],xmm0. */
#define HOOK_OFFSET 0x143cf2eu
static const unsigned char hooked[17]={0xc4,0xc1,0x7a,0x10,0x85,0x40,0x01,0x00,0x00,
                                       0xc5,0xfa,0x11,0x85,0x10,0xfd,0xff,0xff};
/* push rax..r11; sub rsp,264; vmovups [rsp+16*i],xmm0..15; mov rdi,r13; movabs rax,<fn>;
 * call rax; vmovups xmm0..15,[rsp+16*i]; add rsp,264; pop r11..rax. The stack is 16-byte
 * aligned in the function body: nine pushes and 264 bytes keep it so for the call. */
static const unsigned char save_call_restore[]={
    0x50,0x51,0x52,0x56,0x57,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,0x48,0x81,0xec,0x08,0x01,0x00,0x00,
    0xc5,0xf8,0x11,0x04,0x24,0xc5,0xf8,0x11,0x4c,0x24,0x10,0xc5,0xf8,0x11,0x54,0x24,0x20,0xc5,0xf8,0x11,
    0x5c,0x24,0x30,0xc5,0xf8,0x11,0x64,0x24,0x40,0xc5,0xf8,0x11,0x6c,0x24,0x50,0xc5,0xf8,0x11,0x74,0x24,
    0x60,0xc5,0xf8,0x11,0x7c,0x24,0x70,0xc5,0x78,0x11,0x84,0x24,0x80,0x00,0x00,0x00,0xc5,0x78,0x11,0x8c,
    0x24,0x90,0x00,0x00,0x00,0xc5,0x78,0x11,0x94,0x24,0xa0,0x00,0x00,0x00,0xc5,0x78,0x11,0x9c,0x24,0xb0,
    0x00,0x00,0x00,0xc5,0x78,0x11,0xa4,0x24,0xc0,0x00,0x00,0x00,0xc5,0x78,0x11,0xac,0x24,0xd0,0x00,0x00,
    0x00,0xc5,0x78,0x11,0xb4,0x24,0xe0,0x00,0x00,0x00,0xc5,0x78,0x11,0xbc,0x24,0xf0,0x00,0x00,0x00,0x4c,
    0x89,0xef,0x48,0xb8,0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,0xff,0xd0,0xc5,0xf8,0x10,0x04,0x24,0xc5,
    0xf8,0x10,0x4c,0x24,0x10,0xc5,0xf8,0x10,0x54,0x24,0x20,0xc5,0xf8,0x10,0x5c,0x24,0x30,0xc5,0xf8,0x10,
    0x64,0x24,0x40,0xc5,0xf8,0x10,0x6c,0x24,0x50,0xc5,0xf8,0x10,0x74,0x24,0x60,0xc5,0xf8,0x10,0x7c,0x24,
    0x70,0xc5,0x78,0x10,0x84,0x24,0x80,0x00,0x00,0x00,0xc5,0x78,0x10,0x8c,0x24,0x90,0x00,0x00,0x00,0xc5,
    0x78,0x10,0x94,0x24,0xa0,0x00,0x00,0x00,0xc5,0x78,0x10,0x9c,0x24,0xb0,0x00,0x00,0x00,0xc5,0x78,0x10,
    0xa4,0x24,0xc0,0x00,0x00,0x00,0xc5,0x78,0x10,0xac,0x24,0xd0,0x00,0x00,0x00,0xc5,0x78,0x10,0xb4,0x24,
    0xe0,0x00,0x00,0x00,0xc5,0x78,0x10,0xbc,0x24,0xf0,0x00,0x00,0x00,0x48,0x81,0xc4,0x08,0x01,0x00,0x00,
    0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x5f,0x5e,0x5a,0x59,0x58,
};
/* Radians per mouse count at sensitivity 1 (about 0.13 degrees). */
#define RADIANS_PER_COUNT 0.0022f
#define PI 3.14159265f

static volatile int32_t pending_x, pending_y;
static float matrix[4];
static volatile uint32_t calls;
static int active, debug;
static float rest_x, rest_y; /* sub-count remainders, window thread only */

static float wrap(float a) {
    if (a>PI) a-=2.0f*PI;
    else if (a<-PI) a+=2.0f*PI;
    return a;
}
static float field(const unsigned char *cam, unsigned offset) { float v; memcpy(&v,cam+offset,4); return v; }
static void set_field(unsigned char *cam, unsigned offset, float v) { memcpy(cam+offset,&v,4); }

/* Called by the stub on the game's thread, once per camera update. */
static ABI void mouse_camera(unsigned char *cam) {
    uint32_t n=__atomic_add_fetch(&calls,1,__ATOMIC_RELAXED);
    int32_t dx=__atomic_exchange_n(&pending_x,0,__ATOMIC_RELAXED);
    int32_t dy=__atomic_exchange_n(&pending_y,0,__ATOMIC_RELAXED);
    if (dx || dy) {
        const float sens=bbgpu_mouse_sensitivity();
        float pitch=field(cam,0x140)+((float)dx*matrix[0]+(float)dy*matrix[1])*sens;
        float yaw=wrap(field(cam,0x144)+((float)dx*matrix[2]+(float)dy*matrix[3])*sens);
        float low=field(cam,0x1f0), high=field(cam,0x1ec);
        if (pitch<low) pitch=low;
        if (pitch>high) pitch=high;
        set_field(cam,0x140,pitch);
        set_field(cam,0x144,yaw);
        set_field(cam,0x150,pitch);              /* no easing back to the old height */
        set_field(cam,0x130,1.0f);
        set_field(cam,0x134,field(cam,0x1e0));
    }
    if (debug && (n%60==0 || dx || dy))
        printf("Mouse: cam %p update %u d=%d,%d angles 140h=%.3f 144h=%.3f\n",(void *)cam,(unsigned)n,
               (int)dx,(int)dy,(double)field(cam,0x140),(double)field(cam,0x144));
}

void runtime_mouse_install(unsigned char *image) {
    const char *env=getenv("BB_MOUSE");
    if (env && !strcmp(env,"0")) { puts("Mouse: camera off (BB_MOUSE=0)"); return; }
    unsigned char *at=image+HOOK_OFFSET;
    if (memcmp(at,hooked,sizeof(hooked))) {
        puts("Mouse: camera code differs from app 01.09; mouse camera off");
        return;
    }
    const char *invert=getenv("BB_MOUSE_INVERT_Y");
    float s=RADIANS_PER_COUNT, sy=(invert && !strcmp(invert,"1")) ? -s : s;
    matrix[0]=0.0f; matrix[1]=sy; matrix[2]=s; matrix[3]=0.0f;
    const char *axes=getenv("BB_MOUSE_AXES");
    if (axes && sscanf(axes,"%f,%f,%f,%f",&matrix[0],&matrix[1],&matrix[2],&matrix[3])!=4)
        puts("Mouse: BB_MOUSE_AXES needs four numbers; using the defaults");
    const char *dbg=getenv("BB_MOUSE_DEBUG");
    debug=dbg && !strcmp(dbg,"1");
    unsigned char code[512], *p=code;
    uintptr_t function=(uintptr_t)mouse_camera, back=(uintptr_t)(at+sizeof(hooked));
    memcpy(p,save_call_restore,sizeof(save_call_restore));
    static const unsigned char placeholder[8]={0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11};
    unsigned char *imm=NULL;
    for (size_t i=0;i+sizeof(placeholder)<=sizeof(save_call_restore);++i)
        if (!memcmp(p+i,placeholder,sizeof(placeholder))) { imm=p+i; break; }
    if (!imm) { puts("Mouse: stub template is broken; mouse camera off"); return; }
    memcpy(imm,&function,8);
    p+=sizeof(save_call_restore);
    memcpy(p,hooked,sizeof(hooked)); p+=sizeof(hooked);
    p[0]=0xff; p[1]=0x25; memset(p+2,0,4); memcpy(p+6,&back,8); p+=14; /* jmp [rip]: back */
#ifdef _WIN32
    unsigned char *stub=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if (!stub) { puts("Mouse: no memory for the stub; mouse camera off"); return; }
    memcpy(stub,code,(size_t)(p-code));
    DWORD old;
    if (!VirtualProtect(stub,4096,PAGE_EXECUTE_READ,&old)) { puts("Mouse: VirtualProtect failed; mouse camera off"); return; }
    FlushInstructionCache(GetCurrentProcess(),stub,4096);
#else
    unsigned char *stub=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (stub==MAP_FAILED) { puts("Mouse: no memory for the stub; mouse camera off"); return; }
    memcpy(stub,code,(size_t)(p-code));
    if (mprotect(stub,4096,PROT_READ|PROT_EXEC)) { puts("Mouse: mprotect failed; mouse camera off"); return; }
#endif
    /* The image is still writable here (the loader protects segments afterwards). */
    uintptr_t target=(uintptr_t)stub;
    unsigned char jump[17]={0xff,0x25};                                /* jmp [rip]: stub */
    memcpy(jump+6,&target,8);
    jump[14]=jump[15]=jump[16]=0x90;
    memcpy(at,jump,sizeof(jump));
    active=1;
    printf("Mouse: camera hook at 0x183CF2E (image %p), sensitivity %.2f, matrix %g %g %g %g\n",(void *)image,
           (double)bbgpu_mouse_sensitivity(),(double)matrix[0],(double)matrix[1],(double)matrix[2],(double)matrix[3]);
}

int runtime_mouse_active(void) { return active; }

void runtime_mouse_motion(float x, float y) {
    if (!active) return;
    rest_x+=x; rest_y+=y;
    int32_t ix=(int32_t)rest_x, iy=(int32_t)rest_y;
    rest_x-=(float)ix; rest_y-=(float)iy;
    if (ix) __atomic_fetch_add(&pending_x,ix,__ATOMIC_RELAXED);
    if (iy) __atomic_fetch_add(&pending_y,iy,__ATOMIC_RELAXED);
}

/* Drops motion gathered while the camera did not run (menus): no jump when it resumes. */
void runtime_mouse_reset(void) {
    __atomic_exchange_n(&pending_x,0,__ATOMIC_RELAXED);
    __atomic_exchange_n(&pending_y,0,__ATOMIC_RELAXED);
    rest_x=rest_y=0.0f;
}

/* Buttons (bit = SDL button index) while the cursor is captured, and wheel notches, kept for
 * the pad sampler (runtime_pad.c). A notch is held as a press for WHEEL_HOLD_US so the game,
 * which polls the pad once per frame, sees it. */
#define WHEEL_HOLD_US 90000u
static volatile uint32_t buttons, captured;
static volatile uint64_t wheel_up_until, wheel_down_until;
static uint64_t clock_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }

void runtime_mouse_set_captured(int on) {
    __atomic_store_n(&captured,(uint32_t)!!on,__ATOMIC_RELAXED);
    if (!on) __atomic_store_n(&buttons,0,__ATOMIC_RELAXED);
}
int runtime_mouse_captured(void) { return (int)__atomic_load_n(&captured,__ATOMIC_RELAXED); }

void runtime_mouse_button(int button, int down) {
    if (button<1 || button>31) return;
    if (down) __atomic_fetch_or(&buttons,1u<<button,__ATOMIC_RELAXED);
    else __atomic_fetch_and(&buttons,~(1u<<button),__ATOMIC_RELAXED);
}
uint32_t runtime_mouse_buttons(void) { return __atomic_load_n(&buttons,__ATOMIC_RELAXED); }

void runtime_mouse_wheel(float y) {
    uint64_t now_us=clock_us();
    if (y>0) __atomic_store_n(&wheel_up_until,now_us+WHEEL_HOLD_US,__ATOMIC_RELAXED);
    else if (y<0) __atomic_store_n(&wheel_down_until,now_us+WHEEL_HOLD_US,__ATOMIC_RELAXED);
}
/* 1 wheel up held, -1 down held, 0 neither. */
int runtime_mouse_wheel_held(void) {
    uint64_t now_us=clock_us();
    if (now_us<__atomic_load_n(&wheel_up_until,__ATOMIC_RELAXED)) return 1;
    if (now_us<__atomic_load_n(&wheel_down_until,__ATOMIC_RELAXED)) return -1;
    return 0;
}

/* Window thread, every poll: 1 while the camera update ran in the last 250 ms (the player is
 * in the world), so the cursor is captured only then. */
int runtime_mouse_camera_live(uint64_t now_ms) {
    static uint32_t last_calls;
    static uint64_t last_change;
    if (!active) return 0;
    uint32_t now_calls=__atomic_load_n(&calls,__ATOMIC_RELAXED);
    if (now_calls!=last_calls) { last_calls=now_calls; last_change=now_ms; }
    return now_ms-last_change<250;
}

void runtime_mouse_report(void) {
    if (active) printf("Runtime: mouse camera updates=%u\n",(unsigned)calls);
}
