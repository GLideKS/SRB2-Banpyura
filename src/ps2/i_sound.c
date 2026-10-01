// SONIC ROBO BLAST 2 - PS2 sound: silent stand-in until phase 4 (audsrv).
// Nothing is output, but the contract the game logic relies on is kept: I_GetSfx gives a non-NULL
// "loaded" marker, a started sound counts as playing for a short while (so s_sound.c frees its
// channel), a loaded song plays until it is stopped, and music fades complete (and run their callback
// from I_UpdateSound), because S_ChangeMusic queues the next track from a fade callback.
#include "../doomdef.h"
#include "../i_sound.h"
#include "../i_system.h"
#include "../i_time.h"

#define STUB_CHANNELS 256
#define STUB_SFX_MS 160 // how long a started sound is reported as playing

UINT8 sound_started = 0;

static UINT8 sfx_marker[8]; // I_GetSfx result: opaque, never dereferenced
static precise_t sfx_end[STUB_CHANNELS]; // 0 = silent; else the precise time the sound "ends"
static UINT8 sfx_volume = 31;

static boolean song_loaded, song_playing, song_paused, song_looping;
static UINT8 music_volume = 31;
static UINT8 internal_volume = 100;
static UINT32 loop_point; // ms
static UINT32 song_position; // ms at song_epoch (or at the pause)
static precise_t song_epoch;

static boolean is_fading;
static UINT8 fading_source, fading_target;
static INT64 fading_remaining, fading_duration; // precise ticks
static precise_t fading_last;
static void (*fading_callback)(void);

static INT64 MsToPrecise(UINT32 ms)
{
	return (INT64)(I_GetPrecisePrecision() / 1000) * ms;
}

static UINT32 PreciseToMs(INT64 ticks)
{
	return (UINT32)(ticks / (INT64)(I_GetPrecisePrecision() / 1000));
}

/// ------------------------
///  SFX
/// ------------------------

void *I_GetSfx(sfxinfo_t *sfx)
{
	(void)sfx;
	return sfx_marker;
}

void I_FreeSfx(sfxinfo_t *sfx)
{
	sfx->data = NULL;
}

void I_StartupSound(void)
{
	sound_started = 1; // return early if already up
}

void I_ShutdownSound(void)
{
	INT32 i;

	for (i = 0; i < STUB_CHANNELS; i++)
		sfx_end[i] = 0;
	sound_started = 0;
}

INT32 I_StartSound(sfxenum_t id, UINT8 vol, UINT8 sep, UINT8 pitch, UINT8 priority, INT32 channel)
{
	(void)id;
	(void)vol;
	(void)sep;
	(void)pitch;
	(void)priority;

	if (!sound_started || channel < 0 || channel >= STUB_CHANNELS)
		return -1;
	sfx_end[channel] = I_GetPreciseTime() + (precise_t)MsToPrecise(STUB_SFX_MS);
	return channel; // the handle is the channel, as with SDL_mixer
}

void I_StopSound(INT32 handle)
{
	if (handle >= 0 && handle < STUB_CHANNELS)
		sfx_end[handle] = 0;
}

boolean I_SoundIsPlaying(INT32 handle)
{
	if (handle < 0 || handle >= STUB_CHANNELS || !sfx_end[handle])
		return false;
	if ((INT64)(sfx_end[handle] - I_GetPreciseTime()) > 0)
		return true;
	sfx_end[handle] = 0;
	return false;
}

void I_UpdateSoundParams(INT32 handle, UINT8 vol, UINT8 sep, UINT8 pitch)
{
	(void)handle;
	(void)vol;
	(void)sep;
	(void)pitch;
}

void I_SetSfxVolume(UINT8 volume)
{
	sfx_volume = volume;
}

/// ------------------------
///  MUSIC state and fades
/// ------------------------

static void StopFading(void)
{
	is_fading = false;
	fading_source = fading_target = 0;
	fading_remaining = fading_duration = 0;
}

static void SongCleanup(void)
{
	song_playing = song_paused = song_looping = false;
	song_position = 0;
	loop_point = 0;
	StopFading();
	fading_callback = NULL;
	internal_volume = 100;
}

// Runs from I_UpdateSound (the main thread): SDL_mixer's fade timer has no equivalent here.
static void FadeTick(void)
{
	const precise_t now = I_GetPreciseTime();
	void (*callback)(void);

	if (!is_fading)
		return;
	if (song_paused) // don't decrement the timer
	{
		fading_last = now;
		return;
	}

	fading_remaining -= (INT64)(now - fading_last);
	fading_last = now;

	if (fading_remaining <= 0)
	{
		internal_volume = fading_target;
		callback = fading_callback;
		StopFading();
		fading_callback = NULL; // cleared first: the callback may start another fade
		if (callback)
			callback();
	}
	else
	{
		const INT32 delta = (INT32)fading_target - (INT32)fading_source;
		const INT32 done = (INT32)(((fading_duration - fading_remaining) * 1024) / fading_duration); // 0..1024
		internal_volume = (UINT8)((INT32)fading_source + delta * done / 1024);
	}
}

