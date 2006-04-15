/*
 * PSPLINK
 * -----------------------------------------------------------------------
 * Licensed under the BSD license, see LICENSE in PSPLINK root for details.
 *
 * main.c - Main code for PC side of USB HostFS
 *
 * Copyright (c) 2006 James F <tyranid@gmail.com>
 *
 * $HeadURL$
 * $Id$
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <usb.h>
#include <limits.h>
#include <fcntl.h>
#include <usbhostfs.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <utime.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "psp_fileio.h"

#define MAX_FILES 256
#define MAX_DIRS  256
#define MAX_TOKENS 256

#define BASE_PORT 10000

#ifdef __CYGWIN__
#define USB_TIMEOUT 1000
#else
#define USB_TIMEOUT 0
#endif

/* TODO: Make the response encode the errno so newlib handles it correctly
 * i.e. setting 0x8001<errno>
 */

#define MAX_HOSTDRIVES 8

/* Contains the paths for a single hist drive */
struct HostDrive
{
	char rootdir[PATH_MAX];
	char currdir[PATH_MAX];
};

struct FileHandle
{
	int opened;
	int mode;
	char *name;
};

struct DirHandle
{
	int opened;
	/* Current count of entries left */
	int count;
	/* Current position in the directory entries */
	int pos;
	/* Head of list, each entry will be freed when read */
	SceIoDirent *pDir;
};

struct FileHandle open_files[MAX_FILES];
struct DirHandle  open_dirs[MAX_DIRS];

static usb_dev_handle *g_hDev = NULL;
static int g_shellserv = -1;
static int g_shellsock = -1;
static int g_gdbserv = -1;
static int g_gdbsock = -1;
static const char *g_mapfile = NULL;

pthread_mutex_t g_drivemtx = PTHREAD_MUTEX_INITIALIZER;
struct HostDrive g_drives[MAX_HOSTDRIVES];
char g_rootdir[PATH_MAX];

int  g_verbose = 0;
int  g_gdbdebug = 0;
int  g_nocase = 0;
int  g_pid = HOSTFSDRIVER_PID;
unsigned short g_shellport = BASE_PORT;
unsigned short g_gdbport = BASE_PORT+1;

#define V_PRINTF(level, fmt, ...) { if(g_verbose >= level) { fprintf(stderr, fmt, ## __VA_ARGS__); } }

#if defined BUILD_BIGENDIAN || defined _BIG_ENDIAN
uint16_t swap16(uint16_t i)
{
	uint8_t *p = (uint8_t *) &i;
	uint16_t ret;

	ret = (p[1] << 8) | p[0];

	return ret;
}

uint32_t swap32(uint32_t i)
{
	uint8_t *p = (uint8_t *) &i;
	uint32_t ret;

	ret = (p[3] << 24) | (p[2] << 16) | (p[1] << 8) | p[0];

	return ret;
}

uint64_t swap64(uint64_t i)
{
	uint8_t *p = (uint8_t *) &i;
	uint64_t ret;

	ret = (uint64_t) p[0] | ((uint64_t) p[1] << 8) | ((uint64_t) p[2] << 16) | ((uint64_t) p[3] << 24) 
		| ((uint64_t) p[4] << 32) | ((uint64_t) p[5] << 40) | ((uint64_t) p[6] << 48) | ((uint64_t) p[7] << 56);

	return ret;
}
#define LE16(x) swap16(x)
#define LE32(x) swap32(x)
#define LE64(x) swap64(x)
#else
#define LE16(x) (x)
#define LE32(x) (x)
#define LE64(x) (x)
#endif

void print_gdbdebug(int dir, const uint8_t *data, int len)
{
	int i;

	if(dir)
	{
		printf("HOST->GDB (");
	}
	else
	{
		printf("GDB->HOST (");
	}

	for(i = 0; i < len; i++)
	{
		if(data[i] >= 32)
		{
			putchar(data[i]);
		}
		else
		{
			printf("\\%02x", data[i]);
		}
	}

	printf(")\n");
}

/* Define wrappers for the usb functions we use which can set euid */
int euid_usb_bulk_write(usb_dev_handle *dev, int ep, char *bytes, int size,
	int timeout)
{
	int ret;

	seteuid(0);
	setegid(0);
	ret = usb_bulk_write(dev, ep, bytes, size, timeout);
	seteuid(getuid());
	setegid(getgid());

	return ret;
}

int euid_usb_bulk_read(usb_dev_handle *dev, int ep, char *bytes, int size,
	int timeout)
{
	int ret;

	seteuid(0);
	setegid(0);
	ret = usb_bulk_read(dev, ep, bytes, size, timeout);
	seteuid(getuid());
	setegid(getgid());

	return ret;
}

usb_dev_handle *open_device(struct usb_bus *busses)
{
	struct usb_bus *bus = NULL;
	struct usb_dev_handle *hDev = NULL;

	seteuid(0);
	setegid(0);

	for(bus = busses; bus; bus = bus->next) 
	{
		struct usb_device *dev;

		for(dev = bus->devices; dev; dev = dev->next)
		{
			if((dev->descriptor.idVendor == SONY_VID) 
				&& (dev->descriptor.idProduct == g_pid))
			{
				hDev = usb_open(dev);
				if(hDev != NULL)
				{
					int ret;
					ret = usb_set_configuration(hDev, 1);
					if(ret == 0)
					{
						ret = usb_claim_interface(hDev, 0);
						if(ret == 0)
						{
							seteuid(getuid());
							setegid(getgid());
							return hDev;
						}
						else
						{
							usb_close(hDev);
							hDev = NULL;
						}
					}
					else
					{
						usb_close(hDev);
						hDev = NULL;
					}
				}
			}
		}
	}
	
	if(hDev)
	{
		usb_close(hDev);
	}

	seteuid(getuid());
	setegid(getgid());

	return NULL;
}

void close_device(struct usb_dev_handle *hDev)
{
	seteuid(0);
	setegid(0);
	if(hDev)
	{
		usb_release_interface(hDev, 0);
		usb_reset(hDev);
		usb_close(hDev);
	}
	seteuid(getuid());
	setegid(getgid());
}

int gen_path(char *path, int dir)
{
	char abspath[PATH_MAX];
	const char *tokens[MAX_TOKENS];
	const char *outtokens[MAX_TOKENS];
	int count;
	int token;
	int pathpos;

	strcpy(abspath, path);
	count = 0;
	tokens[0] = strtok(abspath, "/");
	while((tokens[count]) && (count < (MAX_TOKENS-1)))
	{
		tokens[++count] = strtok(NULL, "/");
	}

	/* Remove any single . and .. */
	pathpos = 0;
	for(token = 0; token < count; token++)
	{
		if(strcmp(tokens[token], ".") == 0)
		{
			/* Do nothing */
		}
		else if(strcmp(tokens[token], "..") == 0)
		{
			/* Decrement the path position if > 0 */
			if(pathpos > 0)
			{
				pathpos--;
			}
		}
		else
		{
			outtokens[pathpos++] = tokens[token];
		}
	}

	strcpy(path, "/");
	for(token = 0; token < pathpos; token++)
	{
		strcat(path, outtokens[token]);
		if((dir) || (token < (pathpos-1)))
		{
			strcat(path, "/");
		}
	}

	return 1;
}

int calc_rating(const char *str1, const char *str2)
{
	int rating = 0;

	while((*str1) && (*str2))
	{
		if(*str1 == *str2)
		{
			rating++;
		}
		str1++;
		str2++;
	}

	return rating;
}

/* Scan the directory, return the first name which matches case insensitive */
int find_nocase(const char *rootdir, const char *relpath, char *token)
{
	DIR *dir;
	struct dirent *ent;
	char abspath[PATH_MAX];
	char match[PATH_MAX];
	int len;
	int rating = -1;
	int ret = 0;

	V_PRINTF(2, "Finding token %s\n", token);

	len = snprintf(abspath, PATH_MAX, "%s%s", rootdir, relpath);
	if((len < 0) || (len > PATH_MAX))
	{
		return 0;
	}

	V_PRINTF(2, "Checking %s\n", abspath);
	dir = opendir(abspath);
	if(dir != NULL)
	{
		V_PRINTF(2, "Opened directory\n");
		while((ent = readdir(dir)))
		{
			V_PRINTF(2, "Got dir entry %p->%s\n", ent, ent->d_name);
			if(strcasecmp(ent->d_name, token) == 0)
			{
				int tmp;

				tmp = calc_rating(token, ent->d_name);
				V_PRINTF(2, "Found match %s for %s rating %d\n", ent->d_name, token, tmp);
				if(tmp > rating)
				{
					strcpy(match, ent->d_name);
					rating = tmp;
				}

				ret = 1;
			}
		}

		closedir(dir);
	}
	else
	{
		V_PRINTF(2, "Couldn't open %s\n", abspath);
	}

	if(ret)
	{
		strcpy(token, match);
	}

	return ret;
}

