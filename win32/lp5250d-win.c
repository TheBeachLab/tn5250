/* TN5250 - An implementation of the 5250 telnet protocol.
 * Copyright (C) 2001-2008 Scott Klement
 *
 * This file is part of TN5250
 *
 * TN5250 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1, or (at your option)
 * any later version.
 *
 * TN5250 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307 USA
 *
 */

#include "tn5250-private.h"
#include "resource.h"

#include <commdlg.h>
#include <shellapi.h>
#include <Winspool.h>

#define MAXDEVNAME 64
#define MAXMSG     1024

static void syntax(void);

char* version_string = "not implemented";

#define LP5250D_WINDOW_CLASS  "lp5250d_window_class"
#define WM_TN5250_STREAM_DATA WM_USER + 2000
#define WM_TN5250_NOTIFY_ICON WM_USER + 2001

WNDCLASSEX wndclass;
HWND hWnd;
int WindowIsVisible;

Tn5250PrintSession* printsess = NULL;
Tn5250Stream* stream = NULL;
Tn5250Config* config = NULL;
char globStatus[MAXMSG + 1];
NOTIFYICONDATA nid;
static int printer_established = 0;
static int printer_user_stop = 0;

struct _Tn5250Printer {
    int openpg;
    HANDLE devh;
    int state;
    int ccp;
    int curline;
    int count;
    FILE* fileh;
    unsigned char prevchar;
    Tn5250CharMap* map;
    char* cmd;
    unsigned char* job;
    size_t job_len;
    size_t job_cap;
    int capture;
    int passthrough;
};
typedef struct _Tn5250Printer Tn5250Printer;

void msgboxf(const char* fmt, ...);
Tn5250Printer* tn5250_windows_printer_startdoc(void);
static void printer_job_add(Tn5250Printer* This, const unsigned char* bytes,
                            size_t n);
static int printer_wants_pdf(void);
static void printer_deliver(Tn5250Printer* This);
int tn5250_windows_printer_scs2ascii(Tn5250Printer* This, unsigned char ch);
int tn5250_windows_printer_writechar(Tn5250Printer* This, unsigned char ch);
int tn5250_windows_printer_finishdoc(Tn5250Printer* h);
int tn5250_windows_printer_status(int type, const char* fmt, ...);
int GetDefaultPrinter(char* dftprt, int size);
Tn5250PrintSession* tn5250_windows_print_session_new(void);
void tn5250_windows_print_session_destroy(Tn5250PrintSession* This);
void tn5250_windows_print_session_set_fd(Tn5250PrintSession* This,
                                         SOCKET_TYPE fd);
void tn5250_windows_print_session_set_stream(Tn5250PrintSession* This,
                                             Tn5250Stream* newstream);
void tn5250_windows_print_session_set_char_map(Tn5250PrintSession* This,
                                               const char* map);
int tn5250_windows_print_session_get_response_code(Tn5250PrintSession* This,
                                                   char* code);
void tn5250_windows_print_session_main_loop(Tn5250PrintSession* This);
static int tn5250_windows_print_session_waitevent(Tn5250PrintSession* This);
LRESULT CALLBACK tn5250_windows_printer_wndproc(HWND hwnd, UINT nMsg,
                                                WPARAM wParam, LPARAM lParam);
int parse_cmdline(char* cmdline, char** my_argv);

static int printer_name_same(const char* a, const char* b) {
    size_t i;

    if (a == NULL || b == NULL) {
        return 0;
    }
    for (i = 0;; i++) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'a' && ca <= 'z') {
            ca = (char)(ca - 'a' + 'A');
        }
        if (cb >= 'a' && cb <= 'z') {
            cb = (char)(cb - 'a' + 'A');
        }
        if (ca != cb) {
            return 0;
        }
        if (ca == '\0') {
            return 1;
        }
    }
}

/* attempt 0 is the configured name. Later attempts are NAME2, NAME3, ... */
static void printer_apply_devname(const char* base, int attempt) {
    char name[11];
    char suffix[8];
    size_t n = strlen(base);
    size_t sn;

    if (n > 10) {
        n = 10;
    }
    if (attempt <= 0) {
        memcpy(name, base, n);
        name[n] = '\0';
    }
    else {
        sprintf(suffix, "%d", attempt + 1);
        sn = strlen(suffix);
        if (n + sn > 10) {
            n = 10 - sn;
        }
        memcpy(name, base, n);
        memcpy(name + n, suffix, sn + 1);
    }
    tn5250_config_set(config, "env.DEVNAME", name);
}

static int printer_devname_blocked(void) {
    const char* name = tn5250_config_get(config, "env.DEVNAME");
    const char* avoid = tn5250_config_get(config, "avoid_devname");

    return printer_name_same(name, avoid);
}

static int printer_suffix_on(void) {
    const char* suf = tn5250_config_get(config, "devname_suffix");

    return suf != NULL && (suf[0] == '1' || suf[0] == 'y' || suf[0] == 'Y');
}

