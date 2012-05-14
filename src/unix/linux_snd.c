/*
===========================================================================

Return to Castle Wolfenstein single player GPL Source Code
Copyright (C) 1999-2010 id Software LLC, a ZeniMax Media company.

This file is part of the Return to Castle Wolfenstein single player GPL Source Code ("RTCW SP Source Code").

RTCW SP Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

RTCW SP Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with RTCW SP Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the RTCW SP Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the RTCW SP Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

#include <SDL/SDL.h>
#include <SDL/SDL_audio.h>

#include <dlfcn.h>

#include "../game/q_shared.h"
#include "../client/snd_local.h"

int snd_inited = 0;

typedef struct
{
	void *SDLLib;

	int dma_pos;
	int dma_size;

	qboolean sdl_lib_loaded;
} sdlstate_t;

static sdlstate_t sdl_state;

// SDL.h
int (*__SDL_Init) (Uint32 flags);
Uint32 (*__SDL_WasInit) (Uint32 flags);
char *(*__SDL_GetError) (void);
void (*__SDL_QuitSubSystem) (Uint32 flags);

// SDL_audio.h
int (*__SDL_OpenAudio) (SDL_AudioSpec *desired, SDL_AudioSpec *obtained);
char *(*__SDL_AudioDriverName) (char *namebuf, int maxlen);
void (*__SDL_PauseAudio) (int pause_on);
void (*__SDL_LockAudio) (void);
void (*__SDL_UnlockAudio) (void);
void (*__SDL_CloseAudio) (void);

static void sdl_callback(void *userdata, Uint8 *stream, int len);

cvar_t *sndbits;
cvar_t *sndspeed;
cvar_t *sndchannels;
cvar_t *sdlsamplesmult;

static qboolean use_custom_memset = qfalse;
// show_bug.cgi?id=371
void Snd_Memset( void* dest, const int val, const size_t count ) {
	int *pDest;
	int i, iterate;

	if ( !use_custom_memset ) {
		Com_Memset( dest,val,count );
		return;
	}
	iterate = count / sizeof( int );
	pDest = (int*)dest;
	for ( i = 0; i < iterate; i++ )
	{
		pDest[i] = val;
	}
}

qboolean SNDDMA_Init( void ) {
	const char *dllname;
	SDL_AudioSpec desired, obtained;
	char drivername[128];
	int samples;

	if ( snd_inited ) {
		return 1;
	}

	if ( !sndbits ) {
		sndbits = Cvar_Get( "sndbits", "16", CVAR_ARCHIVE );
		sndspeed = Cvar_Get( "sndspeed", "0", CVAR_ARCHIVE );
		sndchannels = Cvar_Get( "sndchannels", "2", CVAR_ARCHIVE );
		sdlsamplesmult = Cvar_Get( "sdlsamplesmult", "8", CVAR_ARCHIVE );
	}

	// FIXME: potential security problem!
	dllname = getenv("RTCW_SDL_LIB");
	if ( dllname == NULL ) {
		dllname = "libSDL.so";
	}

	if ( !sdl_state.sdl_lib_loaded ) {
		if ( ( sdl_state.SDLLib = dlopen( dllname, RTLD_LAZY | RTLD_GLOBAL ) ) == 0 ) {
			Com_Printf( "SNDDMA_Init: Can't load %s from /etc/ld.so.conf or current dir: %s\n", dllname, dlerror() );
			return qfalse;
		}

		#define DLSYM_FUNC(type, func) \
		if ( ( __##func = (type) dlsym( sdl_state.SDLLib, #func ) ) == NULL ) { \
			Com_Printf( "SNDDMA_Init: Could not load %s: %s\n", #func, dlerror() ); \
			dlclose( sdl_state.SDLLib ); \
			sdl_state.SDLLib = NULL; \
			return qfalse; \
		}
		DLSYM_FUNC(int (*) (Uint32), SDL_Init)
		DLSYM_FUNC(Uint32 (*) (Uint32), SDL_WasInit)
		DLSYM_FUNC(char *(*) (void), SDL_GetError)
		DLSYM_FUNC(void (*) (Uint32), SDL_QuitSubSystem)
		DLSYM_FUNC(int (*) (SDL_AudioSpec *, SDL_AudioSpec *), SDL_OpenAudio)
		DLSYM_FUNC(char *(*) (char *, int), SDL_AudioDriverName)
		DLSYM_FUNC(void (*) (int), SDL_PauseAudio)
		DLSYM_FUNC(void (*) (void), SDL_LockAudio)
		DLSYM_FUNC(void (*) (void), SDL_UnlockAudio)
		DLSYM_FUNC(void (*) (void), SDL_CloseAudio)
		#undef DLSYM_FUNC
		sdl_state.sdl_lib_loaded = qtrue;
	}

	if ( !__SDL_WasInit(SDL_INIT_AUDIO) ) {
		if ( __SDL_Init(SDL_INIT_AUDIO) == -1 ) {
			Com_Printf( "SNDDMA_Init: SDL_Init(SDL_INIT_AUDIO) failed: %s\n",  __SDL_GetError() );
			return qfalse;
		}
	}

	if ( __SDL_AudioDriverName( drivername, sizeof(drivername) ) == NULL ) {
		Com_Printf( "SNDDMA_Init: SDL audio driver name is NULL\n" );
		__SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return qfalse;
	}

	memset( &desired, '\0', sizeof(desired) );
	memset( &obtained, '\0', sizeof(obtained) );

	desired.freq = (int) sndspeed->value;

	if ( !desired.freq )
		desired.freq = 44100;

	if ( (int) sndbits->value == 8 ) {
		desired.format = AUDIO_U8;
	} else {
		desired.format = AUDIO_S16SYS;
	}

	if ( desired.freq <= 11025 ) {
		desired.samples = 256;
	} else if ( desired.freq <= 22050 ) {
		desired.samples = 512;
	} else if ( desired.freq <= 44100 ) {
		desired.samples = 1024;
	} else {
		desired.samples = 2048;
	}

	desired.channels = (int) sndchannels->value;
	desired.callback = (void (*) (void*, Uint8*, int)) sdl_callback;

	if ( __SDL_OpenAudio( &desired, &obtained ) == -1 ) {
		Com_Printf( "SNDDMA_Init: SDL_OpenAudio() failed: %s\n", __SDL_GetError() );
		__SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return qfalse;
	}

	samples = obtained.samples * obtained.channels;

	if ((int) sdlsamplesmult->value >= 1) {
		samples *= (int) sdlsamplesmult->value;
	}

	// make it power of two
	if (samples & (samples - 1)) {
		int val = 1;
		while (val < samples) {
			val <<= 1;
		}
		samples = val;
	}

	dma.samplebits = obtained.format & 0xff; // first byte of format is bits
	dma.channels = obtained.channels;
	dma.samples = samples;
	dma.submission_chunk = 1;
	dma.speed = obtained.freq;

	sdl_state.dma_pos = 0;
	sdl_state.dma_size = ( dma.samples * ( dma.samplebits / 8 ) );

	dma.buffer = (byte *) malloc( sdl_state.dma_size );
	memset( dma.buffer, '\0', sdl_state.dma_size );

	__SDL_PauseAudio(0); // start callback

	snd_inited = 1;
	return qtrue;
}

int SNDDMA_GetDMAPos( void ) {
	if ( !snd_inited ) {
		return 0;
	}

	return sdl_state.dma_pos;
}

void SNDDMA_Shutdown( void ) {
	__SDL_PauseAudio(1);
	__SDL_CloseAudio();
	__SDL_QuitSubSystem(SDL_INIT_AUDIO);

	if ( dma.buffer != NULL ) {
		free( dma.buffer );
		dma.buffer = NULL;
	}

	sdl_state.dma_pos = 0;
	sdl_state.dma_size = 0;
	snd_inited = 0;
}

/*
==============
SNDDMA_Submit

Send sound to device if buffer isn't really the dma buffer
===============
*/
void SNDDMA_Submit( void ) {
	__SDL_UnlockAudio();
}

void SNDDMA_BeginPainting( void ) {
	__SDL_LockAudio();
}

static void sdl_callback(void *userdata, Uint8 *stream, int len) {
	int tobufend;
	int len1;
	int len2;
	int pos = ( sdl_state.dma_pos * ( dma.samplebits / 8 ) );

	if ( pos > sdl_state.dma_size ) {
		sdl_state.dma_pos = 0;
		pos = 0;
	}

	if ( !snd_inited ) {
		memset( stream, '\0', len );
		return;
	}

	tobufend = sdl_state.dma_size - pos;
	len1 = len;
	len2 = 0;

	if ( len1 > tobufend ) {
		len1 = tobufend;
		len2 = len - len1;
	}

	memcpy( stream, dma.buffer + pos, len1 );

	if ( len2 <= 0 ) {
		sdl_state.dma_pos += ( len1 / ( dma.samplebits / 8 ) );
	} else {
		memcpy( stream + len1, dma.buffer, len2 );
		sdl_state.dma_pos = ( len2 / ( dma.samplebits / 8 ) );
	}

	if ( sdl_state.dma_pos >= sdl_state.dma_size ) {
		sdl_state.dma_pos = 0;
	}
}