/* Make a relative path case insensitive, if we fail then leave the path as is, just in case */
void make_nocase(const char *rootdir, char *path, int dir)
{
	char abspath[PATH_MAX];
	char retpath[PATH_MAX];
	char *tokens[MAX_TOKENS];
	int count;
	int token;

	strcpy(abspath, path);
	count = 0;
	tokens[0] = strtok(abspath, "/");
	while((tokens[count]) && (count < (MAX_TOKENS-1)))
	{
		tokens[++count] = strtok(NULL, "/");
	}

	strcpy(retpath, "/");
	for(token = 0; token < count; token++)
	{
		if(!find_nocase(rootdir, retpath, tokens[token]))
		{
			/* Might only be an error if this is not the last token, otherwise we could be
			 * trying to create a new directory or file, if we are not then the rest of the code
			 * will handle the error */
			if((token < (count-1)))
			{
				break;
			}
		}

		strcat(retpath, tokens[token]);
		if((dir) || (token < (count-1)))
		{
			strcat(retpath, "/");
		}
	}

	if(token == count)
	{
		strcpy(path, retpath);
	}
}

int make_path(unsigned int drive, const char *path, char *retpath, int dir)
{
	char hostpath[PATH_MAX];
	int len;
	int ret = -1;

	if(drive >= MAX_HOSTDRIVES)
	{
		fprintf(stderr, "Host drive number is too large (%d)\n", drive);
		return -1;
	}

	if(pthread_mutex_lock(&g_drivemtx))
	{
		fprintf(stderr, "Could not lock mutex (%s)\n", strerror(errno));
		return -1;
	}

	do
	{

		len = snprintf(hostpath, PATH_MAX, "%s%s", g_drives[drive].currdir, path);
		if((len < 0) || (len >= PATH_MAX))
		{
			fprintf(stderr, "Path length too big (%d)\n", len);
			break;
		}

		if(gen_path(hostpath, dir) == 0)
		{
			break;
		}

		/* Make the relative path case insensitive if needed */
		if(g_nocase)
		{
			make_nocase(g_drives[drive].rootdir, hostpath, dir);
		}

		len = snprintf(retpath, PATH_MAX, "%s/%s", g_drives[drive].rootdir, hostpath);
		if((len < 0) || (len >= PATH_MAX))
		{
			fprintf(stderr, "Path length too big (%d)\n", len);
			break;
		}

		if(gen_path(retpath, dir) == 0)
		{
			break;
		}

		ret = 0;
	}
	while(0);

	pthread_mutex_unlock(&g_drivemtx);

	return ret;
}

int open_file(int drive, const char *path, unsigned int mode, unsigned int mask)
{
	char fullpath[PATH_MAX];
	unsigned int real_mode = 0;
	int fd = -1;
	
	if(make_path(drive, path, fullpath, 0) < 0)
	{
		V_PRINTF(1, "Invalid file path %s\n", path);
		return -1;
	}

	V_PRINTF(2, "open: %s\n", fullpath);
	V_PRINTF(1, "Opening file %s\n", fullpath);

	if((mode & PSP_O_RDWR) == PSP_O_RDWR)
	{
		V_PRINTF(2, "Read/Write mode\n");
		real_mode = O_RDWR;
	}
	else
	{
		if(mode & PSP_O_RDONLY)
		{
			V_PRINTF(2, "Read mode\n");
			real_mode = O_RDONLY;
		}
		else if(mode & PSP_O_WRONLY)
		{
			V_PRINTF(2, "Write mode\n");
			real_mode = O_WRONLY;
		}
		else
		{
			fprintf(stderr, "No access mode specified\n");
			return -1;
		}
	}

	if(mode & PSP_O_APPEND)
	{
		real_mode |= O_APPEND;
	}

	if(mode & PSP_O_CREAT)
	{
		real_mode |= O_CREAT;
	}

	if(mode & PSP_O_TRUNC)
	{
		real_mode |= O_TRUNC;
	}

	if(mode & PSP_O_EXCL)
	{
		real_mode |= O_EXCL;
	}

	fd = open(fullpath, real_mode, mask & ~0111);
	if(fd >= 0)
	{
		if(fd < MAX_FILES)
		{
			open_files[fd].opened = 1;
			open_files[fd].mode = mode;
			open_files[fd].name = strdup(fullpath);
		}
		else
		{
			close(fd);
			fprintf(stderr, "Error filedescriptor out of range\n");
			fd = -1;
		}
	}
	else
	{
		V_PRINTF(1, "Could not open file %s\n", fullpath);
	}

	return fd;
}

void fill_time(time_t t, ScePspDateTime *scetime)
{
	struct tm *filetime;

	memset(scetime, 0, sizeof(*scetime));
	filetime = localtime(&t);
	scetime->year = LE16(filetime->tm_year + 1900);
	scetime->month = LE16(filetime->tm_mon + 1);
	scetime->day = LE16(filetime->tm_mday);
	scetime->hour = LE16(filetime->tm_hour);
	scetime->minute = LE16(filetime->tm_min);
	scetime->second = LE16(filetime->tm_sec);
}

int fill_stat(const char *dirname, const char *name, SceIoStat *scestat)
{
	char path[PATH_MAX];
	struct stat st;
	int len;

	/* If dirname is NULL then name is a preconverted path */
	if(dirname != NULL)
	{
		if(dirname[strlen(dirname)-1] == '/')
		{
			len = snprintf(path, PATH_MAX, "%s%s", dirname, name);
		}
		else
		{
			len = snprintf(path, PATH_MAX, "%s/%s", dirname, name);
		}
		if((len < 0) || (len > PATH_MAX))
		{
			fprintf(stderr, "Couldn't fill in directory name\n");
			return -1;
		}
	}
	else
	{
		strcpy(path, name);
	}

	if(stat(path, &st) < 0)
	{
		fprintf(stderr, "Couldn't stat file %s (%s)\n", path, strerror(errno));
		return -1;
	}

	scestat->size = LE64(st.st_size);
	scestat->mode = 0;
	scestat->attr = 0;
	if(S_ISLNK(st.st_mode))
	{
		scestat->attr = LE32(FIO_SO_IFLNK);
		scestat->mode = LE32(FIO_S_IFLNK);
	}
	else if(S_ISDIR(st.st_mode))
	{
		scestat->attr = LE32(FIO_SO_IFDIR);
		scestat->mode = LE32(FIO_S_IFDIR);
	}
	else
	{
		scestat->attr = LE32(FIO_SO_IFREG);
		scestat->mode = LE32(FIO_S_IFREG);
	}

	scestat->mode |= LE32(st.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO));

	fill_time(st.st_ctime, &scestat->ctime);
	fill_time(st.st_atime, &scestat->atime);
	fill_time(st.st_mtime, &scestat->mtime);

	return 0;
}

int dir_open(int drive, const char *dirname)
{
	char fulldir[PATH_MAX];
	struct dirent **entries;
	int ret = -1;
	int i;
	int did;
	int dirnum;

	do
	{
		for(did = 0; did < MAX_DIRS; did++)
		{
			if(!open_dirs[did].opened)
			{
				break;
			}
		}

		if(did == MAX_DIRS)
		{
			fprintf(stderr, "Could not find free directory handle\n");
			break;
		}

		if(make_path(drive, dirname, fulldir, 1) < 0)
		{
			break;
		}

		V_PRINTF(2, "dopen: %s, fsnum %d\n", fulldir, drive);
		V_PRINTF(1, "Opening directory %s\n", fulldir);

		memset(&open_dirs[did], 0, sizeof(open_dirs[did]));

		dirnum = scandir(fulldir, &entries, NULL, alphasort);
		if(dirnum <= 0)
		{
			fprintf(stderr, "Could not scan directory %s (%s)\n", fulldir, strerror(errno));
			break;
		}

		V_PRINTF(2, "Number of dir entries %d\n", dirnum);

		open_dirs[did].pDir = malloc(sizeof(SceIoDirent) * dirnum);
		if(open_dirs[did].pDir != NULL)
		{
			memset(open_dirs[did].pDir, 0, sizeof(SceIoDirent) * dirnum);
			for(i = 0; i < dirnum; i++)
			{
				strcpy(open_dirs[did].pDir[i].name, entries[i]->d_name);
				V_PRINTF(2, "Dirent %d: %s\n", i, entries[i]->d_name);
				if(fill_stat(fulldir, entries[i]->d_name, &open_dirs[did].pDir[i].stat) < 0)
				{
					fprintf(stderr, "Error filling in directory structure\n");
					break;
				}
			}

			if(i == dirnum)
			{
				ret = did;
				open_dirs[did].pos = 0;
				open_dirs[did].count = dirnum;
				open_dirs[did].opened = 1;
			}
			else
			{
				free(open_dirs[did].pDir);
			}
		}
		else
		{
			fprintf(stderr, "Could not allocate memory for directories\n");
		}

		if(ret < 0)
		{
			for(i = 0; i < dirnum; i++)
			{
				free(entries[i]);
			}
			free(entries);
		}
	}
	while(0);

	return ret;
}