int WINAPI WinMain(HINSTANCE i, HINSTANCE p, PSTR cmd, int show) {

    char** argv = NULL;
    int argc, x;
    WORD winsock_ver;
    WSADATA wsadata;
    HDC devctx;
    TEXTMETRIC txm;
    RECT rec;
    int newwidth, newheight;

    /* Create a window to show status messages to the user */

    wndclass.lpszClassName = LP5250D_WINDOW_CLASS;
    wndclass.cbSize = sizeof(WNDCLASSEX);
    wndclass.style = CS_HREDRAW | CS_VREDRAW;
    wndclass.lpfnWndProc = tn5250_windows_printer_wndproc;
    wndclass.cbClsExtra = 0;
    wndclass.cbWndExtra = 0;
    wndclass.hInstance = i;
    wndclass.hIcon = LoadIcon(i, MAKEINTRESOURCE(IDI_TN5250_ICON));
    wndclass.hIconSm = NULL;
    wndclass.hCursor = LoadCursor(NULL, IDC_IBEAM);
    wndclass.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wndclass.lpszMenuName = NULL;
    RegisterClassEx(&wndclass);

    hWnd = CreateWindow(LP5250D_WINDOW_CLASS, /* window class name */
                        "lp5250d status",     /* window name */
                        WS_OVERLAPPEDWINDOW,  /* window style */
                        CW_USEDEFAULT,        /* horizontal position */
                        CW_USEDEFAULT,        /* vertical position */
                        CW_USEDEFAULT,        /* width */
                        CW_USEDEFAULT,        /* height */
                        NULL,                 /* parent window */
                        NULL,                 /* menu or child window */
                        i,                    /* application instance */
                        NULL);                /* extra WM_CREATE data */

    if (hWnd == NULL) {
        msgboxf("Unable to create new window!\n");
        exit(1);
    }

    /* Make our window be big enough for 10 lines of 40 chars */

    devctx = GetDC(hWnd);
    SelectObject(devctx, GetStockObject(SYSTEM_FIXED_FONT));
    GetTextMetrics(devctx, &txm);
    newwidth = txm.tmAveCharWidth * 40 + 10;
    newheight = (txm.tmHeight + txm.tmExternalLeading) * 10 + 10;
    ReleaseDC(hWnd, devctx);

    GetClientRect(hWnd, &rec);
    MoveWindow(hWnd, rec.left, rec.top, newwidth, newheight, FALSE);

    ShowWindow(hWnd, SW_HIDE);
    ShowWindow(hWnd, SW_HIDE);
    WindowIsVisible = 0;

    /* Start up Windows Sockets, and make sure we've got a compatable
       winsock layer */

    winsock_ver = MAKEWORD(1, 1);
    if (WSAStartup(winsock_ver, &wsadata)) {
        msgboxf("Unable to initialize WinSock");
        exit(1);
    }
    if (LOBYTE(wsadata.wVersion) != 1 || HIBYTE(wsadata.wVersion) != 1) {
        msgboxf("WinSock version is not 1.1 compatable!");
        WSACleanup();
        exit(1);
    }

    /* make us appear in system tray */
    nid.cbSize = sizeof(nid);
    nid.hWnd = hWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TN5250_NOTIFY_ICON;
    nid.hIcon = LoadIcon(i, MAKEINTRESOURCE(IDI_TN5250_ICON));
    strcpy(nid.szTip, "5250 printer");
    Shell_NotifyIcon(NIM_ADD, &nid);

    /* parse the "cmd" variable into a seperate string for each argument */
    argc = parse_cmdline(cmd, NULL);
    if (argc > 0) {
        argv = (char**)malloc(argc * sizeof(char*));
        for (x = 0; x < argc; x++) {
            argv[x] = NULL;
        }
        parse_cmdline(cmd, argv);
    }

    /* Load TN5250 Configuration File */

    config = tn5250_config_new();
    if (tn5250_config_load_default(config) == -1) {
        tn5250_config_unref(config);
        Shell_NotifyIcon(NIM_DELETE, &nid);
        exit(1);
    }
    if (tn5250_config_parse_argv(config, argc, argv) == -1) {
        tn5250_config_unref(config);
        syntax();
    }

    /* We must have "help", "version" or a "host" in order to do anything
        meaningful */

    if (tn5250_config_get(config, "help")) {
        syntax();
    }
    else if (tn5250_config_get(config, "version")) {
        msgboxf("tn5250 version %s\n", version_string);
        Shell_NotifyIcon(NIM_DELETE, &nid);
        exit(0);
    }
    else if (!tn5250_config_get(config, "host")) {
        syntax();
    }

    /* Create a trace file if requested */

#ifndef NDEBUG
    if (tn5250_config_get(config, "trace")) {
        tn5250_log_open(tn5250_config_get(config, "trace"));
        TN5250_LOG(
            ("lp5250d version %s, built on %s\n", version_string, __DATE__));
        TN5250_LOG(("host = %s\n", tn5250_config_get(config, "host")));
    }
#endif

    /* One printer device, distinct from the display name in
     * avoid_devname. If that device is rejected, try NAME2..NAME9. */
    {
        const char* configured = tn5250_config_get(config, "env.DEVNAME");
        char base[11];
        int attempt = 0;
        int tries = 1;
        size_t n;

        base[0] = '\0';
        if (configured != NULL) {
            n = strlen(configured);
            if (n > 10) {
                n = 10;
            }
            memcpy(base, configured, n);
            base[n] = '\0';
            if (printer_suffix_on()) {
                tries = 9;
            }
        }
        for (; attempt < tries; attempt++) {
            if (base[0] != '\0') {
                printer_apply_devname(base, attempt);
                if (printer_devname_blocked()) {
                    continue;
                }
            }
            stream =
                tn5250_stream_open(tn5250_config_get(config, "host"), config);
            if (stream == NULL) {
                msgboxf("Couldn't connect to %s",
                        tn5250_config_get(config, "host"));
                break;
            }
            printsess = tn5250_windows_print_session_new();
            tn5250_stream_setenv(stream, "TERM", "IBM-3812-1");
            tn5250_stream_setenv(stream, "IBMFONT", "12");
            tn5250_stream_keepalive_arm(stream);
            SetTimer(hWnd, 2, TN5250_KEEPALIVE_PULSE_SEC * 1000, NULL);
            SetTimer(hWnd, 3, 12000, NULL);
            tn5250_windows_printer_status(
                1, "DEVNAME = %s", tn5250_stream_getenv(stream, "DEVNAME"));
            if (tn5250_stream_getenv(stream, "IBMMFRTYPMDL")) {
                tn5250_stream_setenv(stream, "IBMTRANSFORM", "1");
            }
            else {
                tn5250_stream_setenv(stream, "IBMTRANSFORM", "0");
            }
            tn5250_windows_print_session_set_fd(
                printsess, tn5250_stream_socket_handle(stream));
            tn5250_windows_print_session_set_stream(printsess, stream);
            if (tn5250_config_get(config, "map")) {
                tn5250_windows_print_session_set_char_map(
                    printsess, tn5250_config_get(config, "map"));
            }
            else {
                tn5250_windows_print_session_set_char_map(printsess, "37");
            }
            printer_established = 0;
            tn5250_windows_print_session_main_loop(printsess);
            tn5250_windows_print_session_destroy(printsess);
            printsess = NULL;
            stream = NULL;
            if (printer_established || printer_user_stop) {
                break;
            }
        }
        if (!printer_established && !printer_user_stop && base[0] != '\0' &&
            printer_suffix_on()) {
            const char* busy = tn5250_config_get(config, "device_busy_message");
            if (busy != NULL && busy[0] != '\0') {
                msgboxf("%s", busy);
            }
            else {
                msgboxf("Printer device name is in use.");
            }
        }
    }

    /* shut down, we're done */

    Shell_NotifyIcon(NIM_DELETE, &nid);
    if (config != NULL) {
        tn5250_config_unref(config);
    }
#ifndef NDEBUG
    tn5250_log_close();
#endif
    WSACleanup();
    return 0;
}

static void syntax() {
    msgboxf(
        "Usage:  lp5250d [options] host[:port]\n"
        "Options:\n"
        "\ttrace=FILE			specify FULL path to log file\n"
        "\tmap=NAME  			specify translation map\n"
        "\tenv.DEVNAME=NAME		specify session name\n"
        "\tenv.IBMMFRTYPMDL=NAME	specify host print transform name\n"
        "\toutputcommand=CMD		specify the print output command\n"
        "\t+/-version			display version\n");

    Shell_NotifyIcon(NIM_DELETE, &nid);
    exit(255);
}

void msgboxf(const char* fmt, ...) {
    char themsg[MAXMSG + 1];
    va_list vl;
    va_start(vl, fmt);
    _vsnprintf(themsg, MAXMSG, fmt, vl);
    va_end(vl);
    MessageBox(NULL, themsg, "lp5250d", MB_OK);
}

int tn5250_windows_printer_status(int type, const char* fmt, ...) {

    char data[MAXMSG + 1];
    va_list vl;
    int len;

    va_start(vl, fmt);
    _vsnprintf(data, MAXMSG, fmt, vl);
    va_end(vl);

    strcpy(globStatus, data);
    len = strlen(globStatus);
    while ((len > 0) &&
           ((globStatus[len] == '\n') || (globStatus[len] == '\r') ||
            (globStatus[len] == '\0'))) {
        globStatus[len] = '\0';
        len--;
    }

    if (type < 2) {
        TN5250_LOG(("%s", data));
    }
    else {
        msgboxf("%s", data);
    }

    InvalidateRect(hWnd, NULL, 1);
    UpdateWindow(hWnd);

    return 0;
}

/*
 *  This returns the windows "default printer".
 *
 */
