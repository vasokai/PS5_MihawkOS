/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WOW_LOADER_H
#define PW_WOW_LOADER_H
/*
 * WoW game-image predicate from bizkut/ps5-forever-wine
 * ee64b784239b73f4ff7aa30adc8b3dcd1966ded0
 * (include/wine/ps5_title_private.h, wine_ps5_title_game_image).
 * That file is LGPL-2.1-or-later. This copy is GPL-3.0-or-later with the
 * rest of wine/ps5, which that licence permits.
 *
 * A drive-qualified Wow*.exe (C: or W:) is a game image when a sibling
 * <stem>_loader.dll exists. The derived spelling is tried first, then the
 * basename is lowercased without changing the directory:
 * Wow.exe -> wow_loader.dll, WowB.exe -> WowB_loader.dll,
 * WoWClassic.exe -> WowClassic_loader.dll.
 * The caller performs the filesystem check.
 */
#include <string.h>

static inline int pw_wow_game_image( const char *dos, const char *unix_image,
                                     int (*exists)( const char * ))
{
    const char *base, *dot, *slash;
    char loader[4096];
    size_t prefix, length;

    if (!dos || !unix_image || !exists ||
        !((dos[0] == 'C' || dos[0] == 'c' || dos[0] == 'W' || dos[0] == 'w') &&
          dos[1] == ':' && (dos[2] == '\\' || dos[2] == '/')))
        return 0;
    base = dos;
    for (const char *p = dos; *p; ++p) if (*p == '\\' || *p == '/') base = p + 1;
    length = strlen( base );
    if (length < 7 || !((base[0] | 32) == 'w' && (base[1] | 32) == 'o' &&
                        (base[2] | 32) == 'w')) return 0;
    dot = base + length - 4;
    if ((dot[0] | 32) != '.' || (dot[1] | 32) != 'e' ||
        (dot[2] | 32) != 'x' || (dot[3] | 32) != 'e') return 0;
    slash = strrchr( unix_image, '/' );
    prefix = slash ? (size_t)(slash + 1 - unix_image) : 0;
    length = strlen( slash ? slash + 1 : unix_image );
    if (length < 4 || prefix + length - 4 + sizeof("_loader.dll") > sizeof(loader)) return 0;
    memcpy( loader, unix_image, prefix + length - 4 );
    memcpy( loader + prefix + length - 4, "_loader.dll", sizeof("_loader.dll") );
    if (exists( loader )) return 1;
    for (size_t i = prefix; loader[i]; ++i)
        if (loader[i] >= 'A' && loader[i] <= 'Z') loader[i] += 'a' - 'A';
    return exists( loader );
}
#endif
