#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <iconv.h>

#include "utils.h"

//Build with: gcc -o darctool darctool.c utils.c -liconv

typedef struct {
	u8 magicnum[4];//0x63726164 "darc"
	u8 bom[2];
	u8 headerlen[2];
	u8 version[4];//Not sure if the high u16/u8 is really part of the "version".
	u8 filesize[4];
	u8 table_offset[4];
	u8 table_size[4];
	u8 filedata_offset[4];
} darc_header;

typedef struct {
	u8 filename_offset[4];
	u8 offset[4];
	u8 size[4];
} darc_table_entry;

typedef struct _darcbuild_table_entry {
	u32 initialized;
	u32 entryindex;
	u32 total_children;
	struct _darcbuild_table_entry *next, *child, *parent;
	darc_table_entry entry;
	char fs_path[256];
	u16 arc_name[256];
} darcbuild_table_entry;

struct stat filestats;

u32 table_size = 0;
u32 filenametable_offset = 0;
u32 total_table_entries = 0;
u32 fn_offset_raw = 0, fn_offset = 0;
u32 filedata_offset = 0, filedata_size = 0;
u32 filenametable_curentryoffset = 0;

u8 *tablebuf = NULL;
darc_table_entry *table = NULL;
darcbuild_table_entry *buildtable_firstentry = NULL;

FILE *farchive = NULL;

iconv_t iconvdesc;

char *fs_directory_path = NULL;
char basearc_path[256];

int extract_darc(u32 start_entry, u32 end_entry)
{
	u32 pos, pos2;
	u32 typeflag;
	u32 offset, size;
	int ret=0;
	size_t tmpsize;

	FILE *f = NULL;

	u8 *filebuf = NULL;

	char **strconv_in = NULL;
	char **strconv_out = NULL;
	char *strptr, *strptr2;
	size_t filenamein_size, filenameout_maxsize;
	char filenameout[256];

	char arcpath[256];
	char tmp_path[256];
	char fs_path[256];

	for(pos=start_entry; pos<end_entry; pos++)
	{
		fn_offset_raw = getle32(table[pos].filename_offset);
		fn_offset = fn_offset_raw & 0xffffff;
		typeflag = fn_offset_raw & 0x01000000;
		if(typeflag)typeflag = 1;

		offset = getle32(table[pos].offset);
		size = getle32(table[pos].size);

		#ifdef DEBUG
		printf("0x%x: fn_offset=0x%x directory=%s offset=0x%x size=0x%x ", pos, fn_offset, typeflag ? "yes":"no", offset, size);
		#endif

		if(fn_offset >= table_size - filenametable_offset)
		{
			printf("The filename offset is invalid.\n");
			return 8;
		}

		memset(filenameout, 0, sizeof(filenameout));
		strptr2 = filenameout;
		strconv_out = &strptr2;
		filenameout_maxsize = sizeof(filenameout)-1;

		filenamein_size = 0;
		strptr = (char*)&tablebuf[filenametable_offset + fn_offset];
		strconv_in = &strptr;
		while(((u16*)strptr)[filenamein_size])filenamein_size++;
		filenamein_size++;
		filenamein_size*= 2;

		if(iconv(iconvdesc, strconv_in, &filenamein_size, strconv_out, &filenameout_maxsize)==-1)
		{
			printf("Failed to convert filename.\n");
			return 9;
		}

		#ifdef DEBUG
		printf("object name: %s\n", filenameout);
		#endif

		memset(arcpath, 0, sizeof(arcpath));
		snprintf(arcpath, sizeof(arcpath)-1, "%s%s", basearc_path, filenameout);

		memset(fs_path, 0, sizeof(fs_path));
		snprintf(fs_path, sizeof(fs_path)-1, "%s%s", fs_directory_path, arcpath);

		printf("%s\n", arcpath);

		if(typeflag)//directory
		{
			if(size > total_table_entries)
			{
				printf("The directory size value is invalid.\n");
				return 10;
			}

			makedir(fs_path);

			pos2 = strlen(basearc_path);
			memset(tmp_path, 0, sizeof(tmp_path));
			snprintf(tmp_path, sizeof(tmp_path)-1, "%s%s%c", basearc_path, filenameout, PATH_SEPERATOR);

			memset(basearc_path, 0, sizeof(basearc_path));
			strncpy(basearc_path, tmp_path, sizeof(basearc_path)-1);

			ret = extract_darc(pos+1, size);
			if(ret)return ret;

			pos = size-1;

			memset(&basearc_path[pos2], 0, sizeof(basearc_path) - pos2);
		}
		else//file
		{
			if((offset >= filestats.st_size) || (size >= filestats.st_size) || (offset+size > filestats.st_size))
			{
				printf("The offset/size fields for this file are invalid for this archive.\n");
				return 4;
			}

			fseek(farchive, offset, SEEK_SET);

			filebuf = malloc(size);
			if(filebuf==NULL)
			{
				printf("Failed to alloc the memory for the filebuf.\n");
				return 13;
			}
			memset(filebuf, 0, size);

			tmpsize = fread(filebuf, 1, size, farchive);
			if(tmpsize!=size)
			{
				printf("Failed to read the file-data from the archive.\n");
				free(filebuf);
				return 12;
			}

			f = fopen(fs_path, "wb");
			if(f==NULL)
			{
				printf("Failed to open the following filepath for writing: %s\n", fs_path);
				free(filebuf);
				return 10;
			}

			tmpsize = fwrite(filebuf, 1, size, f);
			fclose(f);
			free(filebuf);
			filebuf = NULL;

			if(tmpsize!=size)
			{
				printf("Failed to write the output file.\n");
				return 11;
			}
		}
	}

	return 0;
}

