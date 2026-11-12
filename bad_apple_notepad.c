/*
 * bad_apple_notepad.c
 * ---------------------------------------------------------------------------
 * Plays the Bad Apple!! video inside a real Windows Notepad window, in sync
 * with its audio track.
 *
 *   video : ffmpeg  ->  _popen()      -> raw 8-bit gray, 480x360, 30 fps
 *   text  : box-downsampled to the current window size -> ASCII ramp
 *           -> WM_SETTEXT into Notepad's text control
 *   audio : ffmpeg  ->  bad_apple.wav -> PlaySoundA(SND_FILENAME | SND_ASYNC)
 *   clock : QueryPerformanceCounter, absolute schedule, late frames dropped
 *
 * The character grid is recomputed from Notepad's live client area, so you
 * can resize / maximize the window (or change the font size) while it plays:
 * the pipe keeps streaming at 480x360, only the ASCII conversion changes.
 * The picture keeps its 4:3 shape at every window size; spare space becomes
 * blank margins instead of stretching the video.
 *
 * Build (MinGW / gcc):
 *   gcc -O2 -Wall -Wextra bad_apple_notepad.c -o bad_apple_notepad.exe -lwinmm -lgdi32
 *
 * Build (MSVC / cl):
 *   cl /O2 /W3 bad_apple_notepad.c /link winmm.lib gdi32.lib
 *   (user32.lib / gdi32.lib are linked by default, but naming them is harmless)
 *
 * Run:
 *   bad_apple_notepad.exe
 *   bad_apple_notepad.exe D:\videos\bad_apple.mp4     (optional path)
 *   ffmpeg.exe must be reachable from the current directory / PATH.
 *   The .wav cache is written next to the video file.
 *
 * Stop:
 *   Ctrl+C in the console, or close the Notepad window.
 *
 * Notepad setup:
 *   Font: Consolas (any monospaced font), size 10-11, Word Wrap OFF, Zoom 100%.
 *   Any window shape works: the picture is centred and keeps its proportions.
 * ---------------------------------------------------------------------------
 */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <mmsystem.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdi32.lib")
#endif

#define VIDEO_FILE  "bad_apple.mp4"

/* Fixed size of the ffmpeg pipe.  Big enough for very large windows, small
   enough that decoding + downsampling is free.  4:3, like the source. */
#define PIPE_W 480
#define PIPE_H 360
#define FRAME_BYTES (PIPE_W * PIPE_H)

#define FPS 30

/* Safety caps so a 4K maximized window cannot make WM_SETTEXT crawl. */
#define MAX_COLS 400
#define MAX_ROWS 200
#define MIN_COLS 4
#define MIN_ROWS 2

#define TEXT_BYTES (MAX_ROWS * (MAX_COLS + 2) + 1)

static const char kRamp[] = " .:-=+*#%@";       /* dark -> bright, 10 levels   */

static volatile LONG g_stop = 0;

typedef struct {
    int cols, rows;         /* size of the picture, in characters          */
    int pad_x, pad_y;       /* centring offsets inside the visible area    */
    int cell_w, cell_h;     /* font cell size in pixels                    */
    int area_w, area_h;     /* edit-control window size the grid came from */
} LAYOUT;

/* ------------------------------------------------------------------------- */
/* Ctrl+C / console-close handler                                            */
/* ------------------------------------------------------------------------- */
static BOOL WINAPI on_ctrl_handler(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_stop = 1;
        return TRUE;            /* handled: give the main loop time to clean up */
    default:
        return FALSE;
    }
}

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */
static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n = -1;

    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) == 0)
        n = ftell(f);
    fclose(f);
    return n;
}