void I_UpdateSound(void)
{
	FadeTick();
}

/// ------------------------
///  MUSIC SYSTEM
/// ------------------------

void I_InitMusic(void)
{
}

void I_ShutdownMusic(void)
{
	I_UnloadSong();
}

/// ------------------------
///  MUSIC PROPERTIES
/// ------------------------

musictype_t I_SongType(void)
{
	return song_loaded ? MU_OGG : MU_NONE; // anything but MIDI: no sequencer volume path
}

boolean I_SongPlaying(void)
{
	return song_playing;
}

boolean I_SongPaused(void)
{
	return song_paused;
}

boolean I_SetSongSpeed(float speed)
{
	(void)speed;
	return false;
}

/// ------------------------
///  MUSIC SEEKING
/// ------------------------

UINT32 I_GetSongLength(void)
{
	return 0; // unknown
}

boolean I_SetSongLoopPoint(UINT32 looppoint)
{
	if (!song_loaded)
		return false;
	loop_point = looppoint;
	return true;
}

UINT32 I_GetSongLoopPoint(void)
{
	return loop_point;
}

boolean I_SetSongPosition(UINT32 position)
{
	if (!song_loaded)
		return false;
	song_position = position;
	song_epoch = I_GetPreciseTime();
	return true;
}

UINT32 I_GetSongPosition(void)
{
	if (!song_playing || song_paused)
		return song_position;
	return song_position + PreciseToMs((INT64)(I_GetPreciseTime() - song_epoch));
}

/// ------------------------
///  MUSIC PLAYBACK
/// ------------------------

boolean I_LoadSong(char *data, size_t len)
{
	(void)data;
	(void)len;

	if (song_loaded)
		I_UnloadSong();
	song_loaded = true;
	return true;
}

void I_UnloadSong(void)
{
	song_loaded = false;
	SongCleanup();
}

boolean I_PlaySong(boolean looping)
{
	if (!song_loaded)
		return false;
	song_playing = true;
	song_paused = false;
	song_looping = looping;
	song_position = 0;
	song_epoch = I_GetPreciseTime();
	return true;
}

void I_StopSong(void)
{
	SongCleanup();
}

void I_PauseSong(void)
{
	if (!song_playing || song_paused)
		return;
	song_position = I_GetSongPosition();
	song_paused = true;
}

void I_ResumeSong(void)
{
	if (!song_playing || !song_paused)
		return;
	song_paused = false;
	song_epoch = I_GetPreciseTime();
}

void I_SetMusicVolume(UINT8 volume)
{
	music_volume = volume;
}

boolean I_SetSongTrack(INT32 track)
{
	(void)track;
	return false;
}

/// ------------------------
/// MUSIC FADING
/// ------------------------

void I_SetInternalMusicVolume(UINT8 volume)
{
	internal_volume = volume;
}

void I_StopFadingSong(void)
{
	StopFading();
}

boolean I_FadeSongFromVolume(UINT8 target_volume, UINT8 source_volume, UINT32 ms, void (*callback)(void))
{
	INT16 volume_delta;

	source_volume = min(source_volume, 100);
	volume_delta = (INT16)(target_volume - source_volume);

	I_StopFadingSong();

	if (!ms && volume_delta)
	{
		I_SetInternalMusicVolume(target_volume);
		if (callback)
			(*callback)();
		return true;
	}
	else if (!volume_delta)
	{
		if (callback)
			(*callback)();
		return true;
	}

	// Round MS to nearest 10, as the SDL timer does
	ms = (ms - ((ms / 10) * 10) > (((ms / 10) * 10) + 10) - ms) ?
		(((ms / 10) * 10) + 10) // higher
		: ((ms / 10) * 10); // lower

	if (!ms)
		I_SetInternalMusicVolume(target_volume);
	else if (source_volume != target_volume)
	{
		is_fading = true;
		fading_duration = fading_remaining = MsToPrecise(ms);
		fading_last = I_GetPreciseTime();
		fading_source = source_volume;
		fading_target = target_volume;
		fading_callback = callback;

		if (internal_volume != source_volume)
			I_SetInternalMusicVolume(source_volume);
	}

	return is_fading;
}

boolean I_FadeSong(UINT8 target_volume, UINT32 ms, void (*callback)(void))
{
	return I_FadeSongFromVolume(target_volume, internal_volume, ms, callback);
}

boolean I_FadeOutStopSong(UINT32 ms)
{
	return I_FadeSongFromVolume(0, internal_volume, ms, &I_StopSong);
}

boolean I_FadeInPlaySong(UINT32 ms, boolean looping)
{
	if (I_PlaySong(looping))
		return I_FadeSongFromVolume(100, 0, ms, NULL);
	else
		return false;
}