int dir_close(int did)
{
	int ret = -1;
	if((did >= 0) && (did < MAX_DIRS))
	{
		if(open_dirs[did].opened)
		{
			if(open_dirs[did].pDir)
			{
				free(open_dirs[did].pDir);
			}

			open_dirs[did].opened = 0;
			open_dirs[did].count = 0;
			open_dirs[did].pos = 0;
			open_dirs[did].pDir = NULL;

			ret = 0;
		}
	}

	return ret;
}

int handle_hello(struct usb_dev_handle *hDev)
{
	struct HostFsHelloResp resp;

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_HELLO);

	return usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
}

int handle_open(struct usb_dev_handle *hDev, struct HostFsOpenCmd *cmd, int cmdlen)
{
	struct HostFsOpenResp resp;
	int  ret = -1;
	char path[HOSTFS_PATHMAX];

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_OPEN);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsOpenCmd)) 
		{
			fprintf(stderr, "Error, invalid open command size %d\n", cmdlen);
			break;
		}

		if(LE32(cmd->cmd.extralen) == 0)
		{
			fprintf(stderr, "Error, no filename passed with open command\n");
			break;
		}

		/* TODO: Should check that length is within a valid range */

		ret = euid_usb_bulk_read(hDev, 0x81, path, LE32(cmd->cmd.extralen), 10000);
		if(ret != LE32(cmd->cmd.extralen))
		{
			fprintf(stderr, "Error reading open data cmd->extralen %ud, ret %d\n", LE32(cmd->cmd.extralen), ret);
			break;
		}

		V_PRINTF(2, "Open command mode %08X mask %08X name %s\n", LE32(cmd->mode), LE32(cmd->mask), path);
		resp.res = LE32(open_file(LE32(cmd->fsnum), path, LE32(cmd->mode), LE32(cmd->mask)));

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
	}
	while(0);

	return ret;
}

int handle_dopen(struct usb_dev_handle *hDev, struct HostFsDopenCmd *cmd, int cmdlen)
{
	struct HostFsDopenResp resp;
	int  ret = -1;
	char path[HOSTFS_PATHMAX];

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_DOPEN);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsDopenCmd)) 
		{
			fprintf(stderr, "Error, invalid dopen command size %d\n", cmdlen);
			break;
		}

		if(LE32(cmd->cmd.extralen) == 0)
		{
			fprintf(stderr, "Error, no dirname passed with dopen command\n");
			break;
		}

		/* TODO: Should check that length is within a valid range */

		ret = euid_usb_bulk_read(hDev, 0x81, path, LE32(cmd->cmd.extralen), 10000);
		if(ret != LE32(cmd->cmd.extralen))
		{
			fprintf(stderr, "Error reading open data cmd->extralen %d, ret %d\n", LE32(cmd->cmd.extralen), ret);
			break;
		}

		V_PRINTF(2, "Dopen command name %s\n", path);
		resp.res = LE32(dir_open(LE32(cmd->fsnum), path));

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
	}
	while(0);

	return ret;
}

int fixed_write(int fd, const void *data, int len)
{
	int byteswrite = 0;
	while(byteswrite < len)
	{
		int ret;

		ret = write(fd, data+byteswrite, len-byteswrite);
		if(ret < 0)
		{
			if(errno != EINTR)
			{
				fprintf(stderr, "Error writing to file (%s)\n", strerror(errno));
				byteswrite = -1;
				break;
			}
		}
		else if(ret == 0) /* EOF? */
		{
			break;
		}
		else
		{
			byteswrite += ret;
		}
	}

	return byteswrite;
}

int handle_write(struct usb_dev_handle *hDev, struct HostFsWriteCmd *cmd, int cmdlen)
{
	static char write_block[HOSTFS_MAX_BLOCK];
	struct HostFsWriteResp resp;
	int  fid;
	int  ret = -1;

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_WRITE);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsWriteCmd)) 
		{
			fprintf(stderr, "Error, invalid write command size %d\n", cmdlen);
			break;
		}

		/* TODO: Check upper bound */
		if(LE32(cmd->cmd.extralen) <= 0)
		{
			fprintf(stderr, "Error extralen invalid (%d)\n", LE32(cmd->cmd.extralen));
			break;
		}

		/* TODO: Should check that length is within a valid range */

		ret = euid_usb_bulk_read(hDev, 0x81, write_block, LE32(cmd->cmd.extralen), 10000);
		if(ret != LE32(cmd->cmd.extralen))
		{
			fprintf(stderr, "Error reading write data cmd->extralen %d, ret %d\n", LE32(cmd->cmd.extralen), ret);
			break;
		}

		fid = LE32(cmd->fid);

		V_PRINTF(2, "Write command fid: %d, length: %d\n", fid, LE32(cmd->cmd.extralen));

		if((fid >= 0) && (fid < MAX_FILES))
		{
			if(open_files[fid].opened)
			{
				resp.res = LE32(fixed_write(fid, write_block, LE32(cmd->cmd.extralen)));
			}
			else
			{
				fprintf(stderr, "Error fid not open %d\n", fid);
			}
		}
		else
		{
			fprintf(stderr, "Error invalid fid %d\n", fid);
		}

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
	}
	while(0);

	return ret;
}

int fixed_read(int fd, void *data, int len)
{
	int bytesread = 0;

	while(bytesread < len)
	{
		int ret;

		ret = read(fd, data+bytesread, len-bytesread);
		if(ret < 0)
		{
			if(errno != EINTR)
			{
				bytesread = -1;
				break;
			}
		}
		else if(ret == 0)
		{
			/* No more to read */
			break;
		}
		else
		{
			bytesread += ret;
		}
	}

	return bytesread;
}

int handle_read(struct usb_dev_handle *hDev, struct HostFsReadCmd *cmd, int cmdlen)
{
	static char read_block[HOSTFS_MAX_BLOCK];
	struct HostFsReadResp resp;
	int  fid;
	int  ret = -1;

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_READ);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsReadCmd)) 
		{
			fprintf(stderr, "Error, invalid read command size %d\n", cmdlen);
			break;
		}

		/* TODO: Check upper bound */
		if(LE32(cmd->len) <= 0)
		{
			fprintf(stderr, "Error extralen invalid (%d)\n", LE32(cmd->len));
			break;
		}

		fid = LE32(cmd->fid);
		V_PRINTF(2, "Read command fid: %d, length: %d\n", fid, LE32(cmd->len));

		if((fid >= 0) && (fid < MAX_FILES))
		{
			if(open_files[fid].opened)
			{
				resp.res = LE32(fixed_read(fid, read_block, LE32(cmd->len)));
				if(resp.res >= 0)
				{
					resp.cmd.extralen = resp.res;
				}
			}
			else
			{
				fprintf(stderr, "Error fid not open %d\n", fid);
			}
		}
		else
		{
			fprintf(stderr, "Error invalid fid %d\n", fid);
		}

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
		if(ret < 0)
		{
			fprintf(stderr, "Error writing read response (%d)\n", ret);
			break;
		}

		if(LE32(resp.cmd.extralen) > 0)
		{
			ret = euid_usb_bulk_write(hDev, 0x2, read_block, LE32(resp.cmd.extralen), 10000);
		}
	}
	while(0);

	return ret;
}

int handle_close(struct usb_dev_handle *hDev, struct HostFsCloseCmd *cmd, int cmdlen)
{
	struct HostFsCloseResp resp;
	int  ret = -1;
	int  fid;

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_CLOSE);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsCloseCmd)) 
		{
			fprintf(stderr, "Error, invalid close command size %d\n", cmdlen);
			break;
		}

		fid = LE32(cmd->fid);
		V_PRINTF(2, "Close command fid: %d\n", fid);
		if((fid > STDERR_FILENO) && (fid < MAX_FILES) && (open_files[fid].opened))
		{
			resp.res = LE32(close(fid));
			open_files[fid].opened = 0;
			if(open_files[fid].name)
			{
				free(open_files[fid].name);
				open_files[fid].name = NULL;
			}
		}
		else
		{
			fprintf(stderr, "Error invalid file id in close command (%d)\n", fid);
		}

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
	}
	while(0);

	return ret;
}