/* Font cell dimensions of the edit control. */
static int measure_cell(HWND edit, int *cell_w, int *cell_h)
{
    HDC dc;
    HGDIOBJ old_font;
    HFONT font;
    TEXTMETRICA tm;
    SIZE sz;
    static const char probe[] = "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM";  /* 32 glyphs */

    dc = GetDC(edit);
    if (!dc)
        return 0;

    font = (HFONT)SendMessageA(edit, WM_GETFONT, 0, 0);
    if (!font)
        font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    old_font = SelectObject(dc, font);

    ZeroMemory(&tm, sizeof tm);
    if (!GetTextMetricsA(dc, &tm) || tm.tmHeight <= 0) {
        SelectObject(dc, old_font);
        ReleaseDC(edit, dc);
        return 0;
    }

    *cell_h = tm.tmHeight;

    /*
     * tm.tmAveCharWidth is the *average* glyph width, not the advance of a
     * monospaced cell - trusting it makes the grid wider than the window and
     * Notepad clips the right-hand side (looks like broken scaling).
     * Measure the real advance of 32 identical glyphs instead.
     */
    if (GetTextExtentPoint32A(dc, probe, (int)sizeof probe - 1, &sz) &&
        sz.cx > 0) {
        *cell_w = (sz.cx + ((int)sizeof probe - 1) / 2) / ((int)sizeof probe - 1);
    } else if (tm.tmMaxCharWidth > 0) {
        *cell_w = tm.tmMaxCharWidth;
    } else {
        *cell_w = tm.tmAveCharWidth > 0 ? tm.tmAveCharWidth : 8;
    }

    if (*cell_w <= 0)
        *cell_w = 8;

    SelectObject(dc, old_font);
    ReleaseDC(edit, dc);
    return 1;
}

/*
 * Recompute the character grid from the edit control's current client area.
 * Returns 1 when the grid changed (caller must re-render), 0 otherwise.
 * "force" re-reads the font metrics (cheap, used to catch Ctrl+wheel zoom).
 */
static int update_layout(HWND edit, LAYOUT *L, int force)
{
    RECT rc;
    int cw, ch, area_w, area_h;
    int max_cols, max_rows, cols, rows, pad_x, pad_y;
    int avail_w, avail_h, pic_w, pic_h;

    /*
     * Use the *window* rect, never the client rect.  Notepad's RichEdit puts
     * its scrollbars in the non-client area, so GetClientRect() shrinks when
     * a scrollbar appears, which changes the grid, which changes the text
     * size, which toggles the scrollbar... an endless flicker loop.
     * GetWindowRect() is stable, so the grid only changes when the user
     * resizes the window or changes the font.
     */
    if (!GetWindowRect(edit, &rc))
        return 0;

    area_w = rc.right - rc.left;
    area_h = rc.bottom - rc.top;

    if (!force && area_w == L->area_w && area_h == L->area_h)
        return 0;
    if (!measure_cell(edit, &cw, &ch))
        return 0;

    /* Characters that fit, with room for borders, padding and scrollbars. */
    max_cols = (area_w - 32) / cw;
    max_rows = (area_h - 32) / ch;
    if (max_cols > MAX_COLS)
        max_cols = MAX_COLS;
    if (max_rows > MAX_ROWS)
        max_rows = MAX_ROWS;
    if (max_cols < MIN_COLS)
        max_cols = MIN_COLS;
    if (max_rows < MIN_ROWS)
        max_rows = MIN_ROWS;

    /*
     * Fit the video's aspect ratio inside that area instead of stretching to
     * fill it.  A wide Notepad window would otherwise squash the picture
     * horizontally (or a tall one would stretch it).  The spare room becomes
     * blank columns/rows, so the image stays centred and undistorted.
     */
    avail_w = max_cols * cw;
    avail_h = max_rows * ch;

    if ((long long)avail_w * PIPE_H > (long long)avail_h * PIPE_W) {
        /* window is wider than the video: height is the limit */
        pic_h = avail_h;
        pic_w = avail_h * PIPE_W / PIPE_H;
    } else {
        /* window is taller than the video: width is the limit */
        pic_w = avail_w;
        pic_h = avail_w * PIPE_H / PIPE_W;
    }

    cols = pic_w / cw;
    rows = pic_h / ch;
    if (cols < MIN_COLS)
        cols = MIN_COLS;
    if (rows < MIN_ROWS)
        rows = MIN_ROWS;
    if (cols > max_cols)
        cols = max_cols;
    if (rows > max_rows)
        rows = max_rows;

    pad_x = (max_cols - cols) / 2;
    pad_y = (max_rows - rows) / 2;

    if (cols == L->cols && rows == L->rows &&
        pad_x == L->pad_x && pad_y == L->pad_y &&
        cw == L->cell_w && ch == L->cell_h) {
        L->area_w = area_w;             /* size wobble, same grid */
        L->area_h = area_h;
        return 0;
    }

    L->cols = cols;
    L->rows = rows;
    L->pad_x = pad_x;
    L->pad_y = pad_y;
    L->cell_w = cw;
    L->cell_h = ch;
    L->area_w = area_w;
    L->area_h = area_h;
    return 1;
}

