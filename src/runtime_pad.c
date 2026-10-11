/* libScePad on SDL3 gamepads plus keyboard and mouse. SDL events are pumped by the
 * window thread (gpu/shim/window.cpp); here state is only sampled. Keyboard and mouse
 * work by action with bindings from bbport.ini (sample_bindings), next to a gamepad. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define PAD_HANDLE 1

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized, opened, sdl_ready;
static SDL_Gamepad *gamepad;
static size_t reads;
static uint8_t connected_count;

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
static uint8_t axis(int16_t v) { int x=(v+32768)>>8; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}
static void touch_click(PadData *d, int right) {
    d->buttons|=BTN_TOUCHPAD;
    d->touch_count=1;
    d->touches[0]=(PadTouch){.x=right ? 1440 : 480,.y=471,.id=0};
}

/* Opens the first gamepad SDL knows about; called under lock. */
static SDL_Gamepad *current_gamepad(void) {
    if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
    if (sdl_ready<0) return NULL;
    if (gamepad && !SDL_GamepadConnected(gamepad)) { SDL_CloseGamepad(gamepad); gamepad=NULL; }
    if (!gamepad) {
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        if (ids && count>0) {
            gamepad=SDL_OpenGamepad(ids[0]);
            if (gamepad) { ++connected_count; printf("Runtime: gamepad connected: %s\n",SDL_GetGamepadName(gamepad)); }
        }
        SDL_free(ids);
    }
    return gamepad;
}
/* Keyboard and mouse, by action, as in the PC Souls games: bbport.ini key_<action>=binding|binding
 * (the launcher's Controls page edits them; an empty value unbinds). A binding is an SDL key name
 * ("Space", "Return", "Left Shift", "F"), Mouse1..Mouse5 or WheelUp/WheelDown, optionally after
 * Shift+, Ctrl+ or Alt+. A binding with a modifier wins over the same input without one
 * (Shift+Mouse1 strong attack, Mouse1 attack). Mouse inputs count only while the cursor is
 * captured (runtime_mouse.c). The file is re-read when it changes, also during play. Keys held
 * for movement override the stick; diagonals stay on the stick's circle. Reserved by the port:
 * Insert (menu), F9 (pad recording), F10 (mouse camera), left Alt (free the cursor). */
enum { IN_KEY, IN_MOUSE, IN_WHEEL };
enum { MOD_SHIFT=1, MOD_CTRL=2, MOD_ALT=4 };
enum { AX_LX, AX_LY, AX_RX, AX_RY };
typedef struct { uint8_t type, mod; int16_t code; } Input;
typedef struct {
    const char *name;
    uint32_t button;  /* pad buttons, or 0 */
    int8_t axis, dir; /* stick direction when axis >= 0 */
    int8_t touch;     /* 1 left, 2 right touchpad half */
    const char *defaults;
} Action;
static const Action actions[]={
    {"move_forward",0,AX_LY,-1,0,"W"}, {"move_back",0,AX_LY,1,0,"S"},
    {"move_left",0,AX_LX,-1,0,"A"}, {"move_right",0,AX_LX,1,0,"D"},
    {"dodge_run",BTN_CIRCLE,-1,0,0,"Space"},
    {"jump",BTN_L3,-1,0,0,"F"}, /* L3: free camera, enemy control; jumping is dodge_run again while running */
    {"interact",BTN_CROSS,-1,0,0,"E|Return"},
    {"back",BTN_CIRCLE,-1,0,0,"Backspace"},
    {"attack",BTN_R1,-1,0,0,"Mouse1"},
    {"strong_attack",BTN_R2,-1,0,0,"Shift+Mouse1|Mouse5"},
    {"gun",BTN_L2,-1,0,0,"Mouse2"},
    {"transform",BTN_L1,-1,0,0,"Shift+Mouse2|Mouse4"},
    {"lock_on",BTN_R3,-1,0,0,"Q|Mouse3"},
    {"blood_vial",BTN_TRIANGLE,-1,0,0,"R"},
    {"use_item",BTN_SQUARE,-1,0,0,"C"},
    {"dpad_up",BTN_UP,-1,0,0,"Up|WheelUp"},
    {"dpad_down",BTN_DOWN,-1,0,0,"Down|WheelDown"},
    {"dpad_left",BTN_LEFT,-1,0,0,"Left"},
    {"dpad_right",BTN_RIGHT,-1,0,0,"Right"},
    {"menu",BTN_OPTIONS,-1,0,0,"Escape"},
    {"gestures",0,-1,0,1,"G"},
    {"key_items",0,-1,0,2,"T"},
    {"camera_up",0,AX_RY,-1,0,""}, {"camera_down",0,AX_RY,1,0,""},
    {"camera_left",0,AX_RX,-1,0,""}, {"camera_right",0,AX_RX,1,0,""},
};
#define ACTION_COUNT (sizeof(actions)/sizeof(*actions))
#define MAX_BINDINGS 4
static Input bound[ACTION_COUNT][MAX_BINDINGS];
static uint8_t bound_count[ACTION_COUNT];

