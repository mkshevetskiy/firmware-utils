// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ptgen - partition table generator
 * Copyright (C) 2006 by Felix Fietkau <nbd@nbd.name>
 *
 * uses parts of afdisk
 * Copyright (C) 2002 by David Roetzel <david@roetzel.de>
 *
 * UUID/GUID definition stolen from kernel/include/uapi/linux/uuid.h
 * Copyright (C) 2010, Intel Corp. Huang Ying <ying.huang@intel.com>
 */

#include <byteswap.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <inttypes.h>
#include <fcntl.h>
#include <stdint.h>
#include "cyg_crc.h"

#if __BYTE_ORDER == __BIG_ENDIAN
#define cpu_to_le16(x) bswap_16(x)
#define cpu_to_le32(x) bswap_32(x)
#define cpu_to_le64(x) bswap_64(x)
#elif __BYTE_ORDER == __LITTLE_ENDIAN
#define cpu_to_le16(x) (x)
#define cpu_to_le32(x) (x)
#define cpu_to_le64(x) (x)
#else
#error unknown endianness!
#endif

#define swap(a, b) \
	do { typeof(a) __tmp = (a); (a) = (b); (b) = __tmp; } while (0)

#define BIT(_x)		(1UL << (_x))

typedef struct {
	uint8_t b[16];
} guid_t;

#define GUID_INIT(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7)			\
((guid_t)								\
{{ (a) & 0xff, ((a) >> 8) & 0xff, ((a) >> 16) & 0xff, ((a) >> 24) & 0xff, \
   (b) & 0xff, ((b) >> 8) & 0xff,					\
   (c) & 0xff, ((c) >> 8) & 0xff,					\
   (d0), (d1), (d2), (d3), (d4), (d5), (d6), (d7) }})

#define GUID_STRING_LENGTH      36

#define GPT_SIGNATURE 0x5452415020494645ULL
#define GPT_REVISION 0x00010000

#define GUID_PARTITION_SYSTEM \
	GUID_INIT( 0xC12A7328, 0xF81F, 0x11d2, \
			0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B)

#define GUID_PARTITION_BASIC_DATA \
	GUID_INIT( 0xEBD0A0A2, 0xB9E5, 0x4433, \
			0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7)

#define GUID_PARTITION_BIOS_BOOT \
	GUID_INIT( 0x21686148, 0x6449, 0x6E6F, \
			0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49)

#define GUID_PARTITION_CHROME_OS_KERNEL \
	GUID_INIT( 0xFE3A2A5D, 0x4F32, 0x41A7, \
			0xB7, 0x25, 0xAC, 0xCC, 0x32, 0x85, 0xA3, 0x09)

#define GUID_PARTITION_LINUX_FIT_GUID \
	GUID_INIT( 0xcae9be83, 0xb15f, 0x49cc, \
			0x86, 0x3f, 0x08, 0x1b, 0x74, 0x4a, 0x2d, 0x93)

#define GUID_PARTITION_LINUX_FS_GUID \
	GUID_INIT( 0x0fc63daf, 0x8483, 0x4772, \
			0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4)

#define GUID_PARTITION_SIFIVE_SPL \
	GUID_INIT( 0x5b193300, 0xfc78, 0x40cd, \
			0x80, 0x02, 0xe8, 0x6c, 0x45, 0x58, 0x0b, 0x47)

#define GUID_PARTITION_SIFIVE_UBOOT \
	GUID_INIT( 0x2e54b353, 0x1271, 0x4842, \
			0x80, 0x6f, 0xe4, 0x36, 0xd6, 0xaf, 0x69, 0x85)

#define GPT_HEADER_SIZE         92
#define GPT_ENTRY_SIZE          128
#define GPT_ENTRY_MAX           128
#define GPT_ENTRY_NAME_SIZE     72
#define GPT_SIZE		GPT_ENTRY_SIZE * GPT_ENTRY_MAX / DISK_SECTOR_SIZE

#define GPT_ATTR_PLAT_REQUIRED  BIT(0)
#define GPT_ATTR_EFI_IGNORE     BIT(1)
#define GPT_ATTR_LEGACY_BOOT    BIT(2)

#define GPT_HEADER_SECTOR       1
#define GPT_FIRST_ENTRY_SECTOR  2

