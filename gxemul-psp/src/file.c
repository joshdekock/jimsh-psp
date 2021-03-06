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
 *  $Id: file.c,v 1.127 2006/01/22 23:20:33 debug Exp $
 *
 *  This file contains functions which load executable images into (emulated)
 *  memory. File formats recognized so far are:
 *
 *	a.out		old format used by OpenBSD 2.x pmax kernels
 *	Mach-O		MacOS X format, etc.
 *	ecoff		old format used by Ultrix, Windows NT, etc
 *	srec		Motorola SREC format
 *	raw		raw binaries, "address:[skiplen:[entrypoint:]]filename"
 *	ELF		32-bit and 64-bit ELFs
 *
 *  If a file is not of one of the above mentioned formats, it is assumed
 *  to be symbol data generated by 'nm' or 'nm -S'.
 */

#define DEBUG_PSP_LOADER

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "cpu.h"
#include "cop0.h"
#include "exec_elf.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"
#include "symbol.h"

extern int quiet_mode;
extern int verbose;

/*  ELF machine types as strings: (same as exec_elf.h)  */
#define N_ELF_MACHINE_TYPES	64
static char *elf_machine_type[N_ELF_MACHINE_TYPES] = {
	"NONE", "M32", "SPARC", "386",				/*  0..3  */
	"68K", "88K", "486", "860",				/*  4..7  */
	"MIPS", "S370", "MIPS_RS3_LE", "RS6000",		/*  8..11  */
	"unknown12", "unknown13", "unknown14", "PARISC",	/*  12..15  */
	"NCUBE", "VPP500", "SPARC32PLUS", "960",		/*  16..19  */
	"PPC", "PPC64", "unknown22", "unknown23",		/*  20..23  */
	"unknown24", "unknown25", "unknown26", "unknown27",	/*  24..27  */
	"unknown28", "unknown29", "unknown30", "unknown31",	/*  28..31  */
	"unknown32", "unknown33", "unknown34", "unknown35",	/*  32..35  */
	"V800", "FR20", "RH32", "RCE",				/*  36..39  */
	"ARM", "ALPHA", "SH", "SPARCV9",			/*  40..43  */
	"TRICORE", "ARC", "H8_300", "H8_300H",			/*  44..47  */
	"H8S", "H8_500", "IA_64", "MIPS_X",			/*  48..51  */
	"COLDFIRE", "68HC12", "unknown54", "unknown55",		/*  52..55  */
	"unknown56", "unknown57", "unknown58", "unknown59",	/*  56..59  */
	"unknown60", "unknown61", "AMD64", "unknown63"		/*  60..63  */
};


/*
 *  This should be increased by every routine here that actually loads an
 *  executable file into memory.  (For example, loading a symbol file should
 *  NOT increase this.)
 */
static int n_executables_loaded = 0;

/*
 *  file_load_raw():
 *
 *  Loads a raw binary into emulated memory. The filename should be
 *  of the following form:     loadaddress:filename
 *  or    loadaddress:skiplen:filename
 *  or    loadaddress:skiplen:pc:filename
 */
static void file_load_raw(struct machine *m, struct memory *mem,
	char *filename, uint64_t *entrypointp)
{
	FILE *f;
	int len;
	unsigned char buf[4096];
	uint64_t entry, loadaddr, vaddr, binsize,skip = 0;
	char *p, *p2;

	p = strchr(filename, ':');
	if (p == NULL) {
		fprintf(stderr, "\n");
		perror(filename);
		exit(1);
	}

	loadaddr = vaddr = entry = strtoull(filename, NULL, 0);
	p2 = p+1;

	/*  A second value? That's the optional skip value  */
	p = strchr(p2, ':');
	if (p != NULL) {
		skip = strtoull(p2, NULL, 0);
		p = p+1;
		/*  A third value? That's the initial pc:  */
		if (strchr(p, ':') != NULL) {
			entry = strtoull(p, NULL, 0);
			p = strchr(p, ':') + 1;
		}
	} else
		p = p2;

	f = fopen(strrchr(filename, ':')+1, "r");
	if (f == NULL) {
		perror(p);
		exit(1);
	}

	fseek(f, skip, SEEK_SET);

	/*  Load file contents:  */
	while (!feof(f)) {
		len = fread(buf, 1, sizeof(buf), f);

		if (len > 0)
			m->cpus[0]->memory_rw(m->cpus[0], mem, vaddr, &buf[0],
			    len, MEM_WRITE, NO_EXCEPTIONS);

		vaddr += len;
	}

	binsize=(long long) (ftello(f) - skip);

	debug("RAW: 0x%llx bytes @ 0x%08llx",
	    binsize, (long long)loadaddr);
	if (skip != 0)
		debug(" (0x%llx bytes of header skipped)", (long long)skip);
	debug("\n");

	*entrypointp = entry;

//	add_symbol_name(&m->symbol_context,loadaddr,0, "BINFILE", 0, 0);
	add_symbol_name(&m->symbol_context,loadaddr        ,binsize, ".binfile", 0, 0);
	add_symbol_name(&m->symbol_context,loadaddr        ,1, ".binfile.start", 0, 0);
	add_symbol_name(&m->symbol_context,loadaddr+binsize,1, ".binfile.end"  , 0, 0);

	add_symbol_name(&m->symbol_context,loadaddr+binsize,1, ".binfile.bss.start"  , 0, 0);
	add_symbol_name(&m->symbol_context,loadaddr+binsize,0, ".binfile.bss"  , 0, 0);

	add_symbol_name(&m->symbol_context,entry           ,1, "ENTRYPOINT"    , 0, 2);

	add_symbol_name(&m->symbol_context,0xffffffff      ,1, "HIGHESTADDR"    , 0, 0);

	fclose(f);

	n_executables_loaded ++;
}

