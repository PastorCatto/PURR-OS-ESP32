/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *  Misc system stuff needed by Doom, implemented for Linux.
 *  Mainly timer handling, and ENDOOM/ENDBOOM.
 *
 *-----------------------------------------------------------------------------
 */

#include <stdio.h>

#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <signal.h>
#ifdef _MSC_VER
#define    F_OK    0    /* Check for file existence */
#define    W_OK    2    /* Check for write permission */
#define    R_OK    4    /* Check for read permission */
#include <io.h>
#include <direct.h>
#else
#include <unistd.h>
#endif
#include <sys/stat.h>



#include "config.h"
#include <unistd.h>
#include <sched.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#include "m_argv.h"
#include "lprintf.h"
#include "doomtype.h"
#include "doomdef.h"
#include "lprintf.h"
#include "m_fixed.h"
#include "r_fps.h"
#include "i_system.h"
#include "i_joy.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#ifdef __GNUG__
#pragma implementation "i_system.h"
#endif
#include "i_system.h"

#include <sys/time.h>

int realtime=0;


void I_uSleep(unsigned long usecs)
{
	vTaskDelay(usecs/1000);
}

static unsigned long getMsTicks() {
  struct timeval tv;
  struct timezone tz;
  unsigned long thistimereply;

  gettimeofday(&tv, &tz);

  //convert to ms
  unsigned long now = tv.tv_usec/1000+tv.tv_sec*1000;
  return now;
}

int I_GetTime_RealTime (void)
{
  struct timeval tv;
  struct timezone tz;
  unsigned long thistimereply;

  gettimeofday(&tv, &tz);

  thistimereply = (tv.tv_sec * TICRATE + (tv.tv_usec * TICRATE) / 1000000);

  return thistimereply;

}

const int displaytime=0;

fixed_t I_GetTimeFrac (void)
{
  unsigned long now;
  fixed_t frac;


  now = getMsTicks();

  if (tic_vars.step == 0)
    return FRACUNIT;
  else
  {
    frac = (fixed_t)((now - tic_vars.start + displaytime) * FRACUNIT / tic_vars.step);
    if (frac < 0)
      frac = 0;
    if (frac > FRACUNIT)
      frac = FRACUNIT;
    return frac;
  }
}


void I_GetTime_SaveMS(void)
{
  if (!movement_smooth)
    return;

  tic_vars.start = getMsTicks();
  tic_vars.next = (unsigned int) ((tic_vars.start * tic_vars.msec + 1.0f) / tic_vars.msec);
  tic_vars.step = tic_vars.next - tic_vars.start;
}

unsigned long I_GetRandomTimeSeed(void)
{
	return 4; //per https://xkcd.com/221/
}

const char* I_GetVersionString(char* buf, size_t sz)
{
  sprintf(buf,"%s v%s (http://prboom.sourceforge.net/)",PACKAGE,VERSION);
  return buf;
}

const char* I_SigString(char* buf, size_t sz, int signum)
{
  return buf;
}

// ── WAD access ──────────────────────────────────────────────────────────────
//
// Everything below the I_ file layer was replaced for PURR OS. What was here
// before mapped the WAD out of a dedicated flash partition with
// esp_partition_mmap(), and I_Read() was implemented on top of I_Mmap() — the
// whole engine reads lumps as pointers into memory-mapped flash and never
// copies them into the zone heap. That zero-copy design is a large part of why
// PrBoom fits on this class of device at all, so it is preserved exactly.
//
// What changed is where the bytes live. The WAD is read from the SD card
// (/sdcard/doom/) into PSRAM once, at startup, and I_Mmap() is then pointer
// arithmetic into that buffer. Three reasons:
//
//   1. FAT on SD cannot be memory-mapped, so the original approach is simply
//      unavailable without a dedicated WAD partition and a re-flash to change
//      the WAD.
//   2. PSRAM is the abundant resource here — ~8.15MB free under speed demon,
//      against a 3-4MB WAD. Internal DRAM, the scarce one, is untouched.
//   3. It takes the SD card off the SPI bus for the whole of gameplay. The bus
//      is shared with the display; speed demon has already removed the LoRa
//      radio from it, and this removes the other contender. Loading is one
//      bulk read at startup rather than seeks during play.
//
// See doom_wad.c for the loader and doom_wad.h for the interface.

#include "esp_timer.h"
#include "speed_demon.h"
#include "doom_wad.h"

// Descriptor table. Handles start at 3 to stay clear of stdin/stdout/stderr,
// which is what the original did — PrBoom passes these to its own I_ calls
// only, but the offset costs nothing and keeps them distinguishable in a log.
typedef struct {
	int in_use;
	int offset;
	int size;
} FileDesc;

static FileDesc fds[32];