int GetDefaultPrinter(char* dftprt, int size) {

    PRINTDLG pd;
    HDC dc;
    LPDEVNAMES pDevNames;
    LPTSTR pDevice;

    if (size < 2) {
        return -1;
    }
    *dftprt = '\0';

    memset(&pd, 0, sizeof(PRINTDLG));
    pd.lStructSize = sizeof(PRINTDLG);
    pd.Flags = PD_RETURNDEFAULT | PD_RETURNDC;

    if (!PrintDlg(&pd)) {
        return -1;
    }

    pDevNames = (LPDEVNAMES)GlobalLock(pd.hDevNames);
    pDevice = (LPTSTR)pDevNames + pDevNames->wDeviceOffset;
    strncpy(dftprt, pDevice, size);
    GlobalUnlock(pd.hDevNames);

    GlobalFree(pd.hDevMode);
    GlobalFree(pd.hDevNames);

    return 0;
}

#define SCS_STATE_NORMAL       1
#define SCS_STATE_TRANSPARENT1 2
#define SCS_STATE_TRANSPARENT2 3
#define SCS_STATE_GOBBLE       4
#define SCS_STATE_34           5
#define SCS_STATE_2B           6
#define SCS_STATE_2BD1         7
#define SCS_STATE_2BD2         8
#define SCS_STATE_2BD3         9
#define SCS_STATE_2BD2_2       10
#define SCS_STATE_2BD103       11
#define SCS_STATE_2BD106       12
#define SCS_STATE_2BD107       13
#define SCS_STATE_2BD3_2       14
#define SCS_STATE_AHPP         15
#define SCS_STATE_SCS          16
#define SCS_STATE_AVPP         17

/*
 * Buffer one print job, then either draw it on a Windows printer
 * (Microsoft Print to PDF shows its Save As dialog) or write a PDF
 * file. Host print transform bytes are not SCS: escape sequences are
 * skipped and the remaining text is what gets printed.
 */

static void printer_job_add(Tn5250Printer* This, const unsigned char* bytes,
                            size_t n) {
    size_t need;
    unsigned char* grown;

    if (This == NULL || n == 0 || This->job_len > (8 * 1024 * 1024)) {
        return;
    }
    need = This->job_len + n;
    if (need > (8 * 1024 * 1024)) {
        n = (8 * 1024 * 1024) - This->job_len;
        need = This->job_len + n;
    }
    if (need + 1 > This->job_cap) {
        size_t cap = This->job_cap == 0 ? 4096 : This->job_cap;
        while (cap < need + 1) {
            cap *= 2;
        }
        grown = (unsigned char*)realloc(This->job, cap);
        if (grown == NULL) {
            return;
        }
        This->job = grown;
        This->job_cap = cap;
    }
    memcpy(This->job + This->job_len, bytes, n);
    This->job_len += n;
    This->job[This->job_len] = '\0';
}

static int printer_wants_pdf(void) {
    const char* dir = tn5250_config_get(config, "pdf_dir");
    const char* prn = tn5250_config_get(config, "pdf_printer");

    if (dir != NULL && dir[0] != '\0') {
        return 1;
    }
    if (prn != NULL && prn[0] != '\0') {
        return 1;
    }
    return 0;
}

