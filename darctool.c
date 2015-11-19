#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <iconv.h>

#include "utils.h"

//Build with: gcc -o darctool darctool.c utils.c -liconv

typedef struct {
	u8 magicnum[4];//0x63726164 "darc"
	u8 bom[2];
	u8 headerlen[2];
	u8 version[4];
	u8 filesize[4];
	u8 table_offset[4];
	u8 table_size[4];
	u8 filedata_offset[4];
} darc_header;

typedef struct {
	u8 filename_offset[4];
	u8 offset[4];
	u8 size[4];
} darc_table;

struct stat filestats;

u32 table_size = 0;
u32 filenametable_offset = 0;
u32 total_table_entries = 0;
u32 fn_offset_raw = 0, fn_offset = 0;
u32 filedata_offset = 0, filedata_size = 0;

u8 *tablebuf;
darc_table *table;

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

int main(int argc, char **argv)
{
	int ret=0;
	int processing_mode = -1;
	u32 tmp0, tmp1;

	darc_header header;

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

	if(stat(argv[2], &filestats)==-1)
	{
		printf("Failed to stat the input archive file.\n");
		return 2;
	}

	farchive = fopen(argv[2], "rb");
	if(farchive==NULL)
	{
		printf("Failed to open the input archive file.\n");
		return 2;
	}

	memset(&header, 0, sizeof(darc_header));
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
	table = (darc_table*)tablebuf;

	if(fread(tablebuf, 1, table_size, farchive) != table_size)
	{
		printf("Failed to read the table.\n");
		fclose(farchive);
		free(tablebuf);
		return 3;
	}

	tmp0 = getle32(table[0].size) & 0xffffff;

	filenametable_offset = tmp0 * sizeof(darc_table);
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

	iconvdesc = iconv_open("UTF-8", "UTF-16LE");
	if(iconvdesc==(iconv_t)-1)
	{
		printf("iconv_open() failed.\n");
		fclose(farchive);
		free(tablebuf);
		return 9;
	}

	memset(basearc_path, 0, sizeof(basearc_path));
	basearc_path[0] = PATH_SEPERATOR;

	fs_directory_path = argv[3];
	makedir(fs_directory_path);

	ret = extract_darc(2, total_table_entries);

	iconv_close(iconvdesc);

	fclose(farchive);
	free(tablebuf);

	return ret;
}