/*
 * Box-filter the 480x360 gray frame down (or up) to cols x rows characters.
 * Every character is the average brightness of the source pixels it covers.
 * The picture is padded with blank space so it sits centred (and square) in
 * whatever window the user has opened.
 */
static void frame_to_ascii(const unsigned char *frame, const LAYOUT *L,
                           char *out)
{
    char *dst = out;
    int cols = L->cols;
    int rows = L->rows;
    int gx, gy, i;

    for (i = 0; i < L->pad_y; ++i) {
        *dst++ = '\r';
        *dst++ = '\n';
    }

    for (gy = 0; gy < rows; ++gy) {
        int y0 = (gy * PIPE_H) / rows;
        int y1 = ((gy + 1) * PIPE_H) / rows;
        int y, x;

        if (y1 <= y0)
            y1 = y0 + 1;

        for (i = 0; i < L->pad_x; ++i)
            *dst++ = ' ';

        for (gx = 0; gx < cols; ++gx) {
            int x0 = (gx * PIPE_W) / cols;
            int x1 = ((gx + 1) * PIPE_W) / cols;
            unsigned sum = 0;
            unsigned n;
            unsigned v;

            if (x1 <= x0)
                x1 = x0 + 1;

            for (y = y0; y < y1; ++y) {
                const unsigned char *src = frame + (size_t)y * PIPE_W + x0;
                for (x = x0; x < x1; ++x)
                    sum += *src++;
            }
            n = (unsigned)((x1 - x0) * (y1 - y0));
            v = sum / n;                       /* 0..255 */
            *dst++ = kRamp[(v * 10) >> 8];     /* 0..9   */
        }

        for (i = 0; i < L->pad_x; ++i)
            *dst++ = ' ';

        *dst++ = '\r';
        *dst++ = '\n';
    }

    for (i = 0; i < L->pad_y; ++i) {
        *dst++ = '\r';
        *dst++ = '\n';
    }

    *dst = '\0';
}

/* "C:\clips\bad_apple.mp4" -> "C:\clips\bad_apple.wav" */
static void make_wav_path(const char *video, char *out, int cap)
{
    int i, n = 0, start = 0, dot = -1, base;

    for (i = 0; video[i] != '\0' && n < cap - 5; ++i) {
        if (video[i] == '\\' || video[i] == '/') {
            start = n + 1;
            dot = -1;
        } else if (video[i] == '.') {
            dot = n;
        }
        out[n++] = video[i];
    }

    base = (dot >= start) ? dot : n;
    out[base + 0] = '.';
    out[base + 1] = 'w';
    out[base + 2] = 'a';
    out[base + 3] = 'v';
    out[base + 4] = '\0';
}

/* ------------------------------------------------------------------------- */
/* top-level window lookup                                                   */
/* ------------------------------------------------------------------------- */
typedef struct {
    DWORD pid;
    HWND  found;        /* class "Notepad" (preferred)                        */
    HWND  fallback;     /* any other visible, unowned top-level window        */
} TOP_SEARCH;

/* Snapshot of the Notepad windows that already exist, taken before launch. */
typedef struct {
    HWND wins[32];
    int  count;
} SNAP;

typedef struct {
    const SNAP *before;
    HWND found;
} NEW_SEARCH;

