#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <io.h>
#include "save.h"
#include "cpu_68k.h"
#include "z80.h"
#include "gens.h"
#include "g_main.h"
#include "g_ddraw.h"
#include "g_dsound.h"
#include "g_input.h"
#include "vdp_io.h"
#include "vdp_rend.h"
#include "rom.h"
#include "mem_m68k.h"
#include "mem_z80.h"
#include "ym2612.h"
#include "psg.h"
#include "pcm.h"
#include "pwm.h"
#include "ggenie.h"
#include "joypads.h"
#include "misc.h"
#include "movie.h"
#include "ram_search.h"
#include "ramwatch.h"
#include "hexeditor.h"
#include "luascript.h"
#include <direct.h>
#if defined(_DEBUG) && (LATEST_SAVESTATE_VERSION >= 9)
#include <assert.h>
#else
#define assert(x) (void)0
#endif

int Current_State = 0;
char State_Dir[1024] = "";
char SRAM_Dir[1024] = "";
extern char Lua_Dir[1024];
unsigned short FrameBuffer[336 * 240];
unsigned int FrameBuffer32[336 * 240];
ALIGN16 unsigned char State_Buffer[MAX_STATE_FILE_LENGTH];
unsigned char Version;

bool UseMovieStates;
bool SkipNextRerecordIncrement = false;
static unsigned char InBaseGenesis = 1;
//extern long x, y, xg, yg; // G_Main.cpp
extern "C" unsigned int Current_OUT_Pos, Current_OUT_Size; // cdda_mp3.c
extern "C" int fatal_mp3_error; // cdda_mp3.c
extern "C" char played_tracks_linear[105]; // cd_sys.c
extern "C" char Track_Played; // cd_file.c
extern int FS_Minimised;
extern bool skipLagNow, frameadvSkipLag_Rewind_State_Buffer_Valid;
extern int frameSearchFrames;
extern bool s_nextInputCondensedSet;

bool g_disableStatestateWarnings = false;
bool g_onlyCallSavestateCallbacks = false;

#ifdef _WIN32
#define MINIMIZE								\
	{if (Sound_Initialised) Clear_Sound_Buffer();	\
	if (Full_Screen)								\
        	{												\
		Set_Render(HWnd, 0, -1, true);				\
		FS_Minimised = 1;							\
        	}}

#define WARNINGBOX(message, title) \
	do{	if(!g_disableStatestateWarnings) { MINIMIZE \
		DialogsOpen++; \
		MessageBox(HWnd, message, title, MB_OK | MB_ICONWARNING); \
		DialogsOpen--; } } while(0)
#else
#define WARNINGBOX(message, title) \
		if(!g_disableStatestateWarnings) fprintf(stderr, "%s: %s\n", title, message)
#endif

int Change_File_S(char *Dest, char *Dir, char *Titre, char *Filter, char *Ext, HWND hwnd)
{
    OPENFILENAME ofn;

    SetCurrentDirectory(Gens_Path);

    if (!strcmp(Dest, ""))
    {
        strcpy(Dest, "default.");
        strcat(Dest, Ext);
    }

    memset(&ofn, 0, sizeof(OPENFILENAME));

    ofn.lStructSize = sizeof(OPENFILENAME);
    ofn.hwndOwner = hwnd;
    ofn.hInstance = ghInstance;
    ofn.lpstrFile = Dest;
    ofn.nMaxFile = 2047;
    ofn.lpstrFilter = Filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrInitialDir = Dir;
    ofn.lpstrTitle = Titre;
    ofn.lpstrDefExt = Ext;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (GetSaveFileName(&ofn)) return 1;

    return 0;
}

int Change_File_L(char *Dest, char *Dir, char *Titre, char *Filter, char *Ext, HWND hwnd)
{
    OPENFILENAME ofn;

    SetCurrentDirectory(Gens_Path);

    if (!strcmp(Dest, ""))
    {
        strcpy(Dest, "default.");
        strcat(Dest, Ext);
    }

    memset(&ofn, 0, sizeof(OPENFILENAME));

    ofn.lStructSize = sizeof(OPENFILENAME);
    ofn.hwndOwner = hwnd;
    ofn.hInstance = ghInstance;
    ofn.lpstrFile = Dest;
    ofn.nMaxFile = 2047;
    ofn.lpstrFilter = Filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrInitialDir = Dir;
    ofn.lpstrTitle = Titre;
    ofn.lpstrDefExt = Ext;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;

    if (GetOpenFileName(&ofn)) return 1;

    return 0;
}

int Change_Dir(char *Dest, char *Dir, char *Titre, char *Filter, char *Ext, HWND hwnd)
{
    OPENFILENAME ofn;
    int i;

    SetCurrentDirectory(Gens_Path);

    strcpy(Dest, "default.");
    strcat(Dest, Ext);
    memset(&ofn, 0, sizeof(OPENFILENAME));

    ofn.lStructSize = sizeof(OPENFILENAME);
    ofn.hwndOwner = hwnd;
    ofn.hInstance = ghInstance;
    ofn.lpstrFile = Dest;
    ofn.nMaxFile = 2047;
    ofn.lpstrFilter = Filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrInitialDir = Dir;
    ofn.lpstrTitle = Titre;
    ofn.lpstrDefExt = Ext;
    ofn.Flags = OFN_HIDEREADONLY;

    if (GetSaveFileName(&ofn))
    {
        i = strlen(Dest) - 1;
        while ((i > 0) && (Dest[i] != '\\')) i--;
        if (!i) return 0;
        Dest[++i] = 0;
        return 1;
    }

    return 0;
}

int s_lastStateNumberGotten = -1;

void Get_State_File_Name(char *name)
{
    s_lastStateNumberGotten = Current_State;

    char Ext[64] = ".gsX";

    SetCurrentDirectory(Gens_Path);

    itoa(Current_State, Ext + 3, 10);
    strcpy(name, State_Dir);
    _mkdir(name); // added to avoid a crash
    strcat(name, Rom_Name);
    if (UseMovieStates && MainMovie.Status)
    {
        strcat(name, " - ");
        char* slash = strrchr(MainMovie.FileName, '/');
        slash = max(slash, strrchr(MainMovie.FileName, '\\'));
        slash = max(slash, strrchr(MainMovie.FileName, '|'));
        if (slash)
            slash++;
        else
            slash = MainMovie.FileName;
        strcat(name, slash);
        name[strlen(name) - 4] = '\0'; // note: strlen(Ext) instead of 4 would be wrong
    }
    strcat(name, Ext);
}
FILE *Get_State_File()
{
    char Name[2048];

    SetCurrentDirectory(Gens_Path);

    Get_State_File_Name(Name);

    return fopen(Name, "rb");
}

int Load_State_From_Buffer(unsigned char *buf)
{
    if (!Game)
        return 0;

    if ((MainMovie.Status == MOVIE_PLAYING) || (MainMovie.Status == MOVIE_FINISHED))
    {
        fseek(MainMovie.File, 0, SEEK_END);
        MainMovie.LastFrame = ((ftell(MainMovie.File) - 64) / 3);
    }

    extern bool frameadvSkipLag_Rewind_State_Buffer_Valid;
    frameadvSkipLag_Rewind_State_Buffer_Valid = false;

    assert((((int)buf) & 15) == 0); // want this for alignment performance reasons
    unsigned char* bufStart = buf;

    buf += Import_Genesis(buf); //upthmodif - fixed for new, additive, length determination

    return buf - bufStart;
}

static const char* standardInconsistencyMessage = "Warning: The state you are loading is inconsistent with the current movie.\nYou should either load a different savestate, or turn off movie read-only mode and load this savestate again.";

void TruncateMovieToFrameCount()
{
    MainMovie.LastFrame = FrameCount;
    if (GetFileAttributes(MainMovie.PhysicalFileName) & FILE_ATTRIBUTE_READONLY)
        return;
    fseek(MainMovie.File, 0, SEEK_SET);
    char *tempbuf = new char[64 + (FrameCount * 3)];
    fread(tempbuf, 1, 64 + (FrameCount * 3), MainMovie.File);
    fclose(MainMovie.File);
    MainMovie.File = fopen(MainMovie.PhysicalFileName, "wb");
    fseek(MainMovie.File, 0, SEEK_SET);
    fwrite(tempbuf, 1, 64 + (FrameCount * 3), MainMovie.File);
    WriteMovieHeader(&MainMovie);
    fclose(MainMovie.File);
    MainMovie.File = fopen(MainMovie.PhysicalFileName, "r+b");
    delete[] tempbuf;
}

bool g_refreshScreenAfterLoad = true;

int Load_State(char *Name)
{
    int stateNumber = s_lastStateNumberGotten;
    s_lastStateNumberGotten = -1;

    if (!g_onlyCallSavestateCallbacks)
    {
        FILE *f;
        unsigned char *buf;
        int len;

        len = GENESIS_STATE_LENGTH;
        if (Genesis_Started); //So it doesn't return zero if the SegaCD and 32X aren't running
        else return 0;

        frameadvSkipLag_Rewind_State_Buffer_Valid = false;
        s_nextInputCondensedSet = false;
        if (skipLagNow)
        {
            skipLagNow = false;
            Paused = 1;
        }

        frameSearchFrames = -1;

        if (MainMovie.ReadOnly == 0 && MainMovie.File)
        {
            if (!SkipNextRerecordIncrement)
                MainMovie.NbRerecords++;
            else
                SkipNextRerecordIncrement = false;
            if (!MainMovie.ReadOnly)
            {
                if (MainMovie.TriplePlayerHack)
                    FrameCount = max(max(max(Track1_FrameCount, Track2_FrameCount), Track3_FrameCount), FrameCount);
                else
                    FrameCount = max(max(Track1_FrameCount, Track2_FrameCount), FrameCount);
            }

            if (FrameCount != MainMovie.LastFrame)
                TruncateMovieToFrameCount();
        }

        buf = State_Buffer;

        if ((f = fopen(Name, "rb")) == NULL) return 0;
        fseek(f, 0x50, SEEK_SET);
        char version = fgetc(f);
        fseek(f, 0, SEEK_SET);
        if (version == 0x6) len += 0x1239;
        if (version == 0x7) len -= 5;

        memset(buf, 0, len);
        if (fread(buf, 1, len, f))
        {
            if ((len = Load_State_From_Buffer(buf)) == 0)
                return 0;

            int switched = 0; //Modif N - switched is for displaying "switched to playback" message
            bool truncate = false;

            // Modif N. -- I don't know why MainMovie.LastFrame gets calculated like it does above,
            // but it makes the movie length invalid whenever I load a savestate while recording and in non-readonly mode,
            // causing the movie to run past the end of where it should if I then switch to playback.
            // so I try to correct it here
            if (!MainMovie.ReadOnly)
            {
                int maxtrack = TRACK1 | TRACK2;
                if (MainMovie.TriplePlayerHack) maxtrack |= TRACK3;
                if ((track & maxtrack) == maxtrack) // only do this if all tracks are on
                    if ((MainMovie.File) && FrameCount != MainMovie.LastFrame)
                        truncate = true;
            }

            if ((MainMovie.File) && !(FrameCount < MainMovie.LastFrame))
            {
                if (MainMovie.ReadOnly == 1 && MainMovie.Status != MOVIE_FINISHED)
                {
                    char inconsistencyMessage[1024];
                    sprintf(inconsistencyMessage, "%s\n\nReason: State is after the end of the current movie. (State is on frame %d, Movie ends on frame %d)", standardInconsistencyMessage, FrameCount, MainMovie.LastFrame);
                    WARNINGBOX(inconsistencyMessage, "Movie End Warning");
                }
                MainMovie.Status = MOVIE_FINISHED;
                switched = 2;
            }
            if ((MainMovie.File) && (MainMovie.Status != MOVIE_PLAYING)
                && (MainMovie.ReadOnly == 1) && (FrameCount < MainMovie.LastFrame)) //Modif Upth - Allow switching to playback even if movie finished, if it hasn't been closed //Modif N - allow switching to playback if loading a state while recording in read-only mode
            {
                if (MainMovie.Status == MOVIE_RECORDING)
                    WriteMovieHeader(&MainMovie);
                MainMovie.Status = MOVIE_PLAYING;
                switched = 1;
            }
            if ((MainMovie.File) && (MainMovie.Status != MOVIE_RECORDING)
                && (MainMovie.ReadOnly == 0)) //upthmodif - allow resume record even if we accidentally played past the end; so long as AutoCloseMovie is unchecked.
            {
                if (AutoBackupEnabled)
                {
                    strncpy(Str_Tmp, MainMovie.FileName, 1024);
                    for (int i = strlen(Str_Tmp); i >= 0; i--) if (Str_Tmp[i] == '|') Str_Tmp[i] = '_';
                    strcat(MainMovie.FileName, ".gmv");
                    MainMovie.FileName[strlen(MainMovie.FileName) - 7] = 'b'; // ".bak"
                    MainMovie.FileName[strlen(MainMovie.FileName) - 6] = 'a';
                    MainMovie.FileName[strlen(MainMovie.FileName) - 5] = 'k';
                    BackupMovieFile(&MainMovie);
                    strncpy(MainMovie.FileName, Str_Tmp, 1024);
                    strncpy(MainMovie.PhysicalFileName, MainMovie.FileName, 1024);
                }
                MainMovie.Status = MOVIE_RECORDING;
                switched = 3;
            }

            if (truncate)
                TruncateMovieToFrameCount();
            if (!(switched))
                sprintf(Str_Tmp, "STATE %d LOADED", Current_State);
            else if (switched == 1)
                sprintf(Str_Tmp, "STATE %d LOADED : SWITCHED TO PLAYBACK", Current_State);
            else if (switched == 2)
                sprintf(Str_Tmp, "STATE %d LOADED : MOVIE FINISHED", Current_State);
            else if (switched == 3)
                sprintf(Str_Tmp, "STATE %d LOADED : RECORDING RESUMED", Current_State);

            Put_Info(Str_Tmp);
        }

        //Modif N - bulletproof re-recording (loading)
        if ((MainMovie.Status == MOVIE_RECORDING) && MainMovie.File)
        {
            unsigned long int temp = MainMovie.LastFrame;
            if (MainMovie.TriplePlayerHack)
            {
                if (track & TRACK1) Track1_FrameCount = max(temp, Track1_FrameCount);
                if (track & TRACK2) Track2_FrameCount = max(temp, Track2_FrameCount);
                if (track & TRACK3) Track3_FrameCount = max(temp, Track3_FrameCount);
            }
            else
            {
                if (track & TRACK1) Track1_FrameCount = max(temp, Track1_FrameCount);
                if (track & TRACK2) Track2_FrameCount = max(temp, Track2_FrameCount);
            }

            int m = fgetc(f);
            if (m == 'M' && !feof(f) && !ferror(f))
            {
                int pos = ftell(MainMovie.File);
                fseek(MainMovie.File, 64, SEEK_SET);

                char* bla = new char[FrameCount * 3];
                if (FrameCount * 3 == fread(bla, 1, FrameCount * 3, f))
                    fwrite(bla, 1, FrameCount * 3, MainMovie.File);
                delete[] bla;

                fseek(MainMovie.File, pos, SEEK_SET);
            }
        }

        //Modif N - consistency checking (loading)
        if ((MainMovie.Status == MOVIE_PLAYING) && MainMovie.File)
        {
            unsigned long int temp = MainMovie.LastFrame;
            Track1_FrameCount = Track2_FrameCount = temp;
            if (MainMovie.TriplePlayerHack)
                Track3_FrameCount = temp;

            fseek(MainMovie.File, 64, SEEK_SET);

            char* bla = new char[FrameCount * 3]; // savestate movie input data
            char* bla2 = new char[FrameCount * 3]; // playing movie input data
            if ((FrameCount * 3 != fread(bla, 1, FrameCount * 3, f))
              || (FrameCount * 3 != fread(bla2, 1, FrameCount * 3, MainMovie.File)))
            {
              char inconsistencyMessage[1024];
              sprintf(inconsistencyMessage, "%s\n\nReason: Unable to compare %d frames.", standardInconsistencyMessage, FrameCount);
              WARNINGBOX(inconsistencyMessage, "Desync Warning");
            }
            else if (memcmp(bla, bla2, FrameCount * 3))
            {
              int firstMismatch = -3;
              for (int i = 0; i < (int)FrameCount * 3; i++)
              {
                if (bla[i] != bla2[i])
                {
                  firstMismatch = i;
                  break;
                }
              }

              char inconsistencyMessage[1024];
              sprintf(inconsistencyMessage, "%s\n\nReason: Different input starting on frame %d.", standardInconsistencyMessage, firstMismatch / 3);
              WARNINGBOX(inconsistencyMessage, "Desync Warning");
            }
            delete[] bla;
            delete[] bla2;
        }
        fclose(f);
    }

    {
        LuaSaveData saveData;

        char luaSaveFilename[512];
        strncpy(luaSaveFilename, Name, 512);
        luaSaveFilename[512 - (1 + 7/*strlen(".luasav")*/)] = '\0';
        strcat(luaSaveFilename, ".luasav");
        FILE* luaSaveFile = fopen(luaSaveFilename, "rb");
        if (luaSaveFile)
        {
            saveData.ImportRecords(luaSaveFile);
            fclose(luaSaveFile);
        }

        CallRegisteredLuaLoadFunctions(stateNumber, saveData);
    }

    Update_RAM_Search();

    extern bool g_anyScriptsHighSpeed;
    if (g_refreshScreenAfterLoad && !g_anyScriptsHighSpeed)
    {
        FakeVDPScreen = true;
        Show_Genesis_Screen(HWnd);
    }

    return 1;
}

