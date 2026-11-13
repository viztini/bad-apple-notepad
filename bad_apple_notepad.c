// gcc -O2 -Wall -Wextra bad_apple_notepad.c -o bad_apple_notepad.exe -lwinmm -lgdi32
// usage: bad_apple_notepad.exe [video]   (needs ffmpeg on PATH)

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdi32.lib")
#endif

#define VIDEO_FILE "bad_apple.mp4"
#define PIPE_W 480
#define PIPE_H 360
#define FRAME_BYTES (PIPE_W * PIPE_H)
#define FPS 30
#define WAV_BYTES_PER_SEC (44100 * 2 * 2)

#define MAX_COLS 400
#define MAX_ROWS 200
#define MIN_COLS 4
#define MIN_ROWS 2
#define TEXT_BYTES (MAX_ROWS * (MAX_COLS + 2) + 1)

#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

typedef char caps_fit_pipe[(MAX_COLS <= PIPE_W && MAX_ROWS <= PIPE_H) ? 1 : -1];

static const char kRamp[] = " .:-=+*#%@";
static volatile LONG g_stop = 0;

typedef struct {
    int cols, rows;
    int pad_x, pad_y;
    int cell_w, cell_h;
    int area_w, area_h;
} LAYOUT;

static BOOL WINAPI on_ctrl(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_stop = 1;
        return TRUE;
    }
    return FALSE;
}

static int make_wav_path(const char *video, char *out, size_t cap)
{
    const char *sep = strrchr(video, '\\');
    const char *fwd = strrchr(video, '/');
    const char *dot;
    size_t base;

    if (!sep || (fwd && fwd > sep))
        sep = fwd;
    dot = strrchr(sep ? sep : video, '.');
    base = dot ? (size_t)(dot - video) : strlen(video);
    if (base + 5 > cap)
        return 0;
    memcpy(out, video, base);
    strcpy(out + base, ".wav");
    return 1;
}

static int wav_stale(const char *video, const char *wav)
{
    struct _stat v, w;

    if (_stat(wav, &w) != 0 || w.st_size < 1024)
        return 1;
    return _stat(video, &v) == 0 && v.st_mtime > w.st_mtime;
}

static int measure_cell(HWND edit, int *cw, int *ch)
{
    static const char probe[] = "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM";
    const int n = (int)sizeof probe - 1;
    HDC dc = GetDC(edit);
    HFONT font;
    HGDIOBJ old;
    TEXTMETRICA tm;
    SIZE sz;

    if (!dc)
        return 0;
    font = (HFONT)SendMessageA(edit, WM_GETFONT, 0, 0);
    if (!font)
        font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    old = SelectObject(dc, font);

    ZeroMemory(&tm, sizeof tm);
    if (!GetTextMetricsA(dc, &tm) || tm.tmHeight <= 0) {
        SelectObject(dc, old);
        ReleaseDC(edit, dc);
        return 0;
    }
    *ch = tm.tmHeight;

    if (GetTextExtentPoint32A(dc, probe, n, &sz) && sz.cx > 0)
        *cw = (sz.cx + n / 2) / n;
    else
        *cw = tm.tmAveCharWidth > 0 ? tm.tmAveCharWidth : 8;
    if (*cw <= 0)
        *cw = 8;

    SelectObject(dc, old);
    ReleaseDC(edit, dc);
    return 1;
}

static int update_layout(HWND edit, LAYOUT *L, int force)
{
    RECT rc;
    int cw, ch, area_w, area_h;
    int max_cols, max_rows, avail_w, avail_h, pic_w, pic_h;
    int cols, rows, pad_x, pad_y, changed;

    if (!GetWindowRect(edit, &rc))
        return 0;
    area_w = rc.right - rc.left;
    area_h = rc.bottom - rc.top;
    if (!force && area_w == L->area_w && area_h == L->area_h)
        return 0;
    if (!measure_cell(edit, &cw, &ch))
        return 0;

    max_cols = CLAMP((area_w - 32) / cw, MIN_COLS, MAX_COLS);
    max_rows = CLAMP((area_h - 32) / ch, MIN_ROWS, MAX_ROWS);

    avail_w = max_cols * cw;
    avail_h = max_rows * ch;
    if ((long long)avail_w * PIPE_H > (long long)avail_h * PIPE_W) {
        pic_h = avail_h;
        pic_w = avail_h * PIPE_W / PIPE_H;
    } else {
        pic_w = avail_w;
        pic_h = avail_w * PIPE_H / PIPE_W;
    }
    cols = CLAMP(pic_w / cw, MIN_COLS, max_cols);
    rows = CLAMP(pic_h / ch, MIN_ROWS, max_rows);
    pad_x = (max_cols - cols) / 2;
    pad_y = (max_rows - rows) / 2;

    changed = cols != L->cols || rows != L->rows ||
              pad_x != L->pad_x || pad_y != L->pad_y ||
              cw != L->cell_w || ch != L->cell_h;
    L->cols = cols;
    L->rows = rows;
    L->pad_x = pad_x;
    L->pad_y = pad_y;
    L->cell_w = cw;
    L->cell_h = ch;
    L->area_w = area_w;
    L->area_h = area_h;
    return changed;
}

