# Bloodborne 1.09 (CUSA03173): addresses used by the port

Everything here is for app version **01.09** only. Other versions have different code.

## Address conventions

| What | Value |
|---|---|
| PS4 address (IDA, `patches/Bloodborne.xml`) | eboot base `0x400000` |
| Image offset in the loader (`src/probe.c`) | PS4 address − `0x400000` |
| Host address at run time | image base + image offset |
| Image base seen on Windows | `0x800000000` (printed by `Mouse: camera hook … (image …)`) |

So a hook at PS4 `0x183CF2E` is written at `image + 0x143CF2E`.

The IDA database is built from a plain ELF: the plaintext SELF (`eboot.bin`) is unpacked the way
`launcher/src/prep/image.cpp` (`ParseSelf`) does it, keeping only the two `PT_LOAD` headers rebased
to `0x400000` (`e_type = EXEC`). IDA's GUI crashes during auto-analysis of this 90 MB image; `idat -A` (headless)
finishes in about 20 minutes.

## Used by the port

### Mouse camera hook (`src/runtime_mouse.c`)

| PS4 address | Image offset | Bytes (17) | Instructions |
|---|---|---|---|
| `0x183CF2E` | `0x143CF2E` | `C4 C1 7A 10 85 40 01 00 00 C5 FA 11 85 10 FD FF FF` | `vmovss xmm0,[r13+140h]` / `vmovss [rbp-2F0h],xmm0` |

- Replaced by `FF 25 00 00 00 00 <stub>` plus 3 × `90`; the stub returns to `0x183CF3F`.
- Every branch of the camera update reaches it once per frame (xrefs from `0x183CF25`,
  `0x183C9BE`, `0x183C9C6`, `0x183CE9A`, `0x183CEA4`). No jumps into the replaced bytes.
- At the hook, `r13` = camera object. The stub saves `rax…r11` and `xmm0–15` and calls
  `mouse_camera(cam)` (SysV ABI).
- The install checks the 17 bytes first and leaves the mouse off when they differ.

### Pad reading (`src/runtime_pad.c`)

| PS4 address | What |
|---|---|
| `0x2A91870` | PS4 pad device update (`sub_2A91870`, not auto-detected by IDA) |
| `0x2A91948` | `call scePadReadState` (its return address `0x2A9194D` is logged with `BB_PAD_DEBUG=1`) |

The pad device reads `PadData` into `[dev+150h]`. Stick bytes become floats at
`[dev+1E4h]` (lx), `[dev+1E8h]` (ly), `[dev+1ECh]` (rx), `[dev+1F0h]` (ry); the dead zone comes
from `scePadGetControllerInformation` (`[dev+1D0h]`, `[dev+1D1h]`). There is no smoothing over time
at this stage.

## Player camera: `sub_183AC60`

The camera update (`0x183AC60`, `0x4E6D` bytes). Arguments: `rdi` = camera object (kept in `r13`),
`rsi` = owner (kept in `r15` until `0x183C1BB`), `rdx` stored at `[rbp-2B0h]`, `xmm0` = frame time.
Called from `0x183AC51` (`sub_183A0E0`), `0x1836C00` and `0x1836E15` (`sub_18368B0`).

### Camera object fields

| Offset | Type | Meaning |
|---|---|---|
| `+130h` | float | manual-input weight; the stick sets 1.0 |
| `+134h` | float | manual-input timer; the stick sets it to `[+1E0h]` |
| `+13Ch` | byte | input blocked (stick ignored when non-zero) |
| `+140h` | float | **vertical angle** (pitch), radians |
| `+144h` | float | **horizontal angle** (yaw), radians, wrapped to ±π |
| `+150h` | float | vertical angle the camera eases back to when there is no input |
| `+15Ch` | byte | manual input this frame |
| `+1A8h` | float | ease-back factor for `+140h` toward `+150h` |
| `+1E0h` | float | manual-input hold time |
| `+1ECh` | float | vertical angle maximum |
| `+1F0h` | float | vertical angle minimum |
| `+238h` | float | (angle used by the stick threshold test at `0x183CC3F`) |
| `+260h` | byte | stick branch enabled |
| `+263h` | byte | (cleared on manual input) |
| `+276h` | byte | velocity-smoothing mode |
| `+278h`, `+27Ch` | float | angular velocity for `+140h`, `+144h` (smoothed stick) |
| `+280h`, `+284h` | float | stick speed per axis |
| `+288h` | float | stick velocity smoothing factor |
| `+334h` | byte | read input (0: input vector is zero) |