static char* printer_job_text(Tn5250Printer* This) {
    char* out;
    size_t i;
    size_t o = 0;
    int skip = 0;

    out = (char*)malloc(This->job_len + 1);
    if (out == NULL) {
        return NULL;
    }
    if (This->job == NULL || This->job_len == 0) {
        out[0] = '\0';
        return out;
    }
    if (!This->passthrough) {
        memcpy(out, This->job, This->job_len);
        out[This->job_len] = '\0';
        return out;
    }
    for (i = 0; i < This->job_len; i++) {
        unsigned char c = This->job[i];
        if (skip) {
            if (c >= 0x40 && c <= 0x7e) {
                skip = 0;
            }
            continue;
        }
        if (c == 0x1b) {
            skip = 1;
            continue;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n' || c == '\f' || c == '\t' || (c >= 32 && c != 127)) {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return out;
}

static int pdf_escape(const char* s, int n, char* dst, int dstmax) {
    int i;
    int o = 0;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '(' || c == ')' || c == '\\') {
            if (o + 2 >= dstmax) {
                break;
            }
            dst[o++] = '\\';
            dst[o++] = (char)c;
        }
        else if (c < 32 || c > 126) {
            if (o + 4 >= dstmax) {
                break;
            }
            dst[o++] = '\\';
            dst[o++] = (char)('0' + ((c >> 6) & 7));
            dst[o++] = (char)('0' + ((c >> 3) & 7));
            dst[o++] = (char)('0' + (c & 7));
        }
        else {
            if (o + 1 >= dstmax) {
                break;
            }
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
    return o;
}

static int pdf_add(char** buf, size_t* len, size_t* cap, const char* s) {
    size_t n = strlen(s);
    char* grown;

    if (*len + n + 1 > *cap) {
        size_t next = *cap == 0 ? 4096 : *cap;
        while (*len + n + 1 > next) {
            next *= 2;
        }
        grown = (char*)realloc(*buf, next);
        if (grown == NULL) {
            return 0;
        }
        *buf = grown;
        *cap = next;
    }
    memcpy(*buf + *len, s, n + 1);
    *len += n;
    return 1;
}

static int printer_write_pdf(const char* path, const char* text) {
    FILE* f;
    char* stream = NULL;
    size_t slen = 0;
    size_t scap = 0;
    const char* p;
    int lines = 0;
    long xref[8];
    long content_len;
    int i;
    char piece[900];

    p = text == NULL ? "" : text;
    if (!pdf_add(&stream, &slen, &scap, "BT /F1 9 Tf 36 760 Td 11 TL\n")) {
        free(stream);
        return 0;
    }
    while (*p != '\0' && lines < 64) {
        char esc[700];
        const char* nl = p;
        int n;
        while (*nl != '\0' && *nl != '\n' && *nl != '\f') {
            nl++;
        }
        n = (int)(nl - p);
        if (n > 132) {
            n = 132;
        }
        pdf_escape(p, n, esc, (int)sizeof(esc));
        _snprintf(piece, sizeof(piece), "(%s) Tj T*\n", esc);
        piece[sizeof(piece) - 1] = '\0';
        if (!pdf_add(&stream, &slen, &scap, piece)) {
            free(stream);
            return 0;
        }
        lines++;
        if (*nl == '\0' || *nl == '\f') {
            break;
        }
        p = nl + 1;
    }
    if (!pdf_add(&stream, &slen, &scap, "ET\n")) {
        free(stream);
        return 0;
    }
    f = fopen(path, "wb");
    if (f == NULL) {
        free(stream);
        return 0;
    }
    fprintf(f, "%%PDF-1.4\n");
    xref[1] = ftell(f);
    fprintf(f, "1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n");
    xref[2] = ftell(f);
    fprintf(f, "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n");
    xref[3] = ftell(f);
    fprintf(f, "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
               "/Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >> "
               "endobj\n");
    xref[4] = ftell(f);
    content_len = (long)slen;
    fprintf(f, "4 0 obj << /Length %ld >> stream\n", content_len);
    fwrite(stream, 1, slen, f);
    fprintf(f, "endstream endobj\n");
    xref[5] = ftell(f);
    fprintf(f, "5 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Courier >> "
               "endobj\n");
    xref[6] = ftell(f);
    fprintf(f, "xref\n0 6\n0000000000 65535 f \n");
    for (i = 1; i <= 5; i++) {
        fprintf(f, "%010ld 00000 n \n", xref[i]);
    }
    fprintf(f, "trailer << /Size 6 /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n",
            xref[6]);
    fclose(f);
    free(stream);
    return 1;
}

static void printer_gdi(const char* printer, const char* text) {
    HDC dc;
    HFONT font;
    DOCINFOA di;
    const char* p;
    int y;
    int lh;
    int page = 0;

    dc = CreateDCA("WINSPOOL", printer, NULL, NULL);
    if (dc == NULL) {
        return;
    }
    font = CreateFontA(-MulDiv(10, GetDeviceCaps(dc, LOGPIXELSY), 72), 0, 0, 0,
                       FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                       FIXED_PITCH | FF_MODERN, "Courier New");
    memset(&di, 0, sizeof(di));
    di.cbSize = sizeof(di);
    di.lpszDocName = "5250";
    if (StartDocA(dc, &di) <= 0) {
        if (font != NULL) {
            DeleteObject(font);
        }
        DeleteDC(dc);
        return;
    }
    p = text == NULL ? "" : text;
    while (page < 30) {
        int bottom = GetDeviceCaps(dc, VERTRES) - 48;
        StartPage(dc);
        if (font != NULL) {
            SelectObject(dc, font);
        }
        SetBkMode(dc, TRANSPARENT);
        y = 48;
        lh = MulDiv(12, GetDeviceCaps(dc, LOGPIXELSY), 72);
        if (lh < 1) {
            lh = 12;
        }
        while (*p != '\0' && y < bottom) {
            const char* nl = p;
            int n;
            char line[160];
            while (*nl != '\0' && *nl != '\n' && *nl != '\f') {
                nl++;
            }
            n = (int)(nl - p);
            if (n > 132) {
                n = 132;
            }
            memcpy(line, p, (size_t)n);
            line[n] = '\0';
            TextOutA(dc, 36, y, line, n);
            y += lh;
            if (*nl == '\0') {
                p = nl;
                break;
            }
            if (*nl == '\f') {
                p = nl + 1;
                break;
            }
            p = nl + 1;
        }
        EndPage(dc);
        page++;
        if (*p == '\0') {
            break;
        }
    }
    EndDoc(dc);
    if (font != NULL) {
        DeleteObject(font);
    }
    DeleteDC(dc);
}

static void printer_deliver(Tn5250Printer* This) {
    const char* dir = tn5250_config_get(config, "pdf_dir");
    const char* prn = tn5250_config_get(config, "pdf_printer");
    char* text;
    char path[MAX_PATH];

    text = printer_job_text(This);
    if (dir != NULL && dir[0] != '\0') {
        SYSTEMTIME st;
        GetLocalTime(&st);
        _snprintf(path, sizeof(path), "%s\\5250-%04u%02u%02u-%02u%02u%02u.pdf",
                  dir, (unsigned)st.wYear, (unsigned)st.wMonth,
                  (unsigned)st.wDay, (unsigned)st.wHour, (unsigned)st.wMinute,
                  (unsigned)st.wSecond);
        path[sizeof(path) - 1] = '\0';
        if (printer_write_pdf(path, text == NULL ? "" : text)) {
            ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
        }
    }
    else if (prn != NULL && prn[0] != '\0') {
        HDC probe = CreateDCA("WINSPOOL", prn, NULL, NULL);
        if (probe != NULL) {
            DeleteDC(probe);
            printer_gdi(prn, text == NULL ? "" : text);
        }
        else {
            OPENFILENAMEA ofn;
            memset(&ofn, 0, sizeof(ofn));
            memset(path, 0, sizeof(path));
            strcpy(path, "5250.pdf");
            ofn.lStructSize = sizeof(ofn);
            ofn.lpstrFilter = "PDF\0*.pdf\0";
            ofn.lpstrFile = path;
            ofn.nMaxFile = sizeof(path);
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            ofn.lpstrDefExt = "pdf";
            if (GetSaveFileNameA(&ofn) &&
                printer_write_pdf(path, text == NULL ? "" : text)) {
                ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
            }
        }
    }
    free(text);
}

/*
 *  Start a new document to be printed
 *
 */

Tn5250Printer* tn5250_windows_printer_startdoc(void) {
    DOC_INFO_1 di;
    Tn5250Printer* This;
    LPTSTR devname;
    int isfile = 0, iscmd = 0;
    int len;
    char* temp;
    char* cmd = NULL;

    devname = malloc(MAXDEVNAME + 1);
    devname[0] = '\0';

    if (printer_wants_pdf()) {
        /* PDF output does not need a Windows printer queue up front. */
    }
    else if (tn5250_config_get(config, "outputcommand")) {
        len = strlen(tn5250_config_get(config, "outputcommand"));
        temp = malloc(len + 1);
        strcpy(temp, tn5250_config_get(config, "outputcommand"));
        if (!strncasecmp(temp, "printer:", 8)) {
            strncpy(devname, &temp[8], MAXDEVNAME);
        }
        else if (!strncasecmp(temp, "file:", 5)) {
            strncpy(devname, &temp[5], MAXDEVNAME);
            isfile = 1;
        }
        else if (!strncasecmp(temp, "exec:", 5)) {
            strncpy(devname, &temp[5], MAXDEVNAME);
            isfile = 1;
            iscmd = 1;
        }
        else {
            strncpy(devname, temp, MAXDEVNAME);
        }
        free(temp);
    }
    else {
        if (GetDefaultPrinter(devname, MAXDEVNAME) < 0) {
            msgboxf("ERROR: Unable to retrieve your default printer.");
            free(devname);
            return NULL;
        }
    }

    This = malloc(sizeof(Tn5250Printer));

    if (tn5250_config_get(config, "map")) {
        This->map = tn5250_char_map_new(tn5250_config_get(config, "map"));
    }
    else {
        This->map = tn5250_char_map_new("37");
    }

    This->devh = NULL;
    This->fileh = NULL;
    This->openpg = 0;
    This->state = SCS_STATE_NORMAL;
    This->ccp = 1;
    This->curline = 1;
    This->prevchar = 0;
    This->count = 0;
    This->cmd = NULL;
    This->job = NULL;
    This->job_len = 0;
    This->job_cap = 0;
    This->capture = printer_wants_pdf();
    This->passthrough =
        tn5250_stream_getenv(stream, "IBMMFRTYPMDL") != NULL ? 1 : 0;

    if (This->capture) {
        free(devname);
        return This;
    }

    if (iscmd) {
        cmd = strchr(devname, '?');
        if (cmd != NULL) {
            This->cmd = malloc(strlen(cmd + 1) + 1);
            strcpy(This->cmd, cmd + 1);
            *cmd = '\0';
        }
    }

    if (isfile) {
        This->fileh = fopen(devname, "w");
        if (This->fileh == NULL) {
            msgboxf("fopen() failed.  Check the outputcommand filename.");
            TN5250_LOG(("fopen() failed!\n"));
            free(devname);
            return NULL;
        }
    }
    else {
        if (!OpenPrinter(devname, &(This->devh), NULL)) {
            msgboxf("Call to OpenPrinter() failed.\n"
                    "Maybe you don't have a printer called \"%s\"?\n",
                    devname);
            TN5250_LOG(("OpenPrinter() failed!\n"));
            free(devname);
            return NULL;
        }

        memset(&di, 0, sizeof(DOC_INFO_1));
        di.pDocName = "lp5250d-print-job";
        StartDocPrinter(This->devh, 1, (LPBYTE)&di);
    }

    free(devname);
    return This;
}

/*
 *  write a single character to the print document
 *
 *
 */

int tn5250_windows_printer_writechar(Tn5250Printer* This, unsigned char c) {

    DWORD written;

    if (This->capture) {
        printer_job_add(This, (const unsigned char*)&c, 1);
        return 1;
    }

    if (This->devh != NULL) {
        WritePrinter(This->devh, (LPVOID)&c, 1, (LPDWORD)&written);
        if (c == '\f') {
            EndPagePrinter(This->devh);
            This->openpg = 0;
        }
    }
    else if (This->fileh != NULL) {
        if (c != '\r') {
            fputc(c, This->fileh);
        }
        written = 1;
    }

    return written;
}

/*
 * Finish the printer document
 *
 */

int tn5250_windows_printer_finishdoc(Tn5250Printer* This) {

    if (This->capture) {
        printer_deliver(This);
        free(This->job);
        tn5250_char_map_destroy(This->map);
        free(This);
        return 0;
    }

    if (This->devh != NULL) {
        if (This->openpg == 1) {
            EndPagePrinter(This->devh);
            This->openpg = 0;
        }
        EndDocPrinter(This->devh);
    }
    else if (This->fileh != NULL) {
        fclose(This->fileh);
    }
    if (This->cmd != NULL) {
        win32_run_process(This->cmd, 1);
        free(This->cmd);
    }
    tn5250_char_map_destroy(This->map);
    free(This);
    return 0;
}

/*
 *  This parses a print stream in SCS format, and converts it to
 *  a standard ASCII printer file.
 *
 *  Originally, I tried using an scs2ascii program, like the Linux
 *  version does, but I found that pipes don't perform very well in
 *  Windows.   So, I re-implemented it using a big state machine...
 *  this way I don't have to buffer the data, or throw it into a file
 *  in order to parse it.
 *
 */

int tn5250_windows_printer_scs2ascii(Tn5250Printer* This, unsigned char ch) {

    int written;
    int loop;
    unsigned char temp;

    if ((This->openpg == 0) && (This->devh != NULL)) {
        StartPagePrinter(This->devh);
        This->openpg = 1;
    }

    switch (This->state) {
    case SCS_STATE_NORMAL:
        switch (ch) {
        case SCS_RFF:
        case SCS_NOOP:
        case SCS_CR:
        case SCS_RNL:
        case SCS_HT:
        case 0xFF:
            /* these we ignore for now. */
            break;
        case SCS_TRANSPARENT:
            This->state = SCS_STATE_TRANSPARENT1;
            break;
        case SCS_FF:
            tn5250_windows_printer_writechar(This, '\f');
            This->openpg = 0;
            This->curline = 1;
            break;
        case SCS_NL:
            tn5250_windows_printer_writechar(This, '\r');
            tn5250_windows_printer_writechar(This, '\n');
            This->curline++;
            This->ccp = 1;
            break;
        case 0x34:
            This->state = SCS_STATE_34;
            break;
        case 0x2B:
            This->state = SCS_STATE_2B;
            break;
        default:
            temp = tn5250_char_map_to_local(This->map, ch);
            tn5250_windows_printer_writechar(This, temp);
            This->ccp++;
            TN5250_LOG((">%x\n", ch));
            break;
        }
        break;
    case SCS_STATE_TRANSPARENT1:
        This->count = ch;
        This->state = SCS_STATE_TRANSPARENT2;
        TN5250_LOG(("TRANSPARENT (%d) = ", ch));
        break;
    case SCS_STATE_TRANSPARENT2:
        This->count--;
        tn5250_windows_printer_writechar(This, ch);
        TN5250_LOG(("%x ", ch));
        if (This->count < 1) {
            TN5250_LOG(("\n"));
            This->state = SCS_STATE_NORMAL;
        }
        break;
    case SCS_STATE_GOBBLE:
        This->count--;
        TN5250_LOG(("%x ", ch));
        if (This->count < 1) {
            TN5250_LOG(("\n"));
            This->state = SCS_STATE_NORMAL;
        }
        break;
    case SCS_STATE_34:
        switch (ch) {
        case SCS_AVPP:
            TN5250_LOG(("AVPP "));
            This->state = SCS_STATE_AVPP;
            This->count = 1;
            break;
        case SCS_AHPP:
            This->state = SCS_STATE_AHPP;
            break;
        default:
            TN5250_LOG(("ERROR: Unknown 0x034 command %x\n", ch));
            This->state = SCS_STATE_NORMAL;
            break;
        }
        break;
    case SCS_STATE_AHPP:
        TN5250_LOG(("AHPP %d\n", ch));
        if (This->ccp > ch) {
            tn5250_windows_printer_writechar(This, '\r');
            for (loop = 0; loop < ch; loop++) {
                tn5250_windows_printer_writechar(This, ' ');
            }
        }
        else {
            for (loop = 0; loop < (ch - This->ccp); loop++) {
                tn5250_windows_printer_writechar(This, ' ');
            }
        }
        This->state = SCS_STATE_NORMAL;
        break;
    case SCS_STATE_AVPP:
        TN5250_LOG(("AVPP %d\n", ch));
        if (This->curline > ch) {
            tn5250_windows_printer_writechar(This, '\f');
            if (This->devh != NULL) {
                StartPagePrinter(This->devh);
                This->openpg = 1;
            }
            This->curline = 1;
        }
        while (This->curline < ch) {
            tn5250_windows_printer_writechar(This, '\r');
            tn5250_windows_printer_writechar(This, '\n');
            This->ccp = 1;
            This->curline++;
        }
        This->state = SCS_STATE_NORMAL;
        break;
    case SCS_STATE_2B:
        switch (ch) {
        case 0xD1:
            This->state = SCS_STATE_2BD1;
            break;
        case 0xD2:
            This->state = SCS_STATE_2BD2;
            break;
        case 0xD3:
            This->state = SCS_STATE_2BD3;
            break;
        case 0xC8: /* SGEA */
            TN5250_LOG(("SGEA "));
            This->state = SCS_STATE_GOBBLE;
            This->count = 3;
            break;
        default:
            TN5250_LOG(("ERROR: Unknown 0x2b command: %d\n", ch));
            This->state = SCS_STATE_NORMAL;
            break;
        }
        break;
    case SCS_STATE_2BD1:
        switch (ch) {
        case 0x06:
            This->state = SCS_STATE_2BD106;
            break;
        case 0x07:
            This->state = SCS_STATE_2BD107;
            break;
        case 0x03:
            This->state = SCS_STATE_2BD103;
            break;
        default:
            TN5250_LOG(("ERROR: Unknown 0x2bd1 command: %d\n", ch));
            This->state = SCS_STATE_NORMAL;
            break;
        }
        break;
    case SCS_STATE_2BD106:
        switch (ch) {
        case 0x01: /* GCGID */
            TN5250_LOG(("GCGID "));
            This->state = SCS_STATE_GOBBLE;
            This->count = 4;
            break;
        default:
            TN5250_LOG(("ERROR: Unknown 0x2bd106 command: %d\n", ch));
            This->state = SCS_STATE_NORMAL;
            break;
        }
        break;
    case SCS_STATE_2BD107:
        switch (ch) {
        case 0x05: /* FID */
            TN5250_LOG(("FID "));
            This->state = SCS_STATE_GOBBLE;
            This->count = 5;
            break;
        default:
            TN5250_LOG(("ERROR: Unknown 0x2bd107 command: %d\n", ch));
            This->state = SCS_STATE_NORMAL;
            break;
        }
        break;
    case SCS_STATE_2BD103:
        switch (ch) {
        case 0x81: /* SCGL */
            TN5250_LOG(("SCGL "));
            This->state = SCS_STATE_GOBBLE;
            This->count = 1;
            break;
        default:
            TN5250_LOG(("ERROR: Unknown 0x2bd103 command: %d\n", ch));
            This->state = SCS_STATE_NORMAL;
            break;
        }
        break;
    case SCS_STATE_2BD2:
        This->prevchar = ch;
        This->state = SCS_STATE_2BD2_2;
        break;
    case SCS_STATE_2BD2_2:
        switch (ch) {
        case 0x01: /* STAB */
            TN5250_LOG(("STAB "));
            This->state = SCS_STATE_GOBBLE;
            This->count = This->prevchar - 2;
            break;
        case 0x03: /* JTF */
            TN5250_LOG(("JTF "));
            This->state = SCS_STATE_GOBBLE;
            This->count = This->prevchar - 2;
            break;
        case 0x0D: /* SJM */
            TN5250_LOG(("SJM "));
            This->state = SCS_STATE_GOBBLE;
            This->count = This->prevchar - 2;
            break;
        case 0x48: /* PPM */
            TN5250_LOG(("PPM "));
            This->state = SCS_STATE_GOBBLE;
            This->count = This->prevchar - 2;
            break;
        case 0x49: /* SVM */
            TN5250_LOG(("SVM "));
            This->state = SCS_STATE_GOBBLE;
            This->count = This->prevchar - 2;
            break;
        case 0x4C: /* SPSU */
            TN5250_LOG(("SPSU "));
            This->state = SCS_STATE_GOBBLE;
            This->count = This->prevchar - 2;
            break;
        case 0x85: /* SEA */
            TN5250_LOG(("SEA "));
            This->state = SCS_STATE_GOBBLE;
            This->count = This->prevchar - 2;
            break;
        case 0x11: /* SHM */
            TN5250_LOG(("SHM "));
            This->state = SCS_STATE_GOBBLE;
            This->count = This->prevchar - 2;
            break;
        case 0x40: /* SPPS */
            /* next 4 bytes are length & width, 16 bits each */
            TN5250_LOG(("SPPS "));
            This->state = SCS_STATE_GOBBLE;
            This->count = 4;
            break;
        default:
            switch (This->prevchar) {
            case 0x03:
                switch (ch) {
                case 0x45: /* SIC */
                    TN5250_LOG(("SIC "));
                    This->state = SCS_STATE_GOBBLE;
                    This->count = 1;
                    break;
                case 0x07: /* SIL */
                    TN5250_LOG(("SIL "));
                    This->state = SCS_STATE_GOBBLE;
                    This->count = 1;
                    break;
                case 0x09: /* SLS */
                    TN5250_LOG(("SLS "));
                    This->state = SCS_STATE_GOBBLE;
                    This->count = 1;
                    break;
                default:
                    TN5250_LOG(("ERROR: Unknown 0x2bd203 "
                                "command: %d\n",
                                ch));
                    This->state = SCS_STATE_NORMAL;
                    break;
                }
                break;
            case 0x04:
                switch (ch) {
                case 0x15: /* SSLD */
                    TN5250_LOG(("SSLD "));
                    This->state = SCS_STATE_GOBBLE;
                    This->count = 2;
                    break;
                case 0x29: /* SCS */
                    This->state = SCS_STATE_SCS;
                    break;
                default:
                    TN5250_LOG(("ERROR: Unknown 0x2bd204 "
                                "command: %d\n",
                                ch));
                    This->state = SCS_STATE_NORMAL;
                    break;
                }
                break;
            default:
                TN5250_LOG(("ERROR: Unknown 0x2bd2 command: %d\n", ch));
                This->state = SCS_STATE_NORMAL;
                break;
            }
            break;
        }
        break;
    case SCS_STATE_SCS:
        switch (ch) {
        case 0x00:
            TN5250_LOG(("SCS 0 "));
            This->state = SCS_STATE_GOBBLE;
            This->count = 1;
            break;
        default:
            TN5250_LOG(("ERROR: Unknown 0x2bd20429 command: %d\n", ch));
            This->state = SCS_STATE_NORMAL;
            break;
        }
        break;
    case SCS_STATE_2BD3:
        This->prevchar = ch;
        This->state = SCS_STATE_2BD3_2;
        break;
    case SCS_STATE_2BD3_2:
        switch (ch) {
        case 0xF6: /* STO */
            TN5250_LOG(("STO "));
            This->state = SCS_STATE_GOBBLE;
            This->count = This->prevchar - 2;
            break;
        default:
            TN5250_LOG(("ERROR: Unknown 0x2bd3%02x command: %d\n",
                        This->prevchar, ch));
            This->state = SCS_STATE_NORMAL;
            break;
        }
        break;
    default:
        TN5250_LOG(("tn5250_windows_printer_scs2ascii in invalid state %d\n",
                    This->state));
        TN5250_ASSERT(0);
        break;

    } /* switch (This->state) */

    return 0;
}

static struct winprint_response_code {
    char* code;
    int retval;
    char* text;
} winprint_response_codes[] = {
    // clang-format off
    { "I901", 1, "Virtual device has less function than source device." },
    { "I902", 1, "Session successfully started."                        },
    { "I906", 1, "Automatic sign-on requested, but not allowed.  "
                 "A sign-on screen will follow."                        },
    { "2702", 0, "Device description not found."                        },
    { "2703", 0, "Controller description not found."                    },
    { "2777", 0, "Damaged device description."                          },
    { "8901", 0, "Device not varied on."                                },
    { "8902", 0, "Device not available."                                },
    { "8903", 0, "Device not valid for session."                        },
    { "8906", 0, "Session initiation failed."                           },
    { "8907", 0, "Session failure."                                     },
    { "8910", 0, "Controller not valid for session."                    },
    { "8916", 0, "No matching device found."                            },
    { "8917", 0, "Not authorized to object."                            },
    { "8918", 0, "Job cancelled."                                       },
    { "8920", 0, "Object partially damaged."                            },
                 /* As opposed to fully damaged? */
    { "8921", 0, "Communications error."                                },
    { "8922", 0, "Negative response received."                          },
                 /* From what?!? */
    { "8923", 0, "Start-up record built incorrectly."                   },
    { "8925", 0, "Creation of device failed."                           },
    { "8928", 0, "Change of device failed."                             },
    { "8929", 0, "Vary on or vary off failed."                          },
    { "8930", 0, "Message queue does not exist."                        },
    { "8934", 0, "Start up for S/36 WSF received."                      },
    { "8935", 0, "Session rejected."                                    },
    { "8936", 0, "Security failure on session attempt."                 },
    { "8937", 0, "Automatic sign-on rejected."                          },
    { "8940", 0, "Automatic configuration failed or not allowed."       },
    { "I904", 0, "Source system at incompatible release."               },
    { NULL,   0, NULL                                                   }
    // clang-format on
};

/****f* lib5250/tn5250_windows_print_session_new
 * NAME
 *    tn5250_windows_print_session_new
 * SYNOPSIS
 *    ret = tn5250_windows_print_session_new ();
 * INPUTS
 *    None
 * DESCRIPTION
 *    DOCUMENT ME!!!
 *****/
Tn5250PrintSession* tn5250_windows_print_session_new(void) {
    Tn5250PrintSession* This;

    This = tn5250_new(Tn5250PrintSession, 1);
    if (This == NULL) {
        return NULL;
    }

    This->rec = tn5250_record_new();
    if (This->rec == NULL) {
        free(This);
        return NULL;
    }

    This->stream = NULL;
    This->conn_fd = -1;
    This->map = NULL;

    return This;
}

/****f* lib5250/tn5250_windows_print_session_destroy
 * NAME
 *    tn5250_windows_print_session_destroy
 * SYNOPSIS
 *    tn5250_windows_print_session_destroy (This);
 * INPUTS
 *    Tn5250PrintSession * This       -
 * DESCRIPTION
 *    DOCUMENT ME!!!
 *****/
void tn5250_windows_print_session_destroy(Tn5250PrintSession* This) {
    if (This->stream != NULL) {
        tn5250_stream_destroy(This->stream);
    }
    if (This->rec != NULL) {
        tn5250_record_destroy(This->rec);
    }
    if (This->output_cmd != NULL) {
        free(This->output_cmd);
    }
    if (This->map != NULL) {
        tn5250_char_map_destroy(This->map);
    }
    free(This);
}

/****f* lib5250/tn5250_windows_print_session_set_fd
 * NAME
 *    tn5250_windows_print_session_set_fd
 * SYNOPSIS
 *    tn5250_windows_print_session_set_fd (This, fd);
 * INPUTS
 *    Tn5250PrintSession * This       -
 *    SOCKET_TYPE          fd         -
 * DESCRIPTION
 *    DOCUMENT ME!!!
 *****/
void tn5250_windows_print_session_set_fd(Tn5250PrintSession* This,
                                         SOCKET_TYPE fd) {
    This->conn_fd = fd;
}

/****f* lib5250/tn5250_windows_print_session_set_stream
 * NAME
 *    tn5250_windows_print_session_set_stream
 * SYNOPSIS
 *    tn5250_windows_print_session_set_stream (This, newstream);
 * INPUTS
 *    Tn5250PrintSession * This       -
 *    Tn5250Stream *       newstream  -
 * DESCRIPTION
 *    DOCUMENT ME!!!
 *****/
void tn5250_windows_print_session_set_stream(Tn5250PrintSession* This,
                                             Tn5250Stream* newstream) {
    if (This->stream != NULL) {
        tn5250_stream_destroy(This->stream);
    }
    This->stream = newstream;
}

/****f* lib5250/tn5250_windows_print_session_set_char_map
 * NAME
 *    tn5250_windows_print_session_set_char_map
 * SYNOPSIS
 *    tn5250_windows_print_session_set_char_map (This, map);
 * INPUTS
 *    Tn5250PrintSession * This       -
 *    const char *         map        -
 * DESCRIPTION
 *    Sets the current translation map for this print session.  This is
 *    used to translate response codes to something we can use.
 *****/
void tn5250_windows_print_session_set_char_map(Tn5250PrintSession* This,
                                               const char* map) {
    if (This->map != NULL) {
        tn5250_char_map_destroy(This->map);
    }
    This->map = tn5250_char_map_new(map);
}

/****f* lib5250/tn5250_windows_print_session_get_response_code
 * NAME
 *    tn5250_windows_print_session_get_response_code
 * SYNOPSIS
 *    rc = tn5250_windows_print_session_get_response_code (This, code);
 * INPUTS
 *    Tn5250PrintSession * This       -
 *    char *               code       -
 * DESCRIPTION
 *    Retrieves the response code from the startup response record.  The
 *    function returns 1 for a successful startup, and 0 otherwise.  On return,
 *    code contains the 5 character response code.
 *****/
int tn5250_windows_print_session_get_response_code(Tn5250PrintSession* This,
                                                   char* code) {

    /* Offset of first byte of data after record variable-length header. */
    int o = 6 + tn5250_record_data(This->rec)[6];
    int i;

    for (i = 0; i < 4; i++) {
        if (This->map == NULL) {
            code[i] = tn5250_record_data(This->rec)[o + i + 5];
        }
        else {
            code[i] = tn5250_char_map_to_local(
                This->map, tn5250_record_data(This->rec)[o + i + 5]);
        }
    }

    code[4] = '\0';
    for (i = 0; i < sizeof(winprint_response_codes) /
                        sizeof(struct winprint_response_code);
         i++) {
        if (!strcmp(winprint_response_codes[i].code, code)) {
            tn5250_windows_printer_status(0, "%s : %s\n",
                                          winprint_response_codes[i].code,
                                          winprint_response_codes[i].text);
            return winprint_response_codes[i].retval;
        }
    }
    return 0;
}

/****f* lib5250/tn5250_windows_print_session_main_loop
 * NAME
 *    tn5250_windows_print_session_main_loop
 * SYNOPSIS
 *    tn5250_windows_print_session_main_loop (This);
 * INPUTS
 *    Tn5250PrintSession * This       -
 * DESCRIPTION
 *    This function continually loops, waiting for print jobs from the AS/400.
 *    When it gets one, it sends it to the output command which was specified
 *    on the command line.  If the host closes the socket, we exit.
 *****/
void tn5250_windows_print_session_main_loop(Tn5250PrintSession* This) {
    int pcount;
    int newjob;
    char responsecode[5];
    void* prthdl;
    StreamHeader header;

    while (1) {
        if (!tn5250_windows_print_session_waitevent(This)) {
            return;
        }
        else {
            if (tn5250_stream_handle_receive(This->stream)) {
                pcount = tn5250_stream_record_count(This->stream);
                if (pcount > 0) {
                    if (This->rec != NULL) {
                        tn5250_record_destroy(This->rec);
                    }
                    This->rec = tn5250_stream_get_record(This->stream);
                    if (!tn5250_windows_print_session_get_response_code(
                            This, responsecode)) {
                        Shell_NotifyIcon(NIM_DELETE, &nid);
                        exit(1);
                    }
                    printer_established = 1;
                    KillTimer(hWnd, 3);
                    break;
                }
            }
            else {
                tn5250_windows_printer_status(2, "Socket closed by host.\n");
                return;
            }
        }
    }

    newjob = 1;
    while (1) {
        if (!tn5250_windows_print_session_waitevent(This)) {
            return;
        }
        else {
            if (tn5250_stream_handle_receive(This->stream)) {
                pcount = tn5250_stream_record_count(This->stream);
                if (pcount > 0) {
                    if (newjob) {
                        prthdl = tn5250_windows_printer_startdoc();
                        newjob = 0;
                    }
                    if (This->rec != NULL) {
                        tn5250_record_destroy(This->rec);
                    }
                    This->rec = tn5250_stream_get_record(This->stream);

                    if (tn5250_record_opcode(This->rec) ==
                        TN5250_RECORD_OPCODE_CLEAR) {
                        tn5250_windows_printer_status(
                            0, "Clearing print buffers\n");
                        continue;
                    }

                    header.flowtype = TN5250_RECORD_FLOW_CLIENTO;
                    header.flags = TN5250_RECORD_H_NONE;
                    header.opcode = TN5250_RECORD_OPCODE_PRINT_COMPLETE;

                    tn5250_stream_send_packet(This->stream, 0, header, NULL);

                    if (tn5250_record_length(This->rec) == 0x11) {
                        tn5250_windows_printer_status(0, "Job Complete\n");
                        tn5250_windows_printer_finishdoc(prthdl);
                        newjob = 1;
                    }
                    else {
                        Tn5250Printer* pj = (Tn5250Printer*)prthdl;
                        while (!tn5250_record_is_chain_end(This->rec)) {
                            unsigned char ch =
                                tn5250_record_get_byte(This->rec);
                            /* Host print transform sends the printer
                             * language itself. Do not parse it as SCS. */
                            if (pj != NULL && pj->passthrough) {
                                if (pj->capture) {
                                    printer_job_add(pj, &ch, 1);
                                }
                                else {
                                    tn5250_windows_printer_writechar(pj, ch);
                                }
                            }
                            else {
                                tn5250_windows_printer_scs2ascii(pj, ch);
                            }
                        }
                    }
                }
            }
            else {
                tn5250_windows_printer_status(2, "Socket closed by host\n");
                return;
            }
        }
    }
}

/****f* lib5250/tn5250_windows_print_session_waitevent
 * NAME
 *    tn5250_windows_print_session_waitevent
 * SYNOPSIS
 *    ret = tn5250_windows_print_session_waitevent (This);
 * INPUTS
 *    Tn5250PrintSession * This       -
 * DESCRIPTION
 *    Calls select() to wait for data to arrive on the socket fdr.
 *    This is the socket being used by the print session to communicate
 *    with the AS/400.  There is no timeout, so the function will wait forever
 *    if no data arrives.
 *****/
static int tn5250_windows_print_session_waitevent(Tn5250PrintSession* This) {
    fd_set fdr;
    int result;
    MSG msg;

    /* Turn on async notification of data being received on the socket: */

    if (WSAAsyncSelect(This->conn_fd, hWnd, WM_TN5250_STREAM_DATA,
                       FD_READ | FD_CLOSE) == SOCKET_ERROR) {
        TN5250_LOG(("WSAAsyncSelect failed, reason: %d\n", WSAGetLastError()));
        return 0;
    }

    result = 0;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (msg.message == WM_TN5250_STREAM_DATA) {
            result = 1;
            break;
        }
    }

    /* Turn off async notification of socket data: */
    WSAAsyncSelect(This->conn_fd, hWnd, 0, 0);

    return result;
}

LRESULT CALLBACK tn5250_windows_printer_wndproc(HWND hwnd, UINT nMsg,
                                                WPARAM wParam, LPARAM lParam) {

    static HWND hwndStopButton = 0;
    static int buttonw, buttonh;
    static int cx, cy;
    HDC hdc;
    PAINTSTRUCT ps;
    RECT rc;

    switch (nMsg) {
    case WM_CREATE: {
        /* make the button large enough for 2 rows of 30 chars */
        TEXTMETRIC tm;
        hdc = GetDC(hwnd);
        SelectObject(hdc, GetStockObject(SYSTEM_FIXED_FONT));
        GetTextMetrics(hdc, &tm);
        buttonw = tm.tmAveCharWidth * 30;
        buttonh = (tm.tmHeight + tm.tmExternalLeading) * 2;
        ReleaseDC(hwnd, hdc);

        GetClientRect(hwnd, &rc);
        rc.top = rc.bottom - buttonh;
        rc.left = ((rc.right - rc.left) - buttonw) / 2;

        hwndStopButton = CreateWindow(
            "button", "Stop Printing", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            rc.top, rc.left, buttonw, buttonh, hwnd, (HMENU)1,
            ((LPCREATESTRUCT)lParam)->hInstance, NULL);
        return 0;
        break;
    }

    case WM_WINDOWPOSCHANGING:
        break;

    case WM_CLOSE:
        Shell_NotifyIcon(NIM_DELETE, &nid);
        break;

    case WM_DESTROY:
        printer_user_stop = 1;
        PostQuitMessage(0);
        return 0;
        break;

    case WM_TIMER:
        if (wParam == 3 && !printer_established) {
            KillTimer(hwnd, 3);
            PostQuitMessage(0);
            return 0;
        }
        if (wParam == 2) {
            tn5250_stream_keepalive_pulse();
            return 0;
        }
        break;

    case WM_MOVE:
    case WM_SIZE: /* when something moves/sizes update our button as well */

        GetClientRect(hwnd, &rc);
        rc.top = rc.bottom - buttonh;
        rc.left = ((rc.right - rc.left) - buttonw) / 2;
        MoveWindow(hwndStopButton, rc.left, rc.top, buttonw, buttonh, TRUE);
        break;

    case WM_PAINT:
        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        rc.bottom = rc.bottom / 2;
        DrawText(hdc, globStatus, -1, &rc,
                 DT_NOPREFIX | DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        EndPaint(hwnd, &ps);
        return 0;
        break;

    case WM_SYSCOMMAND:
        switch (wParam) {
        case SC_RESTORE:
            WindowIsVisible = 1;
            ShowWindow(hwnd, SW_SHOW);
            break;
        case SC_MINIMIZE:
            WindowIsVisible = 0;
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        break;

    case WM_COMMAND:
        if (LOWORD(wParam) == 1 && HIWORD(wParam) == BN_CLICKED &&
            (HWND)lParam == hwndStopButton) {
            Shell_NotifyIcon(NIM_DELETE, &nid);
            DestroyWindow(hwnd);
        }
        return 0;
        break;

    case WM_TN5250_NOTIFY_ICON:
        switch (lParam) {
        case WM_LBUTTONDBLCLK:
            if (WindowIsVisible) {
                WindowIsVisible = 0;
                ShowWindow(hwnd, SW_HIDE);
            }
            else {
                WindowIsVisible = 1;
                ShowWindow(hwnd, SW_SHOW);
            }
            break;
        }
        return 0;

    case WM_TIMER:
        if (wParam == 2) {
            tn5250_stream_keepalive_pulse();
            return 0;
        }
        break;

    case WM_TN5250_STREAM_DATA:
        /* this is our special message to tell us that network data
           has arrived.  the waitevent() func will be looking for this
           message, so we don't need to do anything special here,
           just return to prevent the default window proc from handling
           this message */
        return 0;
        break;
    }

    return DefWindowProc(hwnd, nMsg, wParam, lParam);
}

/*
 *  Windows passes all arguments to WinMain() as a single string.
 *  this breaks them up to look like a typical argc, argv on
 *  a DOS or *NIX program.
 *
 *  Returns the number of arguments found.
 *  You can set my_argv to NULL if you just want a count of the arguments.
 *
 */
int parse_cmdline(char* cmdline, char** my_argv) {

#define MAXARG 256
    char q;
    int arglen, argcnt;
    char arg[MAXARG + 1];
    int state = 0, pos = 0;

    if (my_argv != NULL) {
        my_argv[0] = malloc(MAXARG + 1);
        GetModuleFileName(NULL, my_argv[0], MAXARG);
    }

    argcnt = 1;
    arglen = 0;
    pos = 0;

    while (cmdline[pos] != 0) {
        switch (state) {
        case 0:
            switch (cmdline[pos]) {
            case '"':
            case '\'':
                q = cmdline[pos];
                state = 1;
                break;
            case ' ':
                if (arglen > 0 && argcnt < MAXARG) {
                    if (my_argv != NULL) {
                        my_argv[argcnt] =
                            (char*)malloc((arglen + 1) * sizeof(char));
                        strcpy(my_argv[argcnt], arg);
                    }
                    argcnt++;
                    arg[0] = 0;
                    arglen = 0;
                }
                break;
            default:
                if (arglen < MAXARG) {
                    arg[arglen] = cmdline[pos];
                    arglen++;
                    arg[arglen] = '\0';
                }
                break;
            }
            break;
        case 1:
            if (cmdline[pos] == q) {
                state = 0;
            }
            else {
                if (arglen < MAXARG) {
                    arg[arglen] = cmdline[pos];
                    arglen++;
                    arg[arglen] = '\0';
                }
            }
            break;
        }
        pos++;
    }
    if (arglen > 0 && argcnt < MAXARG) {
        if (my_argv != NULL) {
            my_argv[argcnt] = (char*)malloc((arglen + 1) * sizeof(char));
            strcpy(my_argv[argcnt], arg);
        }
        argcnt++;
    }

    return argcnt;
#undef MAXARG
}