#define MBR_ENTRY_MAX           4
#define MBR_DISK_SIGNATURE_OFFSET  440
#define MBR_PARTITION_ENTRY_OFFSET 446
#define MBR_BOOT_SIGNATURE_OFFSET  510

#define DISK_SECTOR_SIZE        512

/* Partition table entry */
struct pte {
	uint8_t active;
	uint8_t chs_start[3];
	uint8_t type;
	uint8_t chs_end[3];
	uint32_t start;
	uint32_t length;
};

struct partinfo {
	unsigned long long actual_start;
	unsigned long long start;
	unsigned long long size;
	int type;
	int hybrid;
	char *name;
	short int required;
	bool has_guid;
	guid_t guid;
	uint64_t gattr;  /* GPT partition attributes */
};

/* GPT Partition table header */
struct gpth {
	uint64_t signature;
	uint32_t revision;
	uint32_t size;
	uint32_t crc32;
	uint32_t reserved;
	uint64_t self;
	uint64_t alternate;
	uint64_t first_usable;
	uint64_t last_usable;
	guid_t disk_guid;
	uint64_t first_entry;
	uint32_t entry_num;
	uint32_t entry_size;
	uint32_t entry_crc32;
} __attribute__((packed));

/* GPT Partition table entry */
struct gpte {
	guid_t type;
	guid_t guid;
	uint64_t start;
	uint64_t end;
	uint64_t attr;
	char name[GPT_ENTRY_NAME_SIZE];
} __attribute__((packed));


int verbose = 0;
int active = 1;
int heads = -1;
int sectors = -1;
int kb_align = 0;
bool ignore_null_sized_partition = false;
bool use_guid_partition_table = false;
struct partinfo parts[GPT_ENTRY_MAX];
char *filename = NULL;

int gpt_split_image = false;
int gpt_alternate = false;
uint64_t gpt_first_entry_sector = GPT_FIRST_ENTRY_SECTOR;
uint64_t gpt_last_usable_sector = 0;

/*
 * parse the size argument, which is either
 * a simple number (K assumed) or
 * K, M or G
 *
 * returns the size in KByte
 */
static long long to_kbytes(const char *string)
{
	int exp = 0;
	long long result;
	char *end;

	result = strtoull(string, &end, 0);
	switch (tolower(*end)) {
		case 'k' :
		case '\0' : exp = 0; break;
		case 'm' : exp = 1; break;
		case 'g' : exp = 2; break;
		default: return 0;
	}

	if (*end)
		end++;

	if (*end) {
		fputs("garbage after end of number\n", stderr);
		return 0;
	}

	/* result: number * 1024^(exp) */
	return result * (1 << (10 * exp));
}

/* convert the sector number into a CHS value for the partition table */
static void to_chs(long sect, unsigned char chs[3])
{
	int c,h,s;

	s = (sect % sectors) + 1;
	sect = sect / sectors;
	h = sect % heads;
	sect = sect / heads;
	c = sect;

	chs[0] = h;
	chs[1] = s | ((c >> 2) & 0xC0);
	chs[2] = c & 0xFF;

	return;
}

/* round the sector number up to the next cylinder */
static inline unsigned long round_to_cyl(long sect)
{
	int cyl_size = heads * sectors;

	return sect + cyl_size - (sect % cyl_size);
}

/* round the sector number up to the kb_align boundary */
static inline unsigned long round_to_kb(long sect) {
        return ((sect - 1) / kb_align + 1) * kb_align;
}

/* Compute a CRC for guid partition table */
static inline unsigned long gpt_crc32(void *buf, unsigned long len)
{
	return cyg_crc32_accumulate(~0L, buf, len) ^ ~0L;
}

/* Parse a guid string to guid_t struct */
static inline int guid_parse(char *buf, guid_t *guid)
{
	char b[4] = {0};
	char *p = buf;
	unsigned i = 0;
	if (strnlen(buf, GUID_STRING_LENGTH) != GUID_STRING_LENGTH)
		return -1;
	for (i = 0; i < sizeof(guid_t); i++) {
		if (*p == '-')
			p++;
		if (*p == '\0')
			return -1;
		memcpy(b, p, 2);
		guid->b[i] = strtol(b, 0, 16);
		p += 2;
	}
	swap(guid->b[0], guid->b[3]);
	swap(guid->b[1], guid->b[2]);
	swap(guid->b[4], guid->b[5]);
	swap(guid->b[6], guid->b[7]);
	return 0;
}

