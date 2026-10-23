/* SPDX-License-Identifier: GPL-3.0-or-later */
/* wine_ps5_title_game_image from ps5-forever-wine ee64b784, as pw_wow_game_image. */
#include "../wine/ps5/pw_wow_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char seen[8][4096];
static int seen_count;
static const char *present[8];
static int present_count;

static int exists( const char *path )
{
    int i;

    if (seen_count < 8) snprintf( seen[seen_count++], sizeof(seen[0]), "%s", path );
    for (i = 0; i < present_count; i++)
        if (!strcmp( present[i], path )) return 1;
    return 0;
}

static void expect( const char *what, int got, int want )
{
    if (got == want) return;
    fprintf( stderr, "%s: got %d want %d\n", what, got, want );
    exit( 1 );
}

static void expect_seen( const char *what, int index, const char *path )
{
    if (index < seen_count && !strcmp( seen[index], path )) return;
    fprintf( stderr, "%s: seen[%d]=\"%s\" want \"%s\"\n", what, index,
             index < seen_count ? seen[index] : "(missing)", path );
    exit( 1 );
}

static void reset( void )
{
    seen_count = 0;
    present_count = 0;
}

int main( void )
{
    reset();
    present[present_count++] = "/data/wow/_classic_era_/WowClassic_loader.dll";
    expect( "classic derived",
            pw_wow_game_image( "C:\\Program Files (x86)\\World of Warcraft\\_classic_era_\\WoWClassic.exe",
                               "/data/wow/_classic_era_/WowClassic.exe", exists ), 1 );
    expect( "one lookup", seen_count, 1 );
    expect_seen( "classic path", 0, "/data/wow/_classic_era_/WowClassic_loader.dll" );

    reset();
    present[present_count++] = "/data/wow/_classic_beta_/WowB_loader.dll";
    expect( "beta derived",
            pw_wow_game_image( "C:\\Wow\\_classic_beta_\\WowB.exe",
                               "/data/wow/_classic_beta_/WowB.exe", exists ), 1 );
    expect_seen( "beta path", 0, "/data/wow/_classic_beta_/WowB_loader.dll" );

    reset();
    present[present_count++] = "/data/wow/_forever_/wow_loader.dll";
    expect( "final lowercase",
            pw_wow_game_image( "W:/World of Warcraft/_forever_/Wow.exe",
                               "/data/wow/_forever_/Wow.exe", exists ), 1 );
    expect( "two lookups", seen_count, 2 );
    expect_seen( "derived Wow_loader", 0, "/data/wow/_forever_/Wow_loader.dll" );
    expect_seen( "lowercase wow_loader", 1, "/data/wow/_forever_/wow_loader.dll" );

    reset();
    present[present_count++] = "/Data/Games/wowclassic_loader.dll";
    expect( "directory case stays",
            pw_wow_game_image( "c:/Games/WoWClassic.exe",
                               "/Data/Games/WoWClassic.exe", exists ), 1 );
    expect_seen( "dir kept", 1, "/Data/Games/wowclassic_loader.dll" );

    reset();
    expect( "other drive",
            pw_wow_game_image( "D:\\Wow.exe", "/data/Wow.exe", exists ), 0 );
    expect( "not wow",
            pw_wow_game_image( "C:\\NotWow.exe", "/data/NotWow.exe", exists ), 0 );
    expect( "no loader",
            pw_wow_game_image( "C:\\Wow.exe", "/data/Wow.exe", exists ), 0 );
    expect( "missing args", pw_wow_game_image( NULL, "/data/Wow.exe", exists ), 0 );
    expect( "relative dos", pw_wow_game_image( "Wow.exe", "/data/Wow.exe", exists ), 0 );
    return 0;
}