static void frame_to_ascii(const unsigned char *frame, const LAYOUT *L, char *out)
{
    int xb[MAX_COLS + 1];
    char *p = out;
    int gx, gy, i;

    for (gx = 0; gx <= L->cols; ++gx)
        xb[gx] = gx * PIPE_W / L->cols;

    for (i = 0; i < L->pad_y; ++i) {
        *p++ = '\r';
        *p++ = '\n';
    }
    for (gy = 0; gy < L->rows; ++gy) {
        int y0 = gy * PIPE_H / L->rows;
        int y1 = (gy + 1) * PIPE_H / L->rows;

        memset(p, ' ', (size_t)L->pad_x);
        p += L->pad_x;
        for (gx = 0; gx < L->cols; ++gx) {
            unsigned sum = 0;
            int x, y;

            for (y = y0; y < y1; ++y) {
                const unsigned char *src = frame + (size_t)y * PIPE_W;
                for (x = xb[gx]; x < xb[gx + 1]; ++x)
                    sum += src[x];
            }
            sum /= (unsigned)((xb[gx + 1] - xb[gx]) * (y1 - y0));
            *p++ = kRamp[(sum * 10) >> 8];
        }
        if (gy < L->rows - 1) {
            *p++ = '\r';
            *p++ = '\n';
        }
    }
    *p = '\0';
}

static void send_text(HWND edit, const char *text)
{
    DWORD_PTR res;
    SendMessageTimeoutA(edit, WM_SETTEXT, 0, (LPARAM)text,
                        SMTO_ABORTIFHUNG, 250, &res);
}

typedef struct { HWND list[32]; int n; } SNAP;
typedef struct { DWORD pid; const SNAP *old; HWND hit; } FIND;
typedef struct { HWND hit; int rank; LONG area; } EDITSEARCH;

static BOOL is_notepad(HWND h)
{
    char cls[32];

    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER))
        return FALSE;
    GetClassNameA(h, cls, (int)sizeof cls);
    return lstrcmpiA(cls, "Notepad") == 0;
}

static BOOL CALLBACK enum_snap(HWND h, LPARAM lp)
{
    SNAP *s = (SNAP *)lp;

    if (is_notepad(h) && s->n < 32)
        s->list[s->n++] = h;
    return TRUE;
}

static BOOL CALLBACK enum_find(HWND h, LPARAM lp)
{
    FIND *f = (FIND *)lp;
    DWORD pid = 0;
    int i;

    if (!is_notepad(h))
        return TRUE;
    GetWindowThreadProcessId(h, &pid);
    for (i = 0; i < f->old->n; ++i)
        if (f->old->list[i] == h && pid != f->pid)
            return TRUE;
    f->hit = h;
    return FALSE;
}

static HWND find_notepad(DWORD pid, const SNAP *old, int timeout_ms)
{
    FIND f;
    int t;

    f.pid = pid;
    f.old = old;
    for (t = 0; t < timeout_ms; t += 50) {
        f.hit = NULL;
        EnumWindows(enum_find, (LPARAM)&f);
        if (f.hit)
            return f.hit;
        Sleep(50);
    }
    return old->n ? old->list[0] : NULL;
}

static BOOL CALLBACK enum_edit(HWND h, LPARAM lp)
{
    EDITSEARCH *s = (EDITSEARCH *)lp;
    char cls[64];
    RECT rc;
    LONG area;
    int rank;

    GetClassNameA(h, cls, (int)sizeof cls);
    if (!lstrcmpiA(cls, "RichEditD2DPT") || !lstrcmpiA(cls, "RichEdit20W") ||
        !lstrcmpiA(cls, "RICHEDIT50W"))
        rank = 2;
    else if (!lstrcmpiA(cls, "Edit"))
        rank = 1;
    else
        return TRUE;
    if (!GetClientRect(h, &rc))
        return TRUE;

    area = rc.right * rc.bottom;
    if (rank > s->rank || (rank == s->rank && area >= s->area)) {
        s->hit = h;
        s->rank = rank;
        s->area = area;
    }
    return TRUE;
}