int Save_State_To_Buffer(unsigned char *buf)
{
    assert((((int)buf) & 15) == 0); // want this for alignment performance reasons
    int len;

    len = GENESIS_STATE_LENGTH; //Upthmodif - tweaked the length determination system;Modif N - used to be GENESIS_STATE_FILE_LENGTH, which I think is a major bug because then the amount written and the amount read are different - this change was necessary to append anything to the save (i.e. for bulletproof re-recording)

    memset(buf, 0, len);

    Export_Genesis(buf);
    buf += GENESIS_STATE_LENGTH; //upthmodif - fixed for new, additive, length determination

    return len;
}
int Save_State(char *Name)
{
    int stateNumber = s_lastStateNumberGotten;
    s_lastStateNumberGotten = -1;

    {
        LuaSaveData saveData;
        CallRegisteredLuaSaveFunctions(stateNumber, saveData);

        char luaSaveFilename[512];
        strncpy(luaSaveFilename, Name, 512);
        luaSaveFilename[512 - (1 + 7/*strlen(".luasav")*/)] = '\0';
        strcat(luaSaveFilename, ".luasav");
        if (saveData.recordList)
        {
            FILE* luaSaveFile = fopen(luaSaveFilename, "wb");
            if (luaSaveFile)
            {
                saveData.ExportRecords(luaSaveFile);
                fclose(luaSaveFile);
            }
        }
        else
        {
            unlink(luaSaveFilename);
        }
    }

    if (g_onlyCallSavestateCallbacks)
        return 1;

    FILE *f;
    unsigned char *buf;
    int len;

    buf = State_Buffer;

    if ((f = fopen(Name, "wb")) == NULL) return 0;
    if ((len = Save_State_To_Buffer(buf)) == 0) return 0;
    fwrite(State_Buffer, 1, len, f);

    //Modif N - bulletproof re-recording (saving)
    if (MainMovie.File && (MainMovie.Status != MOVIE_FINISHED))
    {
        fputc('M', f);
        int pos = ftell(MainMovie.File);
        fseek(MainMovie.File, 64, SEEK_SET);

        char* bla = new char[FrameCount * 3];
        if (FrameCount * 3 == fread(bla, 1, FrameCount * 3, MainMovie.File))
            fwrite(bla, 1, FrameCount * 3, f);
        delete[] bla;

        fseek(MainMovie.File, pos, SEEK_SET);
    }
    else
    {
        fputc('\0', f);
    }

    fclose(f);

    sprintf(Str_Tmp, "STATE %d SAVED", Current_State);
    Put_Info(Str_Tmp);

    return 1;
}

//Modif N. - added ImportData and ExportData because the import/export code was getting a little hairy without these
// The main advantage to using these, besides less lines of code, is that
// you can replace ImportData with ExportData, without changing anything else in the arguments,
// to go from import code to export code.
// ImportDataAuto/ExportDataAuto are like ImportData/ExportData except they also auto-increment the offset by the size

#ifdef _DEBUG // slow debug version:

void ImportDataDebug(void* into, const void* data, unsigned int & offset, unsigned int numBytes, bool autoIncrement)
{
    unsigned char* dst = (unsigned char *)into;
    const unsigned char* src = ((const unsigned char*)data) + offset;

    // if the assert here fails, add alignment padding of less than 16 bytes to make the condition true,
    // otherwise the load operation will run slower than it should in release.
    // (search for "alignment for performance" for examples,
    //  and note that the underlying buffers should have ALIGN16 prefixed to them)
    assert((numBytes < 0x100 || (int(src) & 15) == (int(dst) & 15) || (InBaseGenesis && !autoIncrement) || Version < 9) && "error: source and data are not correctly aligned with each other");

    // loop through copying 1 byte at a time out of the savestate data...
    for (unsigned int i = offset; i < offset + numBytes; i++)
    {
        *dst++ = *src++;
    }

    // if this is ImportDataAuto, increment the offset
    if (autoIncrement)
        offset += numBytes;
}
void ExportDataDebug(const void* from, void* data, unsigned int & offset, unsigned int numBytes, bool autoIncrement)
{
    const unsigned char* src = (const unsigned char *)from;
    unsigned char* dst = ((unsigned char*)data) + offset;

    // if the assert here fails, add alignment padding of less than 16 bytes to make the condition true,
    // otherwise the save operation will run slower than it should in release.
    // (search for "alignment for performance" for examples,
    //  and note that the underlying buffers should have ALIGN16 prefixed to them)
    assert((numBytes < 0x100 || (int(src) & 15) == (int(dst) & 15) || (InBaseGenesis && !autoIncrement) || Version < 9) && "error: source and data are not correctly aligned with each other");

    // loop through copying 1 byte at a time into the savestate data...
    for (unsigned int i = offset; i < offset + numBytes; i++)
    {
        assert((*dst == 0 || *dst == 0xFF) && "error: saved over the same byte twice");
        *dst++ = *src++;
    }

    // if this is ExportDataAuto, increment the offset
    if (autoIncrement)
        offset += numBytes;
}
void ImportData(void* into, const void* data, unsigned int offset, unsigned int numBytes) {
    ImportDataDebug(into, data, offset, numBytes, false);
}
void ExportData(const void* from, void* data, unsigned int offset, unsigned int numBytes) {
    ExportDataDebug(from, data, offset, numBytes, false);
}
void ImportDataAuto(void* into, const void* data, unsigned int & offset, unsigned int numBytes) {
    ImportDataDebug(into, data, offset, numBytes, true);
}
void ExportDataAuto(const void* from, void* data, unsigned int & offset, unsigned int numBytes) {
    ExportDataDebug(from, data, offset, numBytes, true);
}

#else // fast version:

#define ImportDataM(into, data, numBytes) memcpy(into, data,       numBytes)
#define ExportDataM(from, data, numBytes) memcpy(      data, from, numBytes)

template<unsigned int numBytes>
void ImportDataT(void* dst, const void* src) {
    ImportDataM(dst, src, numBytes);
}
template<unsigned int numBytes>
void ExportDataT(const void* src, void* dst) {
    ExportDataM(src, dst, numBytes);
}

#ifndef _MSC_VER
#define __forceinline __attribute__((always_inline))
#endif

// specializations to avoid the overhead of a memcpy call for really small copies:
template<> __forceinline void ImportDataT<1>(void* dst, const void* src) { *(UINT8 *)dst = *(const UINT8 *)src; }
template<> __forceinline void ExportDataT<1>(const void* src, void* dst) { *(UINT8 *)dst = *(const UINT8 *)src; }
template<> __forceinline void ImportDataT<2>(void* dst, const void* src) { *(UINT16*)dst = *(const UINT16*)src; }
template<> __forceinline void ExportDataT<2>(const void* src, void* dst) { *(UINT16*)dst = *(const UINT16*)src; }
template<> __forceinline void ImportDataT<4>(void* dst, const void* src) { *(UINT32*)dst = *(const UINT32*)src; }
template<> __forceinline void ExportDataT<4>(const void* src, void* dst) { *(UINT32*)dst = *(const UINT32*)src; }
template<> __forceinline void ImportDataT<8>(void* dst, const void* src) { *(UINT64*)dst = *(const UINT64*)src; }
template<> __forceinline void ExportDataT<8>(const void* src, void* dst) { *(UINT64*)dst = *(const UINT64*)src; }

#define ImportData(into, data, offset, numBytes)          ImportDataT<numBytes>(into,((const UINT8*)(data))+(offset))
#define ExportData(from, data, offset, numBytes)          ExportDataT<numBytes>(from,((      UINT8*)(data))+(offset))
#define ImportDataAuto(into, data, offset, numBytes) do { ImportDataT<numBytes>(into,((const UINT8*)(data))+(offset)); offset += numBytes; } while(0)
#define ExportDataAuto(from, data, offset, numBytes) do { ExportDataT<numBytes>(from,((      UINT8*)(data))+(offset)); offset += numBytes; } while(0)

#endif