static int parse_input(const char *text, Input *in) {
    in->mod=0;
    for (;;) {
        const char *plus=strchr(text,'+');
        if (!plus || plus==text || !plus[1]) break;
        size_t n=(size_t)(plus-text);
        if (n==5 && !SDL_strncasecmp(text,"Shift",5)) in->mod|=MOD_SHIFT;
        else if (n==4 && !SDL_strncasecmp(text,"Ctrl",4)) in->mod|=MOD_CTRL;
        else if (n==3 && !SDL_strncasecmp(text,"Alt",3)) in->mod|=MOD_ALT;
        else break;
        text=plus+1;
    }
    if (!SDL_strncasecmp(text,"Mouse",5) && text[5]>='1' && text[5]<='5' && !text[6]) {
        /* Windows numbering, as in the launcher: Mouse2 right, Mouse3 middle (SDL: 3 and 2). */
        static const int16_t sdl_button[6]={0,1,3,2,4,5};
        in->type=IN_MOUSE; in->code=sdl_button[text[5]-'0']; return 1;
    }
    if (!SDL_strcasecmp(text,"WheelUp")) { in->type=IN_WHEEL; in->code=1; return 1; }
    if (!SDL_strcasecmp(text,"WheelDown")) { in->type=IN_WHEEL; in->code=-1; return 1; }
    SDL_Scancode sc=SDL_GetScancodeFromName(text);
    if (sc==SDL_SCANCODE_UNKNOWN) return 0;
    in->type=IN_KEY; in->code=(int16_t)sc;
    return 1;
}
static void bind_list(size_t a, const char *list) {
    char buf[256], *save=NULL;
    snprintf(buf,sizeof(buf),"%s",list);
    bound_count[a]=0;
    for (char *tok=SDL_strtok_r(buf,"|",&save);tok;tok=SDL_strtok_r(NULL,"|",&save)) {
        while (*tok==' ' || *tok=='\t') ++tok;
        char *end=tok+strlen(tok);
        while (end>tok && (end[-1]==' ' || end[-1]=='\t' || end[-1]=='\r' || end[-1]=='\n')) *--end=0;
        Input in;
        if (*tok && bound_count[a]<MAX_BINDINGS && parse_input(tok,&in)) bound[a][bound_count[a]++]=in;
        else if (*tok) printf("Runtime: unknown key binding '%s' for %s\n",tok,actions[a].name);
    }
}
/* Defaults, then the key_<action> lines of bbport.ini (BB_CONFIG); again when the file changes. */
static void load_bindings(void) {
    static const char *path; static int ready; static uint64_t last; static long long stamp=-1;
    const uint64_t now=now_us();
    if (ready && now-last<500000) return;
    last=now;
    if (!ready) {
        ready=1;
        path=getenv("BB_CONFIG");
        if (!path || !*path) path="bbport.ini";
        for (size_t a=0;a<ACTION_COUNT;++a) bind_list(a,actions[a].defaults);
    }
    struct stat st;
    if (stat(path,&st)!=0) return;
    const long long now_stamp=(long long)st.st_mtime*1000003LL+(long long)st.st_size;
    if (now_stamp==stamp) return;
    stamp=now_stamp;
    FILE *f=fopen(path,"r");
    if (!f) return;
    for (size_t a=0;a<ACTION_COUNT;++a) bind_list(a,actions[a].defaults);
    char line[512];
    while (fgets(line,sizeof(line),f)) {
        if (strncmp(line,"key_",4)) continue;
        char *eq=strchr(line,'=');
        if (!eq) continue;
        *eq=0;
        for (size_t a=0;a<ACTION_COUNT;++a) if (!strcmp(line+4,actions[a].name)) bind_list(a,eq+1);
    }
    fclose(f);
    printf("Runtime: key bindings from %s\n",path);
}
static int input_down(const Input *in, const bool *k, uint32_t mouse, int wheel) {
    switch (in->type) {
    case IN_KEY: return k && k[in->code];
    case IN_MOUSE: return (int)((mouse>>in->code)&1u);
    default: return wheel==in->code;
    }
}
static uint8_t stick_byte(float v) {
    int x=(int)(128.0f+v*127.5f);
    return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x);
}
/* Walking on keys. A pad stick never jumps: switching W to D passes through the diagonal and
 * never through the centre, and the game stops the character (a stop or turn animation) when
 * the stick snaps to the centre or by 90 degrees between two frames. So: of two opposite keys
 * the one pressed last wins (W+S walks, not stops); a gap of up to WALK_GAP_US with no key
 * (W released just before D is pressed) keeps the last direction; and the direction turns
 * toward the keys at WALK_TURN_RATE instead of jumping, except for a turn back (over 135
 * degrees), which goes at once as on a pad. Returns 0 when no direction is held. */