static HWND find_edit(HWND note, int timeout_ms)
{
    EDITSEARCH s;
    int t;

    for (t = 0; t < timeout_ms; t += 50) {
        s.hit = NULL;
        s.rank = 0;
        s.area = -1;
        EnumChildWindows(note, enum_edit, (LPARAM)&s);
        if (s.hit)
            return s.hit;
        Sleep(50);
    }
    return NULL;
}

static void size_notepad(HWND note, HWND edit)
{
    RECT work, nc, er, wr;
    int chrome_w, chrome_h, ww, wh, w, h;

    if (IsIconic(note) || IsZoomed(note))
        return;
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0))
        SetRect(&work, 0, 0, GetSystemMetrics(SM_CXSCREEN),
                GetSystemMetrics(SM_CYSCREEN));

    GetClientRect(note, &nc);
    GetWindowRect(edit, &er);
    chrome_w = nc.right - (er.right - er.left);
    chrome_h = nc.bottom - (er.bottom - er.top);
    if (chrome_w < 0)
        chrome_w = 0;
    if (chrome_h < 0)
        chrome_h = 0;

    ww = work.right - work.left;
    wh = work.bottom - work.top;
    SetRect(&wr, 0, 0, ww * 70 / 100 + chrome_w, wh * 70 / 100 + chrome_h);
    AdjustWindowRectEx(&wr, (DWORD)GetWindowLongA(note, GWL_STYLE), FALSE,
                       (DWORD)GetWindowLongA(note, GWL_EXSTYLE));
    w = wr.right - wr.left;
    h = wr.bottom - wr.top;
    if (w > ww)
        w = ww;
    if (h > wh)
        h = wh;
    SetWindowPos(note, NULL, work.left + (ww - w) / 2, work.top + (wh - h) / 2,
                 w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

int main(int argc, char **argv)
{
    const char *video = (argc > 1) ? argv[1] : VIDEO_FILE;
    char audio[1024], cmd[2048];
    FILE *pipe = NULL;
    HWND note = NULL, edit = NULL;
    unsigned char *frame = NULL;
    char *text = NULL;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    LARGE_INTEGER freq, t0, now;
    LAYOUT layout;
    SNAP existing;
    struct _stat wst;
    long long decoded = 0, tick = 0;
    double elapsed = 0.0, audio_secs = 0.0;
    int exit_code = 1, timer_set = 0, n;
    char notepad_exe[] = "notepad.exe";

    ZeroMemory(&si, sizeof si);
    ZeroMemory(&pi, sizeof pi);
    ZeroMemory(&layout, sizeof layout);

    SetConsoleCtrlHandler(on_ctrl, TRUE);
    SetProcessDPIAware();

    if (strchr(video, '"') || GetFileAttributesA(video) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "error: video not found (or bad path): %s\n", video);
        goto cleanup;
    }
    if (!make_wav_path(video, audio, sizeof audio)) {
        fprintf(stderr, "error: video path too long.\n");
        goto cleanup;
    }

    if (wav_stale(video, audio)) {
        printf("Extracting audio -> %s\n", audio);
        n = snprintf(cmd, sizeof cmd,
                     "ffmpeg -hide_banner -loglevel error -nostdin -y -i \"%s\" "
                     "-vn -acodec pcm_s16le -ar 44100 -ac 2 -f wav \"%s\"",
                     video, audio);
        if (n < 0 || n >= (int)sizeof cmd || system(cmd) != 0 ||
            _stat(audio, &wst) != 0 || wst.st_size < 1024) {
            fprintf(stderr, "error: ffmpeg could not create %s\n", audio);
            goto cleanup;
        }
    }
    if (_stat(audio, &wst) == 0 && wst.st_size > 44)
        audio_secs = (double)(wst.st_size - 44) / WAV_BYTES_PER_SEC;

    existing.n = 0;
    EnumWindows(enum_snap, (LPARAM)&existing);
    si.cb = sizeof si;
    if (!CreateProcessA(NULL, notepad_exe, NULL, NULL, FALSE, 0, NULL, NULL,
                        &si, &pi)) {
        fprintf(stderr, "error: could not start notepad.exe (%lu)\n",
                (unsigned long)GetLastError());
        goto cleanup;
    }
    WaitForInputIdle(pi.hProcess, 10000);
    note = find_notepad(pi.dwProcessId, &existing, 4000);
    if (!note) {
        fprintf(stderr, "error: Notepad window not found.\n");
        goto cleanup;
    }
    edit = find_edit(note, 5000);
    if (!edit) {
        fprintf(stderr, "error: Notepad text control not found.\n");
        goto cleanup;
    }
    ShowWindow(note, SW_SHOWNORMAL);
    SetForegroundWindow(note);
    Sleep(120);
    size_notepad(note, edit);
    Sleep(120);
    update_layout(edit, &layout, 1);
    printf("Grid: %d x %d (margins %d x %d)\n",
           layout.cols, layout.rows, layout.pad_x, layout.pad_y);

    n = snprintf(cmd, sizeof cmd,
                 "ffmpeg -hide_banner -loglevel error -nostdin -i \"%s\" -an "
                 "-vf \"scale=%d:%d:force_original_aspect_ratio=decrease,"
                 "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black\" -r %d -pix_fmt gray "
                 "-f rawvideo - 2>nul",
                 video, PIPE_W, PIPE_H, PIPE_W, PIPE_H, FPS);
    if (n < 0 || n >= (int)sizeof cmd) {
        fprintf(stderr, "error: command line too long.\n");
        goto cleanup;
    }
    pipe = _popen(cmd, "rb");
    if (!pipe) {
        fprintf(stderr, "error: _popen failed.\n");
        goto cleanup;
    }
    setvbuf(pipe, NULL, _IONBF, 0);

    frame = (unsigned char *)malloc(FRAME_BYTES);
    text = (char *)malloc(TEXT_BYTES);
    if (!frame || !text) {
        fprintf(stderr, "error: out of memory.\n");
        goto cleanup;
    }
    if (fread(frame, 1, FRAME_BYTES, pipe) != FRAME_BYTES) {
        fprintf(stderr, "error: no video frames. Is ffmpeg on PATH?\n");
        goto cleanup;
    }
    decoded = 1;

    timeBeginPeriod(1);
    timer_set = 1;
    QueryPerformanceFrequency(&freq);
    printf("Playing. Ctrl+C or close Notepad to stop.\n");

    if (!PlaySoundA(audio, NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) {
        fprintf(stderr, "warning: could not play %s\n", audio);
        audio_secs = 0.0;
    }
    QueryPerformanceCounter(&t0);

    frame_to_ascii(frame, &layout, text);
    send_text(edit, text);

    while (!g_stop) {
        long long target;

        if (!IsWindow(note))
            break;
        if (!IsWindow(edit)) {
            edit = find_edit(note, 1000);
            if (!edit)
                break;
            update_layout(edit, &layout, 1);
        }

        QueryPerformanceCounter(&now);
        elapsed = (double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
        target = (long long)(elapsed * FPS);

        if (target < decoded) {
            double wait_ms = ((double)decoded / FPS - elapsed) * 1000.0;
            Sleep((DWORD)CLAMP(wait_ms, 1.0, 10.0));
            continue;
        }

        while (decoded <= target) {
            if (fread(frame, 1, FRAME_BYTES, pipe) != FRAME_BYTES)
                goto drain;
            ++decoded;
        }

        if (update_layout(edit, &layout, tick % 15 == 0))
            printf("Grid: %d x %d (margins %d x %d)\n",
                   layout.cols, layout.rows, layout.pad_x, layout.pad_y);
        if (!IsIconic(note)) {
            frame_to_ascii(frame, &layout, text);
            send_text(edit, text);
        }
        ++tick;
    }

drain:

    for (;;) {
        QueryPerformanceCounter(&now);
        elapsed = (double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
        if (g_stop || elapsed >= audio_secs || !IsWindow(note))
            break;
        Sleep(20);
    }
    exit_code = 0;

cleanup:
    if (pipe)
        _pclose(pipe);
    PlaySoundA(NULL, NULL, 0);
    free(frame);
    free(text);
    if (pi.hThread)
        CloseHandle(pi.hThread);
    if (pi.hProcess)
        CloseHandle(pi.hProcess);
    if (timer_set)
        timeEndPeriod(1);
    return exit_code;
}