/*

GST genecyst save file

Range        Size   Description
-----------  -----  -----------
00000-00002  3      "GST"
00006-00007  2      "\xE0\x40"
000FA-00112  24     VDP registers
00112-00191  128    Color RAM
00192-001E1  80     Vertical scroll RAM
001E4-003E3  512    YM2612 registers
00474-02473  8192   Z80 RAM
02478-12477  65536  68K RAM
12478-22477  65536  Video RAM

main 68000 registers
--------------------

00080-0009F : D0-D7
000A0-000BF : A0-A7
000C8 : PC
000D0 : SR
000D2 : USP
000D6 : SSP

Z80 registers
-------------

00404 : AF
00408 : BC
0040C : DE
00410 : HL
00414 : IX
00418 : IY
0041C : PC
00420 : SP
00424 : AF'
00428 : BC'
0042C : DE'
00430 : HL'

00434 : I
00435 : Unknow
00436 : IFF1 = IFF2
00437 : Unknow

The 'R' register is not supported.

Z80 State
---------

00438 : Z80 RESET
00439 : Z80 BUSREQ
0043A : Unknow
0043B : Unknow

0043C : Z80 BANK (DWORD)

Gens and Kega ADD
-----------------

00040 : last VDP Control data written (DWORD)
00044 : second write flag (1 for second write)
00045 : DMA Fill flag (1 mean next data write will cause a DMA fill)
00048 : VDP write address (DWORD)

00050 : Version       (Genecyst=0 ; Kega=5 ; Gens=5)
00051 : Emulator ID   (Genecyst=0 ; Kega=0 ; Gens=1)
00052 : System ID     (Genesis=0 ; SegaCD=1 ; 32X=2 ; SegaCD32X=3)

00060-00070 : PSG registers (WORD).

SEGA CD
-------

+00000-00FFF : Gate array & sub 68K
+01000-80FFF : Prg RAM
+81000-C0FFF : Word RAM (2M mode arrangement)
+C1000-D0FFF : PCM RAM
+D1000-DFFFF : CDD & CDC data (16 kB cache include)

32X
---

main SH2
--------

+00000-00FFF : cache
+01000-011FF : IO registers
+01200-0123F : R0-R15
+01240 : SR
+01244 : GBR
+01248 : VBR
+0124C : MACL
+01250 : MACH
+01254 : PR
+01258 : PC
+0125C : State

sub SH2
-------

+01400-023FF : cache
+02400-025FF : IO registers
+02600-0263F : R0-R15
+02640 : SR
+02644 : GBR
+02648 : VBR
+0264C : MACL
+02650 : MACH
+02654 : PR
+02658 : PC
+0265C : State

others
------
// Fix 32X save state :
// enregistrer correctement les registres syst�mes ...

+02700 : ADEN bit (bit 0)
+02701 : FM bit (bit 7)
+02702 : Master SH2 INT mask register
+02703 : Slave SH2 INT mask register
+02704 : 68000 32X rom bank register
+02705 : RV (Rom to VRAM DMA allowed) bit (bit 0)
+02710-0273F : FIFO stuff (not yet done)
+02740-0274F : 32X communication buffer
+02750-02759 : PWM registers
+02760-0276F : 32X VDP registers
+02800-029FF : 32X palette
+02A00-429FF : SDRAM
+42A00-829FF : FB1 & FB2

*/
int Import_Genesis(unsigned char * Data)
{
    unsigned char* src;
    int i;

    //	VDP_Int = 0;
    //	DMAT_Length = 0;
    int len = GENESIS_STATE_LENGTH;
    Version = Data[0x50];
    if (Version < 6) len -= 0x10000;

    InBaseGenesis = 1;

    for (i = 0; i < 0x40; i++)
    {
        CRam[i] = (Data[i * 2 + 0x112 + 1] << 8) | Data[i * 2 + 0x112 + 0];
    }

    ImportData(VSRam, Data, 0x192, 0x50);
    ImportData(Ram_Z80, Data, 0x474, 0x2000);

    for (i = 0; i < 0x10000; i += 2)
    {
        Ram_68k[i + 0] = Data[i + 0x2478 + 1];
        Ram_68k[i + 1] = Data[i + 0x2478 + 0];
    }

    for (i = 0; i < 0x10000; i += 2)
    {
        VRam[i + 0] = Data[i + 0x12478 + 1];
        VRam[i + 1] = Data[i + 0x12478 + 0];
    }

    YM2612_Restore(Data + 0x1E4);

    if ((Version >= 2) && (Version < 4))
    {
        ImportData(&Ctrl, Data, 0x30, 7 * 4);

        Z80_State &= ~6;
        if (Data[0x440] & 1) Z80_State |= 2;
        if (Data[0x444] & 1) Z80_State |= 4;

        ImportData(&Bank_Z80, Data, 0x448, 4);

        ImportData(&PSG_Save, Data, 0x224B8, 8 * 4);
        PSG_Restore_State();
    }
    else if ((Version >= 4) || (Version == 0)) 		// New version compatible with Kega.
    {
        Z80_State &= ~6;

        if (Version == 4)
        {
            M_Z80.IM = Data[0x437];
            M_Z80.IFF.b.IFF1 = (Data[0x438] & 1) << 2;
            M_Z80.IFF.b.IFF2 = (Data[0x438] & 1) << 2;

            Z80_State |= (Data[0x439] & 1) << 1;
        }
        else
        {
            M_Z80.IM = 1;
            M_Z80.IFF.b.IFF1 = (Data[0x436] & 1) << 2;
            M_Z80.IFF.b.IFF2 = (Data[0x436] & 1) << 2;

            Z80_State |= ((Data[0x439] & 1) ^ 1) << 1;
            Z80_State |= ((Data[0x438] & 1) ^ 1) << 2;
        }

        src = (unsigned char *)&Ctrl;
        for (i = 0; i < 7 * 4; i++) *src++ = 0;

        Write_VDP_Ctrl(Data[0x40] + (Data[0x41] << 8));
        Write_VDP_Ctrl(Data[0x42] + (Data[0x43] << 8));

        Ctrl.Flag = Data[0x44];
        Ctrl.DMA = (Data[0x45] & 1) << 2;
        Ctrl.Access = Data[0x46] + (Data[0x47] << 8); //Nitsuja added this
        Ctrl.Address = Data[0x48] + (Data[0x49] << 8);

        ImportData(&Bank_Z80, Data, 0x43C, 4);

        if (Version >= 4)
        {
            for (i = 0; i < 8; i++)
                PSG_Save[i] = Data[i * 2 + 0x60] + (Data[i * 2 + 0x61] << 8);
            PSG_Restore_State();
        }
    }

    z80_Set_AF(&M_Z80, Data[0x404] + (Data[0x405] << 8));
    M_Z80.AF.b.FXY = Data[0x406]; //Modif N
    M_Z80.BC.w.BC = Data[0x408] + (Data[0x409] << 8);
    M_Z80.DE.w.DE = Data[0x40C] + (Data[0x40D] << 8);
    M_Z80.HL.w.HL = Data[0x410] + (Data[0x411] << 8);
    M_Z80.IX.w.IX = Data[0x414] + (Data[0x415] << 8);
    M_Z80.IY.w.IY = Data[0x418] + (Data[0x419] << 8);
    z80_Set_PC(&M_Z80, Data[0x41C] + (Data[0x41D] << 8));
    M_Z80.SP.w.SP = Data[0x420] + (Data[0x421] << 8);
    z80_Set_AF2(&M_Z80, Data[0x424] + (Data[0x425] << 8));
    M_Z80.BC2.w.BC2 = Data[0x428] + (Data[0x429] << 8);
    M_Z80.DE2.w.DE2 = Data[0x42C] + (Data[0x42D] << 8);
    M_Z80.HL2.w.HL2 = Data[0x430] + (Data[0x431] << 8);
    M_Z80.I = Data[0x434] & 0xFF;

    FrameCount = Data[0x22478] + (Data[0x22479] << 8) + (Data[0x2247A] << 16) + (Data[0x2247B] << 24);

    //	MainMovie.NbRerecords++;
    //	if(MainMovie.Status==MOVIE_RECORDING)
    //		MainMovie.LastFrame=FrameCount;

    main68k_GetContext(&Context_68K);

    for (i = 0; i < 24; i++) Set_VDP_Reg(i, Data[0xFA + i]);

    ImportData(&Context_68K.dreg[0], Data, 0x80, 8 * 2 * 4);

    ImportData(&Context_68K.pc, Data, 0xC8, 4);

    ImportData(&Context_68K.sr, Data, 0xD0, 2);

    if ((Version >= 3) || (Version == 0))
    {
        if (Data[0xD1] & 0x20)
        {
            // Supervisor
            ImportData(&Context_68K.asp, Data, 0xD2, 2);
        }
        else
        {
            // User
            ImportData(&Context_68K.asp, Data, 0xD6, 2);
        }
    }

    unsigned int offset = 0x2247C; // used to be called GENESIS_LENGTH_EX1

    if (Version == 6)
    {
        //Modif N. - saving more stuff (although a couple of these are saved above in a weird way that I don't trust)
        int tmp;

        ImportDataAuto(&Context_68K.dreg, Data, offset, 4 * 8);
        ImportDataAuto(&Context_68K.areg, Data, offset, 4 * 8);
        ImportDataAuto(&Context_68K.asp, Data, offset, 4);
        ImportDataAuto(&Context_68K.pc, Data, offset, 4);
        ImportDataAuto(&Context_68K.odometer, Data, offset, 4);
        ImportDataAuto(&Context_68K.interrupts, Data, offset, 8);
        ImportDataAuto(&Context_68K.sr, Data, offset, 2);
        ImportDataAuto(&Context_68K.contextfiller00, Data, offset, 2);

        ImportDataAuto(&VDP_Reg.H_Int, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.Set1, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.Set2, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.Pat_ScrA_Adr, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.Pat_ScrA_Adr, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.Pat_Win_Adr, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.Pat_ScrB_Adr, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.Spr_Att_Adr, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.Reg6, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.BG_Color, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.Reg8, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.Reg9, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.H_Int, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.Set3, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.Set4, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.H_Scr_Adr, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.Reg14, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.Auto_Inc, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.Scr_Size, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.Win_H_Pos, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.Win_V_Pos, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.DMA_Length_L, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.DMA_Length_H, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.DMA_Src_Adr_L, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.DMA_Src_Adr_M, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.DMA_Src_Adr_H, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.DMA_Length, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.DMA_Address, Data, offset, 4);

        ImportDataAuto(&Controller_1_Counter, Data, offset, 4);
        ImportDataAuto(&Controller_1_Delay, Data, offset, 4);
        ImportDataAuto(&Controller_1_State, Data, offset, 4);
        ImportDataAuto(&Controller_1_COM, Data, offset, 4);
        ImportDataAuto(&Controller_2_Counter, Data, offset, 4);
        ImportDataAuto(&Controller_2_Delay, Data, offset, 4);
        ImportDataAuto(&Controller_2_State, Data, offset, 4);
        ImportDataAuto(&Controller_2_COM, Data, offset, 4);
        ImportDataAuto(&tmp, Data, offset, 4);
        ImportDataAuto(&tmp, Data, offset, 4);
        ImportDataAuto(&Controller_1_Type, Data, offset, 4);
        ImportDataAuto(&Controller_1_Up, Data, offset, 4);
        ImportDataAuto(&Controller_1_Down, Data, offset, 4);
        ImportDataAuto(&Controller_1_Left, Data, offset, 4);
        ImportDataAuto(&Controller_1_Right, Data, offset, 4);
        ImportDataAuto(&Controller_1_Start, Data, offset, 4);
        ImportDataAuto(&Controller_1_Mode, Data, offset, 4);
        ImportDataAuto(&Controller_1_A, Data, offset, 4);
        ImportDataAuto(&Controller_1_B, Data, offset, 4);
        ImportDataAuto(&Controller_1_C, Data, offset, 4);
        ImportDataAuto(&Controller_1_X, Data, offset, 4);
        ImportDataAuto(&Controller_1_Y, Data, offset, 4);
        ImportDataAuto(&Controller_1_Z, Data, offset, 4);
        ImportDataAuto(&Controller_2_Type, Data, offset, 4);
        ImportDataAuto(&Controller_2_Up, Data, offset, 4);
        ImportDataAuto(&Controller_2_Down, Data, offset, 4);
        ImportDataAuto(&Controller_2_Left, Data, offset, 4);
        ImportDataAuto(&Controller_2_Right, Data, offset, 4);
        ImportDataAuto(&Controller_2_Start, Data, offset, 4);
        ImportDataAuto(&Controller_2_Mode, Data, offset, 4);
        ImportDataAuto(&Controller_2_A, Data, offset, 4);
        ImportDataAuto(&Controller_2_B, Data, offset, 4);
        ImportDataAuto(&Controller_2_C, Data, offset, 4);
        ImportDataAuto(&Controller_2_X, Data, offset, 4);
        ImportDataAuto(&Controller_2_Y, Data, offset, 4);
        ImportDataAuto(&Controller_2_Z, Data, offset, 4);

        ImportDataAuto(&DMAT_Length, Data, offset, 4);
        ImportDataAuto(&DMAT_Type, Data, offset, 4);
        ImportDataAuto(&DMAT_Tmp, Data, offset, 4);
        ImportDataAuto(&VDP_Current_Line, Data, offset, 4);
        ImportDataAuto(&VDP_Num_Vis_Lines, Data, offset, 4);
        ImportDataAuto(&VDP_Num_Vis_Lines, Data, offset, 4);
        ImportDataAuto(&Bank_M68K, Data, offset, 4);
        ImportDataAuto(&tmp, Data, offset, 4);
        ImportDataAuto(&Z80_State, Data, offset, 4);
        ImportDataAuto(&Last_BUS_REQ_Cnt, Data, offset, 4);
        ImportDataAuto(&Last_BUS_REQ_St, Data, offset, 4);
        ImportDataAuto(&Fake_Fetch, Data, offset, 4);
        ImportDataAuto(&Game_Mode, Data, offset, 4);
        ImportDataAuto(&CPU_Mode, Data, offset, 4);
        ImportDataAuto(&CPL_M68K, Data, offset, 4);
        ImportDataAuto(&tmp, Data, offset, 4);
        ImportDataAuto(&CPL_Z80, Data, offset, 4);
        ImportDataAuto(&tmp, Data, offset, 4);
        ImportDataAuto(&Cycles_M68K, Data, offset, 4);
        ImportDataAuto(&Cycles_Z80, Data, offset, 4);
        ImportDataAuto(&VDP_Status, Data, offset, 4);
        ImportDataAuto(&VDP_Int, Data, offset, 4);
        ImportDataAuto(&Ctrl.Write, Data, offset, 4);
        ImportDataAuto(&Ctrl.DMA_Mode, Data, offset, 4);
        ImportDataAuto(&Ctrl.DMA, Data, offset, 4);
        //ImportDataAuto(&CRam_Flag, Data, offset, 4); //Causes screen to blank
        //offset+=4;
        ImportDataAuto(&LagCount, Data, offset, 4);
        ImportDataAuto(&VRam_Flag, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.DMA_Length, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.Auto_Inc, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.DMA_Length, Data, offset, 4);
        ////	ImportDataAuto(VRam, Data, offset, 65536);
        ImportDataAuto(CRam, Data, offset, 512);
        ImportDataAuto(H_Counter_Table, Data, offset, 512 * 2);
        ////	ImportDataAuto(Spr_Link, Data, offset, 4*256);
        ////	extern int DMAT_Tmp, VSRam_Over;
        ////	ImportDataAuto(&DMAT_Tmp, Data, offset, 4);
        ////	ImportDataAuto(&VSRam_Over, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.DMA_Length_L, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.DMA_Length_H, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.DMA_Src_Adr_L, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.DMA_Src_Adr_M, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.DMA_Src_Adr_H, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.DMA_Length, Data, offset, 4);
        ImportDataAuto(&VDP_Reg.DMA_Address, Data, offset, 4);
    }
    else if (Version >= 7)
    {
        int tmp;

        if (Version >= 9) offset += 4; // alignment for performance
        YM2612_Restore_Full(Data + offset); // some important parts of this weren't saved above
        offset += sizeof(ym2612_);

        ImportDataAuto(&PSG, Data, offset, sizeof(struct _psg)); // some important parts of this weren't saved above

        //PC and BasePC are 32-bit pointers to system memory space. Only the low 16-bits of PC are valid in the emulated system
        //and BasePC is a pointer to the location (in system memory) of a particular variable
        //Since what BasePC's value _should_ be can change for a given state between sessions/instances
        //and will always be set correctly by the "z80_Set_PC" earlier, we should _not_ allow it to be overwritten here
        int Z80_BasePC = M_Z80.BasePC;
        int Z80_PC = M_Z80.PC.d;
        ImportDataAuto(&M_Z80, Data, offset, 0x5C); // some important parts of this weren't saved above
        M_Z80.PC.d = Z80_PC;
        M_Z80.BasePC = Z80_BasePC;
        ImportDataAuto(&M_Z80.RetIC, Data, offset, 4); // not sure about the last two variables, might as well save them too
        ImportDataAuto(&M_Z80.IntAckC, Data, offset, 4);

        ImportDataAuto(&Context_68K.dreg[0], Data, offset, 86); // some important parts of this weren't saved above

        if (Version >= 9) offset += 6; // alignment for performance
        ImportDataAuto(&Controller_1_State, Data, offset, 448); // apparently necessary (note: 448 == (((char*)&Controller_2D_Z)+sizeof(Controller_2D_Z) - (char*)&Controller_1_State))

        // apparently necessary
        ImportDataAuto(&VDP_Status, Data, offset, 4);
        ImportDataAuto(&VDP_Int, Data, offset, 4);
        ImportDataAuto(&VDP_Current_Line, Data, offset, 4);
        ImportDataAuto(&VDP_Num_Lines, Data, offset, 4);
        ImportDataAuto(&VDP_Num_Vis_Lines, Data, offset, 4);
        ImportDataAuto(&DMAT_Length, Data, offset, 4);
        ImportDataAuto(&DMAT_Type, Data, offset, 4);
        //ImportDataAuto(&CRam_Flag, Data, offset, 4); //emulator flag which causes Gens not to update its draw palette, but doesn't affect sync state
        ImportDataAuto(&LagCount, Data, offset, 4);
        ImportDataAuto(&VRam_Flag, Data, offset, 4);
        if (Version >= 9) offset += 12; // alignment for performance
        ImportDataAuto(&CRam, Data, offset, 256 * 2);

        // it's probably safer sync-wise to keep SRAM stuff in the savestate
        unsigned int preSRAMoffset = offset;
        offset += sizeof(SRAM);
        ImportDataAuto(&SRAM_Start, Data, offset, 4);
        ImportDataAuto(&SRAM_End, Data, offset, 4);
        ImportDataAuto(&SRAM_ON, Data, offset, 4);
        ImportDataAuto(&SRAM_Write, Data, offset, 4);
        ImportDataAuto(&SRAM_Custom, Data, offset, 4);
        unsigned int postSRAMoffset = offset;
        if (SRAM_End != SRAM_Start)
        {
            offset = preSRAMoffset;
            ImportDataAuto(&SRAM, Data, offset, sizeof(SRAM));
            offset = postSRAMoffset;
        }

        // this group I'm not sure about, they don't seem to be necessary but I'm keeping them around just in case
        ImportDataAuto(&Bank_M68K, Data, offset, 4);
        ImportDataAuto(&tmp, Data, offset, 4);
        ImportDataAuto(&Z80_State, Data, offset, 4);
        ImportDataAuto(&Last_BUS_REQ_Cnt, Data, offset, 4);
        ImportDataAuto(&Last_BUS_REQ_St, Data, offset, 4);
        ImportDataAuto(&Fake_Fetch, Data, offset, 4);
        ImportDataAuto(&Game_Mode, Data, offset, 4);
        ImportDataAuto(&CPU_Mode, Data, offset, 4);
        ImportDataAuto(&CPL_M68K, Data, offset, 4);
        ImportDataAuto(&tmp, Data, offset, 4);
        ImportDataAuto(&CPL_Z80, Data, offset, 4);
        ImportDataAuto(&tmp, Data, offset, 4);
        ImportDataAuto(&Cycles_M68K, Data, offset, 4);
        ImportDataAuto(&Cycles_Z80, Data, offset, 4);
        ImportDataAuto(&Gen_Mode, Data, offset, 4);
        ImportDataAuto(&Gen_Version, Data, offset, 4);
        if (Version >= 9) offset += 12; // alignment for performance
        ImportDataAuto(H_Counter_Table, Data, offset, 512 * 2);
        ImportDataAuto(&VDP_Reg, Data, offset, sizeof(VDP_Reg));
        ImportDataAuto(&Ctrl, Data, offset, sizeof(Ctrl));

        ImportDataAuto(&Context_68K.cycles_needed, Data, offset, 44);

        if (Version >= 8)
        {
            ImportDataAuto(&LagCountPersistent, Data, offset, 4);
            ImportDataAuto(&Lag_Frame, Data, offset, 1);
        }
        if (Version >= 9) offset += 11; // alignment for performance
    }

    main68k_SetContext(&Context_68K);

#ifdef _DEBUG
    if (Version == LATEST_SAVESTATE_VERSION)
    {
        int desiredoffset = GENESIS_STATE_LENGTH;
        assert(offset == desiredoffset);
        assert((offset & 15) == 0); // also want this for alignment performance reasons
    }
#endif
    if (Version == 6)
        len += GENESIS_V6_LENGTH - GENESIS_STATE_LENGTH;
    else if (Version == 7)
        len += GENESIS_V7_LENGTH - GENESIS_STATE_LENGTH;
    else if (Version == 8)
        len += GENESIS_V8_LENGTH - GENESIS_STATE_LENGTH;
    //else if(Version == 9) // in the future...
    //	len += GENESIS_V9_LENGTH - GENESIS_STATE_LENGTH;
    return len;
}