/*
 * Map GPT partition types to partition GUIDs.
 * NB: not all GPT partition types have an equivalent MBR type.
 */
static inline bool parse_gpt_parttype(const char *type, struct partinfo *part)
{
	if (!strcmp(type, "cros_kernel")) {
		part->has_guid = true;
		part->guid = GUID_PARTITION_CHROME_OS_KERNEL;
		/* Default attributes: bootable kernel. */
		part->gattr = (1ULL << 48) |  /* priority=1 */
			      (1ULL << 56);  /* success=1 */
		return true;
	}

	if (!strcmp(type, "sifiveu_spl")) {
		part->has_guid = true;
		part->guid = GUID_PARTITION_SIFIVE_SPL;
		return true;
	}

	if (!strcmp(type, "sifiveu_uboot")) {
		part->has_guid = true;
		part->guid = GUID_PARTITION_SIFIVE_UBOOT;
		return true;
	}

	return false;
}

/* init an utf-16 string from utf-8 string */
static inline void init_utf16(char *str, uint16_t *buf, unsigned bufsize)
{
	unsigned i, n = 0;
	for (i = 0; i < bufsize; i++) {
		if (str[n] == 0x00) {
			buf[i] = 0x00;
			return ;
		} else if ((str[n] & 0x80) == 0x00) {//0xxxxxxx
			buf[i] = cpu_to_le16(str[n++]);
		} else if ((str[n] & 0xE0) == 0xC0) {//110xxxxx
			buf[i] = cpu_to_le16((str[n] & 0x1F) << 6 | (str[n + 1] & 0x3F));
			n += 2;
		} else if ((str[n] & 0xF0) == 0xE0) {//1110xxxx
			buf[i] = cpu_to_le16((str[n] & 0x0F) << 12 | (str[n + 1] & 0x3F) << 6 | (str[n + 2] & 0x3F));
			n += 3;
		} else {
			buf[i] = cpu_to_le16('?');
			n++;
		}
	}
}

/* check the partition sizes and write the partition table */
static int gen_ptable(uint32_t signature, int nr)
{
	struct pte pte[MBR_ENTRY_MAX];
	unsigned long long start, len, sect = 0;
	int i, fd, ret = -1;

	memset(pte, 0, sizeof(struct pte) * MBR_ENTRY_MAX);
	for (i = 0; i < nr; i++) {
		if (!parts[i].size) {
			if (ignore_null_sized_partition)
				continue;
			fprintf(stderr, "Invalid size in partition %d!\n", i);
			return ret;
		}

		pte[i].active = ((i + 1) == active) ? 0x80 : 0;
		pte[i].type = parts[i].type;

		start = sect + sectors;
		if (parts[i].start != 0) {
			if (parts[i].start * 2 < start) {
				fprintf(stderr, "Invalid start %lld for partition %d!\n",
					parts[i].start, i);
				return ret;
			}
			start = parts[i].start * 2;
		} else if (kb_align != 0) {
			start = round_to_kb(start);
		}
		pte[i].start = cpu_to_le32(start);

		sect = start + parts[i].size * 2;
		if (kb_align == 0)
			sect = round_to_cyl(sect);
		pte[i].length = cpu_to_le32(len = sect - start);

		to_chs(start, pte[i].chs_start);
		to_chs(start + len - 1, pte[i].chs_end);

		if (verbose)
			fprintf(stderr, "Partition %d: start=%lld, end=%lld, size=%lld\n",
					i,
					start * DISK_SECTOR_SIZE,
					(start + len) * DISK_SECTOR_SIZE,
					len * DISK_SECTOR_SIZE);
		printf("%lld\n", start * DISK_SECTOR_SIZE);
		printf("%lld\n", len * DISK_SECTOR_SIZE);
	}

	if ((fd = open(filename, O_WRONLY|O_CREAT|O_TRUNC, 0644)) < 0) {
		fprintf(stderr, "Can't open output file '%s'\n",filename);
		return ret;
	}

	lseek(fd, MBR_DISK_SIGNATURE_OFFSET, SEEK_SET);
	if (write(fd, &signature, sizeof(signature)) != sizeof(signature)) {
		fputs("write failed.\n", stderr);
		goto fail;
	}

	lseek(fd, MBR_PARTITION_ENTRY_OFFSET, SEEK_SET);
	if (write(fd, pte, sizeof(struct pte) * MBR_ENTRY_MAX) != sizeof(struct pte) * MBR_ENTRY_MAX) {
		fputs("write failed.\n", stderr);
		goto fail;
	}
	lseek(fd, MBR_BOOT_SIGNATURE_OFFSET, SEEK_SET);
	if (write(fd, "\x55\xaa", 2) != 2) {
		fputs("write failed.\n", stderr);
		goto fail;
	}

	ret = 0;
fail:
	close(fd);
	return ret;
}