### Points inside `sub_183AC60`

| PS4 address | What |
|---|---|
| `0x183AC74` | after the prologue: `mov [rbp-2B0h],rdx; mov r15,rsi; mov r13,rdi` (13 bytes, `rax` free). Too early for the mouse: the angles are re-derived later. |
| `0x183B85A`–`0x183B88C` | input vector: `[[[r15+58h]+280h] or [[r15+58h]+80h]]+40h` (right stick, x/y floats); zero vector `qword_4B38000` when `[cam+334h]==0` |
| `0x183B8AE` | zero right stick → jumps to `0x183C579` and skips the manual branch |
| `0x183B918` | input vector saved to `[rbp-3E0h]` |
| `0x183C9CC`–`0x183CAAC` | stick branch: velocity `+278h/+27Ch` eased toward input × speed, then `angle += velocity` at `0x183CA30` (`+140h`) and `0x183CA72` (`+144h`) |
| `0x183CD75`–`0x183CE5D` | angles += stick × time; vertical clamped to `[+1F0h]..[+1ECh]`; written at `0x183CE54` (`+140h`) and `0x183CE5D` (`+144h`) |
| `0x183CE71`–`0x183CE8E` | on manual input: `[+150h] = vertical`, `[+130h] = 1.0`, `[+134h] = [+1E0h]` |
| `0x183CEAA`–`0x183CF25` | no manual input: vertical eases back to `[+150h]` by `[+1A8h]` |
| `0x183CF2E` | angles final; camera built from them (**the mouse hook**) |
| `0x183C147`–`0x183C1C4`, `0x183C2E0`–`0x183C522` | lock-on: angles follow the target |

Constants: `0x40490FDB` = π, `dword_4D25E74` = 2π (angle wrapping).

### Community patches that touch the camera (`patches/Bloodborne.xml`)

| Patch | Addresses |
|---|---|
| Disable Camera Auto Rotation via Movement | `0x183C6E8` (`vmovss [r13+144h],xmm0`), `0x183C984` (`[r13+130h]`), `0x183DDE6`, `0x183C870` (`[r13+144h]`) — 9-byte NOPs |
| Increased camera distance | `0x1836940`, `0x5527A94`, `0x184107F`, `0x18410B1` |

## Input classes (for further work)

FD4 user-input types registered by `sub_2A95CA0` and neighbours:
`DLUserInputPhysicalAnalogModifier`, `DLUserInputVirtualAnalogModifier` (`0x2A95CA0`),
their `…Setting` types (`0x2A973F0`), `DLUserInputDAConverter` (`0x2A981B0`),
`DLUserInputADConverter` (`0x2A938E0`), `DLUserInputDirectConverter`. Names at
`0x4C6DCF8`–`0x4C6E322`. Not traced further: the walking "stick feel" is the character's
locomotion, not input smoothing. Stops when a key changes direction come from the locomotion's
stop/turn animations reacting to a stick that jumps; `keyboard_walk` in `src/runtime_pad.c` turns
the keyboard direction smoothly instead (docs/INPUT.md).

### Native input (`src/runtime_input.c`)

The pad device (vtable `0x56B84B0`, constructors `sub_2A90D10`/`sub_2A90F80` on the base
`sub_2A8D2B0`) owns five converter passes built in the base constructor. Digital buttons go
through the pass at `0x2A98CD0` (vtable `0x56B8DF0`, slot `+0x40` → jump at `0x2A98EF0`):