int I_Open(const char *wad, int flags)
{
	(void)flags;

	// Any name resolves to the single loaded WAD. PrBoom asks for the file it
	// found via I_FindFile/the argv list; we have exactly one, chosen by
	// doom_wad_load() scanning the directory, so matching on the name here
	// would only reintroduce the hardcoded "DOOM1.WAD" that stopped the
	// original from loading anything else.
	if (!doom_wad_data()) {
		lprintf(LO_ERROR, "I_Open: no WAD loaded (asked for %s)\n", wad ? wad : "?");
		return -1;
	}

	for (int x = 3; x < (int)(sizeof(fds) / sizeof(fds[0])); x++) {
		if (!fds[x].in_use) {
			fds[x].in_use = 1;
			fds[x].offset = 0;
			fds[x].size   = (int)doom_wad_size();
			return x;
		}
	}
	lprintf(LO_ERROR, "I_Open: out of descriptors\n");
	return -1;
}

int I_Lseek(int ifd, off_t offset, int whence)
{
	if (ifd < 0 || ifd >= (int)(sizeof(fds) / sizeof(fds[0]))) return -1;

	if (whence == SEEK_SET) {
		fds[ifd].offset = (int)offset;
	} else if (whence == SEEK_CUR) {
		fds[ifd].offset += (int)offset;
	} else if (whence == SEEK_END) {
		// Implemented, unlike the original, which logged "unimplemented" and
		// returned the unchanged offset. It costs one line here and a caller
		// that ever uses it would otherwise fail in a way that looks like WAD
		// corruption rather than a missing seek mode.
		fds[ifd].offset = fds[ifd].size + (int)offset;
	}
	return fds[ifd].offset;
}

int I_Filelength(int ifd)
{
	if (ifd < 0 || ifd >= (int)(sizeof(fds) / sizeof(fds[0]))) return 0;
	return fds[ifd].size;
}

void I_Close(int fd)
{
	if (fd < 0 || fd >= (int)(sizeof(fds) / sizeof(fds[0]))) return;
	fds[fd].in_use = 0;
}

// The whole WAD is already resident, so a "mapping" is an address inside it.
// No handle table, no LRU, no unmapping — the original needed all three because
// the ESP32's flash mmap window is a limited hardware resource that had to be
// recycled; a PSRAM pointer is not.
void *I_Mmap(void *addr, size_t length, int prot, int flags, int ifd, off_t offset)
{
	(void)addr; (void)prot; (void)flags;

	// Heartbeat, throttled to once a second.
	//
	// This covers the gap between the WAD finishing loading and the engine
	// reaching its first I_StartTic (which carries the heartbeat from then on).
	// In between sit W_Init, R_InitData and friends — "Textures Flats Sprites
	// Tranmap build" is seconds of work with no tic loop running, and speed
	// demon reboots the device after ten seconds of silence.
	//
	// I_Mmap is the right hook precisely because it is not a timer: every lump
	// PrBoom touches comes through here, so it beats when the engine is
	// actually making progress and stops when it genuinely wedges. A periodic
	// timer task would have kept beating through a real hang and defeated the
	// watchdog entirely.
	{
		static int64_t last_beat_us = 0;
		int64_t now = esp_timer_get_time();
		if (now - last_beat_us > 1000000) {
			last_beat_us = now;
			purr_speed_demon_heartbeat();
		}
	}

	const uint8_t *base = doom_wad_data();
	if (!base) return NULL;

	size_t size = doom_wad_size();
	if ((size_t)offset > size || (size_t)offset + length > size) {
		lprintf(LO_ERROR, "I_Mmap: out of range (off=%d len=%u size=%u)\n",
		        (int)offset, (unsigned)length, (unsigned)size);
		return NULL;
	}
	(void)ifd;
	return (void *)(base + offset);
}

int I_Munmap(void *addr, size_t length)
{
	// Nothing to release: the address is interior to the WAD buffer, which is
	// freed as a whole by doom_wad_free() when the app exits.
	(void)addr; (void)length;
	return 0;
}

void I_Read(int ifd, void *vbuf, size_t sz)
{
	const uint8_t *src = I_Mmap(NULL, sz, 0, 0, ifd, fds[ifd].offset);
	if (!src) {
		lprintf(LO_ERROR, "I_Read: read past end of WAD\n");
		memset(vbuf, 0, sz);
		return;
	}
	memcpy(vbuf, src, sz);
	// Advance the descriptor. The original did NOT do this — it mapped at the
	// current offset and left it there, so a sequence of I_Read() calls all
	// returned the same bytes. PrBoom happens to I_Lseek() before every read,
	// which is why that went unnoticed; it is still wrong, and a read loop
	// written the obvious way would have silently returned garbage.
	fds[ifd].offset += (int)sz;
}

const char *I_DoomExeDir(void)
{
  return "";
}



char* I_FindFile(const char* wfname, const char* ext)
{
  char *p;
  p = malloc(strlen(wfname)+4);
  sprintf(p, "%s.%s", wfname, ext);
  return NULL;
}

void I_SetAffinityMask(void)
{
}

// access() was stubbed here — `return 1` for every path — because the original
// port linked no filesystem at all and something had to satisfy the reference.
//
// Removed: this build links vfs (for the SD card), which provides the real
// access(), and two definitions is a link error:
//
//   multiple definition of `access'; vfs.c:864: first defined here
//
// Deleting ours is the right way round rather than renaming the VFS one. The
// stub claimed every path was accessible, which was a lie that happened not to
// matter with no files to check; now there is a real filesystem with real files
// on it, so the real answer is both available and more useful.




