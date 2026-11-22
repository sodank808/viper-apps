//
//      ID Engine
//      ID_SD.c - Sound Manager for Wolfenstein 3D
//      v1.2
//      By Jason Blochowiak
//

//
//      This module handles dealing with generating sound on the appropriate
//              hardware
//
//      Depends on: User Mgr (for parm checking)
//
//      Globals:
//              For User Mgr:
//                      SoundBlasterPresent - SoundBlaster card present?
//                      AdLibPresent - AdLib card present?
//                      SoundMode - What device is used for sound effects
//                              (Use SM_SetSoundMode() to set)
//                      MusicMode - What device is used for music
//                              (Use SM_SetMusicMode() to set)
//                      DigiMode - What device is used for digitized sound effects
//                              (Use SM_SetDigiDevice() to set)
//
//              For Cache Mgr:
//                      NeedsDigitized - load digitized sounds?
//                      NeedsMusic - load music?
//

#include "wl_def.h"

// Viper: no SDL_mixer. The engine's thread mixes (SD_Mix()) an OPL2, DOSBox's
// (dosbox/dbopl.cpp, by way of wolf3d_opl.cpp), for the AdLib sound effects
// and music, and channels of digitized sound, which it reads from the page
// file as they are, 8 bits at 7042 Hz.

#define ORIGSAMPLERATE 7042

// A digitized sound, prepared (SD_PrepareSound()) for the mixer
typedef struct
{
    byte     *data;                 // read in the first time it plays
    int      page;                  // where it starts in the page file
    longword length;
} DigiSound;

// A mixer channel. 0 and 1 are kept for the player's and the bosses'
// weapons (DigiChannel[]); the rest are taken by the sounds that ask for
// any channel, the oldest given up when none is free.
typedef struct
{
    DigiSound *sound;
    longword  pos;                  // 16.16, in the sound's samples
    int       leftvol, rightvol;    // 0-256
    longword  started;              // for the oldest
} MixChannel;

static DigiSound  SoundChunks[ STARTMUSIC - STARTDIGISOUNDS];
static MixChannel MixChannels[MIX_CHANNELS];
static longword   MixStarted;

globalsoundpos channelSoundPos[MIX_CHANNELS];

//      Global variables
        boolean         AdLibPresent,
                        SoundBlasterPresent,SBProPresent,
                        SoundPositioned;
        byte            SoundMode;
        byte            MusicMode;
        byte            DigiMode;
static  byte          **SoundTable;
        int             DigiMap[LASTSOUND];
        int             DigiChannel[STARTMUSIC - STARTDIGISOUNDS];

//      Internal variables
static  boolean                 SD_Started;
static  boolean                 nextsoundpos;
static  int                     SoundNumber;
static  int                     DigiNumber;
static  word                    SoundPriority;
static  word                    DigiPriority;
static  int                     LeftPosition;
static  int                     RightPosition;

        word                    NumDigi;
        digiinfo                *DigiList;
static  boolean                 DigiPlaying;

//      PC Sound variables
static  volatile byte           pcLastSample;
static  byte * volatile         pcSound;
static  longword                pcLengthLeft;

//      AdLib variables
static  byte * volatile         alSound;
static  byte                    alBlock;
static  longword                alLengthLeft;
static  longword                alTimeCount;
static  Instrument              alZeroInst;

//      Sequencer variables
static  volatile boolean        sqActive;
static  word                   *sqHack;
static  word                   *sqHackPtr;
static  int                     sqHackLen;
static  int                     sqHackSeqLen;
static  longword                sqHackTime;

// Viper: the OPL is the platform's (wolf3d_platform.h)
#define YM3812Write(chip, reg, val) wolf3d_opl_write(reg, val)
static const int oplChip = 0;

void Delay (int32_t wolfticks)
{
    if (wolfticks > 0)
        SDL_Delay ((wolfticks * 100) / 7);
}


static void SDL_SoundFinished(void)
{
	SoundNumber = 0;
	SoundPriority = 0;
}

///////////////////////////////////////////////////////////////////////////
//
//      SDL_PCPlaySound() - Plays the specified sound on the PC speaker
//
///////////////////////////////////////////////////////////////////////////
static void
SDL_PCPlaySound(PCSound *sound)
{
        pcLastSample = (byte)-1;
        pcLengthLeft = sound->common.length;
        pcSound = sound->data;
}