| Address | What |
|---|---|
| `0x56B8598` | pad vtable slot `0xE8`: `sub_2A91800` GetButton(dev, physical 0..15) = `[dev+150h] & mask[physical]`, masks at `0x4C6DC20` |
| `0x2A98E40`–`0x2A98E90` | for each converter (iterator in `r14`): `call rbx` (GetButton) at `0x2A98E63`, then the converter's virtual input (`vtable+30h`) as a bit in `[[dev+130h]]` |
| `0x2FA4840` | `sub_2FA4840`: the converter at an iterator |
| `0x2A8D790` | the device's virtual bit read (`[dev+130h]` bit set); `0x2A8D780` the analog one (`[dev+100h]`) |
| `0x138E940`, `0x2FA3F30` | key assign (`FD4BaseKeyAssign.cpp`): adds a converter virtual ← physical |

Physical index → PS4 bit: 0 left, 1 down, 2 right, 3 up, 4 options, 5 R3, 6 L3, 7 square,
8 cross, 9 circle, 10 triangle, 11 R1, 12 L1, 13 R2, 14 L2, 15 touchpad. Virtual inputs per
physical button in 1.09 (logged with `BB_INPUT_DEBUG=1`):

| Physical | Virtual inputs |
|---|---|
| 0 left | 213 214 217 221 228 232 272 310 313 341 |
| 1 down | 204 206 210 218 222 227 231 271 312 342 |
| 2 right | 212 215 216 220 229 233 273 309 314 340 |
| 3 up | 205 207 211 219 223 226 230 270 296 311 339 |
| 4 options | 265 277 302 |
| 5 R3 | 201 208 251 257 275 279 343 |
| 6 L3 | 200 209 250 256 274 276 278 289 304 344 |
| 7 square | 225 236 240 244 295 317 325 338 |
| 8 cross | 194 196 198 237 241 245 266 267 303 318 323 334 335 |
| 9 circle | **menus:** 195 197 199 235 239 243 · **character:** 298 299 316 324 336 |
| 10 triangle | 234 238 242 305 306 307 315 326 337 |
| 11 R1 | 202 247 253 259 268 280 285 293 300 320 329 383 387 |
| 12 L1 | 203 246 252 258 269 282 287 291 294 297 319 327 385 389 |
| 13 R2 | 281 286 292 301 322 330 384 388 |
| 14 L2 | 224 283 284 288 290 308 321 328 390 394 |
| 15 touchpad | 262 331 386 391 392 |

The circle split (menus / character) was confirmed in game on 2026-10-11. The character's
circle needs 298 and 299 together (either alone, or 316/324/336, does nothing); roll, run and
jump are one input told apart by timing. The other buttons' groups are not named yet.

Tutorial icon tokens: `sub_1EE7B50` expands `<?OKIcon?>` / `<?CancelIcon?>` to
`<img src='%s' vspace='-8'>` with `L"KG_Cancel"` / `L"KG_OK"` (swapped when `sub_23B8730`
says so); `0x1EE7C70` is `lea rbx,[L"KG_OK"]`. By analogy 194–245 look like menu inputs (with repeat pulses on the
d-pad) and 266+ like the character's.

## Other addresses in the port

| What | Where |
|---|---|
| Scene render size (startup resolution patch) | `0x2196A6B`, `0x2196A7A` (`launcher/src/prep/patches.cpp`) |
| UI movie viewport size | `0x2358554`, `0x235855D` (`launcher/src/prep/patches.cpp`) |
| Sun shadow cascade size (`shadow_quality=low`) | `mov dword [rsp+0A8h/0ACh], 800h` at `0x2697B22`/`0x2697B2D` (imm32 at `0x2697B29`, `0x2697B34`) in `sub_26979B0`: the main scene's render config `{w, h, shadow w, shadow h, …}` passed to `sub_26C39F0`, which calls `sub_12ABF20(this+0x1320, &shadow_size)` for the four cascades. The game's debug light editor (`sub_26CD2D0`, "Shadow Resolution", handler `0x26CDE90`) offers 1024/2048/4096 through the same call. Patch "Shadow Resolution 1024 (perf increase)" |
| Key-guide icons redrawn for the keyboard | `dvdroot_ps4/menu/common.tpf.dcx`: `KG_*` (32×32 BC7), HUD atlas `MENU_Common_00091` (1024×512 BC7) at (661,123), (694,123), (726,123) (`launcher/src/prep/key_icons.cpp`) |
| All community patches (FPS, effects, cheats) | `patches/Bloodborne.xml` |
