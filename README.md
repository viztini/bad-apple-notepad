# Bad Apple!! in Windows Notepad

The Bad Apple!! shadow-art PV, pumped straight into a real Notepad window —
30 frames per second of pure ASCII, frame-locked to the audio track.

Everything is one C file. No frameworks, no dependencies beyond `ffmpeg.exe`
and the Windows API.

![demo](demo.gif)

## Resizing

The grid is re-measured from the live text control, so the picture follows the
window. The 4:3 image is fitted inside the text area instead of stretched, and
spare space becomes blank margins:

![resize demo](demo-resize.gif)

(Both GIFs were captured from the real program running in Notepad.)

## How it works

```
bad_apple.mp4 ──ffmpeg──> raw gray 480×360 @30fps ──pipe──> box downsampler
                                                                    │
                                                              " .:-=+*#%@"
                                                                    │
                                                        WM_SETTEXT ──> Notepad
bad_apple.mp4 ──ffmpeg──> bad_apple.wav ──PlaySoundA(SND_ASYNC)──> speakers
```

* Frames are decoded by an `ffmpeg` child process writing raw 8-bit
  grayscale to a pipe (`_popen`). Exactly `480×360` bytes per frame, no
  parsing, no seeking.
* Each frame is box-filtered down to the number of character cells that fit
  in Notepad's text control, then mapped through a 10-step brightness ramp
  (`" .:-=+*#%@"`).
* The result is pushed into the edit control with `WM_SETTEXT`. Works with
  both the classic Win32 `Edit` control and Windows 11's `RichEditD2DPT`.
* Audio is extracted once to `bad_apple.wav` (cached next to the video) and
  played with `PlaySoundA`.
* Timing uses `QueryPerformanceCounter` against an absolute schedule, so late
  frames are dropped rather than played in slow motion.
* The font cell size is measured from the live control and the character grid
  is recomputed as you resize. The 4:3 picture keeps its proportions at any
  window size — extra space becomes blank margins instead of stretching.

## Requirements

* Windows 10 or 11
* [ffmpeg](https://www.gyan.dev/ffmpeg/builds/) on `PATH` (or in the working
  directory)
* `bad_apple.mp4` — bring your own copy. The program accepts any video,
  4:3 or not (non-4:3 sources are letterboxed by ffmpeg).
* A C compiler: MinGW-w64 / gcc or MSVC.

Tested on Windows 11 (build 26300) with gcc 16.2 and MSVC 19.x, both the
classic `Edit` control and the Windows 11 `RichEditD2DPT` control.

## Build

MinGW-w64:

```sh
gcc -O2 -Wall -Wextra -std=c99 bad_apple_notepad.c -o bad_apple_notepad.exe -lwinmm -lgdi32
```

MSVC:

```bat
cl /O2 /W3 bad_apple_notepad.c /link winmm.lib gdi32.lib
```

Or just run `build.bat`, which tries both.

## Run

```bat
bad_apple_notepad.exe
bad_apple_notepad.exe D:\videos\bad_apple.mp4
```

Put `bad_apple.mp4` in the same folder as the executable, or pass any path.
The first run extracts `bad_apple.wav` next to the video (a few seconds); later
runs reuse it.

A fresh Notepad window is launched automatically, sized to ~70% of the desktop
and centered. Don't touch it and it plays the whole video. Close Notepad or
press `Ctrl+C` in the console to stop.

## Notepad setup (recommended)

* Font: **Consolas** (or any monospaced font) at 10–11pt
* **Word Wrap: off**
* **Zoom: 100%**

The program adapts to whatever you pick — even Ctrl+wheel zoom is picked up
within half a second — but a monospaced font and word wrap off look best.

## Notes & gotchas

* `WM_SETTEXT` and `WM_GETTEXT` are marshalled across processes by the OS, so
  no DLL injection or automation framework is involved.
* Windows 11's Notepad relaunches itself into a new process, so the player
  doesn't rely on the launched PID — it looks for a Notepad-class window that
  did not exist before launch.
* The Windows 11 Notepad text control also puts its scrollbars in the
  non-client area, so the grid is measured from the *window* rect. Using the
  client rect causes an endless scrollbar ↔ grid feedback loop.
* `tmAveCharWidth` is not the advance width of a monospaced glyph; the program
  measures 32 `M`s instead. Trusting `tmAveCharWidth` makes lines wider than
  the window, which wraps them and shears the picture.
* Audio and video are both started from the same clock; `PlaySoundA` is
  asynchronous, so the picture follows the audio rather than blocking on it.
* Notepad itself can drop frames on very large windows — that's the cost of
  re-setting ~30k characters 30 times a second through the message queue.

## License

MIT — see [LICENSE](LICENSE). Bad Apple!! is © its respective rights holders;
this repository ships no video or audio.