#define WALK_GAP_US 60000u
#define WALK_TURN_RATE 14.0f /* radians per second: 90 degrees in about 110 ms */
static int keyboard_walk(const int *neg, const int *pos, float *out_x, float *out_y) {
    static int was_neg[2], was_pos[2], last_dir[2];
    static float angle;
    static int moving;
    static uint64_t last_sample, last_held;
    const uint64_t now=now_us();
    const float dt=last_sample ? (float)(now-last_sample)*1e-6f : 0.0f;
    last_sample=now;
    float t[2];
    for (int a=0;a<2;++a) {
        if (neg[a] && !was_neg[a]) last_dir[a]=-1;
        if (pos[a] && !was_pos[a]) last_dir[a]=1;
        was_neg[a]=neg[a]; was_pos[a]=pos[a];
        t[a]=neg[a] && pos[a] ? (float)last_dir[a] : (float)(pos[a]-neg[a]);
    }
    if (t[0]==0.0f && t[1]==0.0f) {
        if (!moving) return 0;
        if (now-last_held>WALK_GAP_US) { moving=0; return 0; }
    } else {
        const float target=SDL_atan2f(t[1],t[0]);
        float diff=target-angle;
        while (diff>3.14159265f) diff-=6.2831853f;
        while (diff<-3.14159265f) diff+=6.2831853f;
        const float step=WALK_TURN_RATE*(dt<0.1f ? dt : 0.1f);
        if (!moving || SDL_fabsf(diff)>2.36f || SDL_fabsf(diff)<=step) angle=target;
        else angle+=diff>0.0f ? step : -step;
        moving=1;
        last_held=now;
    }
    *out_x=SDL_cosf(angle);
    *out_y=SDL_sinf(angle);
    return 1;
}
static void sample_bindings(PadData *d, const bool *k) {
    load_bindings();
    const int captured=runtime_mouse_captured();
    const uint32_t mouse=captured ? runtime_mouse_buttons() : 0;
    const int wheel=captured ? runtime_mouse_wheel_held() : 0;
    int mods=0;
    if (k) {
        if (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]) mods|=MOD_SHIFT;
        if (k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RCTRL]) mods|=MOD_CTRL;
        if (k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT]) mods|=MOD_ALT;
    }
    /* Inputs claimed by an active binding with a modifier: their plain bindings stay off. */
    Input taken[16]; int ntaken=0;
    for (size_t a=0;a<ACTION_COUNT;++a)
        for (int b=0;b<bound_count[a];++b) {
            const Input *in=&bound[a][b];
            if (in->mod && (mods&in->mod)==in->mod && input_down(in,k,mouse,wheel) && ntaken<16) taken[ntaken++]=*in;
        }
    int neg[4]={0}, pos[4]={0};
    for (size_t a=0;a<ACTION_COUNT;++a) {
        int active=0;
        for (int b=0;b<bound_count[a] && !active;++b) {
            const Input *in=&bound[a][b];
            if (!input_down(in,k,mouse,wheel)) continue;
            if (in->mod) { active=(mods&in->mod)==in->mod; continue; }
            int claimed=0;
            for (int t=0;t<ntaken;++t) if (taken[t].type==in->type && taken[t].code==in->code) claimed=1;
            active=!claimed;
        }
        if (!active) continue;
        const Action *act=&actions[a];
        if (act->axis>=0) { if (act->dir<0) neg[act->axis]=1; else pos[act->axis]=1; }
        else if (act->touch) touch_click(d,act->touch==2);
        else d->buttons|=act->button;
    }
    float lx, ly;
    if (keyboard_walk(neg,pos,&lx,&ly)) { d->left_x=stick_byte(lx); d->left_y=stick_byte(ly); }
    uint8_t *sticks[2][2]={{&d->left_x,&d->left_y},{&d->right_x,&d->right_y}};
    for (int s=1;s<2;++s) {
        float x=(float)(pos[s*2]-neg[s*2]), y=(float)(pos[s*2+1]-neg[s*2+1]);
        if (x==0.0f && y==0.0f) continue;
        if (x!=0.0f && y!=0.0f) { x*=0.70710678f; y*=0.70710678f; }
        *sticks[s][0]=stick_byte(x);
        *sticks[s][1]=stick_byte(y);
    }
    if (d->buttons & BTN_L2) d->l2=255;
    if (d->buttons & BTN_R2) d->r2=255;
}

