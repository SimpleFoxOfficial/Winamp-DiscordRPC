/*
** wa_ipc_min.h - the subset of Winamp's IPC interface used by this plugin.
**
** Values are taken verbatim from the Winamp 5.x SDK (wa_ipc.h). Only the
** messages we actually send are reproduced here so the plugin builds without
** requiring a full SDK checkout.
*/
#ifndef WA_IPC_MIN_H
#define WA_IPC_MIN_H

#include <windows.h>

#define WM_WA_IPC WM_USER

/* --- playback state ---------------------------------------------------- */

/* returns 1 playing, 3 paused, 0 stopped */
#define IPC_ISPLAYING 104

/* wParam 0 -> position in milliseconds, 1 -> track length in SECONDS.
** Returns -1 when unknown (e.g. a stream of indeterminate length). */
#define IPC_GETOUTPUTTIME 105

/* index of the currently playing entry in the playlist */
#define IPC_GETLISTPOS 125

#define IPC_GETLISTLENGTH 124

/* SendMessage(hwnd, WM_WA_IPC, index, IPC_GETPLAYLISTFILEW)
** -> wchar_t* into Winamp's own memory. Valid in-process only, and only
**    until Winamp reuses the buffer, so copy it immediately. */
#define IPC_GETPLAYLISTFILEW 214

/* same contract, but the formatted playlist title ("Artist - Title") */
#define IPC_GETPLAYLISTTITLEW 213

/* returns the Winamp version as 0x5092 style BCD */
#define IPC_GETVERSION 0

/* directory holding winamp.ini -- wchar_t*, in-process only */
#define IPC_GETINIDIRECTORYW 1335

/* --- metadata ---------------------------------------------------------- */

/* (Winamp 5.13+) pass a pointer to extendedFileInfoStructW in wParam.
** Returns 1 if the input plugin implements getExtendedFileInfo. */
#define IPC_GET_EXTENDED_FILE_INFOW 3026

/* ANSI equivalent (Winamp 2.9+), used as a fallback */
#define IPC_GET_EXTENDED_FILE_INFO 290

typedef struct {
    const char *filename;
    const char *metadata;
    char *ret;
    size_t retlen;
} extendedFileInfoStruct;

typedef struct {
    const wchar_t *filename;
    const wchar_t *metadata;
    wchar_t *ret;
    size_t retlen;
} extendedFileInfoStructW;

#endif /* WA_IPC_MIN_H */