/* check the partition sizes and write the guid partition table */
static int gen_gptable(uint32_t signature, guid_t guid, unsigned nr)
{
	struct pte pte[MBR_ENTRY_MAX];
	struct gpth gpth = {
		.signature = cpu_to_le64(GPT_SIGNATURE),
		.revision = cpu_to_le32(GPT_REVISION),
		.size = cpu_to_le32(GPT_HEADER_SIZE),
		.self = cpu_to_le64(GPT_HEADER_SECTOR),
		.first_usable = cpu_to_le64(gpt_first_entry_sector + GPT_SIZE),
		.first_entry = cpu_to_le64(gpt_first_entry_sector),
		.disk_guid = guid,
		.entry_num = cpu_to_le32(GPT_ENTRY_MAX),
		.entry_size = cpu_to_le32(GPT_ENTRY_SIZE),
	};
	struct gpte  gpte[GPT_ENTRY_MAX];
	uint64_t start, end;
	uint64_t sect = GPT_SIZE + gpt_first_entry_sector;
	int fd, ret = -1;
	unsigned i, pmbr = 1;
	char img_name[strlen(filename) + 20];

	memset(pte, 0, sizeof(struct pte) * MBR_ENTRY_MAX);
	memset(gpte, 0, GPT_ENTRY_SIZE * GPT_ENTRY_MAX);
	for (i = 0; i < nr; i++) {
		if (!parts[i].size) {
			if (ignore_null_sized_partition)
				continue;
			fprintf(stderr, "Invalid size in partition %d!\n", i);
			return ret;
		}
		start = sect;
		if (parts[i].start != 0) {
			if (parts[i].start * 2 < start) {
				fprintf(stderr, "Invalid start %lld for partition %d!\n",
					parts[i].start, i);
				return ret;
			}
			start = parts[i].start * 2;
		} else if (kb_align != 0) {
			start = round_to_kb(start);
		}
		if ((gpt_last_usable_sector > 0) &&
		    (start + parts[i].size * 2 > gpt_last_usable_sector + 1)) {
				fprintf(stderr, "Partition %d ends after last usable sector %ld\n",
					i, gpt_last_usable_sector);
				return ret;
		}
		parts[i].actual_start = start;
		gpte[i].start = cpu_to_le64(start);

		sect = start + parts[i].size * 2;
		gpte[i].end = cpu_to_le64(sect -1);
		gpte[i].guid = guid;
		gpte[i].guid.b[sizeof(guid_t) -1] += i + 1;
		gpte[i].type = parts[i].guid;

		if (parts[i].hybrid && pmbr < MBR_ENTRY_MAX) {
			pte[pmbr].active = ((i + 1) == active) ? 0x80 : 0;
			pte[pmbr].type = parts[i].type;
			pte[pmbr].start = cpu_to_le32(start);
			pte[pmbr].length = cpu_to_le32(sect - start);
			to_chs(start, pte[1].chs_start);
			to_chs(sect - 1, pte[1].chs_end);
			pmbr++;
		}
		gpte[i].attr = parts[i].gattr;

		if (parts[i].name)
			init_utf16(parts[i].name, (uint16_t *)gpte[i].name, GPT_ENTRY_NAME_SIZE / sizeof(uint16_t));

		if ((i + 1) == (unsigned)active)
			gpte[i].attr |= GPT_ATTR_LEGACY_BOOT;

		if (parts[i].required)
			gpte[i].attr |= GPT_ATTR_PLAT_REQUIRED;

		if (verbose)
			fprintf(stderr, "Partition %d: start=%" PRIu64 ", end=%" PRIu64 ", size=%"  PRIu64 "\n",
					i,
					start * DISK_SECTOR_SIZE, sect * DISK_SECTOR_SIZE,
					(sect - start) * DISK_SECTOR_SIZE);
		printf("%" PRIu64 "\n", start * DISK_SECTOR_SIZE);
		printf("%" PRIu64 "\n", (sect - start) * DISK_SECTOR_SIZE);
	}

	if ((parts[0].start != 0) &&
	    (parts[0].actual_start > gpt_first_entry_sector + GPT_SIZE)) {
		gpte[GPT_ENTRY_MAX - 1].start = cpu_to_le64(gpt_first_entry_sector + GPT_SIZE);
		gpte[GPT_ENTRY_MAX - 1].end = cpu_to_le64(parts[0].actual_start - 1);
		gpte[GPT_ENTRY_MAX - 1].type = GUID_PARTITION_BIOS_BOOT;
		gpte[GPT_ENTRY_MAX - 1].guid = guid;
		gpte[GPT_ENTRY_MAX - 1].guid.b[sizeof(guid_t) -1] += GPT_ENTRY_MAX;
	}

	if (gpt_last_usable_sector == 0)
		gpt_last_usable_sector = sect - 1;

	end = gpt_last_usable_sector + GPT_SIZE + 1;

	pte[0].type = 0xEE;
	pte[0].start = cpu_to_le32(GPT_HEADER_SECTOR);
	pte[0].length = cpu_to_le32(end + 1 - GPT_HEADER_SECTOR);
	to_chs(GPT_HEADER_SECTOR, pte[0].chs_start);
	to_chs(end, pte[0].chs_end);

	gpth.last_usable = cpu_to_le64(gpt_last_usable_sector);
	gpth.alternate = cpu_to_le64(end);
	gpth.entry_crc32 = cpu_to_le32(gpt_crc32(gpte, GPT_ENTRY_SIZE * GPT_ENTRY_MAX));
	gpth.crc32 = cpu_to_le32(gpt_crc32((char *)&gpth, GPT_HEADER_SIZE));

	if (verbose)
		fprintf(stderr, "PartitionEntryLBA=%" PRIu64 ", FirstUsableLBA=%" PRIu64 ", LastUsableLBA=%" PRIu64 "\n",
			gpt_first_entry_sector, gpt_first_entry_sector + GPT_SIZE, gpt_last_usable_sector);

	if (!gpt_split_image)
		strcpy(img_name, filename);
	else
		snprintf(img_name, sizeof(img_name), "%s.start", filename);

	if ((fd = open(img_name, O_WRONLY|O_CREAT|O_TRUNC, 0644)) < 0) {
		fprintf(stderr, "Can't open output file '%s'\n",img_name);
		return ret;
	}

	lseek(fd, MBR_DISK_SIGNATURE_OFFSET, SEEK_SET);
	if (write(fd, &signature, sizeof(signature)) != sizeof(signature)) {
		fputs("write failed.\n", stderr);
		goto fail;
	}

	lseek(fd, MBR_PARTITION_ENTRY_OFFSET, SEEK_SET);
	if (write(fd, pte, sizeof(struct pte) * MBR_ENTRY_MAX) != sizeof(struct pte) * MBR_ENTRY_MAX) {
		fputs("write failed.\n", stderr);
		goto fail;
	}

	lseek(fd, MBR_BOOT_SIGNATURE_OFFSET, SEEK_SET);
	if (write(fd, "\x55\xaa", 2) != 2) {
		fputs("write failed.\n", stderr);
		goto fail;
	}

	if (write(fd, &gpth, GPT_HEADER_SIZE) != GPT_HEADER_SIZE) {
		fputs("write failed.\n", stderr);
		goto fail;
	}

	lseek(fd, 2 * DISK_SECTOR_SIZE - 1, SEEK_SET);
	if (write(fd, "\x00", 1) != 1) {
		fputs("write failed.\n", stderr);
		goto fail;
	}

	if (!gpt_split_image || (gpt_first_entry_sector == GPT_FIRST_ENTRY_SECTOR)) {
		lseek(fd, gpt_first_entry_sector * DISK_SECTOR_SIZE, SEEK_SET);
	} else {
		close(fd);

		snprintf(img_name, sizeof(img_name), "%s.entry", filename);
		if ((fd = open(img_name, O_WRONLY|O_CREAT|O_TRUNC, 0644)) < 0) {
			fprintf(stderr, "Can't open output file '%s'\n",img_name);
			return ret;
		}
	}

	if (write(fd, &gpte, GPT_ENTRY_SIZE * GPT_ENTRY_MAX) != GPT_ENTRY_SIZE * GPT_ENTRY_MAX) {
		fputs("write failed.\n", stderr);
		goto fail;
	}

	if (gpt_alternate) {
		/* The alternate partition table (We omit it by default) */
		swap(gpth.self, gpth.alternate);
		gpth.first_entry = cpu_to_le64(end - GPT_ENTRY_SIZE * GPT_ENTRY_MAX / DISK_SECTOR_SIZE),
		gpth.crc32 = 0;
		gpth.crc32 = cpu_to_le32(gpt_crc32(&gpth, GPT_HEADER_SIZE));

		if (!gpt_split_image) {
			lseek(fd, end * DISK_SECTOR_SIZE - GPT_ENTRY_SIZE * GPT_ENTRY_MAX, SEEK_SET);
		} else {
			close(fd);

			end = GPT_SIZE;
			snprintf(img_name, sizeof(img_name), "%s.end", filename);
			if ((fd = open(img_name, O_WRONLY|O_CREAT|O_TRUNC, 0644)) < 0) {
				fprintf(stderr, "Can't open output file '%s'\n",img_name);
				return ret;
			}
		}

		if (write(fd, &gpte, GPT_ENTRY_SIZE * GPT_ENTRY_MAX) != GPT_ENTRY_SIZE * GPT_ENTRY_MAX) {
			fputs("write failed.\n", stderr);
			goto fail;
		}

		lseek(fd, end * DISK_SECTOR_SIZE, SEEK_SET);
		if (write(fd, &gpth, GPT_HEADER_SIZE) != GPT_HEADER_SIZE) {
			fputs("write failed.\n", stderr);
			goto fail;
		}
		lseek(fd, (end + 1) * DISK_SECTOR_SIZE -1, SEEK_SET);
		if (write(fd, "\x00", 1) != 1) {
			fputs("write failed.\n", stderr);
			goto fail;
		}
	}

	ret = 0;
fail:
	close(fd);
	return ret;
}