/***************************************************************************************************************
	PSP Specific START
***************************************************************************************************************/

/*
 *  file_load_elf():
 *
 *  Loads an ELF image into the emulated memory.  The entry point (read from
 *  the ELF header) and the initial value of the gp register (read from the
 *  ELF symbol table) are stored in the specified CPU's registers.
 *
 *  This is pretty heavy stuff, but is needed because of the heaviness of
 *  ELF files. :-/   Hopefully it will be able to recognize most valid ELFs.
 */

#include "psp_hle.h"

#define	psp_unencode(var,dataptr,typ)	{				\
		int Wi;  unsigned char Wb;				\
		unsigned char *Wp = (unsigned char *) dataptr;		\
		int Wlen = sizeof(typ);					\
		var = 0;						\
		for (Wi=0; Wi<Wlen; Wi++) {				\
			if (ELFDATA2LSB == ELFDATA2LSB)			\
				Wb = Wp[Wlen-1 - Wi];			\
			else						\
				Wb = Wp[Wi];				\
			if (Wi == 0 && (Wb & 0x80)) {			\
				var --;	/*  set var to -1 :-)  */	\
				var <<= 8;				\
			}						\
			var |= Wb;					\
			if (Wi < Wlen-1)				\
				var <<= 8;				\
		}							\
	}

Elf32_Phdr _phdr32[10];
Elf32_Shdr _shdr32[50];
char *_stringtable[10];
int _lenstringtable[10];;
int _numstringtables;

PSP_sceModuleInfo module_info;
PSP_sceStubInfo   stub_info[0x100];
int _numstubs;

static void file_load_elf_phdr32(Elf32_Phdr *_phdr32,FILE *f,int offs)
{
	Elf32_Phdr phdr32;

	fseek(f, offs, SEEK_SET);
	fread(&phdr32, 1, sizeof(Elf32_Phdr), f);

	psp_unencode(_phdr32->p_type,    &phdr32.p_type,    Elf32_Word);
	psp_unencode(_phdr32->p_offset,  &phdr32.p_offset,  Elf32_Off);
	psp_unencode(_phdr32->p_vaddr,   &phdr32.p_vaddr,   Elf32_Addr);
	psp_unencode(_phdr32->p_paddr,   &phdr32.p_paddr,   Elf32_Addr);
	psp_unencode(_phdr32->p_filesz,  &phdr32.p_filesz,  Elf32_Word);
	psp_unencode(_phdr32->p_memsz,   &phdr32.p_memsz,   Elf32_Word);
	psp_unencode(_phdr32->p_flags,   &phdr32.p_flags,   Elf32_Word);
	psp_unencode(_phdr32->p_align,   &phdr32.p_align,   Elf32_Word);
}
static void file_dump_elf_phdr32(Elf32_Phdr *_phdr32)
{
	debug(" p_type  : %08x\n",_phdr32->p_type);
	debug(" p_offset: %08x\n",_phdr32->p_offset);
	debug(" p_vaddr : %08x\n",_phdr32->p_vaddr);
	debug(" p_paddr : %08x\n",_phdr32->p_paddr);
	debug(" p_filesz: %08x\n",_phdr32->p_filesz);
	debug(" p_memsz : %08x\n",_phdr32->p_memsz);
	debug(" p_flags : %08x\n",_phdr32->p_flags);
	debug(" p_align : %08x\n",_phdr32->p_align);
}

