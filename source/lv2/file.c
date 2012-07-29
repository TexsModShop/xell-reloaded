/*
used for zlib support ...
*/

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <string.h>
#include <zlib.h>
#include <xetypes.h>
#include <elf/elf.h>
#include <network/network.h>
#include <console/console.h>
#include <sys/iosupport.h>
#include <usb/usbmain.h>
#include <ppc/timebase.h>
#include <xenon_nand/xenon_sfcx.h>
#include <xb360/xb360.h>

#include "config.h"
#include "file.h"
#include "kboot/kbootconf.h"
#include "tftp/tftp.h"

#define CHUNK 16384

//int i;

extern char dt_blob_start[];
extern char dt_blob_end[];

const unsigned char elfhdr[] = {0x7f, 'E', 'L', 'F'};
const unsigned char cpiohdr[] = {0x30, 0x37, 0x30, 0x37};
const unsigned char kboothdr[] = "#KBOOTCONFIG";

struct filenames filelist[] = {
    {"updxell.bin",TYPE_UPDXELL},
    {"kboot.conf",TYPE_KBOOT},
    {"xenon.elf",TYPE_ELF},
    {"xenon.z",TYPE_ELF},
    {"vmlinux",TYPE_ELF},
    {"updflash.bin",TYPE_NANDIMAGE},
    {NULL,NULL}
};
//Decompress a gzip file ...
int inflate_read(char *source,int len,char **dest,int * destsize, int gzip) {
	int ret;
	unsigned have;
	z_stream strm;
	unsigned char out[CHUNK];
	int totalsize = 0;

	/* allocate inflate state */
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	strm.avail_in = 0;
	strm.next_in = Z_NULL;
	
	if(gzip)
		ret = inflateInit2(&strm, 16+MAX_WBITS);
	else
		ret = inflateInit(&strm);
		
	if (ret != Z_OK)
		return ret;

	strm.avail_in = len;
	strm.next_in = (Bytef*)source;

	/* run inflate() on input until output buffer not full */
	do {
		strm.avail_out = CHUNK;
		strm.next_out = (Bytef*)out;
		ret = inflate(&strm, Z_NO_FLUSH);
		assert(ret != Z_STREAM_ERROR);  /* state not clobbered */
		switch (ret) {
		case Z_NEED_DICT:
			ret = Z_DATA_ERROR;     /* and fall through */
		case Z_DATA_ERROR:
		case Z_MEM_ERROR:
			inflateEnd(&strm);
			return ret;
		}
		have = CHUNK - strm.avail_out;
		totalsize += have;
                if (totalsize > ELF_MAXSIZE)
                    return Z_BUF_ERROR;
		//*dest = (char*)realloc(*dest,totalsize);
		memcpy(*dest + totalsize - have,out,have);
		*destsize = totalsize;
	} while (strm.avail_out == 0);

	/* clean up and return */
	(void)inflateEnd(&strm);
	return ret == Z_STREAM_END ? Z_OK : Z_DATA_ERROR;
}

void wait_and_cleanup_line()
{
	unsigned int w=0;
	console_get_dimensions(&w,NULL);
	
	char sp[w];

	memset(sp,' ',w);
	sp[w-1]='\0';

	uint64_t t=mftb();
	while(tb_diff_msec(mftb(),t)<200){ // yield to network
		network_poll();
	}
	
	printf("\r%s\r",sp);
}

int launch_file(void * addr, unsigned len, int filetype){
	int ret;
	unsigned char * gzip_file;
    switch (filetype){
            
		case TYPE_ELF:
			// check if addr point to a gzip file
            gzip_file = (unsigned char *)addr;
            if((gzip_file[0]==0x1F)&&(gzip_file[1]==0x8B)){
				//found a gzip file
                printf(" * Found a gzip file...\n");
                char * dest = malloc(ELF_MAXSIZE);
                int destsize = 0;
                if(inflate_read((char*)addr, len, &dest, &destsize, 1) == 0){
					//relocate elf ...
                    memcpy(addr,dest,destsize);
                    printf(" * Successfully unpacked...\n");
                    free(dest);
                    len=destsize;
                }
                else {
					printf(" * Unpacking failed...\n");
                    free(dest);
                    return -1;
                }
			}
            if (memcmp(addr, elfhdr, 4))
				return -1;
            printf(" * Launching ELF...\n");
            ret = elf_runWithDeviceTree(addr,len,dt_blob_start,dt_blob_end-dt_blob_start);
            break;
		case TYPE_INITRD:
			printf(" * Loading initrd into memory ...\n");
            ret = kernel_prepare_initrd(addr,len);
            break;
        case TYPE_KBOOT:
			printf(" * Loading kboot.conf ...\n");
            ret = try_kbootconf(addr,len);
            break;
        case TYPE_UPDXELL:
			if (memcmp(addr + XELL_FOOTER_OFFSET, XELL_FOOTER, XELL_FOOTER_LENGTH) || len != XELL_SIZE)
				return -1;
            printf(" * Loading UpdXeLL binary...\n");
            ret = updateXeLL(addr,len);
            break;
		default:
			printf("! Unsupported filetype supplied!\n");
	}
	return ret;
}

int try_load_file(char *filename, int filetype)
{
	int ret;
	if(filetype == TYPE_NANDIMAGE){
		try_rawflash(filename);
		return -1;
	}
	
	wait_and_cleanup_line();
	printf("Trying %s...",filename);
	
	int f = open(filename, O_RDONLY);
	if (f < 0)
	{
		return f;
	}

	struct stat s;
	fstat(f, &s);

	int size = s.st_size;
	void * buf=malloc(size);

	printf("\n * '%s' found, loading %d...\n",filename,size);
	int r = read(f, buf, size);
	if (r < 0)
	{
		close(f);
		free(buf);
		return r;
	}
	
	if (filetype == TYPE_ELF) {
		char * argv[] = {"xell", filename};
		int argc = sizeof (argv) / sizeof (char *);
		
		elf_setArgcArgv(argc, argv);
	}
	
	ret = launch_file(buf,r,filetype);

	free(buf);
	return ret;
}

void fileloop() {
        char filepath[255];

        int i,j=0;
        for (i = 3; i < 16; i++) {
                if (devoptab_list[i]->structSize) {
                        do{
                           sprintf(filepath, "%s:/%s", devoptab_list[i]->name,filelist[j].filename);
                           try_load_file(filepath,filelist[j].filetype);
                           j++;
                           usb_do_poll();
                        } while(filelist[j].filename != NULL);
                        j = 0;
                }
        }
}

void tftp_loop() {
    int i=0;
    do{
        wait_and_cleanup_line();
	printf("Trying TFTP %s:%s... ",boot_server_name(),filelist[i].filename);
	boot_tftp(boot_server_name(), filelist[i].filename, filelist[i].filetype);
        i++;
        network_poll();
    } while(filelist[i].filename != NULL);
    wait_and_cleanup_line();
    printf("Trying TFTP %s:%s... ",boot_server_name(),boot_file_name());
    /* Assume that bootfile delivered via DHCP is an ELF */
    boot_tftp(boot_server_name(),boot_file_name(),TYPE_ELF);
}