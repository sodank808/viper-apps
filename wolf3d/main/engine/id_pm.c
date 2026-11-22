// ID_PM.C

// Viper: the page file (VSWAP: the walls, the sprites and the digitized
// sounds, 660 KB in the shareware) is read a page at a time as the game asks
// for one, into a cache of wolf3d_page_cache_size() bytes, instead of all at
// once: a device has about that much memory free for the whole game.
//
// A page stays valid until the frame it was asked for in has been shown
// (PM_NextFrame(), called by the platform as each frame goes out): the
// renderer holds on to the pages of the walls and sprites it draws until
// then. Pages from older frames are given up, least recently used first,
// when the cache is full; it goes over only when one frame uses more than it
// holds. The digitized sounds don't go through the cache: SD_PrepareSound()
// reads each into a buffer of its own (PM_ReadPages()), the first time it
// plays.

#include "wl_def.h"

word ChunksInFile;
word PMSpriteStart;
word PMSoundStart;

bool PMSoundInfoPagePadded = false;     // Viper: pages aren't padded here

word *pageLengths;

static FILE     *pageFile;
static uint32_t *pageOffsets;           // ChunksInFile + 1: the last is the file's size
static byte     **pageData;             // the cached pages, or NULL
static uint32_t *pageUsed;              // the frame each was last asked for in
static uint32_t pageFrame = 1;
static size_t   cacheUsed, cacheSize;

/*
==================
=
= PM_Startup
=
==================
*/

void PM_Startup (void)
{
    int      i;
    int32_t  filesize;
    char     fname[13];

    snprintf (fname,sizeof(fname),"vswap.%s",extension);

    pageFile = fopen(fname,"rb");
    if (!pageFile)
        CA_CannotOpen(fname);

    //
    // read in header variables
    //
    fread (&ChunksInFile,sizeof(ChunksInFile),1,pageFile);
    fread (&PMSpriteStart,sizeof(PMSpriteStart),1,pageFile);
    fread (&PMSoundStart,sizeof(PMSoundStart),1,pageFile);

    //
    // read in the chunk offsets
    //
    pageOffsets = SafeMalloc((ChunksInFile + 1) * sizeof(*pageOffsets));
    fread (pageOffsets,sizeof(*pageOffsets),ChunksInFile,pageFile);

    //
    // read in the chunk lengths
    //
    pageLengths = SafeMalloc(ChunksInFile * sizeof(*pageLengths));
    fread (pageLengths,sizeof(*pageLengths),ChunksInFile,pageFile);

    fseek (pageFile,0,SEEK_END);
    filesize = ftell(pageFile);
    pageOffsets[ChunksInFile] = filesize;

    //
    // check that all chunk offsets are valid
    //
    for (i = 0; i < ChunksInFile; i++)
    {
        if (!pageOffsets[i])
            continue;           // sparse page
        if (pageOffsets[i] < pageOffsets[0] || pageOffsets[i] >= (size_t)filesize)
            Quit ("PM_Startup: Illegal page offset for page %i: %u (filesize: %u)",i,pageOffsets[i],filesize);
    }

    pageData = SafeMalloc(ChunksInFile * sizeof(*pageData));
    memset (pageData,0,ChunksInFile * sizeof(*pageData));
    pageUsed = SafeMalloc(ChunksInFile * sizeof(*pageUsed));
    memset (pageUsed,0,ChunksInFile * sizeof(*pageUsed));
    cacheUsed = 0;
    cacheSize = wolf3d_page_cache_size();
    pageFrame = 1;
}

/*
==================
=
= PM_Shutdown
=
==================
*/

void PM_Shutdown (void)
{
    int i;

    if (pageData)
    {
        for (i = 0; i < ChunksInFile; i++)
            free (pageData[i]);
    }
    free (pageData);
    free (pageUsed);
    free (pageOffsets);
    free (pageLengths);
    pageData = NULL;
    pageUsed = NULL;
    pageOffsets = NULL;
    pageLengths = NULL;
    cacheUsed = 0;

    if (pageFile)
        fclose (pageFile);
    pageFile = NULL;
}

/*
==================
=
= PM_GetPageSize
=
==================
*/

uint32_t PM_GetPageSize (int page)
{
    if (page < 0 || page >= ChunksInFile)
        Quit ("PM_GetPageSize: Invalid page request: %i",page);

    if (!pageOffsets[page])
        return 0;               // sparse page

    //
    // use specified page length when next page is sparse
    // otherwise, calculate size from the offset difference between this and the next page
    //
    if (!pageOffsets[page + 1])
        return pageLengths[page];

    return pageOffsets[page + 1] - pageOffsets[page];
}

// Viper: gives up the least recently used pages of earlier frames until
// `size` more fits in the cache, or nothing more can go
static void PM_MakeRoom (size_t size)
{
    while (cacheUsed + size > cacheSize)
    {
        int i, oldest = -1;

        for (i = 0; i < ChunksInFile; i++)
        {
            if (pageData[i] && pageUsed[i] != pageFrame
                    && (oldest < 0 || pageUsed[i] < pageUsed[oldest]))
                oldest = i;
        }
        if (oldest < 0)
            return;             // all this frame's: go over

        cacheUsed -= PM_GetPageSize(oldest);
        free (pageData[oldest]);
        pageData[oldest] = NULL;
    }
}

/*
==================
=
= PM_GetPage
=
= Returns the address of the page
=
==================
*/

byte *PM_GetPage (int page)
{
    uint32_t size;

    if (page < 0 || page >= ChunksInFile)
        Quit ("PM_GetPage: Invalid page request: %i",page);

    pageUsed[page] = pageFrame;
    if (pageData[page])
        return pageData[page];

    // Viper: read it in. A sparse page is an empty one, as Wolf4SDL had it.
    size = PM_GetPageSize(page);
    PM_MakeRoom (size);
    pageData[page] = SafeMalloc(size ? size : 1);
    if (size)
    {
        fseek (pageFile,pageOffsets[page],SEEK_SET);
        if (fread (pageData[page],1,size,pageFile) != size)
            Quit ("PM_GetPage: Can't read page %i",page);
    }
    cacheUsed += size;

    return pageData[page];
}

// Viper: the pages the game asked for before now can be given up
void PM_NextFrame (void)
{
    pageFrame++;
}

// Viper: `length` bytes from the start of `page` on, across the pages after
// it, which follow it in the file: a digitized sound
void PM_ReadPages (int page, uint32_t length, byte *dest)
{
    if (page < 0 || page >= ChunksInFile || !pageOffsets[page]
            || pageOffsets[page] + length > pageOffsets[ChunksInFile])
        Quit ("PM_ReadPages: Invalid request: %i, %u bytes",page,length);

    fseek (pageFile,pageOffsets[page],SEEK_SET);
    if (fread (dest,1,length,pageFile) != length)
        Quit ("PM_ReadPages: Can't read page %i",page);
}
