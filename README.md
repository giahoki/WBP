<div align="center">

# 🩸 WBP — Windows Bloodborne Port

**Bloodborne on Windows 10 and 11 — made to be played with a mouse and keyboard.**

[![Download](https://img.shields.io/github/v/release/giahoki/WBP?label=download&style=for-the-badge&color=2f7383)](https://github.com/giahoki/WBP/releases/latest)
[![Windows](https://img.shields.io/badge/Windows-10%20%7C%2011-2b2b2b?style=for-the-badge&logo=windows)](#-requirements)
[![Game](https://img.shields.io/badge/CUSA03173-v1.09-c8a96a?style=for-the-badge)](#-how-to-play)
[![License](https://img.shields.io/badge/license-GPL--2.0+-555?style=for-the-badge)](LICENSE)

**English** · [Русский](README.ru.md)

[Download](#-download) · [Features](#-features) · [How to play](#-how-to-play) · [Online](#-online-play-beta) · [Controls](#-controls) · [Performance](#-if-the-game-runs-slowly) · [Building](#-building)

</div>

---

The game's own x86-64 code runs **directly on your processor** — nothing is emulated on the CPU
side. Only the graphics are translated, from the PS4's GPU commands to Vulkan. Unpack, start
`Bloodborne.exe`, choose your game folder, press **PLAY**.

> [!IMPORTANT]
> **No game files are included.** You need your own decrypted dump of Bloodborne (CUSA03173),
> version **1.09** for the frame rate, resolution and effect patches (other versions run at 30 FPS).
> This project is not affiliated with Sony Interactive Entertainment or FromSoftware.

## 📦 Download

Get the ready-to-play build from **[the releases of giahoki/WBP](https://github.com/giahoki/WBP/releases/latest)**:
download the zip, unpack it anywhere, done. Nothing else to install. The launcher tells you when
a new version is out and installs it in one click; your saves, settings and mods are kept.

## ✨ Features

<table>
<tr>
<td width="50%" valign="top">

### 🖱️ Mouse and keyboard
- Mouse turns the camera **directly** — no stick acceleration or smoothing
- Bindings **by action**: two keys or mouse buttons each, the wheel, side buttons, Shift / Ctrl / Alt combos
- The game's **button prompts show your keys**
- **Smooth walking**: changing direction never stops the character
- Gamepads keep working alongside

</td>
<td width="50%" valign="top">

### 🎨 Graphics
- **FSR 3.1**, **FSR 4**, **DLSS** (GeForce RTX) or native **TAA**
- **FSR 3 frame generation** — about twice the frames on screen, with any upscaler
- **30 / 60 / 90 FPS** or unlocked with a cap
- Output from **720p to 4K**
- Effects on and off: motion blur, DoF, chromatic aberration, SSAO, shadows, reflections
- In-game settings menu — **Insert**

</td>
</tr>
<tr>
<td valign="top">

### 🕯️ Launcher
- In the game's own menu style: option rows, **Q / E** tabs, a description for every setting
- **13 languages**
- Updates in one click, **Add to Steam**, desktop shortcut

</td>
<td valign="top">

### 🧩 More
- **Online play (beta)** — messages, bloodstains, ghosts, summons
- **Mods** and third-party **patches** — the game files are never changed
- **Cheats** and gameplay tweaks
- Built with **ThinLTO + PGO** for speed

</td>
</tr>
</table>

## 💻 Requirements

| | |
|---|---|
| **OS** | Windows 10 (1903 or later) or Windows 11, 64-bit |
| **Graphics** | Vulkan 1.3 with a current driver; 6 GB of video memory or more recommended |
| **Memory** | about 6 GB free (10 GB for 1440p / 4K output) |
| **DLSS** | GeForce RTX 20 series or newer |

## 🎮 How to play

1. Download the zip from [the releases](https://github.com/giahoki/WBP/releases/latest) and unpack it.
2. Start **`Bloodborne.exe`**.
3. On **Game & effects**, choose your game folder (the one with `eboot.bin`).
4. Press **PLAY**.

> [!TIP]
> `Play Bloodborne.exe` starts the game straight away with your saved settings — handy for
> shortcuts and Steam.

## 🌐 Online play (beta)

- **Messages, bloodstains and wandering ghosts** from other hunters, through
  [The Hunter's Dream](https://thehuntersdream.com).
- **Sign-in, bells and summons** through a shadNet server (shadPS4's public server by default).
- The **Online co-op** page in the launcher, with a **Test connection** button.

**How to go online:**

1. Register an account at **[shadnet.shadps4.net](https://shadnet.shadps4.net)**.
2. In the launcher, open **Online co-op**, tick **Play online**, and enter your **account name**
   (not your email) and password.
3. Press **PLAY** and choose online at the title screen. The Hunter's Dream welcome message means
   you are online.

Everyone you play with needs game version 1.09 and the same co-op server. Turn cheats off when you
play together — they change the game for the others too.

## ⌨️ Controls

| Action | Key | | Action | Key |
|---|---|---|---|---|
| Move | `W` `A` `S` `D` | | Lock on | `Q` / middle mouse |
| Camera | mouse | | Interact | `E` / `Enter` |
| Dodge / run (hold) | `Space` | | Blood vial | `R` |
| Jump | `Space` again while running | | Use item | `C` |
| Attack | left mouse | | Switch items | arrows / mouse wheel |
| Strong attack | `Shift` + left mouse / mouse 5 | | Menu | `Esc` |
| Firearm / parry | right mouse | | Gestures | `G` |
| Transform weapon | `Shift` + right mouse / mouse 4 | | Key items | `T` |

Everything can be rebound on the **Controls** tab. Reserved by the port: **`Insert`** — settings
menu, **left `Alt`** (hold) — free the cursor, **`F10`** — mouse camera on/off.

## 🎞️ Frame generation

*Graphics → Frame generation (FSR 3)*, or **Insert** in the game. One frame is interpolated
between every two game frames, so the screen shows about twice as many. It works with every
upscaler (FSR 3.1, FSR 4, DLSS, TAA), not with the upscaler off. The HUD is kept out of the
interpolation, so it does not wobble.

Two frames are shown per game frame and the monitor shows at most one per refresh, so with frame
generation on the game is capped (*Frame cap with frame generation*):

| Mode | Game FPS | On screen |
|------|----------|-----------|
| **60 / 90 FPS** (default) | 60, or 90 from 180 Hz | 120 / 180 |
| **Half the refresh rate** | 82 at 165 Hz, 72 at 144 Hz | the refresh rate |

- Input delay grows by about half a game frame.
- Worth it when the game runs below about half your refresh rate without it. Above that, plain
  frames feel better than real + generated ones.

## 🚀 If the game runs slowly

- On graphics cards other than AMD Radeon use **FSR 3.1** (or **DLSS** on GeForce RTX): FSR 4 is
  much heavier there. The launcher warns about it.
- Pick the **Performance** preset and a **60 FPS** cap on weaker hardware.
- Turn off **motion blur** and **dynamic light shadows** on *Game & effects*.
- Black screen after an update → **Advanced → Clear shader cache**.

> [!NOTE]
> Reporting a problem? Turn on **Frame statistics in the log** on *Advanced*, play a couple of
> minutes and attach `user\last_run.log` with your graphics card and processor to an
> [issue](https://github.com/giahoki/WBP/issues).

## 🛠️ Building

Install [MSYS2](https://www.msys2.org) with the CLANG64 packages from
[packaging/windows/README.md](packaging/windows/README.md), then:

```bat
build.bat
```

The game, the launcher and the package (`dist\WBP\` + zip) are built in one go, with
ThinLTO and the profile in `pgo\wbp.profdata`. How everything works inside: **[docs/](docs/README.md)**
(loader, graphics, launcher, input, mods, build).

| Part | Language | Where |
|---|---|---|
| Loader and runtime — the PS4 system libraries | C | `src/` |
| Graphics — PS4 GPU to Vulkan, upscalers, in-game menu | C++ | `gpu/` |
| Launcher and game preparation | C++ | `launcher/` |

## ❤️ Credits

Based on **[Bloodborne for Windows](https://github.com/Supermedo/bloodborne_pc)** by Supermedo. Thank you.

## 📄 License

GNU GPL v2 or later ([LICENSE](LICENSE)). Third-party components keep their own licenses
(`licenses\` in the download).