static BOOL CALLBACK enum_top(HWND hwnd, LPARAM lp)
{
    TOP_SEARCH *s = (TOP_SEARCH *)lp;
    char cls[64];
    DWORD pid = 0;
    RECT rc;
    DWORD style;

    if (!IsWindowVisible(hwnd))
        return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != NULL)
        return TRUE;

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != s->pid)
        return TRUE;

    GetClassNameA(hwnd, cls, (int)sizeof cls);
    if (lstrcmpiA(cls, "Notepad") == 0) {
        s->found = hwnd;
        return FALSE;                       /* stop enumerating */
    }

    /* Remember a plausible candidate in case the frame class changes. */
    style = (DWORD)GetWindowLongA(hwnd, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION && GetWindowRect(hwnd, &rc) &&
        (rc.right - rc.left) > 200 && (rc.bottom - rc.top) > 150) {
        if (s->fallback == NULL)
            s->fallback = hwnd;
    }
    return TRUE;
}

static BOOL CALLBACK enum_snapshot(HWND hwnd, LPARAM lp)
{
    SNAP *s = (SNAP *)lp;
    char cls[64];

    if (!IsWindowVisible(hwnd))
        return TRUE;
    GetClassNameA(hwnd, cls, (int)sizeof cls);
    if (lstrcmpiA(cls, "Notepad") == 0 && s->count < 32)
        s->wins[s->count++] = hwnd;
    return TRUE;
}

static BOOL CALLBACK enum_new_notepad(HWND hwnd, LPARAM lp)
{
    NEW_SEARCH *s = (NEW_SEARCH *)lp;
    char cls[64];
    int i;

    if (!IsWindowVisible(hwnd))
        return TRUE;
    GetClassNameA(hwnd, cls, (int)sizeof cls);
    if (lstrcmpiA(cls, "Notepad") != 0)
        return TRUE;

    for (i = 0; i < s->before->count; ++i)
        if (s->before->wins[i] == hwnd)
            return TRUE;                /* already there before we launched */

    s->found = hwnd;
    return FALSE;
}

static HWND find_notepad_window(DWORD pid, const SNAP *before, int timeout_ms)
{
    HWND fallback = NULL;
    int waited;

    for (waited = 0; waited < timeout_ms; waited += 50) {
        TOP_SEARCH s;
        NEW_SEARCH n;

        s.pid = pid;
        s.found = NULL;
        s.fallback = NULL;
        EnumWindows(enum_top, (LPARAM)&s);
        if (s.found)
            return s.found;
        if (s.fallback && !fallback)
            fallback = s.fallback;

        /* Windows 11 Notepad relaunches itself, so the window usually belongs
           to a different process than the one we spawned.  Accept any Notepad
           window that was not on screen before we started. */
        n.before = before;
        n.found = NULL;
        EnumWindows(enum_new_notepad, (LPARAM)&n);
        if (n.found)
            return n.found;

        Sleep(50);
    }

    if (fallback)
        return fallback;

    /* Tabbed Notepad: our launch may have just added a tab to an existing
       window.  Frontmost (first enumerated) wins. */
    if (before->count > 0)
        return before->wins[0];

    return NULL;
}

/* ------------------------------------------------------------------------- */
/* text control lookup: classic "Edit" or Win11 "RichEditD2DPT"              */
/* ------------------------------------------------------------------------- */
typedef struct {
    HWND found;
    LONG best_area;
    int  rich_only;
} EDIT_SEARCH;

static BOOL CALLBACK enum_edit(HWND hwnd, LPARAM lp)
{
    EDIT_SEARCH *s = (EDIT_SEARCH *)lp;
    char cls[128];
    RECT rc;
    LONG area;

    GetClassNameA(hwnd, cls, (int)sizeof cls);

    if (s->rich_only) {
        if (lstrcmpiA(cls, "RichEditD2DPT") != 0 &&
            lstrcmpiA(cls, "RichEdit20W")   != 0 &&
            lstrcmpiA(cls, "RICHEDIT50W")   != 0)
            return TRUE;
    } else {
        if (lstrcmpiA(cls, "Edit") != 0)
            return TRUE;
    }

    if (!GetClientRect(hwnd, &rc))
        return TRUE;

    /* Prefer the biggest control of the wanted class (skips search boxes). */
    area = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (area >= s->best_area) {
        s->best_area = area;
        s->found = hwnd;
    }
    return TRUE;
}