int handle_dclose(struct usb_dev_handle *hDev, struct HostFsDcloseCmd *cmd, int cmdlen)
{
	struct HostFsDcloseResp resp;
	int  ret = -1;
	int  did;

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_DCLOSE);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsDcloseCmd)) 
		{
			fprintf(stderr, "Error, invalid close command size %d\n", cmdlen);
			break;
		}

		did = LE32(cmd->did);
		V_PRINTF(2, "Dclose command did: %d\n", did);
		resp.res = dir_close(did);

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
	}
	while(0);


	return ret;
}

int handle_dread(struct usb_dev_handle *hDev, struct HostFsDreadCmd *cmd, int cmdlen)
{
	struct HostFsDreadResp resp;
	SceIoDirent *dir = NULL;
	int  ret = -1;
	int  did;

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_READ);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsDreadCmd)) 
		{
			fprintf(stderr, "Error, invalid dread command size %d\n", cmdlen);
			break;
		}

		did = LE32(cmd->did);
		V_PRINTF(2, "Dread command did: %d\n", did);

		if((did >= 0) && (did < MAX_FILES))
		{
			if(open_dirs[did].opened)
			{
				if(open_dirs[did].pos < open_dirs[did].count)
				{
					dir = &open_dirs[did].pDir[open_dirs[did].pos++];
					resp.cmd.extralen = LE32(sizeof(SceIoDirent));
					resp.res = LE32(open_dirs[did].count - open_dirs[did].pos + 1);
				}
				else
				{
					resp.res = LE32(0);
				}
			}
			else
			{
				fprintf(stderr, "Error did not open %d\n", did);
			}
		}
		else
		{
			fprintf(stderr, "Error invalid did %d\n", did);
		}

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
		if(ret < 0)
		{
			fprintf(stderr, "Error writing dread response (%d)\n", ret);
			break;
		}

		if(LE32(resp.cmd.extralen) > 0)
		{
			ret = euid_usb_bulk_write(hDev, 0x2, (char *) dir, LE32(resp.cmd.extralen), 10000);
		}
	}
	while(0);

	return ret;
}

int handle_lseek(struct usb_dev_handle *hDev, struct HostFsLseekCmd *cmd, int cmdlen)
{
	struct HostFsLseekResp resp;
	int  ret = -1;
	int  fid;

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_LSEEK);
	resp.res = LE32(-1);
	resp.ofs = LE32(0);

	do
	{
		if(cmdlen != sizeof(struct HostFsLseekCmd)) 
		{
			fprintf(stderr, "Error, invalid lseek command size %d\n", cmdlen);
			break;
		}

		fid = LE32(cmd->fid);
		V_PRINTF(2, "Lseek command fid: %d, ofs: %lld, whence: %d\n", fid, LE64(cmd->ofs), LE32(cmd->whence));
		if((fid > STDERR_FILENO) && (fid < MAX_FILES) && (open_files[fid].opened))
		{
			/* TODO: Probably should ensure whence is mapped across, just in case */
			resp.ofs = LE64((int64_t) lseek(fid, (off_t) LE64(cmd->ofs), LE32(cmd->whence)));
			if(LE64(resp.ofs) < 0)
			{
				resp.res = LE32(-1);
			}
			else
			{
				resp.res = LE32(0);
			}
		}
		else
		{
			fprintf(stderr, "Error invalid file id in close command (%d)\n", fid);
		}

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
	}
	while(0);

	return ret;
}

int handle_remove(struct usb_dev_handle *hDev, struct HostFsRemoveCmd *cmd, int cmdlen)
{
	struct HostFsRemoveResp resp;
	int  ret = -1;
	char path[HOSTFS_PATHMAX];
	char fullpath[PATH_MAX];

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_REMOVE);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsRemoveCmd)) 
		{
			fprintf(stderr, "Error, invalid remove command size %d\n", cmdlen);
			break;
		}

		if(LE32(cmd->cmd.extralen) == 0)
		{
			fprintf(stderr, "Error, no filename passed with remove command\n");
			break;
		}

		/* TODO: Should check that length is within a valid range */

		ret = euid_usb_bulk_read(hDev, 0x81, path, LE32(cmd->cmd.extralen), 10000);
		if(ret != LE32(cmd->cmd.extralen))
		{
			fprintf(stderr, "Error reading remove data cmd->extralen %d, ret %d\n", LE32(cmd->cmd.extralen), ret);
			break;
		}

		V_PRINTF(2, "Remove command name %s\n", path);
		if(make_path(LE32(cmd->fsnum), path, fullpath, 0) == 0)
		{
			resp.res = LE32(unlink(fullpath));
		}

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
	}
	while(0);

	return ret;
}

int handle_rmdir(struct usb_dev_handle *hDev, struct HostFsRmdirCmd *cmd, int cmdlen)
{
	struct HostFsRmdirResp resp;
	int  ret = -1;
	char path[HOSTFS_PATHMAX];
	char fullpath[PATH_MAX];

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_RMDIR);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsRmdirCmd)) 
		{
			fprintf(stderr, "Error, invalid rmdir command size %d\n", cmdlen);
			break;
		}

		if(LE32(cmd->cmd.extralen) == 0)
		{
			fprintf(stderr, "Error, no filename passed with rmdir command\n");
			break;
		}

		/* TODO: Should check that length is within a valid range */

		ret = euid_usb_bulk_read(hDev, 0x81, path, LE32(cmd->cmd.extralen), 10000);
		if(ret != LE32(cmd->cmd.extralen))
		{
			fprintf(stderr, "Error reading rmdir data cmd->extralen %d, ret %d\n", LE32(cmd->cmd.extralen), ret);
			break;
		}

		V_PRINTF(2, "Rmdir command name %s\n", path);
		if(make_path(LE32(cmd->fsnum), path, fullpath, 0) == 0)
		{
			resp.res = LE32(rmdir(fullpath));
		}

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
	}
	while(0);

	return ret;
}

int handle_mkdir(struct usb_dev_handle *hDev, struct HostFsMkdirCmd *cmd, int cmdlen)
{
	struct HostFsMkdirResp resp;
	int  ret = -1;
	char path[HOSTFS_PATHMAX];
	char fullpath[PATH_MAX];

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_MKDIR);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsMkdirCmd)) 
		{
			fprintf(stderr, "Error, invalid mkdir command size %d\n", cmdlen);
			break;
		}

		if(LE32(cmd->cmd.extralen) == 0)
		{
			fprintf(stderr, "Error, no filename passed with mkdir command\n");
			break;
		}

		/* TODO: Should check that length is within a valid range */

		ret = euid_usb_bulk_read(hDev, 0x81, path, LE32(cmd->cmd.extralen), 10000);
		if(ret != LE32(cmd->cmd.extralen))
		{
			fprintf(stderr, "Error reading mkdir data cmd->extralen %d, ret %d\n", LE32(cmd->cmd.extralen), ret);
			break;
		}

		V_PRINTF(2, "Mkdir command mode %08X, name %s\n", LE32(cmd->mode), path);
		if(make_path(LE32(cmd->fsnum), path, fullpath, 0) == 0)
		{
			resp.res = LE32(mkdir(fullpath, LE32(cmd->mode)));
		}

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
	}
	while(0);

	return ret;
}

int handle_getstat(struct usb_dev_handle *hDev, struct HostFsGetstatCmd *cmd, int cmdlen)
{
	struct HostFsGetstatResp resp;
	SceIoStat st;
	int  ret = -1;
	char path[HOSTFS_PATHMAX];
	char fullpath[PATH_MAX];

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_GETSTAT);
	resp.res = LE32(-1);
	memset(&st, 0, sizeof(st));

	do
	{
		if(cmdlen != sizeof(struct HostFsGetstatCmd)) 
		{
			fprintf(stderr, "Error, invalid getstat command size %d\n", cmdlen);
			break;
		}

		if(LE32(cmd->cmd.extralen) == 0)
		{
			fprintf(stderr, "Error, no filename passed with getstat command\n");
			break;
		}

		/* TODO: Should check that length is within a valid range */

		ret = euid_usb_bulk_read(hDev, 0x81, path, LE32(cmd->cmd.extralen), 10000);
		if(ret != LE32(cmd->cmd.extralen))
		{
			fprintf(stderr, "Error reading getstat data cmd->extralen %d, ret %d\n", LE32(cmd->cmd.extralen), ret);
			break;
		}

		V_PRINTF(2, "Getstat command name %s\n", path);
		if(make_path(LE32(cmd->fsnum), path, fullpath, 0) == 0)
		{
			resp.res = LE32(fill_stat(NULL, fullpath, &st));
			if(LE32(resp.res) == 0)
			{
				resp.cmd.extralen = LE32(sizeof(st));
			}
		}

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
		if(ret < 0)
		{
			fprintf(stderr, "Error writing getstat response (%d)\n", ret);
			break;
		}

		if(LE32(resp.cmd.extralen) > 0)
		{
			ret = euid_usb_bulk_write(hDev, 0x2, (char *) &st, sizeof(st), 10000);
		}
	}
	while(0);

	return ret;
}