int build_darc_table(darcbuild_table_entry *startentry)
{
	DIR *dir;
	struct dirent *cur_dirent = NULL;
	struct stat objstats;

	u32 pos2;
	u32 direntry_index = 0;

	darcbuild_table_entry *curentry = startentry, *newentry = NULL;

	char **strconv_in = NULL;
	char **strconv_out = NULL;
	char *strptr, *strptr2;
	size_t filenamein_size, filenameout_maxsize;

	char tmp_path[256];
	char arcpath[256];
	char fs_path[256];

	dir = opendir(basearc_path);
	if(dir==NULL)
	{
		printf("Failed to open FS directory: %s\n", basearc_path);
		return 12;
	}

	while((cur_dirent = readdir(dir)))
	{
		if(strcmp(cur_dirent->d_name, ".")==0 || strcmp(cur_dirent->d_name, "..")==0)continue;

		if(direntry_index)
		{
			newentry = malloc(sizeof(darcbuild_table_entry));
			if(newentry==NULL)
			{
				printf("Failed to allocate memory for the table entry.\n");
				closedir(dir);
				return 5;
			}
			memset(newentry, 0, sizeof(darcbuild_table_entry));

			curentry->next = newentry;
			curentry = newentry;
			newentry = NULL;
		}

		memset(arcpath, 0, sizeof(arcpath));
		snprintf(arcpath, sizeof(arcpath)-1, "%s%s", basearc_path, cur_dirent->d_name);

		curentry->initialized = 1;

		strncpy(curentry->fs_path, arcpath, sizeof(curentry->fs_path)-1);

		printf("%s\n", arcpath);

		strptr = cur_dirent->d_name;
		strconv_in = &strptr;
		filenamein_size = strlen(strptr)+1;

		strptr2 = (char*)curentry->arc_name;
		strconv_out = &strptr2;
		filenameout_maxsize = sizeof(curentry->arc_name)-1;

		if(iconv(iconvdesc, strconv_in, &filenamein_size, strconv_out, &filenameout_maxsize)==-1)
		{
			printf("Failed to convert filename.\n");
			return 9;
		}

		if(stat(arcpath, &objstats)==-1)
		{
			printf("Failed to stat() the object.\n");
			closedir(dir);
			return 14;
		}

		if((objstats.st_mode & S_IFMT) == S_IFDIR)//directory
		{
			printf("directory\n");

			putle32(curentry->entry.filename_offset, 0x01000000);

			pos2 = strlen(basearc_path);
			memset(tmp_path, 0, sizeof(tmp_path));
			snprintf(tmp_path, sizeof(tmp_path)-1, "%s%s%c", basearc_path, cur_dirent->d_name, PATH_SEPERATOR);

			memset(basearc_path, 0, sizeof(basearc_path));
			strncpy(basearc_path, tmp_path, sizeof(basearc_path)-1);

			newentry = malloc(sizeof(darcbuild_table_entry));
			if(newentry==NULL)
			{
				printf("Failed to allocate memory for the table entry.\n");
				closedir(dir);
				return 5;
			}
			memset(newentry, 0, sizeof(darcbuild_table_entry));

			curentry->child = newentry;
			newentry->parent = curentry;

			build_darc_table(newentry);

			if(!newentry->initialized)//The directory is likely empty.
			{
				free(newentry);
				curentry->child = NULL;
			}

			newentry = NULL;

			memset(&basearc_path[pos2], 0, sizeof(basearc_path) - pos2);
		}
		else if((objstats.st_mode & S_IFMT) == S_IFREG)//regular file
		{
			printf("file\n");

			putle32(curentry->entry.size, objstats.st_size);
		}
		else
		{
			printf("Invalid FS object type.\n");
			closedir(dir);

			return 14;
		}

		direntry_index++;
	}

	closedir(dir);

	return 0;
}