static void usage(char *prog)
{
	fprintf(stderr, "Usage: %s [-v] [-n] [-b] [-g] -h <heads> -s <sectors> -o <outputfile>\n"
			"          [-a <part number>] [-l <align kB>] [-G <guid>]\n"
			"          [-e <gpt_entry_offset>] [-d <gpt_disk_size>]\n"
			"          [[-t <type> | -T <GPT part type>] [-r] [-N <name>] -p <size>[@<start>]...] \n", prog);

	exit(EXIT_FAILURE);
}

static guid_t type_to_guid_and_name(unsigned char type, char **name)
{
	guid_t guid = GUID_PARTITION_BASIC_DATA;

	switch (type) {
		case 0xef:
			if(*name == NULL)
				*name = "EFI System Partition";
			guid = GUID_PARTITION_SYSTEM;
			break;
		case 0x83:
			guid = GUID_PARTITION_LINUX_FS_GUID;
			break;
		case 0x2e:
			guid = GUID_PARTITION_LINUX_FIT_GUID;
			break;
	}

	return guid;
}

int main (int argc, char **argv)
{
	unsigned char type = 0x83;
	char *p;
	int ch;
	int part = 0;
	char *name = NULL;
	unsigned short int hybrid = 0, required = 0;
	uint64_t total_sectors;
	uint32_t signature = 0x5452574F; /* 'OWRT' */
	guid_t guid = GUID_INIT( signature, 0x2211, 0x4433, \
			0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0x00);

	while ((ch = getopt(argc, argv, "h:s:p:a:t:T:o:vnbHN:gl:rS:G:e:d:")) != -1) {
		switch (ch) {
		case 'o':
			filename = optarg;
			break;
		case 'v':
			verbose++;
			break;
		case 'n':
			ignore_null_sized_partition = true;
			break;
		case 'g':
			use_guid_partition_table = 1;
			break;
		case 'H':
			hybrid = 1;
			break;
		case 'e':
			/* based on DISK_SECTOR_SIZE = 512 */
			gpt_first_entry_sector = 2 * to_kbytes(optarg);
			if (gpt_first_entry_sector < GPT_FIRST_ENTRY_SECTOR) {
				fprintf(stderr, "GPT First Entry offset must not be smaller than %d KBytes\n",
					GPT_FIRST_ENTRY_SECTOR / 2);
				exit(EXIT_FAILURE);
			}
			break;
		case 'd':
			/*
			 * Zero disk_size is specially allowed. It means: find a disk size
			 * on the base of provided partitions list.
			 *
			 * based on DISK_SECTOR_SIZE = 512
			 */
			gpt_alternate = true;
			total_sectors = 2 * to_kbytes(optarg);
			if (total_sectors != 0) {
				if (total_sectors <= 2 * GPT_SIZE + 3) {
					fprintf(stderr, "GPT disk size must be larger than %d KBytes\n",
						(2 * GPT_SIZE + 3) * DISK_SECTOR_SIZE / 1024);
					exit(EXIT_FAILURE);
				}
				gpt_last_usable_sector = total_sectors - GPT_SIZE - 2;
			}
			break;
		case 'b':
			gpt_alternate = true;
			gpt_split_image = true;
			break;
		case 'h':
			heads = (int)strtoul(optarg, NULL, 0);
			break;
		case 's':
			sectors = (int)strtoul(optarg, NULL, 0);
			break;
		case 'p':
			if (part > GPT_ENTRY_MAX - 1 || (!use_guid_partition_table && part > 3)) {
				fputs("Too many partitions\n", stderr);
				exit(EXIT_FAILURE);
			}
			p = strchr(optarg, '@');
			if (p) {
				*(p++) = 0;
				parts[part].start = to_kbytes(p);
			}
			if (!parts[part].has_guid)
				parts[part].guid = type_to_guid_and_name(type, &name);

			parts[part].size = to_kbytes(optarg);
			parts[part].required = required;
			parts[part].name = name;
			parts[part].hybrid = hybrid;
			fprintf(stderr, "part %lld %lld\n", parts[part].start, parts[part].size);
			parts[part++].type = type;
			/*
			 * reset 'name','required' and 'hybrid'
			 * 'type' is deliberately inherited from the previous delcaration
			 */
			name = NULL;
			required = 0;
			hybrid = 0;
			break;
		case 'N':
			name = optarg;
			break;
		case 'r':
			required = 1;
			break;
		case 't':
			type = (char)strtoul(optarg, NULL, 16);
			break;
		case 'a':
			active = (int)strtoul(optarg, NULL, 0);
			break;
		case 'l':
			kb_align = (int)strtoul(optarg, NULL, 0) * 2;
			break;
		case 'S':
			signature = strtoul(optarg, NULL, 0);
			break;
		case 'T':
			if (!parse_gpt_parttype(optarg, &parts[part])) {
				fprintf(stderr,
					"Invalid GPT partition type \"%s\"\n",
					optarg);
				exit(EXIT_FAILURE);
			}
			break;
		case 'G':
			if (guid_parse(optarg, &guid)) {
				fputs("Invalid guid string\n", stderr);
				exit(EXIT_FAILURE);
			}
			break;
		case '?':
		default:
			usage(argv[0]);
		}
	}
	argc -= optind;
	if (argc || (!use_guid_partition_table && ((heads <= 0) || (sectors <= 0))) || !filename)
		usage(argv[0]);

	if ((use_guid_partition_table && active > GPT_ENTRY_MAX) ||
	    (!use_guid_partition_table && active > MBR_ENTRY_MAX) ||
	    active < 0)
		active  = 0;

	if (use_guid_partition_table) {
		heads = 254;
		sectors = 63;
		return gen_gptable(signature, guid, part) ? EXIT_FAILURE : EXIT_SUCCESS;
	}

	return gen_ptable(signature, part) ? EXIT_FAILURE : EXIT_SUCCESS;
}
