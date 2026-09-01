/*
** gen.h - Winamp general purpose plugin interface.
**
** Minimal reproduction of the struct from the Winamp 5.x SDK, containing only
** what this plugin needs. The layout must match the SDK exactly -- Winamp
** reads these fields by offset.
*/
#ifndef WA_GEN_H
#define WA_GEN_H

#include <windows.h>

/* Plugin header version. 0x10 = ANSI description, 0x11 = wide description
** (Winamp 5.5+). We use 0x10 so the plugin loads on older builds too. */
#define GPPHDR_VER 0x10

typedef struct {
    int version;          /* GPPHDR_VER */
    char *description;    /* name shown in the plugin list */
    int (*init)();        /* 0 = success, anything else aborts the load */
    void (*config)();     /* "Configure" button */
    void (*quit)();       /* called on unload */
    HWND hwndParent;      /* main Winamp window; filled in by Winamp */
    HINSTANCE hDllInstance;
} winampGeneralPurposePlugin;

#endif /* WA_GEN_H */