int update_darc_table(darcbuild_table_entry *startentry)
{
	darcbuild_table_entry *curentry = startentry, *newentry;
	u32 pos;

	while(curentry)
	{
		curentry->entryindex = total_table_entries;

		putle32(curentry->entry.filename_offset, getle32(curentry->entry.filename_offset) + filenametable_curentryoffset);

		for(pos=0; pos<255; pos++)
		{
			if(curentry->arc_name[pos]==0)break;
		}

		filenametable_curentryoffset+= ((pos+1) * 2);

		total_table_entries++;

		if(curentry->child)update_darc_table(curentry->child);

		newentry = startentry->parent;
		while(newentry)
		{
			newentry->total_children++;
			newentry = newentry->parent;
		}

		newentry = curentry->next;
		curentry = newentry;
	}

	return 0;
}

int update_darc_table_final(darcbuild_table_entry *startentry)
{
	darcbuild_table_entry *curentry = startentry, *newentry;
	u32 type;

	while(curentry)
	{
		if((getle32(curentry->entry.filename_offset) & 0x01000000) == 0)//Check whether this entry is a file.
		{
			type = 0;

			if(strcmp(&curentry->fs_path[strlen(curentry->fs_path)-6], ".bclim")==0)type = 1;

			if(type)
			{
				filedata_offset = (filedata_offset + 0x7f) & ~0x7f;//Align the offset for .bclim(GPU texture files) to 0x80-bytes. This is the same alignment required by the GPU for textures' addresses.
			}

			putle32(curentry->entry.offset, filedata_offset);//Setup file data offset.
			filedata_offset+= getle32(curentry->entry.size);

			if(curentry->entryindex != total_table_entries-1)filedata_offset = (filedata_offset + 0x1f) & ~0x1f;
		}
		else
		{
			putle32(curentry->entry.offset, 0x1);//Setup the directory start/end values.
			putle32(curentry->entry.size, curentry->entryindex + 1 + curentry->total_children);
		}

		if(curentry->child)update_darc_table_final(curentry->child);

		newentry = curentry->next;
		curentry = newentry;
	}

	return 0;
}

