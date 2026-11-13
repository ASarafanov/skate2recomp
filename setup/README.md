# Setup and launcher

`Skate2-PC-Setup-<version>.exe` installs Skate 2 PC from the player's own Xbox 360
disc image. The player picks only the ISO and the install folder. The installed
`Launch Skate 2.exe` starts the game without PowerShell, so it also runs under
Wine / Proton (Steam Deck).

## How it is made

There is one program, `Launch Skate 2.exe` (`setup/src`, plain Win32, static CRT,
no dependencies beyond system DLLs). `tools/package_setup.py` appends the program
files to a copy of it. With that payload present, the exe shows the setup window;
without it, it is the launcher.

The payload is an allowlist:

- `skate2.exe`, `rexruntime.dll`, `rexgpu-xenos.dll`, `LICENSE`,
  `THIRD_PARTY_NOTICES.md` and `licenses/`, unchanged from the upstream
  [v0.1.1 player package](https://github.com/christianortel/skate2-recompile-pc/releases/tag/v0.1.1).
  The script downloads it and checks its SHA-256.
- The Microsoft Visual C++ runtime DLLs from the local Visual Studio redist folder.
  The game imports only functions that toolset 14.44 provides. A Proton prefix has
  no runtime installed, so these DLLs are installed next to `skate2.exe`.
- `README.txt` (`setup/README-player.txt`) and the runtime notice.

The script also checks every PE import against the shipped and system DLLs, and
refuses game files.

## Build

Requires Visual Studio 2022 or Build Tools with the C++ workload, plus Python 3.

```bat
setup\build.cmd
python tools\package_setup.py
```

The output is `dist/Skate2-PC-Setup-v<version>.exe`, a `.zip` of it and
`SHA256SUMS.txt`. No game files or ReXGlue SDK are needed to build.

## What the setup does

1. **Checks the source.** It accepts the ISO or an already extracted game folder:
   - **ISO:** the setup reads its XDVDFS file table (plain XISO, XGD1, XGD2 or
     XGD3).
   - **Extracted folder:** the folder must hold `default.xex` and `data`. The
     player can also point at that `default.xex` itself. Links in the folder are
     refused.
   - **Archive:** a `.rar`, `.zip` or `.7z` gets a message to extract it first.

   In every case `default.xex` must match: title ID `4541087F`, then the SHA-256
   `116ba3ee…`. Other releases are rejected before anything is copied.
2. **Picks the folder.** If the chosen folder already holds other files, the setup
   installs into a `Skate 2` folder inside it. The path must be ASCII: the
   prebuilt game opens files through ANSI paths. Free space is checked.
3. **Copies the files.** Copies the program files (SHA-256 checked), then the
   launcher, then the game files into `game\` (skipping `$SystemUpdate`).
   `default.xex` goes last and is checked again.
   - Each file is written as `.part` and renamed when complete.
   - While the install is unfinished, a `.skate2-install` marker stays in the
     folder. A cancelled or interrupted install resumes when the setup runs
     again on the same folder.
4. **Updates.** Running a newer setup on an existing install replaces only the
   program files. It needs no disc and keeps `userdata`, `skate2.toml` and
   `launcher.ini`.
5. **Finishes.** Saves the folder in `HKCU\Software\Skate2PC`. On Windows it also
   creates optional Desktop and Start menu shortcuts.

Unattended install, which writes `setup.log` in the target folder:

```bat
Skate2-PC-Setup-v0.2.1.exe --quiet --iso=D:\Skate2.iso --dir=C:\Games\Skate 2
```

`--iso` also takes an extracted game folder. Exit codes: 0 done, 1 install failed,
2 bad folder, 3 disc rejected.

## Launcher

`Launch Skate 2.exe` starts `skate2.exe` with the profile of upstream's
`Launch-Skate2.ps1`:

- `--gpu_plugin=xenos --input_backend=sdl --render_target_path_d3d12=rov`
- `--async_shader_compilation=false --d3d12_present_idleclean_sync=true`
- the frame rate, the render scale and the game, user data and cache folders
  of the install

It waits for the game to exit, because Steam tracks the process it started.

The options are in `launcher.ini`:

| Option | Default on Windows | Default under Proton |
| --- | --- | --- |
| Render scale | 2× | 1× |
| Frame rate | unlimited | 60 |
| Render path | ROV | ROV |
| Windowed | off | off |

- **Launcher window:** opens on every start, with the options and a **Play** button.
  A controller drives it (d-pad to move and change, A to select, Start to play,
  B to quit), so it works in Steam Deck Game Mode. `--play` skips the window.
- **Upstream arguments** still work: `--render-scale N`, `--frame-rate X`,
  `--windowed`.
- **Game flags:** any other `--flag=value` argument is passed to `skate2.exe` as
  is.

## Steam Deck

1. In Desktop Mode, run the setup with Proton. For example, add it to Steam as a
   non-Steam game and force Proton in its compatibility settings.
2. Install to the suggested `~/Games/Skate 2`. The setup then shows the Linux
   path of the launcher.
3. Add that `Launch Skate 2.exe` to Steam as a non-Steam game, force Proton, and
   play it from Game Mode.

Under Proton the game uses Direct3D 12 through vkd3d-proton. Running on a Steam
Deck has not been tested yet. If the picture is broken, pick RTV in the options.