static void file_load_elf_shdr32(Elf32_Shdr *_shdr32,FILE *f,int offs)
{
	Elf32_Shdr shdr32;

	debug("reading section header at %016llx\n", offs);

	fseek(f, offs, SEEK_SET);

	if (fread(&shdr32, 1, sizeof(Elf32_Shdr), f) != sizeof(Elf32_Shdr)) 
	{
		fprintf(stderr, "couldn't read section header\n");
		exit(1);
	}
	psp_unencode(_shdr32->sh_name,      &shdr32.sh_name,    Elf32_Word);
	psp_unencode(_shdr32->sh_type,      &shdr32.sh_type,    Elf32_Word);
	psp_unencode(_shdr32->sh_flags,     &shdr32.sh_flags,   Elf32_Word);
	psp_unencode(_shdr32->sh_addr,      &shdr32.sh_addr,    Elf32_Addr);
	psp_unencode(_shdr32->sh_offset,    &shdr32.sh_offset,  Elf32_Off);
	psp_unencode(_shdr32->sh_size,      &shdr32.sh_size,    Elf32_Word);
	psp_unencode(_shdr32->sh_link,      &shdr32.sh_link,    Elf32_Word);
	psp_unencode(_shdr32->sh_info,      &shdr32.sh_info,    Elf32_Word);
	psp_unencode(_shdr32->sh_addralign, &shdr32.sh_addralign,Elf32_Word);
	psp_unencode(_shdr32->sh_entsize,   &shdr32.sh_entsize, Elf32_Word);

}
static void file_dump_elf_shdr32(Elf32_Shdr *_shdr32)
{
	int type_proc,type_os,type_std;

	if(_shdr32==NULL) return;

	type_proc=_shdr32->sh_type&(SHF_MASKPROC);
	type_os=_shdr32->sh_type&(SHF_MASKOS);
	type_std=_shdr32->sh_type&(~(SHF_MASKPROC|SHF_MASKOS));

	debug(" sh_name       : %08x %s\n",_shdr32->sh_name,_stringtable[0]+_shdr32->sh_name);
	debug(" sh_type       : %08x ",_shdr32->sh_type);

	if(type_proc>0)
	{
		debug("SHF_MASKPROC:%08x ",type_proc);
	}
	if(type_os>0)
	{
		debug("SHF_MASKOS:%08x ",type_os);
	}

	if (type_std == SHT_NULL)
	{
		debug("SHT_NULL");
	}
	else if (type_std == SHT_PROGBITS)
	{
		debug("SHT_PROGBITS");
	}
	else if (type_std == SHT_SYMTAB)
	{
		debug("SHT_SYMTAB");
	}
	else if (type_std == SHT_STRTAB)
	{
		debug("SHT_STRTAB");
	}
	else if (type_std == SHT_RELA)
	{
		debug("SHT_RELA");
	}
	else if (type_std == SHT_HASH)
	{
		debug("SHT_HASH");
	}
	else if (type_std == SHT_DYNAMIC)
	{
		debug("SHT_DYNAMIC");
	}
	else if (type_std == SHT_NOTE)
	{
		debug("SHT_NOTE");
	}
	else if (type_std == SHT_NOBITS)
	{
		debug("SHT_NOBITS");
	}
	else if (type_std == SHT_REL)
	{
		debug("SHT_REL");
	}
	else if (type_std == SHT_SHLIB)
	{
		debug("SHT_SHLIB");
	}
	else if (type_std == SHT_DYNSYM)
	{
		debug("SHT_DYNSYM");
	}
	else if (type_std == SHT_NUM)
	{
		debug("SHT_NUM");
	}
	debug("\n");

	debug(" sh_flags      : %08x\n",_shdr32->sh_flags);
	debug(" sh_addr       : %08x\n",_shdr32->sh_addr);
	debug(" sh_offset     : %08x\n",_shdr32->sh_offset);
	debug(" sh_size       : %08x\n",_shdr32->sh_size);
	debug(" sh_link       : %08x\n",_shdr32->sh_link);
	debug(" sh_info       : %08x\n",_shdr32->sh_info);
	debug(" sh_addralign  : %08x\n",_shdr32->sh_addralign);
	debug(" sh_entsize    : %08x\n",_shdr32->sh_entsize);
	debug("\n");
}

static void file_dump_sceModuleInfo(PSP_sceModuleInfo *info)
{
char name[8*4];
int i;
	
	memset(&name[0*4],0,4*8);
	memcpy(&name[0*4],&info->w01,4);
	memcpy(&name[1*4],&info->w02,4);
	memcpy(&name[2*4],&info->w03,4);
	memcpy(&name[3*4],&info->w04,4);
	memcpy(&name[4*4],&info->w05,4);
	memcpy(&name[5*4],&info->w06,4);
	memcpy(&name[6*4],&info->w07,4);

	debug("sceModuleInfo:\n");
	debug(" w00           : %08x\n",info->w00);
	debug(" w01      : %08x\n",info->w01);
	debug(" w02      : %08x\n",info->w02);
	debug(" w03      : %08x\n",info->w03);
	debug(" w04      : %08x\n",info->w04);
	debug(" w05      : %08x\n",info->w05);
	debug(" w06      : %08x\n",info->w06);
	debug(" w07      : %08x\n",info->w07);
	debug(" w01-w07       : %s\n",name);
	debug(" w08           : %08x\n",info->w08);
	debug(" lib_ent       : %08x\n",info->lib_ent);
	debug(" lib_ent_btm   : %08x\n",info->lib_ent_btm);
	debug(" lib_stub      : %08x\n",info->lib_stub);
	debug(" lib_stub_btm  : %08x\n",info->lib_stub_btm);
}