///////////////////////////////////////////////////////////////////////////
//
//      SDL_PCStopSound() - Stops the current sound playing on the PC Speaker
//
///////////////////////////////////////////////////////////////////////////
static void
SDL_PCStopSound(void)
{
        pcSound = 0;
}

///////////////////////////////////////////////////////////////////////////
//
//      SDL_ShutPC() - Turns off the pc speaker
//
///////////////////////////////////////////////////////////////////////////
static void
SDL_ShutPC(void)
{
        pcSound = 0;
}

void
SD_StopDigitized(void)
{
    DigiPlaying = false;
    DigiNumber = 0;
    DigiPriority = 0;
    SoundPositioned = false;
    if ((DigiMode == sds_PC) && (SoundMode == sdm_PC))
        SDL_SoundFinished();

    switch (DigiMode)
    {
        case sds_PC:
            SDL_PCStopSound();
            break;
        case sds_SoundBlaster:
            memset(MixChannels,0,sizeof(MixChannels));      // Viper
            for (int i = 0; i < MIX_CHANNELS; i++)
                channelSoundPos[i].valid = 0;
            break;

        default:
            break;
    }
}

// Viper: a free channel past the two kept for weapons, or the one playing
// longest
static int MixGroupChannel (void)
{
    int i, oldest = -1;

    for (i = 2; i < MIX_CHANNELS; i++)
    {
        if (!MixChannels[i].sound)
            return i;
        if (oldest < 0 || MixChannels[i].started < MixChannels[oldest].started)
            oldest = i;
    }
    return oldest;
}

int SD_GetChannelForDigi(int which)
{
    if(DigiChannel[which] != -1) return DigiChannel[which];

    return MixGroupChannel();
}

void SD_SetPosition(int channel, int leftpos, int rightpos)
{
    if((leftpos < 0) || (leftpos > 15) || (rightpos < 0) || (rightpos > 15)
            || ((leftpos == 15) && (rightpos == 15)))
        Quit("SD_SetPosition: Illegal position");

    switch (DigiMode)
    {
        case sds_SoundBlaster:
            // Viper: SDL_mixer's panning, 15-255 of 255
            MixChannels[channel].leftvol = ((((15 - leftpos) << 4) + 15) * 256) / 255;
            MixChannels[channel].rightvol = ((((15 - rightpos) << 4) + 15) * 256) / 255;
            break;
        default:
            break;
    }
}

void SD_PrepareSound(int which)
{
    if(DigiList == NULL)
        Quit("SD_PrepareSound(%i): DigiList not initialized!\n", which);

    // Viper: read in the first time it plays (SD_PlayDigitized()), and
    // played as it is
    SoundChunks[which].data = NULL;
    SoundChunks[which].page = PMSoundStart + DigiList[which].startpage;
    SoundChunks[which].length = DigiList[which].length;
}

int SD_PlayDigitized(word which,int leftpos,int rightpos)
{
    if (!DigiMode)
        return 0;

    if (which >= NumDigi)
        Quit("SD_PlayDigitized: bad sound number %i", which);

    int channel = SD_GetChannelForDigi(which);
    SD_SetPosition(channel, leftpos,rightpos);

    DigiPlaying = true;

    DigiSound *sample = &SoundChunks[which];
    if(sample->length == 0)
    {
        printf("SoundChunks[%i] is empty!\n", which);
        return 0;
    }
    if(sample->data == NULL)
    {
        sample->data = SafeMalloc(sample->length);
        PM_ReadPages(sample->page, sample->length, sample->data);
    }

    // Viper: from the start, on the channel
    channelSoundPos[channel].valid = 0;
    MixChannels[channel].sound = sample;
    MixChannels[channel].pos = 0;
    MixChannels[channel].started = ++MixStarted;

    return channel;
}

void SD_ChannelFinished(int channel)
{
    channelSoundPos[channel].valid = 0;
}

void
SD_SetDigiDevice(byte mode)
{
    boolean devicenotpresent;

    if (mode == DigiMode)
        return;

    SD_StopDigitized();

    devicenotpresent = false;
    switch (mode)
    {
        case sds_SoundBlaster:
            if (!SoundBlasterPresent)
                devicenotpresent = true;
            break;

        default:
            break;
    }

    if (!devicenotpresent)
    {
        DigiMode = mode;
    }
}