int psp_settime(const char *path, const struct HostFsTimeStamp *ts, int set)
{
	time_t convtime;
	struct tm stime;
	struct utimbuf tbuf;
	struct stat st;

	stime.tm_year = LE16(ts->year) - 1900;
	stime.tm_mon = LE16(ts->month) - 1;
	stime.tm_mday = LE16(ts->day);
	stime.tm_hour = LE16(ts->hour);
	stime.tm_min = LE16(ts->minute);
	stime.tm_sec = LE16(ts->second);

	if(stat(path, &st) < 0)
	{
		return -1;
	}

	tbuf.actime = st.st_atime;
	tbuf.modtime = st.st_mtime;

	convtime = mktime(&stime);
	if(convtime == (time_t)-1)
	{
		return -1;
	}

	if(set == PSP_CHSTAT_ATIME)
	{
		tbuf.actime = convtime;
	}
	else if(set == PSP_CHSTAT_MTIME)
	{
		tbuf.modtime = convtime;
	}
	else
	{
		return -1;
	}

	return utime(path, &tbuf);

}

int psp_chstat(const char *path, struct HostFsChstatCmd *cmd)
{
	int ret = 0;

	if(LE32(cmd->bits) & PSP_CHSTAT_MODE)
	{
		int mask;

		mask = LE32(cmd->mode) & (FIO_S_IRWXU | FIO_S_IRWXG | FIO_S_IRWXO);
		ret = chmod(path, mask);
		if(ret < 0)
		{
			V_PRINTF(2, "Could not set file mask\n");
			return -1;
		}
	}

	if(LE32(cmd->bits) & PSP_CHSTAT_SIZE)
	{
		/* Do a truncate */
	}

	if(LE32(cmd->bits) & PSP_CHSTAT_ATIME)
	{
		if(psp_settime(path, &cmd->atime, PSP_CHSTAT_ATIME) < 0)
		{
			V_PRINTF(2, "Could not set access time\n");
			return -1;
		}
	}

	if(LE32(cmd->bits) & PSP_CHSTAT_MTIME)
	{
		if(psp_settime(path, &cmd->mtime, PSP_CHSTAT_MTIME) < 0)
		{
			V_PRINTF(2, "Could not set modification time\n");
			return -1;
		}
	}

	return 0;
}

int handle_chstat(struct usb_dev_handle *hDev, struct HostFsChstatCmd *cmd, int cmdlen)
{
	struct HostFsChstatResp resp;
	int  ret = -1;
	char path[HOSTFS_PATHMAX];
	char fullpath[PATH_MAX];

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_CHSTAT);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsChstatCmd)) 
		{
			fprintf(stderr, "Error, invalid chstat command size %d\n", cmdlen);
			break;
		}

		if(LE32(cmd->cmd.extralen) == 0)
		{
			fprintf(stderr, "Error, no filename passed with chstat command\n");
			break;
		}

		/* TODO: Should check that length is within a valid range */

		ret = euid_usb_bulk_read(hDev, 0x81, path, LE32(cmd->cmd.extralen), 10000);
		if(ret != LE32(cmd->cmd.extralen))
		{
			fprintf(stderr, "Error reading chstat data cmd->extralen %d, ret %d\n", LE32(cmd->cmd.extralen), ret);
			break;
		}

		V_PRINTF(2, "Chstat command name %s, bits %08X\n", path, LE32(cmd->bits));
		if(make_path(LE32(cmd->fsnum), path, fullpath, 0) == 0)
		{
			resp.res = LE32(psp_chstat(fullpath, cmd));
		}

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
	}
	while(0);

	return ret;
}

int handle_rename(struct usb_dev_handle *hDev, struct HostFsRenameCmd *cmd, int cmdlen)
{
	struct HostFsRenameResp resp;
	int  ret = -1;
	char path[HOSTFS_PATHMAX];
	char oldpath[PATH_MAX];
	char newpath[PATH_MAX];
	int  oldpathlen;
	int  newpathlen;

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_RENAME);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsRenameCmd)) 
		{
			fprintf(stderr, "Error, invalid mkdir command size %d\n", cmdlen);
			break;
		}

		if(LE32(cmd->cmd.extralen) == 0)
		{
			fprintf(stderr, "Error, no filenames passed with rename command\n");
			break;
		}

		/* TODO: Should check that length is within a valid range */

		memset(path, 0, sizeof(path));
		ret = euid_usb_bulk_read(hDev, 0x81, path, LE32(cmd->cmd.extralen), 10000);
		if(ret != LE32(cmd->cmd.extralen))
		{
			fprintf(stderr, "Error reading rename data cmd->extralen %d, ret %d\n", LE32(cmd->cmd.extralen), ret);
			break;
		}

		/* Really should check this better ;) */
		oldpathlen = strlen(path);
		newpathlen = strlen(path+oldpathlen+1);

		V_PRINTF(2, "Rename command oldname %s, newname %s\n", path, path+oldpathlen+1);
		if(!make_path(LE32(cmd->fsnum), path, oldpath, 0) && !make_path(LE32(cmd->fsnum), path+oldpathlen+1, newpath, 0))
		{
			resp.res = LE32(rename(oldpath, newpath));
		}

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
	}
	while(0);

	return ret;
}

int handle_chdir(struct usb_dev_handle *hDev, struct HostFsChdirCmd *cmd, int cmdlen)
{
	struct HostFsChdirResp resp;
	int  ret = -1;
	char path[HOSTFS_PATHMAX];
	int fsnum;

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_CHDIR);
	resp.res = -1;

	do
	{
		if(cmdlen != sizeof(struct HostFsChdirCmd)) 
		{
			fprintf(stderr, "Error, invalid chdir command size %d\n", cmdlen);
			break;
		}

		if(LE32(cmd->cmd.extralen) == 0)
		{
			fprintf(stderr, "Error, no filename passed with mkdir command\n");
			break;
		}

		/* TODO: Should check that length is within a valid range */

		ret = euid_usb_bulk_read(hDev, 0x81, path, LE32(cmd->cmd.extralen), 10000);
		if(ret != LE32(cmd->cmd.extralen))
		{
			fprintf(stderr, "Error reading chdir data cmd->extralen %d, ret %d\n", LE32(cmd->cmd.extralen), ret);
			break;
		}

		V_PRINTF(2, "Chdir command name %s\n", path);
		
		fsnum = LE32(cmd->fsnum);
		if((fsnum >= 0) && (fsnum < MAX_HOSTDRIVES))
		{
			strcpy(g_drives[fsnum].currdir, path);
			resp.res = 0;
		}

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
	}
	while(0);

	return ret;
}

int handle_ioctl(struct usb_dev_handle *hDev, struct HostFsIoctlCmd *cmd, int cmdlen)
{
	static char inbuf[64*1024];
	static char outbuf[64*1024];
	int inlen;
	struct HostFsIoctlResp resp;
	int  ret = -1;

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_IOCTL);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsIoctlCmd)) 
		{
			fprintf(stderr, "Error, invalid ioctl command size %d\n", cmdlen);
			break;
		}

		inlen = LE32(cmd->cmd.extralen);
		if(inlen > 0)
		{
			/* TODO: Should check that length is within a valid range */

			ret = euid_usb_bulk_read(hDev, 0x81, inbuf, inlen, 10000);
			if(ret != inlen)
			{
				fprintf(stderr, "Error reading ioctl data cmd->extralen %d, ret %d\n", inlen, ret);
				break;
			}
		}

		V_PRINTF(2, "Ioctl command fid %d, cmdno %d, inlen %d\n", LE32(cmd->fid), LE32(cmd->cmdno), inlen);

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
		if(ret < 0)
		{
			fprintf(stderr, "Error writing ioctl response (%d)\n", ret);
			break;
		}

		if(LE32(resp.cmd.extralen) > 0)
		{
			ret = euid_usb_bulk_write(hDev, 0x2, (char *) outbuf, LE32(resp.cmd.extralen), 10000);
		}
	}
	while(0);

	return ret;
}