static void sample_host(PadData *d) {
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=1; d->connected_count=connected_count ? connected_count : 1;
    d->timestamp=now_us();
    SDL_Gamepad *g=current_gamepad();
    if (bbgpu_overlay_captures_input()) return; /* settings menu open: neutral input */
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    if (g) {
        static const struct { SDL_GamepadButton sdl; uint32_t ps; } map[]={
            {SDL_GAMEPAD_BUTTON_SOUTH,BTN_CROSS}, {SDL_GAMEPAD_BUTTON_EAST,BTN_CIRCLE},
            {SDL_GAMEPAD_BUTTON_WEST,BTN_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH,BTN_TRIANGLE},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,BTN_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,BTN_R1},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK,BTN_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK,BTN_R3},
            {SDL_GAMEPAD_BUTTON_START,BTN_OPTIONS}, {SDL_GAMEPAD_BUTTON_BACK,BTN_TOUCHPAD},
            {SDL_GAMEPAD_BUTTON_TOUCHPAD,BTN_TOUCHPAD},
            {SDL_GAMEPAD_BUTTON_DPAD_UP,BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN,BTN_DOWN},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT,BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,BTN_RIGHT},
        };
        for (size_t i=0;i<sizeof(map)/sizeof(*map);++i) if (SDL_GetGamepadButton(g,map[i].sdl)) d->buttons|=map[i].ps;
        d->left_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX)); d->left_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY));
        d->right_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX)); d->right_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY));
        d->l2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)); d->r2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        if (d->l2>30) d->buttons|=BTN_L2;
        if (d->r2>30) d->buttons|=BTN_R2;
        if (SDL_GetNumGamepadTouchpads(g)>0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
        // Back/Select on pads without a touch surface is a left-side click.
        if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    }
    sample_bindings(d,k);
}

/* BB_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad touchpad_left touchpad_right up down left right) are held while listed;
 * touchpad defaults to a left-side click; lx= ly= rx= ry= (0..255) override
 * the sticks. An empty file releases everything. */
static struct { uint32_t buttons; int stick[4]; int touch_side; } injected={0,{-1,-1,-1,-1},-1};
static int replay_armed;      /* 1 while a BB_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t replay_start; /* 0: (re)start at the next sample */
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check; static struct timespec mtime;
    if (!checked) { path=getenv("BB_PAD_FILE"); checked=1; }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
    struct stat st;
    if (stat(path,&st)!=0) return;
#ifdef _WIN32
    /* Whole-second mtimes: the size tells two edits within a second apart. */
    if (st.st_mtime==mtime.tv_sec && st.st_size==mtime.tv_nsec) return;
    mtime=(struct timespec){st.st_mtime,(long)st.st_size};