void Export_Genesis(unsigned char * Data)
{
    S68000CONTEXT Context_68K; // Modif N.: apparently no longer necessary but I'm leaving it here just to be safe: purposely shadows the global Context_68K variable with this local copy to avoid tampering with it while saving
    unsigned char* src;
    int i;

    InBaseGenesis = 1;

    //if(DMAT_Length)
    //WARNINGBOX("Saving during DMA transfer; savestate may be corrupt. Try advancing the frame and saving again.", "Warning");

    //   while (DMAT_Length) Update_DMA();      // Be sure to finish DMA before save //Modif N.: commented out because it may cause saving to change the current state

    Data[0x00] = 'G';
    Data[0x01] = 'S';
    Data[0x02] = 'T';
    Data[0x03] = 0x40;
    Data[0x04] = 0xE0;

    Version = LATEST_SAVESTATE_VERSION;
    Data[0x50] = Version;      // Version
    Data[0x51] = 0;      // Gens

    PSG_Save_State();

    for (i = 0; i < 8; i++)
    {
        Data[0x60 + i * 2] = PSG_Save[i] & 0xFF;
        Data[0x61 + i * 2] = (PSG_Save[i] >> 8) & 0xFF;
    }

#ifdef _DEBUG
    int contextsize1 = main68k_GetContextSize();
    int contextsize2 = sizeof(Context_68K);
    assert(contextsize1 == contextsize2);
#endif

    main68k_GetContext(&Context_68K);

    ExportData(&Context_68K.dreg[0], Data, 0x80, 8 * 2 * 4);

    ExportData(&Context_68K.pc, Data, 0xC8, 4);

    ExportData(&Context_68K.sr, Data, 0xD0, 2);

    if (Context_68K.sr & 0x2000)
    {
        ExportData(&Context_68K.asp, Data, 0xD2, 4);
        ExportData(&Context_68K.areg[7], Data, 0xD6, 4);
    }
    else
    {
        ExportData(&Context_68K.asp, Data, 0xD6, 4);
        ExportData(&Context_68K.areg[7], Data, 0xD2, 4);
    }

    ExportData(&Ctrl.Data, Data, 0x40, 4);

    Data[0x44] = Ctrl.Flag;
    Data[0x45] = (Ctrl.DMA >> 2) & 1;

    Data[0x46] = Ctrl.Access & 0xFF; //Nitsuja added this
    Data[0x47] = (Ctrl.Access >> 8) & 0xFF; //Nitsuja added this

    Data[0x48] = Ctrl.Address & 0xFF;
    Data[0x49] = (Ctrl.Address >> 8) & 0xFF;

    VDP_Reg.DMA_Length_L = VDP_Reg.DMA_Length & 0xFF;
    VDP_Reg.DMA_Length_H = (VDP_Reg.DMA_Length >> 8) & 0xFF;

    VDP_Reg.DMA_Src_Adr_L = VDP_Reg.DMA_Address & 0xFF;
    VDP_Reg.DMA_Src_Adr_M = (VDP_Reg.DMA_Address >> 8) & 0xFF;
    VDP_Reg.DMA_Src_Adr_H = (VDP_Reg.DMA_Address >> 16) & 0xFF;

    VDP_Reg.DMA_Src_Adr_H |= Ctrl.DMA_Mode & 0xC0;

    src = (unsigned char *)&(VDP_Reg.Set1);
    for (i = 0; i < 24; i++)
    {
        Data[0xFA + i] = *src;
        src += 4;
    }

    YM2612_Save(Data + 0x1E4);

    Data[0x404] = (unsigned char)(z80_Get_AF(&M_Z80) & 0xFF);
    Data[0x405] = (unsigned char)(z80_Get_AF(&M_Z80) >> 8);
    Data[0x406] = (unsigned char)(M_Z80.AF.b.FXY & 0xFF); //Modif N
    Data[0x407] = (unsigned char)0; //Modif N
    Data[0x408] = (unsigned char)(M_Z80.BC.w.BC & 0xFF);
    Data[0x409] = (unsigned char)(M_Z80.BC.w.BC >> 8);
    Data[0x40C] = (unsigned char)(M_Z80.DE.w.DE & 0xFF);
    Data[0x40D] = (unsigned char)(M_Z80.DE.w.DE >> 8);
    Data[0x410] = (unsigned char)(M_Z80.HL.w.HL & 0xFF);
    Data[0x411] = (unsigned char)(M_Z80.HL.w.HL >> 8);
    Data[0x414] = (unsigned char)(M_Z80.IX.w.IX & 0xFF);
    Data[0x415] = (unsigned char)(M_Z80.IX.w.IX >> 8);
    Data[0x418] = (unsigned char)(M_Z80.IY.w.IY & 0xFF);
    Data[0x419] = (unsigned char)(M_Z80.IY.w.IY >> 8);
    Data[0x41C] = (unsigned char)(z80_Get_PC(&M_Z80) & 0xFF);
    Data[0x41D] = (unsigned char)((z80_Get_PC(&M_Z80) >> 8) & 0xFF);
    Data[0x420] = (unsigned char)(M_Z80.SP.w.SP & 0xFF);
    Data[0x421] = (unsigned char)(M_Z80.SP.w.SP >> 8);
    Data[0x424] = (unsigned char)(z80_Get_AF2(&M_Z80) & 0xFF);
    Data[0x425] = (unsigned char)(z80_Get_AF2(&M_Z80) >> 8);
    Data[0x428] = (unsigned char)(M_Z80.BC2.w.BC2 & 0xFF);
    Data[0x429] = (unsigned char)(M_Z80.BC2.w.BC2 >> 8);
    Data[0x42C] = (unsigned char)(M_Z80.DE2.w.DE2 & 0xFF);
    Data[0x42D] = (unsigned char)(M_Z80.DE2.w.DE2 >> 8);
    Data[0x430] = (unsigned char)(M_Z80.HL2.w.HL2 & 0xFF);
    Data[0x431] = (unsigned char)(M_Z80.HL2.w.HL2 >> 8);
    Data[0x434] = (unsigned char)(M_Z80.I);
    Data[0x436] = (unsigned char)(M_Z80.IFF.b.IFF1 >> 2);

    Data[0x438] = (unsigned char)(((Z80_State & 4) >> 2) ^ 1);
    Data[0x439] = (unsigned char)(((Z80_State & 2) >> 1) ^ 1);

    ExportData(&Bank_Z80, Data, 0x43C, 4);

    for (i = 0; i < 0x40; i++)
    {
        Data[i * 2 + 0x112 + 1] = ((CRam[i] >> 8) & 0xFF);
        Data[i * 2 + 0x112 + 0] = ((CRam[i] >> 0) & 0xFF);
    }

    ExportData(VSRam, Data, 0x192, 0x50);
    ExportData(Ram_Z80, Data, 0x474, 0x2000);

    for (i = 0; i < 0x10000; i += 2)
    {
        Data[i + 0x2478 + 1] = Ram_68k[i + 0];
        Data[i + 0x2478 + 0] = Ram_68k[i + 1];
    }

    for (i = 0; i < 0x10000; i += 2)
    {
        Data[i + 0x12478 + 1] = VRam[i + 0];
        Data[i + 0x12478 + 0] = VRam[i + 1];
    }
    Data[0x22478] = unsigned char(FrameCount & 0xFF);   //Modif
    Data[0x22479] = unsigned char((FrameCount >> 8) & 0xFF);   //Modif
    Data[0x2247A] = unsigned char((FrameCount >> 16) & 0xFF);   //Modif
    Data[0x2247B] = unsigned char((FrameCount >> 24) & 0xFF);   //Modif

    // everything after this should use this offset variable for ease of extensibility
    // Modif U. - got rid of about 12 KB of 00 bytes.
    unsigned int offset = 0x2247C; // used to be called GENESIS_LENGTH_EX1

    // version 7 additions (version 6 additions deleted)
    {
        int tmp = 0;
        //Modif N. - saving more stuff (added everything after this)

        if (Version >= 9) offset += 4; // alignment for performance
        YM2612_Save_Full(Data + offset); // some important parts of this weren't saved above
        offset += sizeof(ym2612_);

        ExportDataAuto(&PSG, Data, offset, sizeof(struct _psg));  // some important parts of this weren't saved above

        ExportDataAuto(&M_Z80, Data, offset, 0x5C); // some important parts of this weren't saved above
        ExportDataAuto(&M_Z80.RetIC, Data, offset, 4); // not sure about the last two variables, might as well save them too
        ExportDataAuto(&M_Z80.IntAckC, Data, offset, 4);

        ExportDataAuto(&Context_68K.dreg[0], Data, offset, 86); // some important parts of this weren't saved above

        if (Version >= 9) offset += 6; // alignment for performance
        ExportDataAuto(&Controller_1_State, Data, offset, 448);   // apparently necessary (note: 448 == (((char*)&Controller_2D_Z)+sizeof(Controller_2D_Z) - (char*)&Controller_1_State))

        // apparently necessary
        ExportDataAuto(&VDP_Status, Data, offset, 4);
        ExportDataAuto(&VDP_Int, Data, offset, 4);
        ExportDataAuto(&VDP_Current_Line, Data, offset, 4);
        ExportDataAuto(&VDP_Num_Lines, Data, offset, 4);
        ExportDataAuto(&VDP_Num_Vis_Lines, Data, offset, 4);
        ExportDataAuto(&DMAT_Length, Data, offset, 4);
        ExportDataAuto(&DMAT_Type, Data, offset, 4);
        //ExportDataAuto(&CRam_Flag, Data, offset, 4);
        ExportDataAuto(&LagCount, Data, offset, 4);
        ExportDataAuto(&VRam_Flag, Data, offset, 4);
        if (Version >= 9) offset += 12; // alignment for performance
        ExportDataAuto(&CRam, Data, offset, 256 * 2);

        // it's probably safer sync-wise to keep SRAM stuff in the savestate
        if (SRAM_End != SRAM_Start)
            ExportDataAuto(&SRAM, Data, offset, sizeof(SRAM));
        else
            offset += sizeof(SRAM);
        ExportDataAuto(&SRAM_Start, Data, offset, 4);
        ExportDataAuto(&SRAM_End, Data, offset, 4);
        ExportDataAuto(&SRAM_ON, Data, offset, 4);
        ExportDataAuto(&SRAM_Write, Data, offset, 4);
        ExportDataAuto(&SRAM_Custom, Data, offset, 4);

        // this group I'm not sure about, they don't seem to be necessary but I'm keeping them around just in case
        ExportDataAuto(&Bank_M68K, Data, offset, 4);
        ExportDataAuto(&tmp, Data, offset, 4);
        ExportDataAuto(&Z80_State, Data, offset, 4);
        ExportDataAuto(&Last_BUS_REQ_Cnt, Data, offset, 4);
        ExportDataAuto(&Last_BUS_REQ_St, Data, offset, 4);
        ExportDataAuto(&Fake_Fetch, Data, offset, 4);
        ExportDataAuto(&Game_Mode, Data, offset, 4);
        ExportDataAuto(&CPU_Mode, Data, offset, 4);
        ExportDataAuto(&CPL_M68K, Data, offset, 4);
        ExportDataAuto(&tmp, Data, offset, 4);
        ExportDataAuto(&CPL_Z80, Data, offset, 4);
        ExportDataAuto(&tmp, Data, offset, 4);
        ExportDataAuto(&Cycles_M68K, Data, offset, 4);
        ExportDataAuto(&Cycles_Z80, Data, offset, 4);
        ExportDataAuto(&Gen_Mode, Data, offset, 4);
        ExportDataAuto(&Gen_Version, Data, offset, 4);
        if (Version >= 9) offset += 12; // alignment for performance
        ExportDataAuto(H_Counter_Table, Data, offset, 512 * 2);
        ExportDataAuto(&VDP_Reg, Data, offset, sizeof(VDP_Reg));
        ExportDataAuto(&Ctrl, Data, offset, sizeof(Ctrl));

        ExportDataAuto(&Context_68K.cycles_needed, Data, offset, 44);

        // version 8 additions:
        {
            ExportDataAuto(&LagCountPersistent, Data, offset, 4);
            ExportDataAuto(&Lag_Frame, Data, offset, 1);
        }
        if (Version >= 9) offset += 11; // alignment for performance
    }

#ifdef _DEBUG
    // assert that the final offset value equals our savestate size, otherwise we screwed up
    // if it fails, that probably means you have to add ((int)offset-(int)desiredoffset) to the last GENESIS_LENGTH_EX define
    int desiredoffset = GENESIS_STATE_LENGTH;
    assert(offset == desiredoffset);
    assert((offset & 15) == 0); // also want this for alignment performance reasons
#endif
}