int handle_devctl(struct usb_dev_handle *hDev, struct HostFsDevctlCmd *cmd, int cmdlen)
{
	static char inbuf[64*1024];
	static char outbuf[64*1024];
	int inlen;
	struct HostFsDevctlResp resp;
	int  ret = -1;

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_DEVCTL);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsDevctlCmd)) 
		{
			fprintf(stderr, "Error, invalid devctl command size %d\n", cmdlen);
			break;
		}

		inlen = LE32(cmd->cmd.extralen);
		if(inlen > 0)
		{
			/* TODO: Should check that length is within a valid range */

			ret = euid_usb_bulk_read(hDev, 0x81, inbuf, inlen, 10000);
			if(ret != inlen)
			{
				fprintf(stderr, "Error reading devctl data cmd->extralen %d, ret %d\n", inlen, ret);
				break;
			}
		}

		V_PRINTF(2, "Devctl command cmdno %d, inlen %d\n", LE32(cmd->cmdno), inlen);

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
		if(ret < 0)
		{
			fprintf(stderr, "Error writing devctl response (%d)\n", ret);
			break;
		}

		if(LE32(resp.cmd.extralen) > 0)
		{
			ret = euid_usb_bulk_write(hDev, 0x2, (char *) outbuf, LE32(resp.cmd.extralen), 10000);
		}
	}
	while(0);

	return ret;
}

usb_dev_handle *wait_for_device(void)
{
	usb_dev_handle *hDev = NULL;

	while(hDev == NULL)
	{
		usb_find_busses();
		usb_find_devices();

		hDev = open_device(usb_get_busses());
		if(hDev)
		{
			fprintf(stderr, "Connected to device\n");
			break;
		}

		/* Sleep for one second */
		sleep(1);
	}

	return hDev;
}

int init_hostfs(void)
{
	int i;

	memset(open_files, 0, sizeof(open_files));
	memset(open_dirs, 0, sizeof(open_dirs));
	for(i = 0; i < MAX_HOSTDRIVES; i++)
	{
		strcpy(g_drives[i].currdir, "/");
	}

	return 0;
}

void close_hostfs(void)
{
	int i;

	for(i = 3; i < MAX_FILES; i++)
	{
		if(open_files[i].opened)
		{
			close(i);
			open_files[i].opened = 0;
			if(open_files[i].name)
			{
				free(open_files[i].name);
				open_files[i].name = NULL;
			}
		}
	}

	for(i = 0; i < MAX_DIRS; i++)
	{
		if(open_dirs[i].opened)
		{
			dir_close(i);
		}
	}
}

void do_hostfs(struct HostFsCmd *cmd, int readlen)
{
	V_PRINTF(2, "Magic: %08X\n", LE32(cmd->magic));
	V_PRINTF(2, "Command Num: %08X\n", LE32(cmd->command));
	V_PRINTF(2, "Extra Len: %d\n", LE32(cmd->extralen));

	switch(LE32(cmd->command))
	{
		case HOSTFS_CMD_HELLO: if(handle_hello(g_hDev) < 0)
							   {
								   fprintf(stderr, "Error sending hello response\n");
							   }
							   break;
		case HOSTFS_CMD_OPEN:  if(handle_open(g_hDev, (struct HostFsOpenCmd *) cmd, readlen) < 0)
							   {
								   fprintf(stderr, "Error in open command\n");
							   }
							   break;
		case HOSTFS_CMD_CLOSE: if(handle_close(g_hDev, (struct HostFsCloseCmd *) cmd, readlen) < 0)
							   {
								   fprintf(stderr, "Error in close command\n");
							   }
							   break;
		case HOSTFS_CMD_WRITE: if(handle_write(g_hDev, (struct HostFsWriteCmd *) cmd, readlen) < 0)
							   {
								   fprintf(stderr, "Error in write command\n");
							   }
							   break;
		case HOSTFS_CMD_READ:  if(handle_read(g_hDev, (struct HostFsReadCmd *) cmd, readlen) < 0)
							   {
								   fprintf(stderr, "Error in read command\n");
							   }
							   break;
		case HOSTFS_CMD_LSEEK: if(handle_lseek(g_hDev, (struct HostFsLseekCmd *) cmd, readlen) < 0)
							   {
								   fprintf(stderr, "Error in lseek command\n");
							   }
							   break;
		case HOSTFS_CMD_DOPEN: if(handle_dopen(g_hDev, (struct HostFsDopenCmd *) cmd, readlen) < 0)
							   {
								   fprintf(stderr, "Error in dopen command\n");
							   }
							   break;
		case HOSTFS_CMD_DCLOSE: if(handle_dclose(g_hDev, (struct HostFsDcloseCmd *) cmd, readlen) < 0)
								{
									fprintf(stderr, "Error in dclose command\n");
								}
								break;
		case HOSTFS_CMD_DREAD: if(handle_dread(g_hDev, (struct HostFsDreadCmd *) cmd, readlen) < 0)
							   {
									fprintf(stderr, "Error in dread command\n");
							   }
							   break;
		case HOSTFS_CMD_REMOVE: if(handle_remove(g_hDev, (struct HostFsRemoveCmd *) cmd, readlen) < 0)
								{
									fprintf(stderr, "Error in remove command\n");
								}
								break;
		case HOSTFS_CMD_RMDIR: if(handle_rmdir(g_hDev, (struct HostFsRmdirCmd *) cmd, readlen) < 0)
								{
									fprintf(stderr, "Error in rmdir command\n");
								}
								break;
		case HOSTFS_CMD_MKDIR: if(handle_mkdir(g_hDev, (struct HostFsMkdirCmd *) cmd, readlen) < 0)
								{
									fprintf(stderr, "Error in mkdir command\n");
								}
								break;
		case HOSTFS_CMD_CHDIR: if(handle_chdir(g_hDev, (struct HostFsChdirCmd *) cmd, readlen) < 0)
								{
									fprintf(stderr, "Error in chdir command\n");
								}
								break;
		case HOSTFS_CMD_RENAME: if(handle_rename(g_hDev, (struct HostFsRenameCmd *) cmd, readlen) < 0)
								{
									fprintf(stderr, "Error in rename command\n");
								}
								break;
		case HOSTFS_CMD_GETSTAT:if(handle_getstat(g_hDev, (struct HostFsGetstatCmd *) cmd, readlen) < 0)
								{
									fprintf(stderr, "Error in getstat command\n");
								}
								break;
		case HOSTFS_CMD_CHSTAT: if(handle_chstat(g_hDev, (struct HostFsChstatCmd *) cmd, readlen) < 0)
								{
									fprintf(stderr, "Error in chstat command\n");
								}
								break;
		case HOSTFS_CMD_IOCTL: if(handle_ioctl(g_hDev, (struct HostFsIoctlCmd *) cmd, readlen) < 0)
							   {
								   fprintf(stderr, "Error in ioctl command\n");
							   }
							   break;
		case HOSTFS_CMD_DEVCTL: if(handle_devctl(g_hDev, (struct HostFsDevctlCmd *) cmd, readlen) < 0)
							   {
								   fprintf(stderr, "Error in devctl command\n");
							   }
							   break;
		default: fprintf(stderr, "Error, unknown command %08X\n", cmd->command);
							 break;
	};
}


void do_async(struct AsyncCommand *cmd, int readlen)
{
	uint8_t *data;

	if(readlen > sizeof(struct AsyncCommand))
	{
		data = (uint8_t *) cmd + sizeof(struct AsyncCommand);
		switch(cmd->channel)
		{
			case 0: if(g_shellsock >= 0)
					{
						write(g_shellsock, data, readlen - sizeof(struct AsyncCommand));
					}
					break;
			case 1: if(g_gdbdebug)
					{
						print_gdbdebug(0, data, readlen - sizeof(struct AsyncCommand));
					}

					if(g_gdbsock >= 0)
					{
						write(g_gdbsock, data, readlen - sizeof(struct AsyncCommand));
					}
					break;
			default: /* Do nothing */
					break;
		};
	}
}

int start_hostfs(void)
{
	uint32_t data[512/sizeof(uint32_t)];
	int readlen;

	while(1)
	{
		init_hostfs();

		g_hDev = wait_for_device();

		if(g_hDev)
		{
			uint32_t magic;

			magic = LE32(HOSTFS_MAGIC);

			if(euid_usb_bulk_write(g_hDev, 0x2, (char *) &magic, sizeof(magic), 1000) == sizeof(magic))
			{
				while(1)
				{
					readlen = euid_usb_bulk_read(g_hDev, 0x81, (char*) data, 512, USB_TIMEOUT);
					if(readlen == 0)
					{
						fprintf(stderr, "Read cancelled (remote disconnected)\n");
						break;
					}
					else if(readlen == -ETIMEDOUT)
					{
						continue;
					}
					else if(readlen < 0)
					{
						break;
					}

					if(readlen < sizeof(uint32_t))
					{
						fprintf(stderr, "Error could not read magic\n");
						break;
					}

					if(LE32(data[0]) == HOSTFS_MAGIC)
					{
						if(readlen < sizeof(struct HostFsCmd))
						{
							fprintf(stderr, "Error reading command header %d\n", readlen);
							break;
						}

						do_hostfs((struct HostFsCmd *) data, readlen);
					}
					else if(LE32(data[0]) == ASYNC_MAGIC)
					{
						if(readlen < sizeof(struct AsyncCommand))
						{
							fprintf(stderr, "Error reading async header %d\n", readlen);
							break;
						}

						do_async((struct AsyncCommand *) data, readlen);
					}
					else
					{
						fprintf(stderr, "Error, invalid magic %08X\n", LE32(data[0]));
					}
				}
			}

			close_device(g_hDev);
			g_hDev = NULL;
		}

		close_hostfs();
	}

	return 0;
}

