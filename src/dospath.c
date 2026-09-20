/*
 * dospath.c — _splitpath/_makepath, which the engine expects from the Watcom
 * runtime. FILES.C's AddExt() and DISKFUNC.C take a path apart and put it back
 * together to swap an extension, so these have to round-trip exactly, including
 * the DOS backslash.
 *
 * Same behaviour as the MinGW versions the PC reference build gets for free.
 */

#include <string.h>
#include <ctype.h>

void _splitpath(const char *path, char *drive, char *dir,
                char *fname, char *ext)
{
    const char *p, *slash, *dot, *body;

    if (drive) drive[0] = 0;
    if (dir)   dir[0] = 0;
    if (fname) fname[0] = 0;
    if (ext)   ext[0] = 0;
    if (!path) return;

    p = path;
    if (isalpha((unsigned char)p[0]) && p[1] == ':') {
        if (drive) {
            drive[0] = p[0];
            drive[1] = ':';
            drive[2] = 0;
        }
        p += 2;
    }

    slash = 0;
    for (body = p; *body; body++)
        if (*body == '/' || *body == '\\')
            slash = body;

    if (slash) {
        if (dir) {
            size_t n = (size_t)(slash - p) + 1;
            if (n > 255) n = 255;
            memcpy(dir, p, n);
            dir[n] = 0;
        }
        p = slash + 1;
    }

    dot = 0;
    for (body = p; *body; body++)
        if (*body == '.')
            dot = body;

    if (dot) {
        if (fname) {
            size_t n = (size_t)(dot - p);
            if (n > 255) n = 255;
            memcpy(fname, p, n);
            fname[n] = 0;
        }
        if (ext) {
            strncpy(ext, dot, 255);
            ext[255] = 0;
        }
    } else if (fname) {
        strncpy(fname, p, 255);
        fname[255] = 0;
    }
}

void _makepath(char *path, const char *drive, const char *dir,
               const char *fname, const char *ext)
{
    path[0] = 0;

    if (drive && drive[0]) {
        strcat(path, drive);
        if (path[strlen(path) - 1] != ':')
            strcat(path, ":");
    }
    if (dir && dir[0]) {
        strcat(path, dir);
        if (path[strlen(path) - 1] != '/' && path[strlen(path) - 1] != '\\')
            strcat(path, "\\");
    }
    if (fname)
        strcat(path, fname);
    if (ext && ext[0]) {
        if (ext[0] != '.')
            strcat(path, ".");
        strcat(path, ext);
    }
}
