# Bad Apple!! in Notepad

![demo](demo.gif)

Plays a video as ASCII art inside a real Notepad window - 30 fps, audio in sync.

Resize it freely; the picture keeps its aspect ratio:

![resize demo](demo-resize.gif)

## Build

```sh
gcc -O2 -Wall -Wextra bad_apple_notepad.c -o bad_apple_notepad.exe -lwinmm -lgdi32
```

MSVC: `cl /O2 bad_apple_notepad.c /link winmm.lib gdi32.lib` (or run `build.bat`).

## Run

```bat
bad_apple_notepad.exe
bad_apple_notepad.exe D:\videos\bad_apple.mp4
```

Needs `ffmpeg` on `PATH` and a `bad_apple.mp4` you supply (not included).
Close Notepad or Ctrl+C to stop.

MIT