void
SDL_SetupDigi(void)
{
    // Correct padding enforced by PM_Startup()
    word *soundInfoPage = (word *) (void *) PM_GetPage(ChunksInFile-1);
    NumDigi = (word) PM_GetPageSize(ChunksInFile - 1) / 4;

    DigiList = SafeMalloc(NumDigi * sizeof(*DigiList));
    int i,page;
    for(i = 0; i < NumDigi; i++)
    {
        // Calculate the size of the digi from the sizes of the pages between
        // the start page and the start page of the next sound

        DigiList[i].startpage = soundInfoPage[i * 2];
        if((int) DigiList[i].startpage >= ChunksInFile - 1)
        {
            NumDigi = i;
            break;
        }

        int lastPage;
        if(i < NumDigi - 1)
        {
            lastPage = soundInfoPage[i * 2 + 2];
            if(lastPage == 0 || lastPage + PMSoundStart > ChunksInFile - 1) lastPage = ChunksInFile - 1;
            else lastPage += PMSoundStart;
        }
        else lastPage = ChunksInFile - 1;

        int size = 0;
        for(page = PMSoundStart + DigiList[i].startpage; page < lastPage; page++)
            size += PM_GetPageSize(page);

        // Don't include padding of sound info page, if padding was added
        if(lastPage == ChunksInFile - 1 && PMSoundInfoPagePadded) size--;

        // Patch lower 16-bit of size with size from sound info page.
        // The original VSWAP contains padding which is included in the page size,
        // but not included in the 16-bit size. So we use the more precise value.
        if((size & 0xffff0000) != 0 && (size & 0xffff) < soundInfoPage[i * 2 + 1])
            size -= 0x10000;
        size = (size & 0xffff0000) | soundInfoPage[i * 2 + 1];

        DigiList[i].length = size;
    }

    for(i = 0; i < LASTSOUND; i++)
    {
        DigiMap[i] = -1;
        DigiChannel[i] = -1;
    }
}

//      AdLib Code

///////////////////////////////////////////////////////////////////////////
//
//      SDL_ALStopSound() - Turns off any sound effects playing through the
//              AdLib card
//
///////////////////////////////////////////////////////////////////////////
static void
SDL_ALStopSound(void)
{
    alSound = 0;
    alOut(alFreqH + 0, 0);
}

static void
SDL_AlSetFXInst(Instrument *inst)
{
    byte c,m;

    m = 0;      // modulator cell for channel 0
    c = 3;      // carrier cell for channel 0
    alOut(m + alChar,inst->mChar);
    alOut(m + alScale,inst->mScale);
    alOut(m + alAttack,inst->mAttack);
    alOut(m + alSus,inst->mSus);
    alOut(m + alWave,inst->mWave);
    alOut(c + alChar,inst->cChar);
    alOut(c + alScale,inst->cScale);
    alOut(c + alAttack,inst->cAttack);
    alOut(c + alSus,inst->cSus);
    alOut(c + alWave,inst->cWave);

    // Note: Switch commenting on these lines for old MUSE compatibility
//    alOutInIRQ(alFeedCon,inst->nConn);
    alOut(alFeedCon,0);
}

///////////////////////////////////////////////////////////////////////////
//
//      SDL_ALPlaySound() - Plays the specified sound on the AdLib card
//
///////////////////////////////////////////////////////////////////////////
static void
SDL_ALPlaySound(AdLibSound *sound)
{
    Instrument      *inst;
    byte            *data;

    SDL_ALStopSound();

    alLengthLeft = sound->common.length;
    data = sound->data;
    alBlock = ((sound->block & 7) << 2) | 0x20;
    inst = &sound->inst;

    if (!(inst->mSus | inst->cSus))
    {
        Quit("SDL_ALPlaySound() - Bad instrument");
    }

    SDL_AlSetFXInst(inst);
    alSound = (byte *)data;
}

