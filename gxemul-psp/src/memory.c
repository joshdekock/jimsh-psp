/*
 *  Copyright (C) 2003-2006  Anders Gavare.  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright  
 *     notice, this list of conditions and the following disclaimer in the 
 *     documentation and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 *  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE   
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 *  OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 *  HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 *  OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 *  SUCH DAMAGE.
 *
 *
 *  $Id: memory.c,v 1.187 2006/01/14 12:51:59 debug Exp $
 *
 *  Functions for handling the memory of an emulated machine.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/mman.h>

#include "cpu.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"


extern int verbose;


/*
 *  memory_readmax64():
 *
 *  Read at most 64 bits of data from a buffer.  Length is given by
 *  len, and the byte order by cpu->byte_order.
 *
 *  This function should not be called with cpu == NULL.
 */
uint64_t memory_readmax64(struct cpu *cpu, unsigned char *buf, int len)
{
	int i, byte_order = cpu->byte_order;
	uint64_t x = 0;

	if (len & MEM_PCI_LITTLE_ENDIAN) {
		len &= ~MEM_PCI_LITTLE_ENDIAN;
		byte_order = EMUL_LITTLE_ENDIAN;
	}

	/*  Switch byte order for incoming data, if necessary:  */
	if (byte_order == EMUL_BIG_ENDIAN)
		for (i=0; i<len; i++) {
			x <<= 8;
			x |= buf[i];
		}
	else
		for (i=len-1; i>=0; i--) {
			x <<= 8;
			x |= buf[i];
		}

	return x;
}


/*
 *  memory_writemax64():
 *
 *  Write at most 64 bits of data to a buffer.  Length is given by
 *  len, and the byte order by cpu->byte_order.
 *
 *  This function should not be called with cpu == NULL.
 */
void memory_writemax64(struct cpu *cpu, unsigned char *buf, int len,
	uint64_t data)
{
	int i, byte_order = cpu->byte_order;

	if (len & MEM_PCI_LITTLE_ENDIAN) {
		len &= ~MEM_PCI_LITTLE_ENDIAN;
		byte_order = EMUL_LITTLE_ENDIAN;
	}

	if (byte_order == EMUL_LITTLE_ENDIAN)
		for (i=0; i<len; i++) {
			buf[i] = data & 255;
			data >>= 8;
		}
	else
		for (i=0; i<len; i++) {
			buf[len - 1 - i] = data & 255;
			data >>= 8;
		}
}


/*
 *  zeroed_alloc():
 *
 *  Allocates a block of memory using mmap(), and if that fails, try
 *  malloc() + memset(). The returned memory block contains only zeroes.
 */