static void file_load_elf(struct machine *m, struct memory *mem,
	char *filename, uint64_t *entrypointp, int arch, uint64_t *gpp,
	int *byte_order, uint64_t *tocp)
{
	Elf32_Ehdr hdr32;
	Elf64_Ehdr hdr64;
	FILE *f;
	uint64_t eentry;
	int len, i,ii, ok;
	int elf64, encoding, eflags;
	int etype, emachine;
	int ephnum, ephentsize, eshnum, eshentsize;
	off_t ephoff, eshoff;
	Elf32_Phdr phdr32;
	Elf32_Shdr shdr32;
	Elf32_Sym sym32;
	int ofs;
	int chunk_len = 1024, align_len;
	char *symbol_strings = NULL; size_t symbol_length = 0;
	char *s;
	Elf32_Sym *symbols_sym32 = NULL;  int n_symbols = 0;

	/*
		try to open file
	*/

        if (m->machine_type != MACHINE_PSP) {
		fprintf(stderr, "This ELF Loader works only for PSP\n");
		exit(1);
	}

	f = fopen(filename, "r");
	if (f == NULL) {
		perror(filename);
		exit(1);
	}

	/*
		read ELF Header
	*/

	len = fread(&hdr32, 1, sizeof(Elf32_Ehdr), f);
	if (len < (signed int)sizeof(Elf32_Ehdr)) {
		fprintf(stderr, "%s: not an ELF file image\n", filename);
		exit(1);
	}

	if (memcmp(&hdr32.e_ident[EI_MAG0], ELFMAG, SELFMAG) != 0) {
		fprintf(stderr, "%s: not an ELF file image\n", filename);
		exit(1);
	}

	switch (hdr32.e_ident[EI_CLASS]) {
	case ELFCLASS32:
		elf64 = 0;
		break;
	default:
		fprintf(stderr, "%s: unknown ELF class '%i'\n",
		    filename, hdr32.e_ident[EI_CLASS]);
		exit(1);
	}

	encoding = hdr32.e_ident[EI_DATA];
	if (encoding != ELFDATA2LSB && encoding != ELFDATA2MSB) {
		fprintf(stderr, "%s: unknown data encoding '%i'\n",
		    filename, hdr32.e_ident[EI_DATA]);
		exit(1);
	}

	/*
		check elf header
	*/

	psp_unencode(etype,      &hdr32.e_type,      Elf32_Half);
	psp_unencode(eflags,     &hdr32.e_flags,     Elf32_Word);
	psp_unencode(emachine,   &hdr32.e_machine,   Elf32_Half);
	psp_unencode(eentry,     &hdr32.e_entry,     Elf32_Addr);
	psp_unencode(ephnum,     &hdr32.e_phnum,     Elf32_Half);
	psp_unencode(ephentsize, &hdr32.e_phentsize, Elf32_Half);
	psp_unencode(ephoff,     &hdr32.e_phoff,     Elf32_Off);
	psp_unencode(eshnum,     &hdr32.e_shnum,     Elf32_Half);
	psp_unencode(eshentsize, &hdr32.e_shentsize, Elf32_Half);
	psp_unencode(eshoff,     &hdr32.e_shoff,     Elf32_Off);
	if (ephentsize != sizeof(Elf32_Phdr)) {
		fprintf(stderr, "%s: incorrect phentsize? %i, should "
		    "be %i\nPerhaps this is a dynamically linked "
		    "binary (which isn't supported yet).\n", filename,
		    (int)ephentsize, (int)sizeof(Elf32_Phdr));
		exit(1);
	}
	if (eshentsize != sizeof(Elf32_Shdr)) {
		fprintf(stderr, "%s: incorrect shentsize? %i, should "
		    "be %i\nPerhaps this is a dynamically linked "
		    "binary (which isn't supported yet).\n", filename,
		    (int)eshentsize, (int)sizeof(Elf32_Shdr));
		exit(1);
	}
	if ( etype != ET_EXEC ) {
		fprintf(stderr, "%s is not an ELF Executable file, type = %i\n",
		    filename, etype);
		exit(1);
	}
	ok = 0;
	switch (arch) {
	case ARCH_MIPS:
		switch (emachine) {
		case EM_MIPS:
		case EM_MIPS_RS3_LE:
			ok = 1;
		}
		break;
	default:
		fatal("file.c: INTERNAL ERROR: Unimplemented arch!\n");
	}

	if (!ok) {
		fprintf(stderr, "%s: this is a ", filename);
		if (emachine >= 0 && emachine < N_ELF_MACHINE_TYPES)
			fprintf(stderr, elf_machine_type[emachine]);
		else
			fprintf(stderr, "machine type '%i'", emachine);
		fprintf(stderr, " ELF binary!\n");
		exit(1);
	}
#ifdef DEBUG_PSP_LOADER
	debug("ELF32, Encoding: %s, entry point: 0x%08x\n",encoding == "LSB (LE)" ,(int)eentry);
#endif

	/*  
		Read the program headers
	*/

	for (i=0; i<ephnum; i++) {
		file_load_elf_phdr32(&_phdr32[i],f,ephoff + i * ephentsize);
#ifdef DEBUG_PSP_LOADER
		debug("Program Header #%d:\n",i);
		file_dump_elf_phdr32(&_phdr32[i]);
#endif
	}

	/* read the data pointed to by the program headers */
	for (i=0; i<ephnum; i++) {
		Elf32_Phdr phdr32;
		int p_type;
		uint64_t p_offset;
		uint64_t p_vaddr;
		uint64_t p_paddr;
		uint64_t p_filesz;
		uint64_t p_memsz;
		int p_flags;
		int p_align;
/*
		fseek(f, ephoff + i * ephentsize, SEEK_SET);

		fread(&phdr32, 1, sizeof(Elf32_Phdr), f);
		psp_unencode(p_type,    &phdr32.p_type,    Elf32_Word);
		psp_unencode(p_offset,  &phdr32.p_offset,  Elf32_Off);
		psp_unencode(p_vaddr,   &phdr32.p_vaddr,   Elf32_Addr);
		psp_unencode(p_paddr,   &phdr32.p_paddr,   Elf32_Addr);
		psp_unencode(p_filesz,  &phdr32.p_filesz,  Elf32_Word);
		psp_unencode(p_memsz,   &phdr32.p_memsz,   Elf32_Word);
		psp_unencode(p_flags,   &phdr32.p_flags,   Elf32_Word);
		psp_unencode(p_align,   &phdr32.p_align,   Elf32_Word);
*/
		memcpy(&phdr32,&_phdr32[i], sizeof(Elf32_Phdr));
		p_type=phdr32.p_type;
		p_offset=phdr32.p_offset;
		p_vaddr=phdr32.p_vaddr;
		p_paddr=phdr32.p_paddr;
		p_filesz=phdr32.p_filesz;
		p_memsz=phdr32.p_memsz;
		p_flags=phdr32.p_flags;
		p_align=phdr32.p_align;

		if (p_memsz != 0 && (p_type == PT_LOAD ||
		    (p_type & PF_MASKPROC) == PT_MIPS_REGINFO)) {
#ifdef DEBUG_PSP_LOADER
			debug("chunk %i (", i);
			if (p_type == PT_LOAD)
				debug("load");
			else
				debug("0x%08x", (int)p_type);

			debug(") @ 0x%llx, vaddr 0x", (long long)p_offset);

			debug("%08x", (int)p_vaddr);

			debug(" len=0x%llx\n", (long long)p_memsz);
#endif
			if (p_vaddr != p_paddr) {
				fatal("NOTE: vaddr (0x%08x) and "
				    "paddr (0x%08x) differ; using vaddr"
				    "\n", (int)p_vaddr, (int)p_paddr);
			}

			if (p_memsz < p_filesz) {
				fprintf(stderr, "%s: memsz < filesz. TODO: how"
				    " to handle this? memsz=%016llx filesz="
				    "%016llx\n", filename, (long long)p_memsz,
				    (long long)p_filesz);
				exit(1);
			}

			fseek(f, p_offset, SEEK_SET);
			align_len = 1;
			if ((p_vaddr & 0xf)==0)		align_len = 0x10;
			if ((p_vaddr & 0x3f)==0)	align_len = 0x40;
			if ((p_vaddr & 0xff)==0)	align_len = 0x100;
			if ((p_vaddr & 0xfff)==0)	align_len = 0x1000;
			if ((p_vaddr & 0x3fff)==0)	align_len = 0x4000;
			if ((p_vaddr & 0xffff)==0)	align_len = 0x10000;
			ofs = 0;  len = chunk_len = align_len;
			while (ofs < (int64_t)p_filesz && len==chunk_len) {
				unsigned char *ch = malloc(chunk_len);
				int i = 0;

				/*  Switch to larger size, if possible:  */
				if (align_len < 0x10000 &&
				    ((p_vaddr + ofs) & 0xffff)==0) {
					align_len = 0x10000;
					len = chunk_len = align_len;
					free(ch);
					ch = malloc(chunk_len);
				} else if (align_len < 0x1000 &&
				    ((p_vaddr + ofs) & 0xfff)==0) {
					align_len = 0x1000;
					len = chunk_len = align_len;
					free(ch);
					ch = malloc(chunk_len);
				}

				if (ch == NULL) {
					fprintf(stderr, "out of memory\n");
					exit(1);
				}

				len = fread(&ch[0], 1, chunk_len, f);
				if (ofs + len > (int64_t)p_filesz)
					len = p_filesz - ofs;

				while (i < len) {
					size_t len_to_copy;
					len_to_copy = (i + align_len) <= len?
					    align_len : len - i;
					m->cpus[0]->memory_rw(m->cpus[0], mem,
					    p_vaddr + ofs, &ch[i], len_to_copy,
					    MEM_WRITE, NO_EXCEPTIONS);
					ofs += align_len;
					i += align_len;
				}

				free(ch);
			}
		}
	}

	/*  
		Read the section headers:  
	*/

	for (i=0; i<eshnum; i++) {
		file_load_elf_shdr32(&_shdr32[i],f,eshoff + i * eshentsize);
	}

	/*
	 *  Read the section headers to find the address of the _gp
	 *  symbol (for MIPS):
	 */

	/* read string tables */
#ifdef DEBUG_PSP_LOADER
	debug("reading stringtables\n");
#endif
	_numstringtables=0;
	for (i=0; i<eshnum; i++) {
		
		if (_shdr32[i].sh_type == SHT_STRTAB) {
			size_t len;
			char *symbol_strings;
/*
			if (symbol_strings != NULL)
				free(symbol_strings);
*/
			symbol_strings = malloc(_shdr32[i].sh_size + 1);
#ifdef DEBUG_PSP_LOADER
			debug("%i bytes of symbol strings malloced at 0x%llx\n",
			    (int)_shdr32[i].sh_size + 1, symbol_strings);
#endif
			if (symbol_strings == NULL) {
				fprintf(stderr, "out of memory\n");
				exit(1);
			}

			fseek(f, _shdr32[i].sh_offset, SEEK_SET);
			len = fread(symbol_strings, 1, _shdr32[i].sh_size, f);
			if (len != _shdr32[i].sh_size) {
				fprintf(stderr, "could not read symbols from "
				    "%s\n", filename);
				exit(1);
			}

#ifdef DEBUG_PSP_LOADER
			debug("%i bytes of symbol strings at 0x%llx\n",
			    (int)_shdr32[i].sh_size, (long long)_shdr32[i].sh_offset);
#endif 
			symbol_strings[_shdr32[i].sh_size] = '\0';
//			symbol_length = _shdr32[i].sh_size;
			_stringtable[_numstringtables]=symbol_strings;
			_lenstringtable[_numstringtables]=_shdr32[i].sh_size;
#ifdef DEBUG_PSP_LOADER
			debug("table #%i of symbol strings malloced at 0x%llx, len 0x%08x\n",
			    _numstringtables+1, 
			    _stringtable[_numstringtables],
                            _lenstringtable[_numstringtables]);
#endif
			_numstringtables++;
		}
	}
//exit(-1);

	/* 
		read the symbol table 
	*/
#ifdef DEBUG_PSP_LOADER
	debug("reading symtab\n");
#endif

	for (i=0; i<eshnum; i++) {
		int sh_name, sh_type, sh_flags, sh_link, sh_info, sh_entsize;
		uint64_t sh_addr, sh_size, sh_addralign;
		off_t sh_offset;
		int n_entries;	/*  for reading the symbol / string tables  */
		Elf32_Shdr shdr32;

		memcpy(&shdr32,&_shdr32[i], sizeof(Elf32_Shdr));	
		sh_name=shdr32.sh_name;
		sh_type=shdr32.sh_type;
		sh_flags=shdr32.sh_flags;
		sh_addr=shdr32.sh_addr;
		sh_offset=shdr32.sh_offset;
		sh_size=shdr32.sh_size;
		sh_link=shdr32.sh_link;
		sh_info=shdr32.sh_info;
		sh_addralign=shdr32.sh_addralign;
		sh_entsize=shdr32.sh_entsize;

		/*  debug("section header %i at %016llx\n", i,
		    (long long) eshoff+i*eshentsize);  */

		/*  debug("sh_name=%04lx, sh_type=%08lx, sh_flags=%08lx"
		    " sh_size=%06lx sh_entsize=%03lx\n",
		    (long)sh_name, (long)sh_type, (long)sh_flags,
		    (long)sh_size, (long)sh_entsize);  */

		/*  Perhaps it is bad to reuse sh_entsize like this?  TODO  */
		sh_entsize = sizeof(Elf32_Sym);

		if (sh_type == SHT_SYMTAB) {
			size_t len;
			n_entries = sh_size / sh_entsize;

#ifdef DEBUG_PSP_LOADER
			debug("%i symbol entries at 0x%llx\n",
			    (int)n_entries, (long long)sh_offset);
#endif

			fseek(f, sh_offset, SEEK_SET);

			if (symbols_sym32 != NULL)
				free(symbols_sym32);
			symbols_sym32 = malloc(sh_size);
			if (symbols_sym32 == NULL) {
				fprintf(stderr, "out of memory\n");
				exit(1);
			}

			len = fread(symbols_sym32, 1,
			    sh_entsize * n_entries, f);

			if (len != sh_size) {
				fprintf(stderr, "could not read symbols from "
				    "%s\n", filename);
				exit(1);
			}

			n_symbols = n_entries;
		}

	}

//exit(-1);
{
char *symbol_strings=_stringtable[1];
#ifdef DEBUG_PSP_LOADER
	debug("decoding symtab %08x %08x\n",symbol_strings,_stringtable[1]);
#endif
	/*  Decode symbols:  */
	if (symbol_strings != NULL) {
		for (i=0; i<n_symbols; i++) {
			uint64_t st_name, addr, size;
			int st_info;

			sym32 = symbols_sym32[i];
			psp_unencode(st_name, &sym32.st_name,  Elf32_Word);
			psp_unencode(st_info, &sym32.st_info,  Elf_Byte);
			psp_unencode(addr,    &sym32.st_value, Elf32_Word);
			psp_unencode(size,    &sym32.st_size, Elf32_Word);
#if 0 // DEBUG_PSP_LOADER
			  debug("symbol info=0x%02x addr=0x%016llx"
			    " (%i) '%s'\n", st_info, (long long)addr,
			    st_name, symbol_strings + st_name);  
#endif
			if (size == 0)
				size ++;

			if (addr != 0) /* && ((st_info >> 4) & 0xf)
			    >= STB_GLOBAL) */ {
#ifdef DEBUG_PSP_LOADER
				  debug("symbol info=0x%02x addr=0x%016llx"
				    " '%s'\n", st_info, (long long)addr,
				    symbol_strings + st_name);  
#endif
#if 1
				add_symbol_name(&m->symbol_context,
				    addr, size, symbol_strings + st_name,
				    0, -1);
#endif
			}
#if 1

			if (strcmp(symbol_strings + st_name, "_gp") == 0) {
				debug("found _gp address: 0x");
					debug("%08x\n", (int)addr);
				*gpp = addr;
			}
#endif
		}
	}
}

	/*  Dump the section headers:  */
#ifdef DEBUG_PSP_LOADER
	for (i=0; i<eshnum; i++) {
		debug("Section Header #%d\n",i);
		file_dump_elf_shdr32(&_shdr32[i]);
	}
#endif

	/*
		read sceModuleInfo
	*/

#ifdef DEBUG_PSP_LOADER
	debug("reading .rodata.sceModuleInfo\n");
#endif 
	for (i=0; i<eshnum; i++) 
	{
//fprintf(stderr,">%s\n",_stringtable[0]+_shdr32[i].sh_name);
		if(!strcmp(".rodata.sceModuleInfo",_stringtable[0]+_shdr32[i].sh_name))
		{ 
			fseek(f, _shdr32[i].sh_offset, SEEK_SET);
			fread(&module_info, 1, sizeof(PSP_sceModuleInfo), f);
			file_dump_sceModuleInfo(&module_info);

			
			debug("found _gp address: 0x");
				debug("%08x\n", (int)module_info.w08);
			*gpp = module_info.w08;

			break;
		}
		if(!strcmp(".xodata.sceModuleInfo",_stringtable[0]+_shdr32[i].sh_name))
		{ 
			fseek(f, _shdr32[i].sh_offset, SEEK_SET);
			fread(&module_info, 1, sizeof(PSP_sceModuleInfo), f);
			file_dump_sceModuleInfo(&module_info);

			
			debug("found _gp address: 0x");
				debug("%08x\n", (int)module_info.w08);
			*gpp = module_info.w08;

			break;
		}
	}

#ifdef DEBUG_PSP_LOADER
	debug("reading .lib.stub\n");
#endif
	_numstubs=0;

	for (i=0; i<eshnum; i++) 
	{
		if(!strcmp(".lib.stub",_stringtable[0]+_shdr32[i].sh_name))
		{ 
			fseek(f, _shdr32[i].sh_offset, SEEK_SET);
			for(ii=0;ii<(_shdr32[i].sh_size/sizeof(PSP_sceStubInfo));ii++)
			{
			fread(&stub_info[_numstubs], 1, sizeof(PSP_sceStubInfo), f);
			_numstubs++;
			}
		}
	}

#ifdef DEBUG_PSP_LOADER
	debug("clearing bss\n");
#endif
	for (i=0; i<eshnum; i++) 
	{
		if(!strcmp(".bss",_stringtable[0]+_shdr32[i].sh_name))
		{ 
			int zeroint=0;
			for(ii=0;ii<(_shdr32[i].sh_size);ii++)
			{
				m->cpus[0]->memory_rw(m->cpus[0], mem,
			    		(_shdr32[i].sh_addr+ii), &zeroint, 1,MEM_WRITE, NO_EXCEPTIONS);
			}
		}
	}

	/*
		fix up the syscall stubs
	*/

#ifdef DEBUG_PSP_LOADER
	debug("fixing up .sceStub.text\n");
#endif
/*
	for (i=0; i<eshnum; i++) 
	{
		if(!strcmp(".sceStub.text",_stringtable[0]+_shdr32[i].sh_name))
		{ 
fprintf(stderr,".sceStub.text at: %08x offs: %08x size: %08x\n",_shdr32[i].sh_addr,_shdr32[i].sh_offset,_shdr32[i].sh_size);
			break;
		}
	}
*/
	for (i=0; i<_numstubs; i++) 
	{
		int32_t offs=stub_info[i].stub;
		int32_t num =(stub_info[i].w02>>16)&0x0000ffff;
		int32_t nids=stub_info[i].nidtable;

		int32_t syscall=0x0000000c;
		int32_t thisnid=0xdeadbeaf;

#ifdef DEBUG_PSP_LOADER
		debug("fixing up %d stubs at %08x (nids at:%08x)\n",num,offs,nids);
#endif
		for (ii=0; ii<num; ii++) 
		{

			// read nid from memory
			m->cpus[0]->memory_rw(m->cpus[0], mem,
			    nids, &thisnid, 4,
			    MEM_READ, NO_EXCEPTIONS);

			syscall=PSP_findsyscall_bynid(thisnid);

#ifdef DEBUG_PSP_LOADER
			debug("fixing up stub #%d at %08x (nid:%08x, name:%s)\n",ii,offs,thisnid,PSP_syscall_getname(syscall));
#endif
			syscall<<=6;	// shift up to fit into opcode
			syscall|=0x0c;  // syscall opcode

			// write syscall to memory
			m->cpus[0]->memory_rw(m->cpus[0], mem,
			    offs+4, &syscall, 4,
			    MEM_WRITE, NO_EXCEPTIONS);


		offs+=sizeof(PSP_Stub);
		nids+=4;
		}
	}

	*entrypointp = eentry;
	*byte_order = EMUL_LITTLE_ENDIAN;

  	/* load elf (prx) using kernel */
        {
                /*
                 *  sceKernelLoadExecBufferPlain v1.0
                 *  func  nid:0x71A1D738  addr 88064AB4 
                 */
              m->cpus[0]->cd.mips.gpr[MIPS_GPR_A0] = 1; 
              m->cpus[0]->cd.mips.gpr[MIPS_GPR_A1] = *entrypointp; 
              m->cpus[0]->cd.mips.gpr[MIPS_GPR_A2] = 0; 
//              m->cpus[0]->cd.mips.gpr[MIPS_GPR_RA] = *entrypointp; 
//	      m->cpus[0]->cd.mips.coproc[0]->reg[22] = *entrypointp; 
//	      m->cpus[0]->cd.mips.coproc[0]->reg[COP0_EPC] = *entrypointp; // somethings gotta give
//              *entrypointp = 0x88064ab4;  // perhaps we can just exec using the loaded kernel
//		*entrypointp = 0xbfc00000;	// boot.bin

//		*entrypointp = 0x08900334; // elf_template main thread
//		*entrypointp = 0x0890011c; // 

        }

	fclose(f);

//	exit(1);

	n_executables_loaded ++;

}
/***************************************************************************************************************
	PSP Specific END
***************************************************************************************************************/