///////////////////////////////////////////////////////////////////////////
//
//      SDL_ShutAL() - Shuts down the AdLib card for sound effects
//
///////////////////////////////////////////////////////////////////////////
static void
SDL_ShutAL(void)
{
    alSound = 0;
    alOut(alEffects,0);
    alOut(alFreqH + 0,0);
    SDL_AlSetFXInst(&alZeroInst);
}


///////////////////////////////////////////////////////////////////////////
//
//      SDL_StartAL() - Starts up the AdLib card for sound effects
//
///////////////////////////////////////////////////////////////////////////
static void
SDL_StartAL(void)
{
    alOut(alEffects, 0);
    SDL_AlSetFXInst(&alZeroInst);
}


////////////////////////////////////////////////////////////////////////////
//
//      SDL_ShutDevice() - turns off whatever device was being used for sound fx
//
////////////////////////////////////////////////////////////////////////////
static void
SDL_ShutDevice(void)
{
    switch (SoundMode)
    {
        case sdm_PC:
            SDL_ShutPC();
            break;
        case sdm_AdLib:
            SDL_ShutAL();
            break;

        default:
            break;
    }
    SoundMode = sdm_Off;
}


///////////////////////////////////////////////////////////////////////////
//
//      SDL_StartDevice() - turns on whatever device is to be used for sound fx
//
///////////////////////////////////////////////////////////////////////////
static void
SDL_StartDevice(void)
{
    switch (SoundMode)
    {
        case sdm_AdLib:
            SDL_StartAL();
            break;

        default:
            break;
    }
    SoundNumber = 0;
    SoundPriority = 0;
}

//      Public routines

///////////////////////////////////////////////////////////////////////////
//
//      SD_SetSoundMode() - Sets which sound hardware to use for sound effects
//
///////////////////////////////////////////////////////////////////////////
boolean
SD_SetSoundMode(byte mode)
{
    boolean result = false;
    word    tableoffset;

    SD_StopSound();

    if ((mode == sdm_AdLib) && !AdLibPresent)
        mode = sdm_PC;

    switch (mode)
    {
        case sdm_Off:
            tableoffset = STARTADLIBSOUNDS;
            result = true;
            break;
        case sdm_PC:
            tableoffset = STARTPCSOUNDS;
            result = true;
            break;
        case sdm_AdLib:
            tableoffset = STARTADLIBSOUNDS;
            if (AdLibPresent)
                result = true;
            break;
        default:
            Quit("SD_SetSoundMode: Invalid sound mode %i", mode);
            return false;
    }
    SoundTable = &audiosegs[tableoffset];

    if (result && (mode != SoundMode))
    {
        SDL_ShutDevice();
        SoundMode = mode;
        SDL_StartDevice();
    }

    return(result);
}

///////////////////////////////////////////////////////////////////////////
//
//      SD_SetMusicMode() - sets the device to use for background music
//
///////////////////////////////////////////////////////////////////////////
boolean
SD_SetMusicMode(byte mode)
{
    boolean result = false;

    SD_FadeOutMusic();
    while (SD_MusicPlaying())
        SDL_Delay(5);

    switch (mode)
    {
        case smm_Off:
            result = true;
            break;
        case smm_AdLib:
            if (AdLibPresent)
                result = true;
            break;
    }

    if (result)
        MusicMode = mode;

    return(result);
}

int numreadysamples = 0;
byte *curAlSound = 0;
byte *curAlSoundPtr = 0;
longword curAlLengthLeft = 0;
int soundTimeCounter = 5;
longword samplesPerMusicTick;       // Viper: 16.16, for 700 Hz exactly
longword musicTickFrac;

// Viper: the AdLib sound effects (at 140 Hz) and music (700 Hz), a tick at a
// time; SDL_IMFMusicPlayer()'s steps
static void SDL_IMFTick(void)
{
    soundTimeCounter--;
    if(!soundTimeCounter)
    {
        soundTimeCounter = 5;
        if(curAlSound != alSound)
        {
            curAlSound = curAlSoundPtr = alSound;
            curAlLengthLeft = alLengthLeft;
        }
        if(curAlSound)
        {
            if(*curAlSoundPtr)
            {
                alOut(alFreqL, *curAlSoundPtr);
                alOut(alFreqH, alBlock);
            }
            else alOut(alFreqH, 0);
            curAlSoundPtr++;
            curAlLengthLeft--;
            if(!curAlLengthLeft)
            {
                curAlSound = alSound = 0;
                SoundNumber = 0;
                SoundPriority = 0;
                alOut(alFreqH, 0);
            }
        }
    }
    if(sqActive)
    {
        do
        {
            if(sqHackTime > alTimeCount) break;
            sqHackTime = alTimeCount + *(sqHackPtr+1);
            alOut(*(byte *) sqHackPtr, *(((byte *) sqHackPtr)+1));
            sqHackPtr += 2;
            sqHackLen -= 4;
        }
        while(sqHackLen>0);
        alTimeCount++;
        if(!sqHackLen)
        {
            sqHackPtr = sqHack;
            sqHackLen = sqHackSeqLen;
            sqHackTime = 0;
            alTimeCount = 0;
        }
    }
}

