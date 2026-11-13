/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Wire protocol between the browser UI app (webOS NDK, kit) and wpe-engine (glibc 2.28 toolchain, WPE WebKit).
 *
 * Transport: one AF_UNIX SOCK_SEQPACKET socket (the app's end of a socketpair; the engine gets its end as fd 3).
 * Every message is one packet: a WpeMsgHeader followed by the payload struct named for its type. Both
 * processes run on the same machine with the same ABI (32-bit ARM on the TV, the host ABI in host tests),
 * so structs are sent as-is; every field has a fixed width and the structs have no implicit padding.
 *
 * Frames: the engine creates one memfd holding WPE_IPC_FRAME_SLOTS slots of `slot_bytes` each and sends it
 * once with ENGINE_FRAMES (fd attached as SCM_RIGHTS). For each frame it writes the pixels into a free slot
 * and sends ENGINE_FRAME. The app uploads the slot and answers APP_FRAME_DONE; only then may the engine reuse
 * that slot and tell WebKit the frame was shown (this paces WebKit to the TV's display rate).
 * Tabs: every message carries a tab id in its header. The app opens tabs with APP_TAB_OPEN (one WebKit view
 * each, sharing one web process) and chooses the visible one with APP_TAB_ACTIVATE; navigation commands go to
 * the tab named in the header, input always goes to the active tab, and only the active tab renders frames.
 * Tab id 0 means "no tab" (engine-wide messages such as READY and FRAMES).
 *
 * Pixels: WPE_PIXEL_FORMAT_ARGB8888 as WebKit produces it: 32-bit premultiplied ARGB in native (little-endian)
 * order, i.e. bytes B,G,R,A in memory.
 */
#ifndef WPE_IPC_H
#define WPE_IPC_H

#include <stdint.h>

#define WPE_IPC_VERSION 10u
#define WPE_IPC_ENGINE_FD 3
#define WPE_IPC_FRAME_SLOTS 3u
#define WPE_IPC_MAX_TEXT 2048u   /* bytes including the terminating NUL */
#define WPE_IPC_MAX_DAMAGE 8u    /* damage rectangles per frame; more are merged into their bounding box */
#define WPE_IPC_MAX_PACKET 4096u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* app -> engine */
    WPE_APP_HELLO = 1,        /* WpeAppHello: first message */
    WPE_APP_LOAD_URI,         /* WpeText */
    WPE_APP_GO_BACK,          /* no payload */
    WPE_APP_GO_FORWARD,
    WPE_APP_RELOAD,
    WPE_APP_STOP,
    WPE_APP_RESIZE,           /* WpeSize: page size in CSS pixels at scale 1 */
    WPE_APP_VISIBLE,          /* WpeFlag: page shown (1) or hidden (0, e.g. app in background) */
    WPE_APP_FOCUS,            /* WpeFlag: keyboard focus on the page (1) or on the app's chrome (0) */
    WPE_APP_KEY,              /* WpeKey */
    WPE_APP_POINTER,          /* WpePointer */
    WPE_APP_SCROLL,           /* WpeScroll */
    WPE_APP_TEXT_COMMIT,      /* WpeText: text from the TV keyboard for the focused editable */
    WPE_APP_FRAME_DONE,       /* WpeFrameDone */
    WPE_APP_RUN_JS,           /* WpeText: script for the main frame; result ignored */
    WPE_APP_QUIT,             /* no payload: shut down cleanly */
    WPE_APP_TAB_OPEN,         /* WpeText: first URI (may be empty); header.tab = the app's new id */
    WPE_APP_TAB_CLOSE,        /* no payload: destroy the tab's view */
    WPE_APP_TAB_ACTIVATE,     /* no payload: show this tab, hide the others */
    WPE_APP_CLEAR_DATA,       /* WpeClearData: delete website data (cookies, site storage, caches) */
    WPE_APP_SET_ZOOM,         /* WpeZoom: page zoom for every tab, now and later */
    WPE_APP_SET_SCALE,        /* WpeZoom: device scale for every tab (1500 = render 1.5x the CSS size) */
    WPE_APP_SET_LAYER,        /* WpeText: direct presentation: the surface-group layer pages attach to from now on
                                 (a new one after a web-process crash; the dead process's surface can keep the old) */

    /* engine -> app */
    WPE_ENGINE_READY = 100,   /* WpeEngineReady */
    WPE_ENGINE_FRAMES,        /* WpeFrames + memfd in SCM_RIGHTS */
    WPE_ENGINE_FRAME,         /* WpeFrame */
    WPE_ENGINE_URI,           /* WpeText */
    WPE_ENGINE_TITLE,         /* WpeText */
    WPE_ENGINE_PROGRESS,      /* WpeProgress */
    WPE_ENGINE_LOAD,          /* WpeLoad */
    WPE_ENGINE_HISTORY,       /* WpeHistory */
    WPE_ENGINE_IME,           /* WpeIme: an editable got or lost focus */
    WPE_ENGINE_ERROR,         /* WpeError */
    WPE_ENGINE_DATA_CLEARED,  /* WpeDataCleared: answer to APP_CLEAR_DATA */
    WPE_ENGINE_PRESENT,       /* WpePresent: how the tab's page reaches the screen (sent at start and on changes) */
    WPE_ENGINE_SCROLL,        /* WpeFlag: 1 the page is scrolled to its top, 0 it is not (sent on changes) */
    WPE_ENGINE_TAB_CREATED,   /* WpeTabCreated: a page opened a window (window.open, target=_blank); header.tab = the
                                 engine's id for it (WPE_ENGINE_TAB_ID_BASE and up). The UI adds and shows the tab. */
    WPE_ENGINE_TAB_CLOSE,     /* no payload: the page asked to close its window (window.close()) */
    WPE_ENGINE_FULLSCREEN,    /* WpeFlag: the page entered (1) or left (0) fullscreen (Fullscreen API, a video's
                                 fullscreen button); the UI hides its chrome meanwhile */
    WPE_ENGINE_EDGE,          /* WpeFlag: Up (1) reached the page's top edge: spatial navigation found nothing above,
                                 the page did not scroll and did not handle the key itself (the UI shows its toolbar) */
    WPE_ENGINE_ICON,          /* WpeText: the page's best icon URL (apple-touch-icon, else a PNG/SVG/WebP icon link;
                                 empty if it declares none). The UI downloads it itself (WPE has no favicon API). */
} WpeMsgType;

/* Tabs the engine opens itself (popups) get ids from here, so they never collide with the UI's. */
#define WPE_ENGINE_TAB_ID_BASE 0x40000000u

typedef struct {
    uint32_t opener; /* the tab whose page opened it (0 if unknown) */
} WpeTabCreated;

typedef struct {
    uint32_t type; /* WpeMsgType */
    uint32_t size; /* payload bytes after this header */
    uint32_t tab;  /* tab id, 0 = none */
} WpeMsgHeader;

typedef struct {
    uint32_t version;  /* WPE_IPC_VERSION */
    uint32_t width;    /* initial page size, CSS px */
    uint32_t height;
    uint32_t reserved;
} WpeAppHello;

typedef struct {
    uint32_t version;
    uint32_t webkit_major, webkit_minor, webkit_micro;
} WpeEngineReady;

typedef struct {
    uint32_t length;               /* bytes, without the NUL */
    char text[WPE_IPC_MAX_TEXT];   /* NUL-terminated; only length+1 bytes need to be sent */
} WpeText;

typedef struct { uint32_t width, height; } WpeSize;
typedef struct { uint32_t value; } WpeFlag;

typedef struct {
    uint32_t down;       /* 1 press (or repeat), 0 release */
    uint32_t keyval;     /* XKB keysym (WPE_KEY_*) */
    uint32_t keycode;    /* evdev code + 8 (XKB keycode), 0 if unknown */
    uint32_t modifiers;  /* WPEModifiers keyboard bits */
} WpeKey;

typedef enum { WPE_POINTER_MOVE = 0, WPE_POINTER_DOWN = 1, WPE_POINTER_UP = 2, WPE_POINTER_LEAVE = 3 } WpePointerKind;

typedef struct {
    uint32_t kind;       /* WpePointerKind */
    uint32_t button;     /* 1 left, 2 middle, 3 right */
    int32_t x, y;        /* page CSS px */
    uint32_t time_ms;
} WpePointer;

typedef struct {
    int32_t x, y;        /* page CSS px under the pointer */
    int32_t delta_x;     /* in 1/120 notches like wheel events; positive = right/down */
    int32_t delta_y;
} WpeScroll;

typedef struct {
    uint32_t slot_bytes; /* each slot holds up to slot_bytes of pixels (stride * height) */
    uint32_t slots;      /* WPE_IPC_FRAME_SLOTS */
} WpeFrames;

typedef struct { int32_t x, y, width, height; } WpeRect;

typedef struct {
    uint32_t slot;
    uint32_t serial;     /* increases by one per frame */
    uint32_t width, height, stride;
    uint32_t n_damage;   /* 0 = whole frame */
    WpeRect damage[WPE_IPC_MAX_DAMAGE];
} WpeFrame;

typedef struct { uint32_t slot, serial; } WpeFrameDone;

typedef struct { uint32_t permille; } WpeProgress; /* 0..1000 */

typedef enum { WPE_LOAD_STARTED = 0, WPE_LOAD_COMMITTED = 1, WPE_LOAD_FINISHED = 2, WPE_LOAD_FAILED = 3 } WpeLoadEvent;
typedef struct { uint32_t event; } WpeLoad;

typedef struct { uint32_t can_go_back, can_go_forward; } WpeHistory;

typedef struct {
    uint32_t focused;    /* 1 an editable has focus: show the TV keyboard; 0 hide it */
    uint32_t purpose;    /* WPEInputPurpose (0 free form, URL, email, number, password, ...) */
    int32_t x, y, width, height; /* caret/field area in page CSS px, or zeros */
} WpeIme;

typedef enum { WPE_ERROR_LOAD = 1, WPE_ERROR_WEB_PROCESS_CRASHED = 2, WPE_ERROR_TLS = 3, WPE_ERROR_FATAL = 4 } WpeErrorKind;
typedef struct {
    uint32_t kind;       /* WpeErrorKind */
    uint32_t code;       /* GError code where there is one */
    WpeText message;     /* human-readable, may be empty */
} WpeError;

/* What APP_CLEAR_DATA deletes (bit mask). */
#define WPE_CLEAR_COOKIES_AND_SITE_DATA 1u  /* cookies, local/session storage, IndexedDB, service workers, HSTS */
#define WPE_CLEAR_CACHE 2u                  /* memory and disk caches */

typedef struct {
    uint32_t what;     /* WPE_CLEAR_* bits */
    uint32_t seconds;  /* only data from the last `seconds`; 0 = all time */
} WpeClearData;

typedef struct {
    uint32_t what;
    uint32_t ok;       /* 1 when WebKit reported success */
} WpeDataCleared;

typedef struct { uint32_t permille; } WpeZoom; /* 1000 = 100 % */

/* Page presentation. SHM: the engine sends WPE_ENGINE_FRAME and the UI draws it. DIRECT (UI started the engine
 * with WPE_BROWSER_PRESENT=direct): the web process draws into its own surface in the UI's surface group; the UI
 * sends no FRAME_DONE and leaves the page area of its window transparent once status is FIRST_FRAME (3).
 * A negative status means direct presentation failed for that tab (restart the engine in SHM mode). */
typedef enum { WPE_PRESENT_SHM = 0, WPE_PRESENT_DIRECT = 1 } WpePresentMode;
typedef struct {
    uint32_t mode;     /* WpePresentMode */
    int32_t status;    /* DIRECT: wpeb_webos_status (engine/webos-backend/protocol.h); 0 at start */
    uint32_t width, height; /* DIRECT: surface size in pixels, when known */
} WpePresent;

#ifdef __cplusplus
}
#endif

#endif /* WPE_IPC_H */