int parse_args(int argc, char **argv)
{
	int i;

	if(getcwd(g_rootdir, PATH_MAX) < 0)
	{
		fprintf(stderr, "Could not get current path\n");
		return 0;
	}

	for(i = 0; i < MAX_HOSTDRIVES; i++)
	{
		strcpy(g_drives[i].rootdir, g_rootdir);
	}

	while(1)
	{
		int ch;

		ch = getopt(argc, argv, "vhdcg:s:p:f:");
		if(ch == -1)
		{
			break;
		}

		switch(ch)
		{
			case 'v': g_verbose++;
					  break;
			case 'g': g_gdbport = atoi(optarg);
					  break;
			case 's': g_shellport = atoi(optarg);
					  break;
			case 'p': g_pid = strtoul(optarg, NULL, 0);
					  break;
			case 'd': g_gdbdebug = 1;
					  break;
			case 'c': g_nocase = 1;
					  break;
			case 'f': g_mapfile = optarg;
					  break;
			case 'h': return 0;
			default:  printf("Unknown option\n");
					  return 0;
					  break;
		};
	}

	argc -= optind;
	argv += optind;

	if(argc > 0)
	{
		if(argc > MAX_HOSTDRIVES)
		{
			argc = MAX_HOSTDRIVES;
		}

		for(i = 0; i < argc; i++)
		{
			if(argv[i][0] != '/')
			{
				char tmpdir[PATH_MAX];
				snprintf(tmpdir, PATH_MAX, "%s/%s", g_rootdir, argv[i]);
				strcpy(g_drives[i].rootdir, tmpdir);
			}
			else
			{
				strcpy(g_drives[i].rootdir, argv[i]);
			}
			gen_path(g_drives[i].rootdir, 0);
			V_PRINTF(2, "Root %d: %s\n", i, g_drives[i].rootdir);
		}
	}
	else
	{
		V_PRINTF(2, "Root directory: %s\n", g_rootdir);
	}

	return 1;
}

void print_help(void)
{
	fprintf(stderr, "Usage: usbhostfs_pc [options] [rootdir0..rootdir%d]\n", MAX_HOSTDRIVES-1);
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "-v                : Set verbose mode\n");
	fprintf(stderr, "-vv               : More verbose\n");
	fprintf(stderr, "-s port           : Specify local shell port (default %d)\n", BASE_PORT);
	fprintf(stderr, "-g port           : Specify local GDB port (default %d)\n", BASE_PORT+1);
	fprintf(stderr, "-p pid            : Specify the product ID of the PSP device\n");
	fprintf(stderr, "-d                : Print GDB transfers\n");
	fprintf(stderr, "-f filename       : Load the host drive mappings from a file\n");
	fprintf(stderr, "-c                : Enable case-insensitive filenames\n");
	fprintf(stderr, "-h                : Print this help\n");
}

void shutdown_socket(void)
{
	if(g_shellsock >= 0)
	{
		close(g_shellsock);
		g_shellsock = -1;
	}

	if(g_shellserv >= 0)
	{
		close(g_shellserv);
		g_shellserv = -1;
	}

	if(g_gdbsock >= 0)
	{
		close(g_gdbsock);
		g_gdbsock = -1;
	}

	if(g_gdbserv >= 0)
	{
		close(g_gdbserv);
		g_gdbserv = -1;
	}
}

int exit_app(void)
{
	printf("Exiting\n");
	shutdown_socket();
	if(g_hDev)
	{
		/* Nuke the connection */
		seteuid(0);
		setegid(0);
		close_device(g_hDev);
	}
	exit(1);

	return 0;
}

void signal_handler(int sig)
{
	exit_app();
}

int make_socket(unsigned short port)
{
	int sock;
	struct sockaddr_in name;

	sock = socket(PF_INET, SOCK_STREAM, 0);
	if(sock < 0)
	{
		perror("socket");
		return -1;
	}

	name.sin_family = AF_INET;
	name.sin_port = htons(port);
	name.sin_addr.s_addr = htonl(INADDR_ANY);
	if(bind(sock, (struct sockaddr *) &name, sizeof(name)) < 0)
	{
		perror("bind");
		close(sock);
		return -1;
	}

	if(listen(sock, 1) < 0)
	{
		perror("listen");
		close(sock);
		return -1;
	}

	return sock;
}

int add_drive(int num, const char *dir)
{
	char path[PATH_MAX];
	DIR *pDir;

	if((num < 0) || (num >= MAX_HOSTDRIVES))
	{
		printf("Invalid host driver number '%d'\n", num);
		return 0;
	}

	/* Make path */
	if(dir[0] != '/')
	{
		snprintf(path, PATH_MAX, "%s/%s", g_rootdir, dir);
	}
	else
	{
		strcpy(path, dir);
	}
	gen_path(path, 0);

	pDir = opendir(path);
	if(pDir)
	{
		closedir(pDir);
		if(pthread_mutex_lock(&g_drivemtx))
		{
			printf("Couldn't lock mutex\n");
			return 0;
		}

		strcpy(g_drives[num].rootdir, path);
		strcpy(g_drives[num].currdir, "/");

		pthread_mutex_unlock(&g_drivemtx);
	}
	else
	{
		printf("Invalid directory '%s'\n", path);
		return 0;
	}

	return 1;
}

#define COMMAND_OK   0
#define COMMAND_ERR  1
#define COMMAND_HELP 2

struct ShellCmd
{
	const char *name;
	const char *help;
	int (*fn)(void);
};

int nocase_set(void)
{
	char *set;

	set = strtok(NULL, " \t");
	if(set)
	{
		if(strcmp(set, "on") == 0)
		{
			g_nocase = 1;
		}
		else if(strcmp(set, "off") == 0)
		{
			g_nocase = 0;
		}
		else
		{
			printf("Error setting nocase, invalid option '%s'\n", set);
		}
	}
	else
	{
		printf("nocase: %s\n", g_nocase ? "on" : "off");
	}

	return COMMAND_OK;
}

int gdbdebug_set(void)
{
	char *set;

	set = strtok(NULL, " \t");
	if(set)
	{
		if(strcmp(set, "on") == 0)
		{
			g_gdbdebug = 1;
		}
		else if(strcmp(set, "off") == 0)
		{
			g_gdbdebug = 0;
		}
		else
		{
			printf("Error setting nocase, invalid option '%s'\n", set);
		}
	}
	else
	{
		printf("gdbdebug: %s\n", g_gdbdebug ? "on" : "off");
	}

	return COMMAND_OK;
}

int verbose_set(void)
{
	char *set;

	set = strtok(NULL, " \t");
	if(set)
	{
		g_verbose = atoi(set);
	}
	else
	{
		printf("verbose: %d\n", g_verbose);
	}

	return COMMAND_OK;
}

int list_drives(void)
{
	int i;

	for(i = 0; i < MAX_HOSTDRIVES; i++)
	{
		printf("host%d: %s\n", i, g_drives[i].rootdir);
	}

	return COMMAND_OK;
}

int mount_drive(void)
{
	char *num;
	char *dir;
	int  val;
	char *endp;

	num = strtok(NULL, " \t");
	dir = strtok(NULL, "");

	if((!num) || (!dir))
	{
		printf("Must specify a drive number and a directory\n");
		return COMMAND_ERR;
	}

	val = strtoul(num, &endp, 10);
	if(*endp)
	{
		printf("Invalid host driver number '%s'\n", num);
		return COMMAND_ERR;
	}

	if(!add_drive(val, dir))
	{
		return COMMAND_ERR;
	}

	return COMMAND_OK;
}