// Viper: `frames` stereo frames at WOLF3D_MIX_RATE: the OPL, then the
// digitized sounds over it. Called by the app, on the engine's thread.
void SD_Mix(int16_t *stereo, int frames)
{
    static int32_t mono[256];
    int i, c;

    while (frames > 0)
    {
        int n = frames < (int) lengthof(mono) ? frames : (int) lengthof(mono);
        int done = 0;

        memset(mono, 0, n * sizeof(*mono));
        if (SD_Started)
        {
            while (done < n)
            {
                if (!numreadysamples)
                {
                    SDL_IMFTick();
                    musicTickFrac += samplesPerMusicTick;
                    numreadysamples = musicTickFrac >> 16;
                    musicTickFrac &= 0xffff;
                    continue;
                }
                int len = n - done < numreadysamples ? n - done : numreadysamples;
                wolf3d_opl_generate(mono + done, len);
                done += len;
                numreadysamples -= len;
            }
        }

        for (i = 0; i < n; i++)
        {
            // dbopl's output times 4, as Wolf4SDL has it (MAME's loudness)
            int32_t l = mono[i] * 4, r = l;

            for (c = 0; c < MIX_CHANNELS; c++)
            {
                MixChannel *ch = &MixChannels[c];
                if (!ch->sound)
                    continue;
                int32_t sample = ((int32_t) ch->sound->data[ch->pos >> 16] - 128) << 8;
                l += (sample * ch->leftvol) >> 8;
                r += (sample * ch->rightvol) >> 8;
                ch->pos += (ORIGSAMPLERATE << 16) / WOLF3D_MIX_RATE;
                if ((ch->pos >> 16) >= ch->sound->length)
                {
                    ch->sound = NULL;
                    SD_ChannelFinished(c);
                }
            }
            stereo[i * 2] = (int16_t) (l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            stereo[i * 2 + 1] = (int16_t) (r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
        stereo += n * 2;
        frames -= n;
    }
}

///////////////////////////////////////////////////////////////////////////
//
//      SD_Startup() - starts up the Sound Mgr
//              Detects all additional sound hardware and installs my ISR
//
///////////////////////////////////////////////////////////////////////////
void
SD_Startup(void)
{
    int     i;

    if (SD_Started)
        return;

    // Viper: the mixer is SD_Mix(), at WOLF3D_MIX_RATE
    param_samplerate = WOLF3D_MIX_RATE;
    memset(MixChannels,0,sizeof(MixChannels));
    memset(SoundChunks,0,sizeof(SoundChunks));
    numreadysamples = 0;
    musicTickFrac = 0;
    soundTimeCounter = 5;
    curAlSound = curAlSoundPtr = 0;


    // Init music

    samplesPerMusicTick = (longword) (((uint64_t) param_samplerate << 16) / 700);    // SDL_t0FastAsmService played at 700Hz

    for(i=1;i<0xf6;i++)
        YM3812Write(oplChip,i,0);

    YM3812Write(oplChip,1,0x20); // Set WSE=1
//    YM3812Write(0,8,0); // Set CSM=0 & SEL=0		 // already set in for statement

    AdLibPresent = true;
    SoundBlasterPresent = true;

    alTimeCount = 0;


    SD_SetSoundMode(sdm_Off);
    SD_SetMusicMode(smm_Off);

    SDL_SetupDigi();

    SD_Started = true;
}

///////////////////////////////////////////////////////////////////////////
//
//      SD_Shutdown() - shuts down the Sound Mgr
//              Removes sound ISR and turns off whatever sound hardware was active
//
///////////////////////////////////////////////////////////////////////////
void
SD_Shutdown(void)
{
    int i;

    if (!SD_Started)
        return;

    SD_MusicOff();
    SD_StopSound();

    memset(MixChannels,0,sizeof(MixChannels));      // Viper
    for(i = 0; i < STARTMUSIC - STARTDIGISOUNDS; i++)
    {
        free(SoundChunks[i].data);
        SoundChunks[i].data = NULL;
    }

    free (DigiList);
    DigiList = NULL;

    SD_Started = false;
}

///////////////////////////////////////////////////////////////////////////
//
//      SD_PositionSound() - Sets up a stereo imaging location for the next
//              sound to be played. Each channel ranges from 0 to 15.
//
///////////////////////////////////////////////////////////////////////////
void
SD_PositionSound(int leftvol,int rightvol)
{
    LeftPosition = leftvol;
    RightPosition = rightvol;
    nextsoundpos = true;
}

///////////////////////////////////////////////////////////////////////////
//
//      SD_PlaySound() - plays the specified sound on the appropriate hardware
//
///////////////////////////////////////////////////////////////////////////
boolean
SD_PlaySound(int sound)
{
    boolean         ispos;
    SoundCommon     *s;
    int             lp,rp;

    lp = LeftPosition;
    rp = RightPosition;
    LeftPosition = 0;
    RightPosition = 0;

    ispos = nextsoundpos;
    nextsoundpos = false;

    if (sound == -1 || (DigiMode == sds_Off && SoundMode == sdm_Off))
        return 0;

    s = (SoundCommon *) SoundTable[sound];

    if ((SoundMode != sdm_Off) && !s)
            Quit("SD_PlaySound() - Uncached sound");

    if ((DigiMode != sds_Off) && (DigiMap[sound] != -1))
    {
        if ((DigiMode == sds_PC) && (SoundMode == sdm_PC))
        {
            if (s->priority < SoundPriority)
                return 0;

            SDL_PCStopSound();

            SD_PlayDigitized(DigiMap[sound],lp,rp);
            SoundPositioned = ispos;
            SoundNumber = sound;
            SoundPriority = s->priority;
        }
        else
        {
#ifdef NOTYET
            if (s->priority < DigiPriority)
                return(false);
#endif

            int channel = SD_PlayDigitized(DigiMap[sound], lp, rp);
            SoundPositioned = ispos;
            DigiNumber = sound;
            DigiPriority = s->priority;
            return channel + 1;
        }

        return(true);
    }

    if (SoundMode == sdm_Off)
        return 0;

    if (!s->length)
        Quit("SD_PlaySound() - Zero length sound");
    if (s->priority < SoundPriority)
        return 0;

    switch (SoundMode)
    {
        case sdm_PC:
            SDL_PCPlaySound((PCSound *)s);
            break;
        case sdm_AdLib:
#ifdef ADDEDFIX // 2
            curAlSound = alSound = 0;                // Tricob
            alOut(alFreqH, 0);
#endif
            SDL_ALPlaySound((AdLibSound *)s);
            break;

        default:
            break;
    }

    SoundNumber = sound;
    SoundPriority = s->priority;

    return 0;
}

///////////////////////////////////////////////////////////////////////////
//
//      SD_SoundPlaying() - returns the sound number that's playing, or 0 if
//              no sound is playing
//
///////////////////////////////////////////////////////////////////////////
word
SD_SoundPlaying(void)
{
    boolean result = false;

    switch (SoundMode)
    {
        case sdm_PC:
            result = pcSound? true : false;
            break;
        case sdm_AdLib:
            result = alSound? true : false;
            break;

        default:
            break;
    }

    if (result)
        return(SoundNumber);
    else
        return(false);
}

///////////////////////////////////////////////////////////////////////////
//
//      SD_StopSound() - if a sound is playing, stops it
//
///////////////////////////////////////////////////////////////////////////
void
SD_StopSound(void)
{
    if (DigiPlaying)
        SD_StopDigitized();

    switch (SoundMode)
    {
        case sdm_PC:
            SDL_PCStopSound();
            break;
        case sdm_AdLib:
            SDL_ALStopSound();
            break;

        default:
            break;
    }

    SoundPositioned = false;

    SDL_SoundFinished();
}

///////////////////////////////////////////////////////////////////////////
//
//      SD_WaitSoundDone() - waits until the current sound is done playing
//
///////////////////////////////////////////////////////////////////////////
void
SD_WaitSoundDone(void)
{
    while (SD_SoundPlaying())
        SDL_Delay(5);
}

///////////////////////////////////////////////////////////////////////////
//
//      SD_MusicOn() - turns on the sequencer
//
///////////////////////////////////////////////////////////////////////////
void
SD_MusicOn(void)
{
    sqActive = true;
}

///////////////////////////////////////////////////////////////////////////
//
//      SD_MusicOff() - turns off the sequencer and any playing notes
//      returns the last music offset for music continue
//
///////////////////////////////////////////////////////////////////////////
int
SD_MusicOff(void)
{
    word    i;

    sqActive = false;
    switch (MusicMode)
    {
        case smm_AdLib:
            alOut(alEffects, 0);
            for (i = 0;i < sqMaxTracks;i++)
                alOut(alFreqH + i + 1, 0);
            break;

        default:
            break;
    }

    return (int) (sqHackPtr-sqHack);
}

///////////////////////////////////////////////////////////////////////////
//
//      SD_StartMusic() - starts playing the music pointed to
//
///////////////////////////////////////////////////////////////////////////
void
SD_StartMusic(int chunk)
{
    SD_MusicOff();

    if (MusicMode == smm_AdLib)
    {
        int32_t chunkLen = CA_CacheAudioChunk(chunk);
        sqHack = (word *)(void *) audiosegs[chunk];     // alignment is correct
        if(*sqHack == 0) sqHackLen = sqHackSeqLen = chunkLen;
        else sqHackLen = sqHackSeqLen = *sqHack++;
        sqHackPtr = sqHack;
        sqHackTime = 0;
        alTimeCount = 0;
        SD_MusicOn();
    }
}

void
SD_ContinueMusic(int chunk, int startoffs)
{
    int i;

    SD_MusicOff();

    if (MusicMode == smm_AdLib)
    {
        int32_t chunkLen = CA_CacheAudioChunk(chunk);
        sqHack = (word *)(void *) audiosegs[chunk];     // alignment is correct
        if(*sqHack == 0) sqHackLen = sqHackSeqLen = chunkLen;
        else sqHackLen = sqHackSeqLen = *sqHack++;
        sqHackPtr = sqHack;

        if(startoffs >= sqHackLen)
        {
#ifdef ADDEDFIX // 7                     // Andy, improved by Chris Chokan
            startoffs = 0;
#else
            Quit("SD_StartMusic: Illegal startoffs provided!");
#endif
        }

        // fast forward to correct position
        // (needed to reconstruct the instruments)

        for(i = 0; i < startoffs; i += 2)
        {
            byte reg = *(byte *)sqHackPtr;
            byte val = *(((byte *)sqHackPtr) + 1);
            if(reg >= 0xb1 && reg <= 0xb8) val &= 0xdf;           // disable play note flag
            else if(reg == 0xbd) val &= 0xe0;                     // disable drum flags

            alOut(reg,val);
            sqHackPtr += 2;
            sqHackLen -= 4;
        }
        sqHackTime = 0;
        alTimeCount = 0;

        SD_MusicOn();
    }
}

///////////////////////////////////////////////////////////////////////////
//
//      SD_FadeOutMusic() - starts fading out the music. Call SD_MusicPlaying()
//              to see if the fadeout is complete
//
///////////////////////////////////////////////////////////////////////////
void
SD_FadeOutMusic(void)
{
    switch (MusicMode)
    {
        case smm_AdLib:
            // DEBUG - quick hack to turn the music off
            SD_MusicOff();
            break;

        default:
            break;
    }
}

///////////////////////////////////////////////////////////////////////////
//
//      SD_MusicPlaying() - returns true if music is currently playing, false if
//              not
//
///////////////////////////////////////////////////////////////////////////
boolean
SD_MusicPlaying(void)
{
    boolean result;

    switch (MusicMode)
    {
        case smm_AdLib:
            result = sqActive;
            break;
        default:
            result = false;
            break;
    }

    return(result);
}