int Save_Config(char *File_Name)
{
    char Conf_File[1024];

    strcpy(Conf_File, File_Name);

    WritePrivateProfileString("General", "Rom path", Rom_Dir, Conf_File);
    WritePrivateProfileString("General", "Save path", State_Dir, Conf_File);
    WritePrivateProfileString("General", "SRAM path", SRAM_Dir, Conf_File);
    WritePrivateProfileString("General", "Dump path", Dump_Dir, Conf_File);
    WritePrivateProfileString("General", "Patch path", Patch_Dir, Conf_File);
    WritePrivateProfileString("General", "IPS Patch path", IPS_Dir, Conf_File);
    WritePrivateProfileString("General", "Movie path", Movie_Dir, Conf_File);
    WritePrivateProfileString("General", "Watch path", Watch_Dir, Conf_File);
    WritePrivateProfileString("General", "Lua path", Lua_Dir, Conf_File);

    WritePrivateProfileString("General", "Genesis Bios", Genesis_Bios, Conf_File);

    for (int i = 0; i < MAX_RECENT_ROMS; i++)
    {
        char str[256];
        sprintf(str, "Rom %d", i + 1);
        WritePrivateProfileString("General", str, Recent_Rom[i], Conf_File);
    }

    for (int i = 0; i < MAX_RECENT_MOVIES; i++)
    {
        char str[256];
        sprintf(str, "Movie %d", i + 1);
        WritePrivateProfileString("General", str, Recent_Movie[i], Conf_File);
    }

    for (int i = 0; i < MAX_RECENT_SCRIPTS; i++)
    {
        char str[256];
        sprintf(str, "Script %d", i + 1);
        WritePrivateProfileString("General", str, Recent_Scripts[i], Conf_File);
    }

    wsprintf(Str_Tmp, "%d", File_Type_Index);
    WritePrivateProfileString("General", "File type index", Str_Tmp, Conf_File);
    WritePrivateProfileString("General", "File type index version", "1", Conf_File);

    wsprintf(Str_Tmp, "%d", Current_State);
    WritePrivateProfileString("General", "State Number", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Window_Pos.x);
    WritePrivateProfileString("General", "Window X", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Window_Pos.y);
    WritePrivateProfileString("General", "Window Y", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Intro_Style); //Modif N. - Save intro style to config, instead of only reading it from there
    WritePrivateProfileString("General", "Intro Style", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Effect_Color);
    WritePrivateProfileString("General", "Free Mode Color", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Def_Read_Only); //Upth-Add - Save the default read only flag to config
    WritePrivateProfileString("General", "Movie Default Read Only", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", AutoCloseMovie); //Upth-Add - Save the auto close movie flag to config
    WritePrivateProfileString("General", "Movie Auto Close", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", UseMovieStates); //Upth-Add - Save the auto close movie flag to config
    WritePrivateProfileString("General", "Movie Based State Names", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", SlowDownSpeed); //Upth-Add - Save the current slowdown speed to config
    WritePrivateProfileString("General", "Slow Down Speed", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", DelayFactor); //Upth-Add - Make frame advance speed configurable
    WritePrivateProfileString("General", "Frame Advance Delay Factor", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Sleep_Time); //Modif N. - CPU hogging now a real setting
    WritePrivateProfileString("General", "Allow Idle", Str_Tmp, Conf_File);

    wsprintf(Str_Tmp, "%d", Full_Screen & 1);
    WritePrivateProfileString("Graphics", "Full Screen", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", FS_VSync & 1);
    WritePrivateProfileString("Graphics", "Full Screen VSync", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Res_X); //Upth-Add - Save our full screen X resolution to config
    WritePrivateProfileString("Graphics", "Full Screen X Resolution", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Res_Y); //Upth-Add - Save our full screen Y resolution to config
    WritePrivateProfileString("Graphics", "Full Screen Y Resolution", Str_Tmp, Conf_File);
    sprintf(Str_Tmp, "%.8f", ScaleFactor); // wsprintf forbids float
    WritePrivateProfileString("Graphics", "Screen Scale Factor", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", FS_No_Res_Change); //Upth-Add - Save our fullscreen without resolution change flag
    WritePrivateProfileString("Graphics", "Full Screen No Res Change", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", W_VSync & 1);
    WritePrivateProfileString("Graphics", "Windows VSync", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Render_W);
    WritePrivateProfileString("Graphics", "Render Windowed", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Render_FS);
    WritePrivateProfileString("Graphics", "Render Fullscreen", Str_Tmp, Conf_File);

    wsprintf(Str_Tmp, "%d", Never_Skip_Frame); //Modif N. -- added never skip frame to preferences
    WritePrivateProfileString("Graphics", "Never Skip Frame", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", VideoLatencyCompensation);
    WritePrivateProfileString("Graphics", "Video Latency Compensation", Str_Tmp, Conf_File);

    wsprintf(Str_Tmp, "%d", Stretch & 1);
    WritePrivateProfileString("Graphics", "Stretch", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Blit_Soft & 1);
    WritePrivateProfileString("Graphics", "Software Blit", Str_Tmp, Conf_File);

    wsprintf(Str_Tmp, "%d", Contrast_Level);
    WritePrivateProfileString("Graphics", "Contrast", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Brightness_Level);
    WritePrivateProfileString("Graphics", "Brightness", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Greyscale & 1);
    WritePrivateProfileString("Graphics", "Greyscale", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Invert_Color & 1);
    WritePrivateProfileString("Graphics", "Invert", Str_Tmp, Conf_File);

    wsprintf(Str_Tmp, "%d", ScrollAOn);
    WritePrivateProfileString("Graphics", "Scroll A layer", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", ScrollBOn);
    WritePrivateProfileString("Graphics", "Scroll B layer", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", SpriteOn);
    WritePrivateProfileString("Graphics", "Sprites layer", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Sprite_Over & 1);
    WritePrivateProfileString("Graphics", "Sprite limit", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", PinkBG);
    WritePrivateProfileString("Graphics", "Pink Background", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Frame_Skip);
    WritePrivateProfileString("Graphics", "Frame skip", Str_Tmp, Conf_File);

    wsprintf(Str_Tmp, "%d", Correct_256_Aspect_Ratio); //Modif N.
    WritePrivateProfileString("Graphics", "Proper Aspect Ratio", Str_Tmp, Conf_File);

    wsprintf(Str_Tmp, "%d", Sound_Rate);
    WritePrivateProfileString("Sound", "Rate", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Sound_Stereo);
    WritePrivateProfileString("Sound", "Stereo", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Sound_Soften); // Modif N.
    WritePrivateProfileString("Sound", "SoundSoftenFilter", Str_Tmp, Conf_File); // Modif N.
    wsprintf(Str_Tmp, "%d", Z80_State & 1);
    WritePrivateProfileString("Sound", "Z80 State", Str_Tmp, Conf_File);

    wsprintf(Str_Tmp, "%d", Country);
    WritePrivateProfileString("CPU", "Country", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Country_Order[0]);
    WritePrivateProfileString("CPU", "Prefered Country 1", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Country_Order[1]);
    WritePrivateProfileString("CPU", "Prefered Country 2", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Country_Order[2]);
    WritePrivateProfileString("CPU", "Prefered Country 3", Str_Tmp, Conf_File);

    wsprintf(Str_Tmp, "%d", HexCommon.DialogPosX);
    WritePrivateProfileString("Tools", "Hex Editor X Position", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", HexCommon.DialogPosY);
    WritePrivateProfileString("Tools", "Hex Editor Y Position", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", HexCommon.OffsetVisibleFirst);
    WritePrivateProfileString("Tools", "Hex Editor First Offset", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", HexCommon.OffsetVisibleTotal);
    WritePrivateProfileString("Tools", "Hex Editor Total Offset", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", HexCommon.TextView);
    WritePrivateProfileString("Tools", "Hex Editor Text View", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", HexCommon.DrawLines);
    WritePrivateProfileString("Tools", "Hex Editor Draw Lines", Str_Tmp, Conf_File);

    wsprintf(Str_Tmp, "%d", Fast_Blur & 1);
    WritePrivateProfileString("Options", "Fast Blur", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Show_FPS & 1);
    WritePrivateProfileString("Options", "FPS", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", FPS_Style);
    WritePrivateProfileString("Options", "FPS Style", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Show_Message & 1);
    WritePrivateProfileString("Options", "Message", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Message_Style);
    WritePrivateProfileString("Options", "Message Style", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Auto_Fix_CS & 1);
    WritePrivateProfileString("Options", "Auto Fix Checksum", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Auto_Pause & 1);
    WritePrivateProfileString("Options", "Auto Pause", Str_Tmp, Conf_File);

    wsprintf(Str_Tmp, "%d", Disable_Blue_Screen);//Modif
    WritePrivateProfileString("Options", "Disable Blue Screen", Str_Tmp, Conf_File);//Modif
    wsprintf(Str_Tmp, "%d", FrameCounterEnabled);//Modif
    WritePrivateProfileString("Options", "FrameCounterEnabled", Str_Tmp, Conf_File);//Modif
    wsprintf(Str_Tmp, "%d", FrameCounterFrames);//Modif N.
    WritePrivateProfileString("Options", "FrameCounterFrames", Str_Tmp, Conf_File);//Modif N.
    wsprintf(Str_Tmp, "%d", FrameCounterPosition);//Modif
    WritePrivateProfileString("Options", "FrameCounterPosition", Str_Tmp, Conf_File);//Modif
    wsprintf(Str_Tmp, "%d", LagCounterEnabled);//Modif
    WritePrivateProfileString("Options", "LagCounterEnabled", Str_Tmp, Conf_File);//Modif
    wsprintf(Str_Tmp, "%d", LagCounterFrames);//Modif N.
    WritePrivateProfileString("Options", "LagCounterFrames", Str_Tmp, Conf_File);//Modif N.
    wsprintf(Str_Tmp, "%d", ShowInputEnabled);//Modif
    WritePrivateProfileString("Options", "ShowInputEnabled", Str_Tmp, Conf_File);//Modif
    wsprintf(Str_Tmp, "%d", AutoBackupEnabled);//Modif
    WritePrivateProfileString("Options", "AutoBackupEnabled", Str_Tmp, Conf_File);//Modif

    wsprintf(Str_Tmp, "%d", Controller_1_Type & 0x13);
    WritePrivateProfileString("Input", "P1.Type", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[0].Up);
    WritePrivateProfileString("Input", "P1.Up", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[0].Down);
    WritePrivateProfileString("Input", "P1.Down", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[0].Left);
    WritePrivateProfileString("Input", "P1.Left", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[0].Right);
    WritePrivateProfileString("Input", "P1.Right", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[0].Start);
    WritePrivateProfileString("Input", "P1.Start", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[0].A);
    WritePrivateProfileString("Input", "P1.A", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[0].B);
    WritePrivateProfileString("Input", "P1.B", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[0].C);
    WritePrivateProfileString("Input", "P1.C", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[0].Mode);
    WritePrivateProfileString("Input", "P1.Mode", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[0].X);
    WritePrivateProfileString("Input", "P1.X", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[0].Y);
    WritePrivateProfileString("Input", "P1.Y", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[0].Z);
    WritePrivateProfileString("Input", "P1.Z", Str_Tmp, Conf_File);

    wsprintf(Str_Tmp, "%d", Controller_1B_Type & 0x03);
    WritePrivateProfileString("Input", "P1B.Type", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[2].Up);
    WritePrivateProfileString("Input", "P1B.Up", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[2].Down);
    WritePrivateProfileString("Input", "P1B.Down", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[2].Left);
    WritePrivateProfileString("Input", "P1B.Left", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[2].Right);
    WritePrivateProfileString("Input", "P1B.Right", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[2].Start);
    WritePrivateProfileString("Input", "P1B.Start", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[2].A);
    WritePrivateProfileString("Input", "P1B.A", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[2].B);
    WritePrivateProfileString("Input", "P1B.B", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[2].C);
    WritePrivateProfileString("Input", "P1B.C", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[2].Mode);
    WritePrivateProfileString("Input", "P1B.Mode", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[2].X);
    WritePrivateProfileString("Input", "P1B.X", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[2].Y);
    WritePrivateProfileString("Input", "P1B.Y", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[2].Z);
    WritePrivateProfileString("Input", "P1B.Z", Str_Tmp, Conf_File);

    wsprintf(Str_Tmp, "%d", Controller_1C_Type & 0x03);
    WritePrivateProfileString("Input", "P1C.Type", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[3].Up);
    WritePrivateProfileString("Input", "P1C.Up", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[3].Down);
    WritePrivateProfileString("Input", "P1C.Down", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[3].Left);
    WritePrivateProfileString("Input", "P1C.Left", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[3].Right);
    WritePrivateProfileString("Input", "P1C.Right", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[3].Start);
    WritePrivateProfileString("Input", "P1C.Start", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[3].A);
    WritePrivateProfileString("Input", "P1C.A", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[3].B);
    WritePrivateProfileString("Input", "P1C.B", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[3].C);
    WritePrivateProfileString("Input", "P1C.C", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[3].Mode);
    WritePrivateProfileString("Input", "P1C.Mode", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[3].X);
    WritePrivateProfileString("Input", "P1C.X", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[3].Y);
    WritePrivateProfileString("Input", "P1C.Y", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[3].Z);
    WritePrivateProfileString("Input", "P1C.Z", Str_Tmp, Conf_File);

    wsprintf(Str_Tmp, "%d", Controller_1D_Type & 0x03);
    WritePrivateProfileString("Input", "P1D.Type", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[4].Up);
    WritePrivateProfileString("Input", "P1D.Up", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[4].Down);
    WritePrivateProfileString("Input", "P1D.Down", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[4].Left);
    WritePrivateProfileString("Input", "P1D.Left", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[4].Right);
    WritePrivateProfileString("Input", "P1D.Right", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[4].Start);
    WritePrivateProfileString("Input", "P1D.Start", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[4].A);
    WritePrivateProfileString("Input", "P1D.A", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[4].B);
    WritePrivateProfileString("Input", "P1D.B", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[4].C);
    WritePrivateProfileString("Input", "P1D.C", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[4].Mode);
    WritePrivateProfileString("Input", "P1D.Mode", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[4].X);
    WritePrivateProfileString("Input", "P1D.X", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[4].Y);
    WritePrivateProfileString("Input", "P1D.Y", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[4].Z);
    WritePrivateProfileString("Input", "P1D.Z", Str_Tmp, Conf_File);

    wsprintf(Str_Tmp, "%d", Controller_2_Type & 0x13);
    WritePrivateProfileString("Input", "P2.Type", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[1].Up);
    WritePrivateProfileString("Input", "P2.Up", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[1].Down);
    WritePrivateProfileString("Input", "P2.Down", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[1].Left);
    WritePrivateProfileString("Input", "P2.Left", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[1].Right);
    WritePrivateProfileString("Input", "P2.Right", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[1].Start);
    WritePrivateProfileString("Input", "P2.Start", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[1].A);
    WritePrivateProfileString("Input", "P2.A", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[1].B);
    WritePrivateProfileString("Input", "P2.B", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[1].C);
    WritePrivateProfileString("Input", "P2.C", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[1].Mode);
    WritePrivateProfileString("Input", "P2.Mode", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[1].X);
    WritePrivateProfileString("Input", "P2.X", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[1].Y);
    WritePrivateProfileString("Input", "P2.Y", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[1].Z);
    WritePrivateProfileString("Input", "P2.Z", Str_Tmp, Conf_File);

    wsprintf(Str_Tmp, "%d", Controller_2B_Type & 0x03);
    WritePrivateProfileString("Input", "P2B.Type", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[5].Up);
    WritePrivateProfileString("Input", "P2B.Up", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[5].Down);
    WritePrivateProfileString("Input", "P2B.Down", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[5].Left);
    WritePrivateProfileString("Input", "P2B.Left", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[5].Right);
    WritePrivateProfileString("Input", "P2B.Right", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[5].Start);
    WritePrivateProfileString("Input", "P2B.Start", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[5].A);
    WritePrivateProfileString("Input", "P2B.A", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[5].B);
    WritePrivateProfileString("Input", "P2B.B", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[5].C);
    WritePrivateProfileString("Input", "P2B.C", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[5].Mode);
    WritePrivateProfileString("Input", "P2B.Mode", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[5].X);
    WritePrivateProfileString("Input", "P2B.X", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[5].Y);
    WritePrivateProfileString("Input", "P2B.Y", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[5].Z);
    WritePrivateProfileString("Input", "P2B.Z", Str_Tmp, Conf_File);

    wsprintf(Str_Tmp, "%d", Controller_2C_Type & 0x03);
    WritePrivateProfileString("Input", "P2C.Type", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[6].Up);
    WritePrivateProfileString("Input", "P2C.Up", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[6].Down);
    WritePrivateProfileString("Input", "P2C.Down", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[6].Left);
    WritePrivateProfileString("Input", "P2C.Left", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[6].Right);
    WritePrivateProfileString("Input", "P2C.Right", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[6].Start);
    WritePrivateProfileString("Input", "P2C.Start", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[6].A);
    WritePrivateProfileString("Input", "P2C.A", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[6].B);
    WritePrivateProfileString("Input", "P2C.B", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[6].C);
    WritePrivateProfileString("Input", "P2C.C", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[6].Mode);
    WritePrivateProfileString("Input", "P2C.Mode", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[6].X);
    WritePrivateProfileString("Input", "P2C.X", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[6].Y);
    WritePrivateProfileString("Input", "P2C.Y", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[6].Z);
    WritePrivateProfileString("Input", "P2C.Z", Str_Tmp, Conf_File);

    wsprintf(Str_Tmp, "%d", Controller_2D_Type & 0x03);
    WritePrivateProfileString("Input", "P2D.Type", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[7].Up);
    WritePrivateProfileString("Input", "P2D.Up", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[7].Down);
    WritePrivateProfileString("Input", "P2D.Down", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[7].Left);
    WritePrivateProfileString("Input", "P2D.Left", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[7].Right);
    WritePrivateProfileString("Input", "P2D.Right", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[7].Start);
    WritePrivateProfileString("Input", "P2D.Start", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[7].A);
    WritePrivateProfileString("Input", "P2D.A", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[7].B);
    WritePrivateProfileString("Input", "P2D.B", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[7].C);
    WritePrivateProfileString("Input", "P2D.C", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[7].Mode);
    WritePrivateProfileString("Input", "P2D.Mode", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[7].X);
    WritePrivateProfileString("Input", "P2D.X", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[7].Y);
    WritePrivateProfileString("Input", "P2D.Y", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", Keys_Def[7].Z);
    WritePrivateProfileString("Input", "P2D.Z", Str_Tmp, Conf_File);

    wsprintf(Str_Tmp, "%d", LeftRightEnabled);//Modif
    WritePrivateProfileString("Input", "LeftRightEnabled", Str_Tmp, Conf_File);//Modif
    wsprintf(Str_Tmp, "%d", (BackgroundInput ? 1 : 0)); //Upth-Add - Allow hotkeys and joypad to function while gens window is inactive
    WritePrivateProfileString("Input", "Allow Background Input", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", (frameadvSkipLag ? 1 : 0));
    WritePrivateProfileString("Input", "Frame Adv. - Skip Lag", Str_Tmp, Conf_File);
    wsprintf(Str_Tmp, "%d", StateSelectCfg);//Modif U.
    WritePrivateProfileString("Input", "StateSelectType", Str_Tmp, Conf_File);//Modif N.

    WritePrivateProfileString("Splice", "SpliceMovie", SpliceMovie, Conf_File);
    sprintf(Str_Tmp, "%d", SpliceFrame);
    WritePrivateProfileString("Splice", "SpliceFrame", Str_Tmp, Conf_File);
    WritePrivateProfileString("Splice", "TempFile", TempName, Conf_File);

    wsprintf(Str_Tmp, "%d", AutoRWLoad);
    WritePrivateProfileString("Watches", "AutoLoadWatches", Str_Tmp, Conf_File);

    wsprintf(Str_Tmp, "%d", RWSaveWindowPos);
    WritePrivateProfileString("Watches", "SaveWindowPosition", Str_Tmp, Conf_File);

    if (RWSaveWindowPos)
    {
        wsprintf(Str_Tmp, "%d", ramw_x);
        WritePrivateProfileString("Watches", "Ramwatch_X", Str_Tmp, Conf_File);
        wsprintf(Str_Tmp, "%d", ramw_y);
        WritePrivateProfileString("Watches", "Ramwatch_Y", Str_Tmp, Conf_File);
    }

    for (int i = 0; i < MAX_RECENT_WATCHES; i++)
    {
        char str[256];
        sprintf(str, "Recent Watch %d", i + 1);
        WritePrivateProfileString("Watches", str, &rw_recent_files[i][0], Conf_File);
    }

    SaveAccelerators(Conf_File);

    return 1;
}

int Save_As_Config(HWND hWnd)
{
    char Name[2048];
    OPENFILENAME ofn;

    SetCurrentDirectory(Gens_Path);

    strcpy(&Name[0], "Gens.cfg");
    memset(&ofn, 0, sizeof(OPENFILENAME));

    ofn.lStructSize = sizeof(OPENFILENAME);
    ofn.hwndOwner = hWnd;
    ofn.hInstance = ghInstance;
    ofn.lpstrFile = Name;
    ofn.nMaxFile = 2047;
    ofn.lpstrFilter = "Config Files\0*.cfg\0All Files\0*.*\0\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrInitialDir = Gens_Path;
    ofn.lpstrDefExt = "cfg";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (GetSaveFileName(&ofn))
    {
        Save_Config(Name);
        strcpy(Str_Tmp, "config saved in ");
        strcat(Str_Tmp, Name);
        Put_Info(Str_Tmp);
        return 1;
    }
    else return 0;
}

int Load_Config(char *File_Name, void *Game_Active)
{
    char Conf_File[1024];

    SetCurrentDirectory(Gens_Path);
    strcpy(Conf_File, File_Name);

    CRam_Flag = 1;

    GetPrivateProfileString("General", "Rom path", ".\\", &Rom_Dir[0], 1024, Conf_File);
    GetPrivateProfileString("General", "Save path", Rom_Dir, &State_Dir[0], 1024, Conf_File);
    GetPrivateProfileString("General", "SRAM path", Rom_Dir, &SRAM_Dir[0], 1024, Conf_File);
    GetPrivateProfileString("General", "Dump path", Rom_Dir, &Dump_Dir[0], 1024, Conf_File);
    GetPrivateProfileString("General", "Patch path", Rom_Dir, &Patch_Dir[0], 1024, Conf_File);
    GetPrivateProfileString("General", "IPS Patch path", Rom_Dir, &IPS_Dir[0], 1024, Conf_File);
    GetPrivateProfileString("General", "Movie path", Rom_Dir, &Movie_Dir[0], 1024, Conf_File);
    GetPrivateProfileString("General", "Watch path", Rom_Dir, &Watch_Dir[0], 1024, Conf_File);
    GetPrivateProfileString("General", "Lua path", Rom_Dir, &Lua_Dir[0], 1024, Conf_File);

    GetPrivateProfileString("General", "Genesis Bios", Rom_Dir, &Genesis_Bios[0], 1024, Conf_File);

    for (int i = 0; i < MAX_RECENT_ROMS; i++)
    {
        char str[256];
        sprintf(str, "Rom %d", i + 1);
        GetPrivateProfileString("General", str, "", &Recent_Rom[i][0], 1024, Conf_File);
    }

    for (int i = 0; i < MAX_RECENT_MOVIES; i++)
    {
        char str[256];
        sprintf(str, "Movie %d", i + 1);
        GetPrivateProfileString("General", str, "", &Recent_Movie[i][0], 1024, Conf_File);
    }

    for (int i = 0; i < MAX_RECENT_SCRIPTS; i++)
    {
        char str[256];
        sprintf(str, "Script %d", i + 1);
        GetPrivateProfileString("General", str, "", &Recent_Scripts[i][0], 1024, Conf_File);
    }

    File_Type_Index = GetPrivateProfileInt("General", "File type index", 1, Conf_File);

    int File_Type_Index_Version = GetPrivateProfileInt("General", "File type index version", 0, Conf_File);
    if (File_Type_Index_Version == 0 && File_Type_Index == 5)
        File_Type_Index = 8;

    Current_State = GetPrivateProfileInt("General", "State Number", 0, Conf_File);
    Window_Pos.x = GetPrivateProfileInt("General", "Window X", 0, Conf_File);
    Window_Pos.y = GetPrivateProfileInt("General", "Window Y", 0, Conf_File);
    Intro_Style = GetPrivateProfileInt("General", "Intro Style", 0, Conf_File); //Modif N. - default to not using the "crazy" intro effect, which used to guzzle more CPU than the actual emulation
    Effect_Color = Intro_Style ? GetPrivateProfileInt("General", "Free Mode Color", 7, Conf_File) : 0; //Modif N. - no intro style = no intro style color
    Sleep_Time = GetPrivateProfileInt("General", "Allow Idle", 5, Conf_File); //Modif N. - CPU hogging now off by default
    Gens_Priority = GetPrivateProfileInt("General", "Priority", 1, Conf_File);
    Def_Read_Only = (bool)(GetPrivateProfileInt("General", "Movie Default Read Only", 1, Conf_File) > 0); //Upth-Add - Load the flag from config
    AutoCloseMovie = (bool)(GetPrivateProfileInt("General", "Movie Auto Close", 0, Conf_File) > 0); //Upth-Add - Load the flag from config
    UseMovieStates = (bool)(GetPrivateProfileInt("General", "Movie Based State Names", 1, Conf_File) > 0); //Upth-Add - Load the flag from config
    SlowDownSpeed = GetPrivateProfileInt("General", "Slow Down Speed", 1, Conf_File); //Upth-Add - Load the slowdown speed from config
    DelayFactor = GetPrivateProfileInt("General", "Frame Advance Delay Factor", 5, Conf_File); //Upth-Add - Frame advance speed configurable

    if (GetPrivateProfileInt("Graphics", "Force 555", 0, Conf_File)) Mode_555 = 3;
    else if (GetPrivateProfileInt("Graphics", "Force 565", 0, Conf_File)) Mode_555 = 2;
    else Mode_555 = 0;

    RMax_Level = GetPrivateProfileInt("Graphics", "Red Max", 255, Conf_File);
    GMax_Level = GetPrivateProfileInt("Graphics", "Green Max", 255, Conf_File);
    BMax_Level = GetPrivateProfileInt("Graphics", "Blue Max", 255, Conf_File);
    Contrast_Level = GetPrivateProfileInt("Graphics", "Contrast", 100, Conf_File);
    Brightness_Level = GetPrivateProfileInt("Graphics", "Brightness", 100, Conf_File);
    Greyscale = GetPrivateProfileInt("Graphics", "Greyscale", 0, Conf_File);
    Invert_Color = GetPrivateProfileInt("Graphics", "Invert", 0, Conf_File);
    PinkBG = !!GetPrivateProfileInt("Graphics", "Pink Background", 0, Conf_File); // affects palette

    Recalculate_Palettes();

    FS_VSync = GetPrivateProfileInt("Graphics", "Full Screen VSync", 0, Conf_File);
    Res_X = GetPrivateProfileInt("Graphics", "Full Screen X Resolution", 0, Conf_File); //Upth-Add - Load the fullscreen
    Res_Y = GetPrivateProfileInt("Graphics", "Full Screen Y Resolution", 0, Conf_File); //Upth-Add - resolution from config
    char str[11];
    GetPrivateProfileString("Graphics", "Screen Scale Factor", "1.0", str, 11, Conf_File);
    ScaleFactor = (float)atof(str);
    FS_No_Res_Change = (bool)(GetPrivateProfileInt("Graphics", "Full Screen No Res Change", 0, Conf_File) > 0); //Upth-Add - and the no_res_change flag
    W_VSync = GetPrivateProfileInt("Graphics", "Windows VSync", 0, Conf_File);
    Full_Screen = GetPrivateProfileInt("Graphics", "Full Screen", 0, Conf_File);
    Render_W = GetPrivateProfileInt("Graphics", "Render Windowed", 0, Conf_File);
    Render_FS = GetPrivateProfileInt("Graphics", "Render Fullscreen", 1, Conf_File);
    Never_Skip_Frame = (bool)(GetPrivateProfileInt("Graphics", "Never Skip Frame", 1, Conf_File) > 0); //Modif N. -- added never skip frame to preferences
    VideoLatencyCompensation = GetPrivateProfileInt("Graphics", "Video Latency Compensation", 0, Conf_File);
    if (Res_X < 320) Res_X = 320; //Upth-Add - Make sure our resolution
    if (Res_Y < 240) Res_Y = 240; //Upth-Add - is at least 320x240

    Set_Render(HWnd, Full_Screen, -1, 1);

    ScrollAOn = GetPrivateProfileInt("Graphics", "Scroll A layer", 1, Conf_File);
    ScrollBOn = GetPrivateProfileInt("Graphics", "Scroll B layer", 1, Conf_File);
    SpriteOn = GetPrivateProfileInt("Graphics", "Sprites layer", 1, Conf_File);
    Stretch = GetPrivateProfileInt("Graphics", "Stretch", 0, Conf_File);
    Blit_Soft = GetPrivateProfileInt("Graphics", "Software Blit", 0, Conf_File);
    Sprite_Over = GetPrivateProfileInt("Graphics", "Sprite limit", 1, Conf_File);
    Frame_Skip = GetPrivateProfileInt("Graphics", "Frame skip", -1, Conf_File);

    Correct_256_Aspect_Ratio = GetPrivateProfileInt("Graphics", "Proper Aspect Ratio", 1, Conf_File); //Modif N.

    Sound_Rate = GetPrivateProfileInt("Sound", "Rate", 44100, Conf_File);
    Sound_Stereo = GetPrivateProfileInt("Sound", "Stereo", 1, Conf_File);
    Sound_Soften = GetPrivateProfileInt("Sound", "SoundSoftenFilter", 0, Conf_File); // Modif N.

    if (GetPrivateProfileInt("Sound", "Z80 State", 1, Conf_File)) Z80_State |= 1;
    else Z80_State &= ~1;

    Country = GetPrivateProfileInt("CPU", "Country", -1, Conf_File);
    Country_Order[0] = GetPrivateProfileInt("CPU", "Prefered Country 1", 0, Conf_File);
    Country_Order[1] = GetPrivateProfileInt("CPU", "Prefered Country 2", 1, Conf_File);
    Country_Order[2] = GetPrivateProfileInt("CPU", "Prefered Country 3", 2, Conf_File);

    Check_Country_Order();

    HexCommon.DialogPosX = GetPrivateProfileInt("Tools", "Hex Editor X Position", 0, Conf_File);
    HexCommon.DialogPosY = GetPrivateProfileInt("Tools", "Hex Editor Y Position", 0, Conf_File);
    HexCommon.OffsetVisibleFirst = GetPrivateProfileInt("Tools", "Hex Editor First Offset", 0, Conf_File);
    HexCommon.OffsetVisibleTotal = GetPrivateProfileInt("Tools", "Hex Editor Total Offset", 16, Conf_File);
    HexCommon.TextView = !!GetPrivateProfileInt("Tools", "Hex Editor Text View", 1, Conf_File);
    HexCommon.DrawLines = !!GetPrivateProfileInt("Tools", "Hex Editor Draw Lines", 0, Conf_File);

    Fast_Blur = GetPrivateProfileInt("Options", "Fast Blur", 0, Conf_File);
    Show_FPS = GetPrivateProfileInt("Options", "FPS", 0, Conf_File);
    FPS_Style = GetPrivateProfileInt("Options", "FPS Style", 0, Conf_File);
    Show_Message = GetPrivateProfileInt("Options", "Message", 1, Conf_File);
    Message_Style = GetPrivateProfileInt("Options", "Message Style", 0, Conf_File);
    Auto_Fix_CS = GetPrivateProfileInt("Options", "Auto Fix Checksum", 0, Conf_File);
    Auto_Pause = GetPrivateProfileInt("Options", "Auto Pause", 0, Conf_File);

    Disable_Blue_Screen = GetPrivateProfileInt("Options", "Disable Blue Screen", 1, Conf_File); //Modif
    FrameCounterEnabled = GetPrivateProfileInt("Options", "FrameCounterEnabled", 1, Conf_File); //Modif N
    FrameCounterFrames = GetPrivateProfileInt("Options", "FrameCounterFrames", 1, Conf_File); // Modif N
    FrameCounterPosition = GetPrivateProfileInt("Options", "FrameCounterPosition", FRAME_COUNTER_TOP_LEFT, Conf_File);
    LagCounterEnabled = GetPrivateProfileInt("Options", "LagCounterEnabled", 1, Conf_File); //Modif N
    LagCounterFrames = GetPrivateProfileInt("Options", "LagCounterFrames", 1, Conf_File); // Modif N
    ShowInputEnabled = GetPrivateProfileInt("Options", "ShowInputEnabled", 1, Conf_File); //Modif N
    AutoBackupEnabled = GetPrivateProfileInt("Options", "AutoBackupEnabled", 0, Conf_File);

    Controller_1_Type = GetPrivateProfileInt("Input", "P1.Type", 1, Conf_File);
    Keys_Def[0].Up = GetPrivateProfileInt("Input", "P1.Up", DIK_UP, Conf_File);
    Keys_Def[0].Down = GetPrivateProfileInt("Input", "P1.Down", DIK_DOWN, Conf_File);
    Keys_Def[0].Left = GetPrivateProfileInt("Input", "P1.Left", DIK_LEFT, Conf_File);
    Keys_Def[0].Right = GetPrivateProfileInt("Input", "P1.Right", DIK_RIGHT, Conf_File);
    Keys_Def[0].Start = GetPrivateProfileInt("Input", "P1.Start", DIK_RETURN, Conf_File);
    Keys_Def[0].A = GetPrivateProfileInt("Input", "P1.A", DIK_A, Conf_File);
    Keys_Def[0].B = GetPrivateProfileInt("Input", "P1.B", DIK_S, Conf_File);
    Keys_Def[0].C = GetPrivateProfileInt("Input", "P1.C", DIK_D, Conf_File);
    Keys_Def[0].Mode = GetPrivateProfileInt("Input", "P1.Mode", DIK_RSHIFT, Conf_File);
    Keys_Def[0].X = GetPrivateProfileInt("Input", "P1.X", DIK_Z, Conf_File);
    Keys_Def[0].Y = GetPrivateProfileInt("Input", "P1.Y", DIK_X, Conf_File);
    Keys_Def[0].Z = GetPrivateProfileInt("Input", "P1.Z", DIK_C, Conf_File);

    Controller_1B_Type = GetPrivateProfileInt("Input", "P1B.Type", 0, Conf_File);
    Keys_Def[2].Up = GetPrivateProfileInt("Input", "P1B.Up", 0, Conf_File);
    Keys_Def[2].Down = GetPrivateProfileInt("Input", "P1B.Down", 0, Conf_File);
    Keys_Def[2].Left = GetPrivateProfileInt("Input", "P1B.Left", 0, Conf_File);
    Keys_Def[2].Right = GetPrivateProfileInt("Input", "P1B.Right", 0, Conf_File);
    Keys_Def[2].Start = GetPrivateProfileInt("Input", "P1B.Start", 0, Conf_File);
    Keys_Def[2].A = GetPrivateProfileInt("Input", "P1B.A", 0, Conf_File);
    Keys_Def[2].B = GetPrivateProfileInt("Input", "P1B.B", 0, Conf_File);
    Keys_Def[2].C = GetPrivateProfileInt("Input", "P1B.C", 0, Conf_File);
    Keys_Def[2].Mode = GetPrivateProfileInt("Input", "P1B.Mode", 0, Conf_File);
    Keys_Def[2].X = GetPrivateProfileInt("Input", "P1B.X", 0, Conf_File);
    Keys_Def[2].Y = GetPrivateProfileInt("Input", "P1B.Y", 0, Conf_File);
    Keys_Def[2].Z = GetPrivateProfileInt("Input", "P1B.Z", 0, Conf_File);

    Controller_1C_Type = GetPrivateProfileInt("Input", "P1C.Type", 0, Conf_File);
    Keys_Def[3].Up = GetPrivateProfileInt("Input", "P1C.Up", 0, Conf_File);
    Keys_Def[3].Down = GetPrivateProfileInt("Input", "P1C.Down", 0, Conf_File);
    Keys_Def[3].Left = GetPrivateProfileInt("Input", "P1C.Left", 0, Conf_File);
    Keys_Def[3].Right = GetPrivateProfileInt("Input", "P1C.Right", 0, Conf_File);
    Keys_Def[3].Start = GetPrivateProfileInt("Input", "P1C.Start", 0, Conf_File);
    Keys_Def[3].A = GetPrivateProfileInt("Input", "P1C.A", 0, Conf_File);
    Keys_Def[3].B = GetPrivateProfileInt("Input", "P1C.B", 0, Conf_File);
    Keys_Def[3].C = GetPrivateProfileInt("Input", "P1C.C", 0, Conf_File);
    Keys_Def[3].Mode = GetPrivateProfileInt("Input", "P1C.Mode", 0, Conf_File);
    Keys_Def[3].X = GetPrivateProfileInt("Input", "P1C.X", 0, Conf_File);
    Keys_Def[3].Y = GetPrivateProfileInt("Input", "P1C.Y", 0, Conf_File);
    Keys_Def[3].Z = GetPrivateProfileInt("Input", "P1C.Z", 0, Conf_File);

    Controller_1D_Type = GetPrivateProfileInt("Input", "P1D.Type", 0, Conf_File);
    Keys_Def[4].Up = GetPrivateProfileInt("Input", "P1D.Up", 0, Conf_File);
    Keys_Def[4].Down = GetPrivateProfileInt("Input", "P1D.Down", 0, Conf_File);
    Keys_Def[4].Left = GetPrivateProfileInt("Input", "P1D.Left", 0, Conf_File);
    Keys_Def[4].Right = GetPrivateProfileInt("Input", "P1D.Right", 0, Conf_File);
    Keys_Def[4].Start = GetPrivateProfileInt("Input", "P1D.Start", 0, Conf_File);
    Keys_Def[4].A = GetPrivateProfileInt("Input", "P1D.A", 0, Conf_File);
    Keys_Def[4].B = GetPrivateProfileInt("Input", "P1D.B", 0, Conf_File);
    Keys_Def[4].C = GetPrivateProfileInt("Input", "P1D.C", 0, Conf_File);
    Keys_Def[4].Mode = GetPrivateProfileInt("Input", "P1D.Mode", 0, Conf_File);
    Keys_Def[4].X = GetPrivateProfileInt("Input", "P1D.X", 0, Conf_File);
    Keys_Def[4].Y = GetPrivateProfileInt("Input", "P1D.Y", 0, Conf_File);
    Keys_Def[4].Z = GetPrivateProfileInt("Input", "P1D.Z", 0, Conf_File);

    Controller_2_Type = GetPrivateProfileInt("Input", "P2.Type", 1, Conf_File);
    Keys_Def[1].Up = GetPrivateProfileInt("Input", "P2.Up", DIK_Y, Conf_File);
    Keys_Def[1].Down = GetPrivateProfileInt("Input", "P2.Down", DIK_H, Conf_File);
    Keys_Def[1].Left = GetPrivateProfileInt("Input", "P2.Left", DIK_G, Conf_File);
    Keys_Def[1].Right = GetPrivateProfileInt("Input", "P2.Right", DIK_J, Conf_File);
    Keys_Def[1].Start = GetPrivateProfileInt("Input", "P2.Start", DIK_U, Conf_File);
    Keys_Def[1].A = GetPrivateProfileInt("Input", "P2.A", DIK_K, Conf_File);
    Keys_Def[1].B = GetPrivateProfileInt("Input", "P2.B", DIK_L, Conf_File);
    Keys_Def[1].C = GetPrivateProfileInt("Input", "P2.C", DIK_M, Conf_File);
    Keys_Def[1].Mode = GetPrivateProfileInt("Input", "P2.Mode", DIK_T, Conf_File);
    Keys_Def[1].X = GetPrivateProfileInt("Input", "P2.X", DIK_I, Conf_File);
    Keys_Def[1].Y = GetPrivateProfileInt("Input", "P2.Y", DIK_O, Conf_File);
    Keys_Def[1].Z = GetPrivateProfileInt("Input", "P2.Z", DIK_P, Conf_File);

    Controller_2B_Type = GetPrivateProfileInt("Input", "P2B.Type", 0, Conf_File);
    Keys_Def[5].Up = GetPrivateProfileInt("Input", "P2B.Up", 0, Conf_File);
    Keys_Def[5].Down = GetPrivateProfileInt("Input", "P2B.Down", 0, Conf_File);
    Keys_Def[5].Left = GetPrivateProfileInt("Input", "P2B.Left", 0, Conf_File);
    Keys_Def[5].Right = GetPrivateProfileInt("Input", "P2B.Right", 0, Conf_File);
    Keys_Def[5].Start = GetPrivateProfileInt("Input", "P2B.Start", 0, Conf_File);
    Keys_Def[5].A = GetPrivateProfileInt("Input", "P2B.A", 0, Conf_File);
    Keys_Def[5].B = GetPrivateProfileInt("Input", "P2B.B", 0, Conf_File);
    Keys_Def[5].C = GetPrivateProfileInt("Input", "P2B.C", 0, Conf_File);
    Keys_Def[5].Mode = GetPrivateProfileInt("Input", "P2B.Mode", 0, Conf_File);
    Keys_Def[5].X = GetPrivateProfileInt("Input", "P2B.X", 0, Conf_File);
    Keys_Def[5].Y = GetPrivateProfileInt("Input", "P2B.Y", 0, Conf_File);
    Keys_Def[5].Z = GetPrivateProfileInt("Input", "P2B.Z", 0, Conf_File);

    Controller_2C_Type = GetPrivateProfileInt("Input", "P2C.Type", 0, Conf_File);
    Keys_Def[6].Up = GetPrivateProfileInt("Input", "P2C.Up", 0, Conf_File);
    Keys_Def[6].Down = GetPrivateProfileInt("Input", "P2C.Down", 0, Conf_File);
    Keys_Def[6].Left = GetPrivateProfileInt("Input", "P2C.Left", 0, Conf_File);
    Keys_Def[6].Right = GetPrivateProfileInt("Input", "P2C.Right", 0, Conf_File);
    Keys_Def[6].Start = GetPrivateProfileInt("Input", "P2C.Start", 0, Conf_File);
    Keys_Def[6].A = GetPrivateProfileInt("Input", "P2C.A", 0, Conf_File);
    Keys_Def[6].B = GetPrivateProfileInt("Input", "P2C.B", 0, Conf_File);
    Keys_Def[6].C = GetPrivateProfileInt("Input", "P2C.C", 0, Conf_File);
    Keys_Def[6].Mode = GetPrivateProfileInt("Input", "P2C.Mode", 0, Conf_File);
    Keys_Def[6].X = GetPrivateProfileInt("Input", "P2C.X", 0, Conf_File);
    Keys_Def[6].Y = GetPrivateProfileInt("Input", "P2C.Y", 0, Conf_File);
    Keys_Def[6].Z = GetPrivateProfileInt("Input", "P2C.Z", 0, Conf_File);

    Controller_2D_Type = GetPrivateProfileInt("Input", "P2D.Type", 0, Conf_File);
    Keys_Def[7].Up = GetPrivateProfileInt("Input", "P2D.Up", 0, Conf_File);
    Keys_Def[7].Down = GetPrivateProfileInt("Input", "P2D.Down", 0, Conf_File);
    Keys_Def[7].Left = GetPrivateProfileInt("Input", "P2D.Left", 0, Conf_File);
    Keys_Def[7].Right = GetPrivateProfileInt("Input", "P2D.Right", 0, Conf_File);
    Keys_Def[7].Start = GetPrivateProfileInt("Input", "P2D.Start", 0, Conf_File);
    Keys_Def[7].A = GetPrivateProfileInt("Input", "P2D.A", 0, Conf_File);
    Keys_Def[7].B = GetPrivateProfileInt("Input", "P2D.B", 0, Conf_File);
    Keys_Def[7].C = GetPrivateProfileInt("Input", "P2D.C", 0, Conf_File);
    Keys_Def[7].Mode = GetPrivateProfileInt("Input", "P2D.Mode", 0, Conf_File);
    Keys_Def[7].X = GetPrivateProfileInt("Input", "P2D.X", 0, Conf_File);
    Keys_Def[7].Y = GetPrivateProfileInt("Input", "P2D.Y", 0, Conf_File);
    Keys_Def[7].Z = GetPrivateProfileInt("Input", "P2D.Z", 0, Conf_File);

    LeftRightEnabled = GetPrivateProfileInt("Input", "LeftRightEnabled", 0, Conf_File);
    BackgroundInput = GetPrivateProfileInt("Input", "Allow Background Input", 0, Conf_File) != 0; //Upth-add - Allow input of hotkeys and joypad buttons while window is inactive
    frameadvSkipLag = GetPrivateProfileInt("Input", "Frame Adv. - Skip Lag", 0, Conf_File) != 0;
    StateSelectCfg = GetPrivateProfileInt("Input", "StateSelectType", 5, Conf_File); //Modif N

    GetPrivateProfileString("Splice", "SpliceMovie", "", SpliceMovie, 1024, Conf_File);
    SpliceFrame = GetPrivateProfileInt("Splice", "SpliceFrame", 0, Conf_File);
    GetPrivateProfileString("Splice", "TempFile", "", Str_Tmp, 1024, Conf_File);

    AutoRWLoad = GetPrivateProfileInt("Watches", "AutoLoadWatches", false, Conf_File) != 0;
    RWSaveWindowPos = GetPrivateProfileInt("Watches", "SaveWindowPosition", false, Conf_File) != 0;

    if (RWSaveWindowPos)
    {
        ramw_x = GetPrivateProfileInt("Watches", "Ramwatch_X", 0, Conf_File);
        ramw_y = GetPrivateProfileInt("Watches", "Ramwatch_Y", 0, Conf_File);
    }

    for (int i = 0; i < MAX_RECENT_WATCHES; i++)
    {
        char str[256];
        sprintf(str, "Recent Watch %d", i + 1);
        GetPrivateProfileString("Watches", str, "", &rw_recent_files[i][0], 1024, Conf_File);
    }

    LoadAccelerators(Conf_File);

    // done loading
    // do some post-processing:

#ifdef _WIN32
    if (SpliceFrame)
    {
        MINIMIZE
            DialogsOpen++;
        TempName = (char *)malloc(strlen(Str_Tmp) + 2);
        strcpy(TempName, Str_Tmp);
        sprintf(Str_Tmp, "Incomplete splice session detected for %s.\n\
                                                  						 Do you wish to continue this session?\n\
                                                                                                                          						 (Warning: If you do not restore the session, it will be permanently discarded.)", SpliceMovie);
        int response = MessageBox(HWnd, Str_Tmp, "NOTICE", MB_YESNO | MB_ICONQUESTION);
        while ((response != IDYES) && (response != IDNO))
            response = MessageBox(HWnd, Str_Tmp, "NOTICE", MB_YESNO | MB_ICONQUESTION);
        if (response == IDNO)
        {
            if (MessageBox(HWnd, "Restore gmv from backup?", "PROMPT", MB_YESNO | MB_ICONQUESTION) == IDYES)
            {
                strcpy(Str_Tmp, SpliceMovie);
                Str_Tmp[strlen(Str_Tmp) - 3] = 0;
                strcat(Str_Tmp, "spl.gmv");
                MainMovie.File = fopen(Str_Tmp, "rb");
                if (!MainMovie.File)
                    WARNINGBOX("Error opening movie backup.", "ERROR");
                else
                {
                    strcpy(MainMovie.FileName, SpliceMovie);
                    BackupMovieFile(&MainMovie);
                    strcpy(MainMovie.FileName, "");
                    fclose(MainMovie.File);
                    remove(Str_Tmp);
                }
            }
            remove(TempName);
            free(TempName);
            TempName = NULL;
            SpliceFrame = 0;
            strcpy(SpliceMovie, "");
        }
        else
        {
            sprintf(Str_Tmp, "%s splice session restored", SpliceMovie);
            Put_Info(Str_Tmp);
        }
        DialogsOpen--;
    }
#endif

    Make_IO_Table();
    DestroyMenu(Gens_Menu);

    if (Full_Screen) Build_Main_Menu();
    else SetMenu(HWnd, Build_Main_Menu());		// Update new menu

    extern HACCEL hAccelTable;
    BuildAccelerators(hAccelTable);
    return 1;
}

int Load_As_Config(HWND hWnd, void *Game_Active)
{
    char Name[2048];
    OPENFILENAME ofn;

    SetCurrentDirectory(Gens_Path);

    strcpy(&Name[0], "Gens.cfg");
    memset(&ofn, 0, sizeof(OPENFILENAME));

    ofn.lStructSize = sizeof(OPENFILENAME);
    ofn.hwndOwner = hWnd;
    ofn.hInstance = ghInstance;
    ofn.lpstrFile = Name;
    ofn.nMaxFile = 2047;
    ofn.lpstrFilter = "Config Files\0*.cfg\0All Files\0*.*\0\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrInitialDir = Gens_Path;
    ofn.lpstrDefExt = "cfg";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;

    if (GetOpenFileName(&ofn))
    {
        Load_Config(Name, Game_Active);
        strcpy(Str_Tmp, "config loaded from ");
        strcat(Str_Tmp, Name);
        Put_Info(Str_Tmp);
        return 1;
    }
    else return 0;
}

int Load_SRAM(void)
{
    int bResult;
    HANDLE SRAM_File;
    char Name[2048];
    unsigned long Bytes_Read;

    SetCurrentDirectory(Gens_Path);

    memset(SRAM, 0, 64 * 1024);

    strcpy(Name, SRAM_Dir);
    strcat(Name, Rom_Name);
    strcat(Name, ".srm");

    SRAM_File = CreateFile(Name, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (SRAM_File == INVALID_HANDLE_VALUE) return 0;

    bResult = ReadFile(SRAM_File, SRAM, 64 * 1024, &Bytes_Read, NULL);

    CloseHandle(SRAM_File);

    strcpy(Str_Tmp, "SRAM loaded from ");
    strcat(Str_Tmp, Name);
    Put_Info(Str_Tmp);

    return 1;
}
int Load_SRAMFromBuf(char *buf)
{
    strcpy((char *)SRAM, buf);
    //	strcpy(Str_Tmp, "SRAM loaded from embedded movie data.");
    Put_Info(Str_Tmp);

    return 1;
}

int Save_SRAM(void)
{
    HANDLE SRAM_File;
    int bResult, size_to_save, i;
    char Name[2048];
    unsigned long Bytes_Write;

    SetCurrentDirectory(Gens_Path);

    i = (64 * 1024) - 1;
    while ((i >= 0) && (SRAM[i] == 0)) i--;

    if (i < 0) return 0;

    i++;

    size_to_save = 1;
    while (i > size_to_save) size_to_save <<= 1;

    strcpy(Name, SRAM_Dir);
    strcat(Name, Rom_Name);
    strcat(Name, ".srm");

    SRAM_File = CreateFile(Name, GENERIC_WRITE, NULL,
        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (SRAM_File == INVALID_HANDLE_VALUE) return 0;

    bResult = WriteFile(SRAM_File, SRAM, size_to_save, &Bytes_Write, NULL);

    CloseHandle(SRAM_File);

    strcpy(Str_Tmp, "SRAM saved in ");
    strcat(Str_Tmp, Name);
    Put_Info(Str_Tmp);

    return 1;
}