int build_darc(darcbuild_table_entry *startentry)
{
	int ret=0;
	u32 tmp;

	ret = build_darc_table(startentry);
	if(ret)return ret;

	if(!startentry->initialized)
	{
		printf("Error: the first table actual entry wasn't initialized, the input root directory is likely empty.\n");
		return 16;
	}

	filenametable_curentryoffset = 0x6;
	total_table_entries = 2;

	ret = update_darc_table(startentry);
	if(ret)return ret;

	filenametable_offset = total_table_entries * sizeof(darc_table_entry);
	table_size = filenametable_offset + filenametable_curentryoffset;
	table_size = (table_size + 0x3) & ~0x3;
	filedata_offset = 0x1c + table_size;
	
	tmp = filedata_offset & 0x1f;
	if(tmp)
	{
		table_size+= 0x20 - tmp;
		filedata_offset+= 0x20 - tmp;
	}

	printf("Total table entries: 0x%x.\n", total_table_entries);

	ret = update_darc_table_final(startentry);
	if(ret)return ret;

	return 0;
}

int writeout_table_entries(darcbuild_table_entry *startentry, int type)
{
	darcbuild_table_entry *curentry = startentry, *newentry;
	size_t transfersize;
	u32 pos;
	u32 size;
	u32 tmp0=0, tmp1=0;
	FILE *f = NULL;
	u8 *filebuf = NULL;

	while(curentry)
	{
		if(type==0)
		{
			transfersize = fwrite(&curentry->entry, 1, sizeof(darc_table_entry), farchive);
			if(transfersize != sizeof(darc_table_entry))
			{
				printf("Failed to write the table entry to the file.\n");
				return 15;
			}
		}
		else if(type==1)
		{
			for(pos=0; pos<255; pos++)
			{
				if(curentry->arc_name[pos]==0)break;
			}

			pos = (pos+1) * 2;

			transfersize = fwrite(curentry->arc_name, 1, pos, farchive);
			if(transfersize != pos)
			{
				printf("Failed to write the object-name entry to the file.\n");
				return 15;
			}
		}
		else if(type==2)
		{
			if((getle32(curentry->entry.filename_offset) & 0x01000000) == 0)//Check whether this is a file.
			{
				size = getle32(curentry->entry.size);

				tmp0 = getle32(curentry->entry.offset);
				tmp1 = 0;
				while(ftell(farchive) < tmp0)
				{
					transfersize = fwrite(&tmp1, 1,1, farchive);
					if(transfersize != 1)
					{
						printf("Failed to write the filedata padding to the file.\n");
						return 15;
					}
				}

				filebuf = malloc(size);
				if(filebuf==NULL)
				{
					printf("Failed to allocate memory for the filebuf.\n");
					return 13;
				}
				memset(filebuf, 0, size);

				f = fopen(curentry->fs_path, "rb");
				if(f==NULL)
				{
					printf("Failed to open the following file for reading: %s\n", curentry->fs_path);
					free(filebuf);
					return 10;
				}

				transfersize = fread(filebuf, 1, size, f);
				fclose(f);
				if(transfersize != size)
				{
					printf("Failed to read the filedata from the FS file.\n");
					free(filebuf);
					return 15;
				}

				transfersize = fwrite(filebuf, 1, size, farchive);
				free(filebuf);
				if(transfersize != size)
				{
					printf("Failed to write the filedata to the archive file.\n");
					free(filebuf);
					return 15;
				}
			}
		}

		if(curentry->child)writeout_table_entries(curentry->child, type);

		newentry = curentry->next;
		curentry = newentry;
	}

	return 0;
}

void free_buildtable(darcbuild_table_entry *startentry)
{
	darcbuild_table_entry *curentry = startentry, *newentry;

	while(curentry)
	{
		if(curentry->child)free_buildtable(curentry->child);

		newentry = curentry->next;
		free(curentry);
		curentry = newentry;
	}
}