void *zeroed_alloc(size_t s)
{
	void *p = mmap(NULL, s, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	if (p == NULL) {
		p = malloc(s);
		if (p == NULL) {
			fprintf(stderr, "out of memory\n");
			exit(1);
		}
		memset(p, 0, s);
	}
	return p;
}


/*
 *  memory_new():
 *
 *  This function creates a new memory object. An emulated machine needs one
 *  of these.
 */
struct memory *memory_new(uint64_t physical_max, int arch)
{
	struct memory *mem;
	int bits_per_pagetable = BITS_PER_PAGETABLE;
	int bits_per_memblock = BITS_PER_MEMBLOCK;
	int entries_per_pagetable = 1 << BITS_PER_PAGETABLE;
	int max_bits = MAX_BITS;
	size_t s;

	mem = malloc(sizeof(struct memory));
	if (mem == NULL) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	memset(mem, 0, sizeof(struct memory));

	/*  Check bits_per_pagetable and bits_per_memblock for sanity:  */
	if (bits_per_pagetable + bits_per_memblock != max_bits) {
		fprintf(stderr, "memory_new(): bits_per_pagetable and "
		    "bits_per_memblock mismatch\n");
		exit(1);
	}

	mem->physical_max = physical_max;
	mem->dev_dyntrans_alignment = 4095;
	if (arch == ARCH_ALPHA)
		mem->dev_dyntrans_alignment = 8191;

	s = entries_per_pagetable * sizeof(void *);

	mem->pagetable = (unsigned char *) mmap(NULL, s,
	    PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (mem->pagetable == NULL) {
		mem->pagetable = malloc(s);
		if (mem->pagetable == NULL) {
			fprintf(stderr, "out of memory\n");
			exit(1);
		}
		memset(mem->pagetable, 0, s);
	}

	mem->mmap_dev_minaddr = 0xffffffffffffffffULL;
	mem->mmap_dev_maxaddr = 0;

	return mem;
}


/*
 *  memory_points_to_string():
 *
 *  Returns 1 if there's something string-like in emulated memory at address
 *  addr, otherwise 0.
 */
int memory_points_to_string(struct cpu *cpu, struct memory *mem, uint64_t addr,
	int min_string_length)
{
	int cur_length = 0;
	unsigned char c;

	for (;;) {
		c = '\0';
		cpu->memory_rw(cpu, mem, addr+cur_length,
		    &c, sizeof(c), MEM_READ, CACHE_NONE | NO_EXCEPTIONS);
		if (c=='\n' || c=='\t' || c=='\r' || (c>=' ' && c<127)) {
			cur_length ++;
			if (cur_length >= min_string_length)
				return 1;
		} else {
			if (cur_length >= min_string_length)
				return 1;
			else
				return 0;
		}
	}
}


/*
 *  memory_conv_to_string():
 *
 *  Convert emulated memory contents to a string, placing it in a buffer
 *  provided by the caller.
 */
char *memory_conv_to_string(struct cpu *cpu, struct memory *mem, uint64_t addr,
	char *buf, int bufsize)
{
	int len = 0;
	int output_index = 0;
	unsigned char c, p='\0';

	while (output_index < bufsize-1) {
		c = '\0';
		cpu->memory_rw(cpu, mem, addr+len, &c, sizeof(c), MEM_READ,
		    CACHE_NONE | NO_EXCEPTIONS);
		buf[output_index] = c;
		if (c>=' ' && c<127) {
			len ++;
			output_index ++;
		} else if (c=='\n' || c=='\r' || c=='\t') {
			len ++;
			buf[output_index] = '\\';
			output_index ++;
			switch (c) {
			case '\n':	p = 'n'; break;
			case '\r':	p = 'r'; break;
			case '\t':	p = 't'; break;
			}
			if (output_index < bufsize-1) {
				buf[output_index] = p;
				output_index ++;
			}
		} else {
			buf[output_index] = '\0';
			return buf;
		}
	}

	buf[bufsize-1] = '\0';
	return buf;
}


/*
 *  memory_device_dyntrans_access():
 *
 *  Get the lowest and highest dyntrans access since last time.
 */
void memory_device_dyntrans_access(struct cpu *cpu, struct memory *mem,
	void *extra, uint64_t *low, uint64_t *high)
{
	int i, j;
	size_t s;
	int need_inval = 0;

	/*  TODO: This is O(n), so it might be good to rewrite it some day.
	    For now, it will be enough, as long as this function is not
	    called too often.  */

	for (i=0; i<mem->n_mmapped_devices; i++) {
		if (mem->dev_extra[i] == extra &&
		    mem->dev_flags[i] & DM_DYNTRANS_WRITE_OK &&
		    mem->dev_dyntrans_data[i] != NULL) {
			if (mem->dev_dyntrans_write_low[i] != (uint64_t) -1)
				need_inval = 1;
			if (low != NULL)
				*low = mem->dev_dyntrans_write_low[i];
			mem->dev_dyntrans_write_low[i] = (uint64_t) -1;

			if (high != NULL)
				*high = mem->dev_dyntrans_write_high[i];
			mem->dev_dyntrans_write_high[i] = 0;

			if (!need_inval)
				return;

			/*  Invalidate any pages of this device that might
			    be in the dyntrans load/store cache, by marking
			    the pages read-only.  */
			if (cpu->invalidate_translation_caches != NULL) {
				for (s=0; s<mem->dev_length[i];
				    s+=cpu->machine->arch_pagesize)
					cpu->invalidate_translation_caches
					    (cpu, mem->dev_baseaddr[i] + s,
					    JUST_MARK_AS_NON_WRITABLE
					    | INVALIDATE_PADDR);
			}

			if (cpu->machine->arch == ARCH_MIPS) {
				/*
				 *  ... and invalidate the "fast_vaddr_to_
				 *  hostaddr" cache entries that contain
				 *  pointers to this device:  (NOTE: Device i,
				 *  cache entry j)
				 */
				for (j=0; j<N_BINTRANS_VADDR_TO_HOST; j++) {
					if (cpu->cd.
					    mips.bintrans_data_hostpage[j] >=
					    mem->dev_dyntrans_data[i] &&
					    cpu->cd.mips.
					    bintrans_data_hostpage[j] <
					    mem->dev_dyntrans_data[i] +
					    mem->dev_length[i])
						cpu->cd.mips.
						    bintrans_data_hostpage[j]
						    = NULL;
				}
			}
			return;
		}
	}
}


/*
 *  memory_device_register():
 *
 *  Register a (memory mapped) device by adding it to the dev_* fields of a
 *  memory struct.
 */
void memory_device_register(struct memory *mem, const char *device_name,
	uint64_t baseaddr, uint64_t len,
	int (*f)(struct cpu *,struct memory *,uint64_t,unsigned char *,
		size_t,int,void *),
	void *extra, int flags, unsigned char *dyntrans_data)
{
	int i, newi = 0;

	if (mem->n_mmapped_devices >= MAX_DEVICES) {
		fprintf(stderr, "memory_device_register(): too many "
		    "devices registered, cannot register '%s'\n", device_name);
		exit(1);
	}

	/*
	 *  Figure out at which index to insert this device, and simultaneously
	 *  check for collisions:
	 */
	newi = -1;
	for (i=0; i<mem->n_mmapped_devices; i++) {
		if (i == 0 && baseaddr + len <= mem->dev_baseaddr[i])
			newi = i;
		if (i > 0 && baseaddr + len <= mem->dev_baseaddr[i] &&
		    baseaddr >= mem->dev_endaddr[i-1])
			newi = i;
		if (i == mem->n_mmapped_devices - 1 &&
		    baseaddr >= mem->dev_endaddr[i])
			newi = i + 1;

		/*  If we are not colliding with device i, then continue:  */
		if (baseaddr + len <= mem->dev_baseaddr[i])
			continue;
		if (baseaddr >= mem->dev_endaddr[i])
			continue;

		fatal("\nERROR! \"%s\" collides with device %i (\"%s\")!\n",
		    device_name, i, mem->dev_name[i]);
		exit(1);
	}
	if (mem->n_mmapped_devices == 0)
		newi = 0;
	if (newi == -1) {
		fatal("INTERNAL ERROR\n");
		exit(1);
	}

	if (verbose >= 2) {
		/*  (40 bits of physical address is displayed)  */
		debug("device at 0x%010llx: %s", (long long)baseaddr,
		    device_name);

		if (flags & (DM_DYNTRANS_OK | DM_DYNTRANS_WRITE_OK)
		    && (baseaddr & mem->dev_dyntrans_alignment) != 0) {
			fatal("\nWARNING: Device dyntrans access, but unaligned"
			    " baseaddr 0x%llx.\n", (long long)baseaddr);
		}

		if (flags & (DM_DYNTRANS_OK | DM_DYNTRANS_WRITE_OK)) {
			debug(" (dyntrans %s)",
			    (flags & DM_DYNTRANS_WRITE_OK)? "R/W" : "R");
		}
		debug("\n");
	}

	for (i=0; i<mem->n_mmapped_devices; i++) {
		if (dyntrans_data == mem->dev_dyntrans_data[i] &&
		    mem->dev_flags[i] & (DM_DYNTRANS_OK | DM_DYNTRANS_WRITE_OK)
		    && flags & (DM_DYNTRANS_OK | DM_DYNTRANS_WRITE_OK)) {
			fatal("ERROR: the data pointer used for dyntrans "
			    "accesses must only be used once!\n");
			fatal("(%p cannot be used by '%s'; already in use by '"
			    "%s')\n", dyntrans_data, device_name,
			    mem->dev_name[i]);
			exit(1);
		}
	}

	mem->n_mmapped_devices++;

	/*
	 *  YUCK! This is ugly. TODO: fix
	 */
	/*  Make space for the new entry:  */
	memmove(&mem->dev_name[newi+1], &mem->dev_name[newi], sizeof(char *) *
	    (MAX_DEVICES - newi - 1));
	memmove(&mem->dev_baseaddr[newi+1], &mem->dev_baseaddr[newi],
	    sizeof(uint64_t) * (MAX_DEVICES - newi - 1));
	memmove(&mem->dev_endaddr[newi+1], &mem->dev_endaddr[newi],
	    sizeof(uint64_t) * (MAX_DEVICES - newi - 1));
	memmove(&mem->dev_length[newi+1], &mem->dev_length[newi],
	    sizeof(uint64_t) * (MAX_DEVICES - newi - 1));
	memmove(&mem->dev_flags[newi+1], &mem->dev_flags[newi], sizeof(int) *
	    (MAX_DEVICES - newi - 1));
	memmove(&mem->dev_extra[newi+1], &mem->dev_extra[newi], sizeof(void *) *
	    (MAX_DEVICES - newi - 1));
	memmove(&mem->dev_f[newi+1], &mem->dev_f[newi], sizeof(void *) *
	    (MAX_DEVICES - newi - 1));
	memmove(&mem->dev_dyntrans_data[newi+1], &mem->dev_dyntrans_data[newi],
	    sizeof(void *) * (MAX_DEVICES - newi - 1));
	memmove(&mem->dev_dyntrans_write_low[newi+1],
	    &mem->dev_dyntrans_write_low[newi],
	    sizeof(uint64_t) * (MAX_DEVICES - newi - 1));
	memmove(&mem->dev_dyntrans_write_high[newi+1],
	    &mem->dev_dyntrans_write_high[newi],
	    sizeof(uint64_t) * (MAX_DEVICES - newi - 1));


	mem->dev_name[newi] = strdup(device_name);
	mem->dev_baseaddr[newi] = baseaddr;
	mem->dev_endaddr[newi] = baseaddr + len;
	mem->dev_length[newi] = len;
	mem->dev_flags[newi] = flags;
	mem->dev_dyntrans_data[newi] = dyntrans_data;

	if (mem->dev_name[newi] == NULL) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	if (flags & (DM_DYNTRANS_OK | DM_DYNTRANS_WRITE_OK)
	    && !(flags & DM_EMULATED_RAM) && dyntrans_data == NULL) {
		fatal("\nERROR: Device dyntrans access, but dyntrans_data"
		    " = NULL!\n");
		exit(1);
	}

	if ((size_t)dyntrans_data & (sizeof(void *) - 1)) {
		fprintf(stderr, "memory_device_register():"
		    " dyntrans_data not aligned correctly (%p)\n",
		    dyntrans_data);
		exit(1);
	}

	mem->dev_dyntrans_write_low[newi] = (uint64_t)-1;
	mem->dev_dyntrans_write_high[newi] = 0;
	mem->dev_f[newi] = f;
	mem->dev_extra[newi] = extra;

	if (baseaddr < mem->mmap_dev_minaddr)
		mem->mmap_dev_minaddr = baseaddr & ~mem->dev_dyntrans_alignment;
	if (baseaddr + len > mem->mmap_dev_maxaddr)
		mem->mmap_dev_maxaddr = (((baseaddr + len) - 1) |
		    mem->dev_dyntrans_alignment) + 1;
}


/*
 *  memory_device_remove():
 *
 *  Unregister a (memory mapped) device from a memory struct.
 */
void memory_device_remove(struct memory *mem, int i)
{
	if (i < 0 || i >= mem->n_mmapped_devices) {
		fatal("memory_device_remove(): invalid device number %i\n", i);
		return;
	}

	mem->n_mmapped_devices --;

	if (i == mem->n_mmapped_devices)
		return;

	/*
	 *  YUCK! This is ugly. TODO: fix
	 */

	memmove(&mem->dev_name[i], &mem->dev_name[i+1], sizeof(char *) *
	    (MAX_DEVICES - i - 1));
	memmove(&mem->dev_baseaddr[i], &mem->dev_baseaddr[i+1],
	    sizeof(uint64_t) * (MAX_DEVICES - i - 1));
	memmove(&mem->dev_endaddr[i], &mem->dev_endaddr[i+1],
	    sizeof(uint64_t) * (MAX_DEVICES - i - 1));
	memmove(&mem->dev_length[i], &mem->dev_length[i+1], sizeof(uint64_t) *
	    (MAX_DEVICES - i - 1));
	memmove(&mem->dev_flags[i], &mem->dev_flags[i+1], sizeof(int) *
	    (MAX_DEVICES - i - 1));
	memmove(&mem->dev_extra[i], &mem->dev_extra[i+1], sizeof(void *) *
	    (MAX_DEVICES - i - 1));
	memmove(&mem->dev_f[i], &mem->dev_f[i+1], sizeof(void *) *
	    (MAX_DEVICES - i - 1));
	memmove(&mem->dev_dyntrans_data[i], &mem->dev_dyntrans_data[i+1],
	    sizeof(void *) * (MAX_DEVICES - i - 1));
	memmove(&mem->dev_dyntrans_write_low[i], &mem->dev_dyntrans_write_low
	    [i+1], sizeof(uint64_t) * (MAX_DEVICES - i - 1));
	memmove(&mem->dev_dyntrans_write_high[i], &mem->dev_dyntrans_write_high
	    [i+1], sizeof(uint64_t) * (MAX_DEVICES - i - 1));
}


#define MEMORY_RW	userland_memory_rw
#define MEM_USERLAND
#include "memory_rw.c"
#undef MEM_USERLAND
#undef MEMORY_RW


/*
 *  memory_paddr_to_hostaddr():
 *
 *  Translate a physical address into a host address.
 *
 *  Return value is a pointer to a host memblock, or NULL on failure.
 *  On reads, a NULL return value should be interpreted as reading all zeroes.
 */
unsigned char *memory_paddr_to_hostaddr(struct memory *mem,
	uint64_t paddr, int writeflag)
{
	void **table;
	int entry;
	const int mask = (1 << BITS_PER_PAGETABLE) - 1;
	const int shrcount = MAX_BITS - BITS_PER_PAGETABLE;

	table = mem->pagetable;
	entry = (paddr >> shrcount) & mask;

	/*  printf("memory_paddr_to_hostaddr(): p=%16llx w=%i => entry=0x%x\n",
	    (long long)paddr, writeflag, entry);  */

	if (table[entry] == NULL) {
		size_t alloclen;

		/*
		 *  Special case:  reading from a nonexistant memblock
		 *  returns all zeroes, and doesn't allocate anything.
		 *  (If any intermediate pagetable is nonexistant, then
		 *  the same thing happens):
		 */
		if (writeflag == MEM_READ)
			return NULL;

		/*  Allocate a memblock:  */
		alloclen = 1 << BITS_PER_MEMBLOCK;

		/*  printf("  allocating for entry %i, len=%i\n",
		    entry, alloclen);  */

		/*  Anonymous mmap() should return zero-filled memory,
		    try malloc + memset if mmap failed.  */
		table[entry] = (void *) mmap(NULL, alloclen,
		    PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
		if (table[entry] == NULL) {
			table[entry] = malloc(alloclen);
			if (table[entry] == NULL) {
				fatal("out of memory\n");
				exit(1);
			}
			memset(table[entry], 0, alloclen);
		}
	}

	return (unsigned char *) table[entry];
}