/*
 *  file_n_executables_loaded():
 *
 *  Returns the number of executable files loaded into emulated memory.
 */
int file_n_executables_loaded(void)
{
	return n_executables_loaded;
}


/*
 *  file_load():
 *
 *  Sense the file format of a file (ELF, a.out, ecoff), and call the
 *  right file_load_XXX() function.  If the file isn't of a recognized
 *  binary format, assume that it contains symbol definitions.
 *
 *  If the filename doesn't exist, try to treat the name as
 *   "address:filename" and load the file as a raw binary.
 */
void file_load(struct machine *machine, struct memory *mem,
	char *filename, uint64_t *entrypointp,
	int arch, uint64_t *gpp, int *byte_orderp, uint64_t *tocp)
{
	int iadd = DEBUG_INDENTATION, old_quiet_mode;
	FILE *f;
	unsigned char buf[12];
	unsigned char buf2[2];
	size_t len, len2, i;
	off_t size;

	if (byte_orderp == NULL) {
		fprintf(stderr, "file_load(): byte_order == NULL\n");
		exit(1);
	}

	if (arch == ARCH_NOARCH) {
		fprintf(stderr, "file_load(): FATAL ERROR: no arch?\n");
		exit(1);
	}

	if (mem == NULL || filename == NULL) {
		fprintf(stderr, "file_load(): mem or filename is NULL\n");
		exit(1);
	}

	/*  Skip configuration files:  */
	if (filename[0] == '@')
		return;

	debug("loading %s%s\n", filename, verbose >= 2? ":" : "");
	debug_indentation(iadd);

	old_quiet_mode = quiet_mode;
	if (verbose < 2)
		quiet_mode = 1;

	f = fopen(filename, "r");
	if (f == NULL) {
		file_load_raw(machine, mem, filename, entrypointp);
		goto ret;
	}

	fseek(f, 0, SEEK_END);
	size = ftello(f);
	fseek(f, 0, SEEK_SET);

	memset(buf, 0, sizeof(buf));
	len = fread(buf, 1, sizeof(buf), f);
	fseek(f, 510, SEEK_SET);
	len2 = fread(buf2, 1, sizeof(buf2), f);
	fclose(f);

	if (len < (signed int)sizeof(buf)) {
		fprintf(stderr, "\nThis file is too small to contain "
		    "anything useful\n");
		exit(1);
	}

	/*  Is it an ELF?  */
	if (buf[0] == 0x7f && buf[1]=='E' && buf[2]=='L' && buf[3]=='F') {
		file_load_elf(machine, mem, filename,
		    entrypointp, arch, gpp, byte_orderp, tocp);
		goto ret;
	}

	/*  gzipped files are not supported:  */
	if (buf[0]==0x1f && buf[1]==0x8b) {
		fprintf(stderr, "\nYou need to gunzip the file before you"
		    " try to use it.\n");
		exit(1);
	}

	if (size > 24000000) {
		fprintf(stderr, "\nThis file is very large (%lli bytes)\n",
		    (long long)size);
		fprintf(stderr, "Are you sure it is a kernel and not a disk "
		    "image? (Use the -d option.)\n");
		exit(1);
	}

	if (size == 1474560)
		fprintf(stderr, "Hm... this file is the size of a 1.44 MB "
		    "floppy image. Maybe you forgot the\n-d switch?\n");

	/*
	 *  Last resort:  symbol definitions from nm (or nm -S):
	 *
	 *  If the buf contains typical 'binary' characters, then print
	 *  an error message and quit instead of assuming that it is a
	 *  symbol file.
	 */
	for (i=0; i<(signed)sizeof(buf); i++)
		if (buf[i] < 32 && buf[i] != '\t' &&
		    buf[i] != '\n' && buf[i] != '\r' &&
		    buf[i] != '\f') {
			fprintf(stderr, "\nThe file format of '%s' is "
			    "unknown.\n\n ", filename);
			for (i=0; i<(signed)sizeof(buf); i++)
				fprintf(stderr, " %02x", buf[i]);

			if (len2 == 2 && buf2[0] == 0x55 && buf2[1] == 0xaa)
				fprintf(stderr, "\n\nIt has a PC-style "
				    "bootsector marker.");

			fprintf(stderr, "\n\nPossible explanations:\n\n"
			    "  o)  If this is a disk image, you forgot '-d' "
			    "on the command line.\n"
			    "  o)  This is an unsupported binary format.\n\n");
			exit(1);
		}

	symbol_readfile(&machine->symbol_context, filename);

ret:
	debug_indentation(-iadd);
	quiet_mode = old_quiet_mode;
}