void load_mapfile(const char *mapfile)
{
	char path[PATH_MAX];
	FILE *fp;
	int line = 0;

	fp = fopen(mapfile, "r");
	if(fp == NULL)
	{
		printf("Couldn't open mapfile '%s'\n", g_mapfile);
		return;
	}

	while(fgets(path, PATH_MAX, fp))
	{
		char *buf = path;
		int len;
		int num;

		line++;
		/* Remove whitespace */
		len = strlen(buf);
		while((len > 0) && (isspace(buf[len-1])))
		{
			buf[len-1] = 0;
			len--;
		}

		while(isspace(*buf))
		{
			buf++;
			len--;
		}

		if(!isdigit(*buf))
		{
			printf("Line %d: Entry does not start with the host number\n", line);
			continue;
		}

		if(len > 0)
		{
			char *endp;
			num = strtoul(buf, &endp, 10);
			if((*endp != '=') || (*(endp+1) == 0) || (isspace(*(endp+1))))
			{
				printf("Line %d: Entry is not of the form 'num=path'\n", line);
				continue;
			}

			endp++;

			add_drive(num, endp);
		}
	}

	fclose(fp);
}

int load_drives(void)
{
	char *mapfile;

	mapfile = strtok(NULL, "");
	if(mapfile == NULL)
	{
		printf("Must specify a filename\n");
		return COMMAND_ERR;
	}

	load_mapfile(mapfile);

	return COMMAND_OK;
}

int save_drives(void)
{
	char *mapfile;
	FILE *fp;
	int i;

	mapfile = strtok(NULL, "");
	if(mapfile == NULL)
	{
		printf("Must specify a filename\n");
		return COMMAND_ERR;
	}

	fp = fopen(mapfile, "w");
	if(fp == NULL)
	{
		printf("Couldn't open file '%s'\n", mapfile);
		return COMMAND_ERR;
	}

	for(i = 0; i < MAX_HOSTDRIVES; i++)
	{
		fprintf(fp, "%d=%s\n", i, g_drives[i].rootdir);
	}

	fclose(fp);

	return COMMAND_OK;
}

int help_cmd(void)
{
	return COMMAND_HELP;
}

struct ShellCmd g_commands[] = {
	{ "drives", "Print the current drives", list_drives },
	{ "mount", "Mount a directory (mount num dir)", mount_drive },
	{ "save",  "Save the list of mounts to a file (save filename)", save_drives },
	{ "load",  "Load a list of mounts from a file (load filename)", load_drives },
	{ "nocase", "Set case sensitivity (nocase on|off)", nocase_set },
	{ "gdbdebug", "Set the GDB debug option (gdbdebug on|off)", gdbdebug_set },
	{ "verbose", "Set the verbose level (verbose 0|1|2)", verbose_set },
	{ "help", "Print this help", help_cmd },
	{ "exit", "Exit the application", exit_app },
};

void parse_shell(char *buf)
{
	int len;

	/* Remove whitespace */
	len = strlen(buf);
	while((len > 0) && (isspace(buf[len-1])))
	{
		buf[len-1] = 0;
		len--;
	}

	while(isspace(*buf))
	{
		buf++;
		len--;
	}

	if(len > 0)
	{
		const char *cmd;
		int i;
		int ret = COMMAND_HELP;

		cmd = strtok(buf, " \t");
		for(i = 0; i < (sizeof(g_commands) / sizeof(struct ShellCmd)); i++)
		{
			if(strcmp(cmd, g_commands[i].name) == 0)
			{
				if(g_commands[i].fn)
				{
					ret = g_commands[i].fn();
				}
				break;
			}
		}

		if(ret == COMMAND_HELP)
		{
			int i;

			printf("-= Help =-\n");
			for(i = 0; i < (sizeof(g_commands) / sizeof(struct ShellCmd)); i++)
			{
				printf("%-10s: %s\n", g_commands[i].name, g_commands[i].help);
			}
		}
	}
}

void *async_thread(void *arg)
{
	char shell[512];
	char gdb[512];
	char *shdata, *gdbdata;
	struct AsyncCommand *cmd;
	fd_set read_set, read_save;
	struct sockaddr_in client;
	size_t size;
	int max_fd = 0;
	int flag = 1;

	FD_ZERO(&read_save);
	FD_SET(STDIN_FILENO, &read_save);
	max_fd = STDIN_FILENO + 1;

	if(g_shellserv >= 0)
	{
		FD_SET(g_shellserv, &read_save);
		if(g_shellserv > max_fd)
		{
			max_fd = g_shellserv;
		}
	}

	if(g_gdbserv >= 0)
	{
		FD_SET(g_gdbserv, &read_save);
		if(g_gdbserv > max_fd)
		{
			max_fd = g_gdbserv;
		}
	}

	cmd = (struct AsyncCommand *) shell;
	cmd->magic = LE32(ASYNC_MAGIC);
	cmd->channel = LE32(0);

	cmd = (struct AsyncCommand *) gdb;
	cmd->magic = LE32(ASYNC_MAGIC);
	cmd->channel = LE32(1);

	shdata = shell + sizeof(struct AsyncCommand);
	gdbdata = gdb + sizeof(struct AsyncCommand);

	while(1)
	{
		read_set = read_save;
		if(select(max_fd+1, &read_set, NULL, NULL, NULL) > 0)
		{
			if(FD_ISSET(STDIN_FILENO, &read_set))
			{
				char buffer[4096];

				if(fgets(buffer, sizeof(buffer), stdin))
				{
					parse_shell(buffer);
				}
			}

			if(FD_ISSET(g_shellserv, &read_set))
			{
				if(g_shellsock >= 0)
				{
					FD_CLR(g_shellsock, &read_save);
					close(g_shellsock);
				}
				size = sizeof(client);
				g_shellsock = accept(g_shellserv, (struct sockaddr *) &client, &size);
				if(g_shellsock >= 0)
				{
					printf("Accepting shell connection from %s\n", inet_ntoa(client.sin_addr));
					FD_SET(g_shellsock, &read_save);
					setsockopt(g_shellsock, SOL_TCP, TCP_NODELAY, &flag, sizeof(int));
					if(g_shellsock > max_fd)
					{
						max_fd = g_shellsock;
					}
				}
			}

			if(FD_ISSET(g_gdbserv, &read_set))
			{
				if(g_gdbsock >= 0)
				{
					FD_CLR(g_gdbsock, &read_save);
					close(g_gdbsock);
				}
				size = sizeof(client);
				g_gdbsock = accept(g_gdbserv, (struct sockaddr *) &client, &size);
				if(g_gdbsock >= 0)
				{
					printf("Accepting gdb connection from %s\n", inet_ntoa(client.sin_addr));
					FD_SET(g_gdbsock, &read_save);
					setsockopt(g_gdbsock, SOL_TCP, TCP_NODELAY, &flag, sizeof(int));
					if(g_gdbsock > max_fd)
					{
						max_fd = g_gdbsock;
					}
				}
			}

			if(g_shellsock >= 0)
			{
				if(FD_ISSET(g_shellsock, &read_set))
				{
					int readbytes;

					readbytes = read(g_shellsock, shdata, sizeof(shell) - sizeof(struct AsyncCommand));
					if(readbytes > 0)
					{
						if(g_hDev)
						{
							euid_usb_bulk_write(g_hDev, 0x3, shell, readbytes+sizeof(struct AsyncCommand), 10000);
						}
					}
					else
					{
						FD_CLR(g_shellsock, &read_save);
						close(g_shellsock);
						g_shellsock = -1;
						printf("Closing shell connection\n");
					}
				}
			}

			if(g_gdbsock >= 0)
			{
				if(FD_ISSET(g_gdbsock, &read_set))
				{
					int readbytes;

					readbytes = read(g_gdbsock, gdbdata, sizeof(gdb) - sizeof(struct AsyncCommand));
					if(readbytes > 0)
					{
						if(g_gdbdebug)
						{
							print_gdbdebug(1, (uint8_t *) gdbdata, readbytes);
						}

						if(g_hDev)
						{
							euid_usb_bulk_write(g_hDev, 0x3, gdb, readbytes+sizeof(struct AsyncCommand), 10000);
						}
					}
					else
					{
						FD_CLR(g_gdbsock, &read_save);
						close(g_gdbsock);
						g_gdbsock = -1;
						printf("Closing gdb connection\n");
					}
				}
			}
		}
	}
	
	return NULL;
}

int main(int argc, char **argv)
{
	printf("USBHostFS (c) TyRaNiD 2k6\n");
#ifndef __CYGWIN__
	if(geteuid() != 0)
	{
		fprintf(stderr, "Error this application must be run as root or SUID root\n");
		return 1;
	}
#endif
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	if(parse_args(argc, argv))
	{
		pthread_t thid;
		usb_init();

		if(g_mapfile)
		{
			load_mapfile(g_mapfile);
		}

		/* Create sockets */
		g_shellserv = make_socket(g_shellport);
		g_gdbserv = make_socket(g_gdbport);

		/* Mask out any executable bits, as they don't make sense */
		pthread_create(&thid, NULL, async_thread, NULL);
		start_hostfs();
	}
	else
	{
		print_help();
	}

	return 0;
}
