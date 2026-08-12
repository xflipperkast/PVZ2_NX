<div align="center">

<img src="icon.jpg" alt="pvz2_nx" width="160">

# pvz2_nx

**Plants vs. Zombies 2 on Nintendo Switch**

An unofficial Nintendo Switch port of the Android version of  
**Plants vs. Zombies 2 — 13.3.1**

[![Switch](https://img.shields.io/badge/Nintendo_Switch-Homebrew-E60012?style=for-the-badge&logo=nintendoswitch&logoColor=white)](#)
[![Ko-fi](https://img.shields.io/badge/Support_on_Ko--fi-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/flippyy)

</div>

---

## About

`pvz2_nx` is an experimental port of **Plants vs. Zombies 2 Android 13.3.1** to Nintendo Switch.

The project provides the Switch-side compatibility layer needed to run the Android game code under Horizon OS.

> This repository does **not** include Plants vs. Zombies 2 game assets or other proprietary files.

---

## Build

### Requirements

- [devkitPro](https://devkitpro.org/)
- devkitA64
- libnx
- GNU Make
- The original `pvz2_nx` Makefile / linker setup
- Your own legally obtained **PvZ2 Android 13.3.1** files

### Compile

Clone the repository:

```bash
git clone <repository-url>
cd pvz2_nx
```

Make sure your devkitPro environment is configured:

```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITA64=$DEVKITPRO/devkitA64
```

Then build:

```bash
make -j
```

To rebuild from scratch:

```bash
make clean
make -j
```

> Some source-only development packages do not include the original Makefile, linker configuration, or `elf2nro` setup.  
> If they are missing, use the build files from your existing working `pvz2_nx` environment. (HOLD R when opening homebrew manu true a game) applet mode does NOT work propperly

---

## Running

Place the generated `.nro` and the required game files in the appropriate `pvz2_nx` folder on your SD card, then launch it through your Nintendo Switch homebrew environment.

---

## Status

`pvz2_nx` is still under development.

Some builds may contain experimental fixes or diagnostics. A build should not be considered hardware-stable until it has been tested on an actual Nintendo Switch. Exampled: (Stable 1.0.0/ Dev 1.0.1)

---

## Support

If you like the project and want to support development:

<div align="center">

[![Support me on Ko-fi](https://img.shields.io/badge/☕_Support_me_on_Ko--fi-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/flippyy)

</div>

---

## Disclaimer

This is an unofficial fan-made project and is not affiliated with or endorsed by **PopCap Games** or **Electronic Arts**.

Plants vs. Zombies 2 and all related trademarks, artwork, audio, game assets, and other copyrighted material belong to their respective owners.