int main(int argc, char **argv)
{
	int ret=0;
	int processing_mode = -1;
	u32 tmp0, tmp1;
	size_t transfersize;

	darc_header header;
	darc_table_entry tmpentries[2];

	u16 tmpstr[3] = {0x0, 0x2e, 0x0};

	if(argc<4)
	{
		printf("%s by yellows8\n", argv[0]);
		printf("Tool for handling 3DS darc archive files.\n");
		printf("Usage:\n");
		printf("%s <--extract|--build> <archive file> <directory>\n", argv[0]);
		return 0;
	}

	if(strcmp(argv[1], "--extract")==0)
	{
		processing_mode = 0;
	}
	else if(strcmp(argv[1], "--build")==0)
	{
		processing_mode = 1;
	}
	else
	{
		printf("Invalid processing mode.\n");
		return 1;
	}

	if(processing_mode==0)
	{
		if(stat(argv[2], &filestats)==-1)
		{
			printf("Failed to stat the input archive file.\n");
			return 2;
		}
	}

	if(processing_mode==0)
	{
		farchive = fopen(argv[2], "rb");
		if(farchive==NULL)
		{
			printf("Failed to open the input archive file.\n");
			return 2;
		}
	}
	else
	{
		farchive = fopen(argv[2], "wb");
		if(farchive==NULL)
		{
			printf("Failed to open the output archive file.\n");
			return 2;
		}
	}

	memset(&header, 0, sizeof(darc_header));
	if(processing_mode==0)
	{
		if(fread(&header, 1, sizeof(darc_header), farchive) != sizeof(darc_header))
		{
			printf("Failed to read the header.\n");
			fclose(farchive);
			return 3;
		}

		if(getle32(header.magicnum) != 0x63726164)
		{
			printf("Invalid archive magicnum.\n");
			fclose(farchive);
			return 4;
		}

		if(getle16(header.bom) != 0xfeff)
		{
			printf("Invalid archive BOM.\n");
			fclose(farchive);
			return 4;
		}

		if(getle16(header.headerlen) != 0x1c)
		{
			printf("Invalid archive header length.\n");
			fclose(farchive);
			return 4;
		}

		if(getle32(header.version) != 0x1000000)
		{
			printf("Invalid archive version.\n");
			fclose(farchive);
			return 4;
		}
	}
	else
	{
		putle32(header.magicnum, 0x63726164);
		putle16(header.bom, 0xfeff);
		putle16(header.headerlen, 0x1c);
		putle32(header.version, 0x1000000);
		putle32(header.table_offset, 0x1c);
	}

	if(processing_mode==0)
	{
		tmp0 = getle32(header.table_offset);
		tmp1 = getle32(header.table_size);
		filedata_offset = getle32(header.filedata_offset);

		if((tmp0 >= filestats.st_size) || (tmp1 >= filestats.st_size) || (tmp0+tmp1 >= filestats.st_size) || (filedata_offset >= filestats.st_size))
		{
			printf("The offset/size fields in the header are invalid for this filesize.\n");
			fclose(farchive);
			return 4;
		}

		table_size = getle32(header.table_size);

		tablebuf = malloc(table_size);
		if(tablebuf==NULL)
		{
			printf("Failed to alloc memory for the table.\n");
			fclose(farchive);
			return 5;
		}
		memset(tablebuf, 0, table_size);
		table = (darc_table_entry*)tablebuf;

		if(fread(tablebuf, 1, table_size, farchive) != table_size)
		{
			printf("Failed to read the table.\n");
			fclose(farchive);
			free(tablebuf);
			return 3;
		}

		tmp0 = getle32(table[0].size) & 0xffffff;

		filenametable_offset = tmp0 * sizeof(darc_table_entry);
		if(getle32(header.table_size) < filenametable_offset)
		{
			printf("The size field for the first entry in the table is too large.\n");
			fclose(farchive);
			free(tablebuf);
			return 6;
		}

		total_table_entries = tmp0;

		printf("Total table entries: 0x%x.\n", total_table_entries);

		if(total_table_entries < 2)
		{
			printf("The total number of table entries is invalid.\n");
			fclose(farchive);
			free(tablebuf);
			return 7;
		}
	}

	if(processing_mode==0)iconvdesc = iconv_open("UTF-8", "UTF-16LE");
	if(processing_mode==1)iconvdesc = iconv_open("UTF-16LE", "UTF-8");
	if(iconvdesc==(iconv_t)-1)
	{
		printf("iconv_open() failed.\n");
		fclose(farchive);
		free(tablebuf);
		return 9;
	}

	fs_directory_path = argv[3];
	if(processing_mode==0)makedir(fs_directory_path);

	if(processing_mode==0)
	{
		memset(basearc_path, 0, sizeof(basearc_path));
		basearc_path[0] = PATH_SEPERATOR;

		ret = extract_darc(2, total_table_entries);
	}
	else if(processing_mode==1)
	{
		buildtable_firstentry = malloc(sizeof(darcbuild_table_entry));
		if(buildtable_firstentry==NULL)
		{
			printf("Failed to allocate memory for the first table entry.\n");
			fclose(farchive);
			iconv_close(iconvdesc);
			return 5;
		}
		memset(buildtable_firstentry, 0, sizeof(darcbuild_table_entry));

		memset(basearc_path, 0, sizeof(basearc_path));
		snprintf(basearc_path, sizeof(basearc_path)-1, "%s%c", fs_directory_path, PATH_SEPERATOR);

		printf("Building the archive filesystem...\n");

		ret = build_darc(buildtable_firstentry);

		if(ret==0)
		{
			tmp0 = 0x1c + table_size;
			putle32(header.table_size, table_size);
			putle32(header.filedata_offset, tmp0);
			putle32(header.filesize, filedata_offset);

			printf("Writing the actual archive to the file...\n");

			transfersize = fwrite(&header, 1, sizeof(darc_header), farchive);
			if(transfersize != sizeof(darc_header))
			{
				printf("Failed to write the darc header to the file.\n");
				ret = 15;
			}

			if(ret==0)
			{
				memset(tmpentries, 0, sizeof(tmpentries));

				//Setup the first two entries in the darc: <null> and ".". Offset values is 0, so no need to write 0 again.
				putle32(tmpentries[0].filename_offset, 0x01000000 + 0x0);
				putle32(tmpentries[1].filename_offset, 0x01000000 + 0x2);
				putle32(tmpentries[0].size, total_table_entries);
				putle32(tmpentries[1].size, total_table_entries);

				transfersize = fwrite(tmpentries, 1, sizeof(tmpentries), farchive);
				if(transfersize != sizeof(tmpentries))
				{
					printf("Failed to write the initial table entries to the file.\n");
					ret = 15;
				}
			}

			if(ret==0)ret = writeout_table_entries(buildtable_firstentry, 0);

			if(ret==0)
			{
				transfersize = fwrite(tmpstr, 1, sizeof(tmpstr), farchive);
				if(transfersize != sizeof(tmpstr))
				{
					printf("Failed to write the initial object-names to the file.\n");
					ret = 15;
				}
			}

			if(ret==0)ret = writeout_table_entries(buildtable_firstentry, 1);

			if(ret==0)
			{
				tmp1 = 0;
				while(ftell(farchive) < tmp0)
				{
					transfersize = fwrite(&tmp1, 1,1, farchive);
					if(transfersize != 1)
					{
						printf("Failed to write the initial filedata padding to the file.\n");
						ret = 15;
						break;
					}
				}
			}

			if(ret==0)ret = writeout_table_entries(buildtable_firstentry, 2);
		}

		free_buildtable(buildtable_firstentry);
		buildtable_firstentry = NULL;
	}

	iconv_close(iconvdesc);

	fclose(farchive);
	if(processing_mode==0)free(tablebuf);

	return ret;
}