static HWND find_edit_control(HWND note, int timeout_ms)
{
    int waited;

    for (waited = 0; waited < timeout_ms; waited += 50) {
        EDIT_SEARCH s;

        /* Pass 1: modern Notepad (RichEdit inside NotepadTextBox). */
        s.found = NULL;
        s.best_area = -1;
        s.rich_only = 1;
        EnumChildWindows(note, enum_edit, (LPARAM)&s);
        if (s.found)
            return s.found;

        /* Pass 2: classic Notepad (plain multiline Edit). */
        s.found = NULL;
        s.best_area = -1;
        s.rich_only = 0;
        EnumChildWindows(note, enum_edit, (LPARAM)&s);
        if (s.found)
            return s.found;

        Sleep(50);
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* give the window a sane starting size (any size after that is fine)        */
/* ------------------------------------------------------------------------- */
static void size_notepad_window(HWND note, HWND edit)
{
    RECT work, note_client, edit_rect, win_rc;
    POINT pts[2];
    DWORD style, ex_style;
    int chrome_w, chrome_h, want_w, want_h, win_w, win_h;

    if (!note || !edit)
        return;
    if (IsIconic(note) || IsZoomed(note))
        return;

    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
        work.left = 0;
        work.top = 0;
        work.right = GetSystemMetrics(SM_CXSCREEN);
        work.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    /* How much room does Notepad spend on tab bar / status bar / borders? */
    GetClientRect(note, &note_client);
    GetWindowRect(edit, &edit_rect);
    pts[0].x = edit_rect.left;
    pts[0].y = edit_rect.top;
    pts[1].x = edit_rect.right;
    pts[1].y = edit_rect.bottom;
    MapWindowPoints(HWND_DESKTOP, note, pts, 2);

    chrome_w = note_client.right - (pts[1].x - pts[0].x);
    chrome_h = note_client.bottom - (pts[1].y - pts[0].y);
    if (chrome_w < 0)
        chrome_w = 0;
    if (chrome_h < 0)
        chrome_h = 0;

    want_w = (work.right - work.left) * 70 / 100 + chrome_w;
    want_h = (work.bottom - work.top) * 70 / 100 + chrome_h;

    win_rc.left = 0;
    win_rc.top = 0;
    win_rc.right = want_w;
    win_rc.bottom = want_h;
    style = (DWORD)GetWindowLongA(note, GWL_STYLE);
    ex_style = (DWORD)GetWindowLongA(note, GWL_EXSTYLE);
    AdjustWindowRectEx(&win_rc, style, FALSE, ex_style);

    win_w = win_rc.right - win_rc.left;
    win_h = win_rc.bottom - win_rc.top;
    if (win_w > work.right - work.left)
        win_w = work.right - work.left;
    if (win_h > work.bottom - work.top)
        win_h = work.bottom - work.top;

    SetWindowPos(note, NULL,
                 work.left + ((work.right - work.left) - win_w) / 2,
                 work.top + ((work.bottom - work.top) - win_h) / 2,
                 win_w, win_h, SWP_NOZORDER | SWP_NOACTIVATE);
}

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    const char *video;
    char audio[1024];
    FILE *pipe = NULL;
    HWND note = NULL;
    HWND edit = NULL;
    unsigned char *frame = NULL;
    char *text = NULL;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    LARGE_INTEGER freq, t0, now;
    LAYOUT layout;
    SNAP existing;
    double ticks_per_frame;
    long long decoded = 0;
    long long tick = 0;
    int exit_code = 1;
    int layout_changed;
    char cmd[2048];
    int i;

    ZeroMemory(&si, sizeof si);
    ZeroMemory(&pi, sizeof pi);
    ZeroMemory(&layout, sizeof layout);

    video = (argc > 1) ? argv[1] : VIDEO_FILE;
    make_wav_path(video, audio, (int)sizeof audio);

    SetConsoleCtrlHandler(on_ctrl_handler, TRUE);
    SetProcessDPIAware();               /* keep our pixel math honest */

    printf("Bad Apple!! -> Notepad  (window-sized grid, %d fps)\n", FPS);
    printf("Video: %s\n", video);

    if (GetFileAttributesA(video) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "error: \"%s\" not found.\n", video);
        fprintf(stderr, "       Put bad_apple.mp4 next to this program, or\n");
        fprintf(stderr, "       pass its full path:  bad_apple_notepad.exe D:\\bad_apple.mp4\n");
        goto cleanup;
    }

    /* ---- 1. audio: extract once, then cache ---------------------------- */
    if (file_size(audio) < 1024) {
        printf("[1/4] Extracting audio track -> %s ...\n", audio);
        snprintf(cmd, sizeof cmd,
                 "ffmpeg -hide_banner -loglevel error -nostdin -y -i \"%s\" "
                 "-vn -acodec pcm_s16le -ar 44100 -ac 2 -f wav \"%s\"",
                 video, audio);
        if (system(cmd) != 0 || file_size(audio) < 1024) {
            fprintf(stderr, "error: ffmpeg could not create %s.\n", audio);
            fprintf(stderr, "       command: %s\n", cmd);
            goto cleanup;
        }
    } else {
        printf("[1/4] Reusing cached %s\n", audio);
    }

    /* ---- 2. launch a fresh Notepad ------------------------------------- */
    existing.count = 0;
    EnumWindows(enum_snapshot, (LPARAM)&existing);

    si.cb = sizeof si;
    {
        char exe[] = "notepad.exe";     /* CreateProcess may modify the buffer */
        if (!CreateProcessA(NULL, exe, NULL, NULL, FALSE, 0, NULL, NULL,
                            &si, &pi)) {
            fprintf(stderr, "error: could not start notepad.exe (%lu).\n",
                    (unsigned long)GetLastError());
            goto cleanup;
        }
    }
    printf("[2/4] Started notepad.exe (pid %lu), waiting for its window...\n",
           (unsigned long)pi.dwProcessId);

    WaitForInputIdle(pi.hProcess, 10000);

    note = find_notepad_window(pi.dwProcessId, &existing, 4000);
    if (!note) {
        fprintf(stderr, "error: could not find Notepad's top-level window.\n");
        goto cleanup;
    }

    edit = find_edit_control(note, 5000);
    if (!edit) {
        fprintf(stderr, "error: could not find Notepad's text control.\n");
        goto cleanup;
    }

    ShowWindow(note, SW_SHOWNORMAL);
    SetForegroundWindow(note);
    Sleep(120);                          /* let Notepad finish its first layout */
    size_notepad_window(note, edit);
    Sleep(120);
    update_layout(edit, &layout, 1);

    printf("[*] Grid: %d x %d characters (margins %d x %d)\n",
           layout.cols, layout.rows, layout.pad_x, layout.pad_y);

    /* ---- 3. open the raw video pipe ------------------------------------ */
    printf("[3/4] Starting ffmpeg video pipe...\n");
    snprintf(cmd, sizeof cmd,
             "ffmpeg -hide_banner -loglevel error -nostdin -i \"%s\" -an "
             "-vf \"scale=%d:%d:force_original_aspect_ratio=decrease,"
             "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black\" -r %d -pix_fmt gray "
             "-f rawvideo - 2>nul",
             video, PIPE_W, PIPE_H, PIPE_W, PIPE_H, FPS);

    pipe = _popen(cmd, "rb");
    if (!pipe) {
        fprintf(stderr, "error: _popen() failed.\n");
        goto cleanup;
    }
    setvbuf(pipe, NULL, _IONBF, 0);      /* read exactly what ffmpeg wrote */

    frame = (unsigned char *)malloc(FRAME_BYTES);
    text  = (char *)malloc(TEXT_BYTES);
    if (!frame || !text) {
        fprintf(stderr, "error: out of memory.\n");
        goto cleanup;
    }

    /* Prime the pipe so the first frame is already decoded when we start. */
    if (fread(frame, 1, FRAME_BYTES, pipe) != FRAME_BYTES) {
        fprintf(stderr, "error: ffmpeg produced no video frames.\n");
        fprintf(stderr, "       Is ffmpeg.exe installed and on PATH?\n");
        goto cleanup;
    }
    decoded = 1;

    /* ---- 4. play: audio start == clock zero ---------------------------- */
    timeBeginPeriod(1);                  /* 1 ms timer resolution for Sleep() */
    QueryPerformanceFrequency(&freq);
    ticks_per_frame = (double)freq.QuadPart / (double)FPS;

    printf("[4/4] Playing. Resize Notepad freely. Ctrl+C or close it to stop.\n");

    if (!PlaySoundA(audio, NULL,
                    SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) {
        fprintf(stderr, "warning: PlaySoundA could not play %s.\n", audio);
    }

    QueryPerformanceCounter(&t0);

    /* Frame 0 is due right now. */
    frame_to_ascii(frame, &layout, text);
    SendMessageA(edit, WM_SETTEXT, 0, (LPARAM)text);

    while (!g_stop) {
        long long target;
        double elapsed, wait_ms;

        if (!IsWindow(note)) {
            printf("[*] Notepad window closed - stopping.\n");
            break;
        }
        if (!IsWindow(edit)) {
            /* A new tab can replace the control; re-acquire it. */
            edit = find_edit_control(note, 1000);
            if (!edit) {
                printf("[*] Notepad text control lost - stopping.\n");
                break;
            }
            update_layout(edit, &layout, 1);
        }

        QueryPerformanceCounter(&now);
        elapsed = (double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
        target = (long long)(elapsed * (double)FPS);   /* frame due on screen */

        if (target < decoded) {
            /* Ahead of schedule: wait until the frame in the buffer is due. */
            LONGLONG due = t0.QuadPart +
                           (LONGLONG)((double)decoded * ticks_per_frame);
            wait_ms = (double)(due - now.QuadPart) * 1000.0 /
                      (double)freq.QuadPart;
            if (wait_ms > 10.0)
                wait_ms = 10.0;          /* re-check often enough for Ctrl+C  */
            if (wait_ms < 1.0)
                wait_ms = 1.0;
            Sleep((DWORD)wait_ms);
            continue;
        }

        /* Behind schedule: pull frames until we hold the one that is due,
           silently dropping everything we missed. */
        while (decoded <= target) {
            if (fread(frame, 1, FRAME_BYTES, pipe) != FRAME_BYTES) {
                printf("[*] End of video stream (%lld frames).\n", decoded);
                goto playback_done;
            }
            ++decoded;
        }

        /* Follow window size / font size (checked twice per second). */
        layout_changed = update_layout(edit, &layout, (tick % 15) == 0);
        if (layout_changed)
            printf("[*] Grid now %d x %d characters (margins %d x %d)\n",
                   layout.cols, layout.rows, layout.pad_x, layout.pad_y);

        if (!IsIconic(note)) {
            frame_to_ascii(frame, &layout, text);
            SendMessageA(edit, WM_SETTEXT, 0, (LPARAM)text);
        }
        ++tick;
    }

    /* Small grace period so the final notes are not cut off. */
    for (i = 0; i < 20; ++i) {
        if (g_stop || !IsWindow(note))
            break;
        Sleep(10);
    }

playback_done:
    exit_code = 0;

cleanup:
    if (pipe) {
        _pclose(pipe);
        pipe = NULL;
    }
    PlaySoundA(NULL, NULL, 0);           /* stop and release the audio device */
    free(frame);
    free(text);
    if (pi.hThread)
        CloseHandle(pi.hThread);
    if (pi.hProcess)
        CloseHandle(pi.hProcess);
    timeEndPeriod(1);
    return exit_code;
}