#else
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
#endif
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const struct { const char *name; uint32_t ps; } names[]={
        {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
        {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
        {"options",BTN_OPTIONS}, {"touchpad",BTN_TOUCHPAD},
        {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
    };
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    injected.buttons=0;
    injected.touch_side=-1;
    for (int i=0;i<4;++i) injected.stick[i]=-1;
    char token[64];
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* BB_PAD_REPLAY */
        if (!strcmp(token,"touchpad_left") || !strcmp(token,"touchpad_right")) {
            injected.buttons|=BTN_TOUCHPAD;
            injected.touch_side=!strcmp(token,"touchpad_right");
        }
        for (size_t i=0;i<sizeof(names)/sizeof(*names);++i) if (!strcmp(token,names[i].name)) injected.buttons|=names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(token,sticks[i],3)) { int v=atoi(token+3); injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
    }
    fclose(f);
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d\n",injected.buttons,
           injected.stick[0],injected.stick[1],injected.stick[2],injected.stick[3]);
}
/* BB_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; BB_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in BB_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
static void record_sample(const PadData *d) {
    static const char *path; static int checked, f9_was_down;
    if (!checked) { path=getenv("BB_PAD_RECORD"); checked=1; }
    if (!path || !*path || !sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9];
    if (f9 && !f9_was_down) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else if ((record_file=fopen(path,"w"))) {
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",path);
        }
    }
    f9_was_down=f9;
    if (!record_file) return;
    PadSample s={(uint32_t)((now_us()-record_start)/1000),d->buttons,
                 {d->left_x,d->left_y,d->right_x,d->right_y},d->l2,d->r2};
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2) return;
    record_last=s;
    fprintf(record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
            s.axes[3],s.l2,s.r2);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static void replay_sample(PadData *d) {
    if (!replay_armed) return;
    if (!replay_start) {
        static int loaded;
        if (!loaded) {
            loaded=1;
            const char *path=getenv("BB_PAD_REPLAY");
            FILE *f=path ? fopen(path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0;
            while (f && fscanf(f,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8) {
                s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
                if (replay_count==cap && !(replay=realloc(replay,(cap=cap ? cap*2 : 1024)*sizeof(*replay)))) break;
                replay[replay_count++]=s;
            }
            if (f) fclose(f);
            printf("Runtime: pad replay of %zu samples from %s\n",replay_count,path ? path : "(unset)");
        }
        replay_start=now_us();
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
}
/* After the menu or the text dialog closes, buttons still held (the Cross that accepted a
 * name) stay hidden until released: the game would take them as a new press. */
static int hold_after_capture;
static void sample(PadData *d) {
    sample_host(d);
    if (bbgpu_overlay_captures_input()) { hold_after_capture=1; return; }
    record_sample(d);
    read_inject();
    replay_sample(d);
    d->buttons|=injected.buttons;
    if (injected.touch_side>=0) touch_click(d,injected.touch_side);
    else if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    if (injected.buttons & BTN_L2) d->l2=255;
    if (injected.buttons & BTN_R2) d->r2=255;
    uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
    for (int i=0;i<4;++i) if (injected.stick[i]>=0) *axes[i]=(uint8_t)injected.stick[i];
    if (hold_after_capture) {
        if (d->buttons) d->buttons=0;
        else hold_after_capture=0;
    }
}

static ABI int32_t pad_init(void) { pthread_mutex_lock(&lock); initialized=1; pthread_mutex_unlock(&lock); return 0; }
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user!=1) return ERR_INVALID_ARG;
    if (type!=0 && type!=2) return ERR_INVALID_ARG; /* standard / special port */
    if (index) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    int already=opened; opened=1;
    pthread_mutex_unlock(&lock);
    if (already) return ERR_ALREADY_OPENED;
    puts("Runtime: pad opened for user 1 (SDL gamepad or keyboard)");
    return PAD_HANDLE;
}
static ABI int32_t pad_close(int32_t handle) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    opened=0; return 0;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    static int caller_logged;
    if (!caller_logged && getenv("BB_PAD_DEBUG")) {
        caller_logged=1;
        printf("Runtime: scePadReadState called from host %p, buffer %p\n",__builtin_return_address(0),(void *)data);
    }
    pthread_mutex_lock(&lock);
    sample(data); ++reads;
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Buffered read: the port samples once per call, so one entry is returned. */
static ABI int32_t pad_read(int32_t handle, PadData *data, int32_t count) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data || count<1 || count>64) return ERR_INVALID_ARG;
    static int caller_logged;
    if (!caller_logged && getenv("BB_PAD_DEBUG")) {
        caller_logged=1;
        printf("Runtime: scePadRead called from host %p, buffer %p\n",__builtin_return_address(0),(void *)data);
    }
    pad_read_state(handle,data);
    return 1;
}
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->connected=1; info->device_class=0;
    pthread_mutex_lock(&lock);
    current_gamepad();
    info->connected_count=connected_count ? connected_count : 1;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    SDL_Gamepad *g=current_gamepad();
    if (g) SDL_RumbleGamepad(g,(uint16_t)(param[0]*257),(uint16_t)(param[1]*257),1000);
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) { return handle==PAD_HANDLE && opened ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { (void)flag; return pad_ok_handle(handle); }

static const RuntimeExport exports[]={
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state}, {"scePadRead",pad_read},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadResetOrientation",pad_ok_handle},
    {"scePadSetAngularVelocityDeadbandState",pad_ok_handle_flag}, {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) { printf("Runtime: pad reads=%zu, gamepad=%s\n",reads,gamepad ? SDL_GetGamepadName(gamepad) : "none"); }
