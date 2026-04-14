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

#include "esp_partition.h"
// #include "spi_flash_mmap.h"  // Not needed anymore - using PSRAM instead

#ifdef __GNUG__
#pragma implementation "i_system.h"
#endif
#include "i_system.h"

// #include <sys/time.h>  // Not needed - using FreeRTOS ticks instead

int realtime=0;


void I_uSleep(unsigned long usecs)
{
	vTaskDelay(usecs/1000);
}

static unsigned long getMsTicks() {
  // Use FreeRTOS tick count instead of gettimeofday to avoid lock issues
  static unsigned long start_ticks = 0;
  static bool initialized = false;

  if (!initialized) {
    start_ticks = xTaskGetTickCount();
    initialized = true;
  }

  unsigned long current_ticks = xTaskGetTickCount();
  return (current_ticks - start_ticks) * portTICK_PERIOD_MS;
}

int I_GetTime_RealTime (void)
{
  // Use FreeRTOS tick count instead of gettimeofday to avoid lock issues
  // TICRATE is 35 ticks per second (Doom standard)
  static unsigned long start_ticks = 0;
  static bool initialized = false;

  if (!initialized) {
    start_ticks = xTaskGetTickCount();
    initialized = true;
  }

  unsigned long current_ticks = xTaskGetTickCount();
  unsigned long elapsed_ms = (current_ticks - start_ticks) * portTICK_PERIOD_MS;

  return (elapsed_ms * TICRATE) / 1000;
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

extern unsigned char *doom1waddata;

typedef struct {
	const esp_partition_t* part;
	int offset;
	int size;
} FileDesc;

static FileDesc fds[32];

int I_Open(const char *wad, int flags) {
	int x=3;
	while (fds[x].part!=NULL) x++;
	if (strcmp(wad, "DOOM1.WAD")==0) {
		fds[x].part=esp_partition_find_first(66, 6, NULL);
		if (fds[x].part == NULL) {
			lprintf(LO_INFO, "I_Open: wad partition not found\n");
			return -1;
		}
		fds[x].offset=0;
		fds[x].size=fds[x].part->size;
		printf("Opened doom1.wad, part size is %d, fd is %d\n", fds[x].size, x);
	} else if (strcmp(wad, "prboom.wad")==0) {
		fds[x].part=esp_partition_find_first(66, 7, NULL);
		if (fds[x].part == NULL) {
			lprintf(LO_INFO, "I_Open: prboom partition not found\n");
			return -1;
		}
		fds[x].offset=0;
		fds[x].size=fds[x].part->size;
		printf("Opened prboom.wad, part size is %d, fd is %d\n", fds[x].size, x);
	} else {
		lprintf(LO_INFO, "I_Open: open %s failed\n", wad);
		return -1;
	}
	return x;
}

int I_Lseek(int ifd, off_t offset, int whence) {
	if (whence==SEEK_SET) {
		fds[ifd].offset=offset;
	} else if (whence==SEEK_CUR) {
		fds[ifd].offset+=offset;
	} else if (whence==SEEK_END) {
		lprintf(LO_INFO, "I_Lseek: SEEK_END unimplemented\n");
	}
	return fds[ifd].offset;
}

int I_Filelength(int ifd)
{
	return fds[ifd].size;
}

void I_Close(int fd) {
	fds[fd].part=NULL;
}


typedef struct {
	void *addr;		// Allocated buffer in PSRAM
	int offset;		// Original offset in partition
	size_t len;		// Length of mapped region
	const esp_partition_t *part;	// Source partition
	int used;		// Reference count for this mapping
} MmapHandle;

#define NO_MMAP_HANDLES 64  // Increased since we're using PSRAM now
static MmapHandle mmapHandle[NO_MMAP_HANDLES];

static int nextHandle=0;
static int getFreeHandle() {
	int n=NO_MMAP_HANDLES;
	while (mmapHandle[nextHandle].used!=0 && n!=0) {
		nextHandle++;
		if (nextHandle==NO_MMAP_HANDLES) nextHandle=0;
		n--;
	}
	if (n==0) {
		lprintf(LO_ERROR, "I_Mmap: More mmaps than NO_MMAP_HANDLES!");
		exit(0);
	}
	
	if (mmapHandle[nextHandle].addr) {
		free(mmapHandle[nextHandle].addr);
		mmapHandle[nextHandle].addr=NULL;
//		printf("mmap: freeing PSRAM handle %d\n", nextHandle);
	}
	int r=nextHandle;
	nextHandle++;
	if (nextHandle==NO_MMAP_HANDLES) nextHandle=0;

	return r;
}

static void freeUnusedMmaps() {
	for (int i=0; i<NO_MMAP_HANDLES; i++) {
		//Check if handle is not in use but is mapped.
		if (mmapHandle[i].used==0 && mmapHandle[i].addr!=NULL) {
			free(mmapHandle[i].addr);
			mmapHandle[i].addr=NULL;
			printf("Freeing handle %d\n", i);
		}
		if ((i & 0x7) == 0x7) vTaskDelay(1);  // Feed watchdog even more frequently (every 8 iterations)
	}
}

void *I_Mmap(void *addr, size_t length, int prot, int flags, int ifd, off_t offset) {
	int i;
	void *retaddr=NULL;

	// Feed watchdog periodically during memory mapping operations
	static int watchdog_counter = 0;
	if (++watchdog_counter % 5 == 0) {  // Feed more frequently
		vTaskDelay(1);
	}

	// Proactive cleanup: free unused mappings before attempting new ones
	// This prevents MMU exhaustion during initialization
	static int cleanup_counter = 0;
	if (++cleanup_counter % 8 == 0) {
		freeUnusedMmaps();
	}

	for (i=0; i<NO_MMAP_HANDLES; i++) {
		if (mmapHandle[i].offset==offset && mmapHandle[i].len==length && mmapHandle[i].part==fds[ifd].part) {
			mmapHandle[i].used++;
			return mmapHandle[i].addr;
		}
	}

	i=getFreeHandle();

//	lprintf(LO_INFO, "I_Mmap: allocating %d bytes from PSRAM for offset %d\n", (int)length, (int)offset);
	// Allocate buffer in PSRAM and read data directly from flash
	// This avoids the limited ESP32 MMU address space
	retaddr = malloc(length);
	if (retaddr == NULL) {
		// Try freeing unused allocations first
		lprintf(LO_ERROR, "I_Mmap: malloc failed, cleaning up unused allocations...\n");
		freeUnusedMmaps();
		retaddr = malloc(length);

		if (retaddr == NULL) {
			lprintf(LO_ERROR, "I_Mmap: Still can't allocate %d bytes!", length);
			return NULL;
		}
	}

	// Read data from flash into PSRAM buffer
	esp_err_t read_err = esp_partition_read(fds[ifd].part, offset, retaddr, length);
	if (read_err != ESP_OK) {
		lprintf(LO_ERROR, "I_Mmap: Can't read from flash: %x (len=%d)!", read_err, length);
		free(retaddr);
		return NULL;
	}

	mmapHandle[i].addr=retaddr;
	mmapHandle[i].len=length;
	mmapHandle[i].used=1;
	mmapHandle[i].offset=offset;
	mmapHandle[i].part=fds[ifd].part;

	return retaddr;
}


int I_Munmap(void *addr, size_t length) {
	int i;
	for (i=0; i<NO_MMAP_HANDLES; i++) {
		if (mmapHandle[i].addr==addr && mmapHandle[i].len==length) break;
	}
	if (i==NO_MMAP_HANDLES) {
		lprintf(LO_ERROR, "I_Mmap: Freeing non-mmapped address/len combo!");
		exit(0);
	}
//	lprintf(LO_INFO, "I_Mmap: freeing handle %d\n", i);
	mmapHandle[i].used--;
	return 0;
}

void I_Read(int ifd, void* vbuf, size_t sz)
{
	if (fds[ifd].offset + sz > fds[ifd].size) {
		sz = fds[ifd].size - fds[ifd].offset;
	}
	// Direct flash read into PSRAM instead of mmap to avoid MMU exhaustion
	esp_partition_read(fds[ifd].part, fds[ifd].offset, vbuf, sz);
	fds[ifd].offset += sz;
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
