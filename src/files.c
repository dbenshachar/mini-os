#include "files.h"
#include "mmio.h"

#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE 0x20
#define FAT_ATTR_LONG_NAME 0x0f
#define FAT_ATTR_VOLUME_ID 0x08

#define FAT_ENTRY_FREE 0x00000000u
#define FAT_ENTRY_EOC 0x0fffffffu
#define FAT_ENTRY_MASK 0x0fffffffu

#define MAX_OPEN_FILES 16
#define MAX_PATH_PART 12

typedef struct {
    uint32_t partition_lba;
    uint32_t sectors_per_fat;
    uint32_t first_fat_sector;
    uint32_t first_data_sector;
    uint32_t root_cluster;
    uint32_t total_clusters;
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint8_t fat_count;
    int mounted;
} Fat32Volume;

typedef struct {
    uint32_t sector;
    uint16_t offset;
    uint8_t data[32];
} DirEntryLoc;

typedef struct {
    int used;
    int flags;
    uint32_t first_cluster;
    uint32_t size;
    uint32_t offset;
    DirEntryLoc entry;
} OpenFile;

static Fat32Volume volume;
static OpenFile open_files[MAX_OPEN_FILES];
static uint8_t scratch[FS_SECTOR_SIZE];
static uint8_t scratch2[FS_SECTOR_SIZE];

/* Reads a little-endian 16-bit value from an on-disk byte buffer. */
static uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/* Reads a little-endian 32-bit value from an on-disk byte buffer. */
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}

/* Writes a 16-bit value to an on-disk byte buffer in little-endian order. */
static void put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}

/* Writes a 32-bit value to an on-disk byte buffer in little-endian order. */
static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

/* Copies a byte range without depending on a hosted C library. */
static void mem_copy(void *dst, const void *src, unsigned long count) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (count--) *d++ = *s++;
}

/* Fills a byte range without depending on a hosted C library. */
static void mem_set(void *dst, uint8_t value, unsigned long count) {
    uint8_t *d = (uint8_t *)dst;
    while (count--) *d++ = value;
}

/* Compares two byte ranges and returns 1 only when every byte matches. */
static int mem_eq(const uint8_t *a, const uint8_t *b, unsigned long count) {
    while (count--) {
        if (*a++ != *b++) return 0;
    }
    return 1;
}

/* Converts an ASCII lowercase letter to uppercase and leaves other bytes unchanged. */
static char upper_char(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - ('a' - 'A'));
    return c;
}

/* Returns whether a character is accepted as a path separator. */
static int is_path_sep(char c) {
    return c == '/' || c == '\\';
}

/*
 * Returns whether a character is legal in the supported FAT short-name subset.
 * The filesystem only supports 8.3 names, so this accepts uppercase letters,
 * digits, and a conservative set of punctuation that FAT short names allow.
 */
static int valid_name_char(char c) {
    c = upper_char(c);
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    if (c == '$' || c == '%' || c == '\'' || c == '-' || c == '_' ||
        c == '@' || c == '~' || c == '`' || c == '!' || c == '(' ||
        c == ')' || c == '{' || c == '}' || c == '^' || c == '#' ||
        c == '&') return 1;
    return 0;
}

/*
 * Converts one path component into an uppercase FAT 8.3 short name.
 * The base name and extension are padded with spaces in the exact 11-byte
 * directory-entry format. Components with multiple dots, empty names, or names
 * exceeding 8.3 limits are rejected.
 */
static int make_short_name(const char *part, unsigned long len, uint8_t out[11]) {
    unsigned long i;
    unsigned long base_len = 0;
    unsigned long ext_len = 0;
    int seen_dot = 0;

    if (len == 0) return -1;
    if (len == 1 && part[0] == '.') return -1;
    if (len == 2 && part[0] == '.' && part[1] == '.') return -1;

    for (i = 0; i < 11; i++) out[i] = ' ';

    for (i = 0; i < len; i++) {
        char c = part[i];
        if (c == '.') {
            if (seen_dot) return -1;
            seen_dot = 1;
            continue;
        }
        if (!valid_name_char(c)) return -1;
        c = upper_char(c);
        if (!seen_dot) {
            if (base_len >= 8) return -1;
            out[base_len++] = (uint8_t)c;
        } else {
            if (ext_len >= 3) return -1;
            out[8 + ext_len++] = (uint8_t)c;
        }
    }

    if (base_len == 0) return -1;
    return 0;
}

/*
 * Parses the next path component and returns it as an 8.3 short name.
 * Leading separators are skipped, the caller's path pointer advances to the next
 * component, and last is set when no more components remain. Invalid or too-long
 * components fail before any FAT lookup is attempted.
 */
static int next_path_part(const char **path, uint8_t short_name[11], int *last) {
    const char *p = *path;
    const char *start;
    unsigned long len = 0;

    while (is_path_sep(*p)) p++;
    if (*p == '\0') return 0;

    start = p;
    while (*p != '\0' && !is_path_sep(*p)) {
        if (len >= MAX_PATH_PART) return -1;
        len++;
        p++;
    }

    if (make_short_name(start, len, short_name) != 0) return -1;

    while (is_path_sep(*p)) p++;
    *last = (*p == '\0');
    *path = p;
    return 1;
}

/* Converts a FAT data cluster number to its first absolute disk sector. */
static uint32_t cluster_to_sector(uint32_t cluster) {
    return volume.first_data_sector + ((cluster - 2) * volume.sectors_per_cluster);
}

/* Returns whether a FAT entry marks the end of a cluster chain. */
static int is_eoc(uint32_t entry) {
    return (entry & FAT_ENTRY_MASK) >= 0x0ffffff8u;
}

/* Reads one disk sector through the block layer. */
static int read_sector(uint32_t lba, uint8_t *buf) {
    return disk_read_sector(lba, buf);
}

/* Writes one disk sector through the block layer. */
static int write_sector(uint32_t lba, const uint8_t *buf) {
    return disk_write_sector(lba, buf);
}

/* Reads one FAT entry and masks it to the 28 bits used by FAT32 cluster chains. */
static int fat_get(uint32_t cluster, uint32_t *value) {
    uint32_t offset = cluster * 4;
    uint32_t sector = volume.first_fat_sector + (offset / FS_SECTOR_SIZE);
    uint32_t in_sector = offset % FS_SECTOR_SIZE;

    if (read_sector(sector, scratch) != 0) return -1;
    *value = le32(&scratch[in_sector]) & FAT_ENTRY_MASK;
    return 0;
}

/*
 * Updates one FAT copy for a cluster while preserving the high reserved bits.
 * FAT32 stores four-byte entries, so this locates the containing sector, patches
 * the entry in the scratch buffer, and writes the sector back.
 */
static int fat_set_one(uint32_t fat_index, uint32_t cluster, uint32_t value) {
    uint32_t offset = cluster * 4;
    uint32_t sector = volume.first_fat_sector +
        (fat_index * volume.sectors_per_fat) +
        (offset / FS_SECTOR_SIZE);
    uint32_t in_sector = offset % FS_SECTOR_SIZE;
    uint32_t old_value;

    if (read_sector(sector, scratch) != 0) return -1;
    old_value = le32(&scratch[in_sector]) & 0xf0000000u;
    put_le32(&scratch[in_sector], old_value | (value & FAT_ENTRY_MASK));
    return write_sector(sector, scratch);
}

/* Writes the same FAT entry value to every FAT copy in the mounted volume. */
static int fat_set(uint32_t cluster, uint32_t value) {
    uint8_t i;
    for (i = 0; i < volume.fat_count; i++) {
        if (fat_set_one(i, cluster, value) != 0) return -1;
    }
    return 0;
}

/* Zeroes every sector in a cluster before it is reused for file or directory data. */
static int clear_cluster(uint32_t cluster) {
    uint8_t s;
    mem_set(scratch2, 0, FS_SECTOR_SIZE);
    for (s = 0; s < volume.sectors_per_cluster; s++) {
        if (write_sector(cluster_to_sector(cluster) + s, scratch2) != 0) return -1;
    }
    return 0;
}

/*
 * Finds a free cluster, marks it as end-of-chain, clears its contents, and returns it.
 * The scan starts at cluster 2 because FAT32 reserves clusters 0 and 1. If no free
 * entry is found or metadata writes fail, allocation reports an error.
 */
static int alloc_cluster(uint32_t *cluster_out) {
    uint32_t cluster;
    uint32_t value;

    for (cluster = 2; cluster < volume.total_clusters + 2; cluster++) {
        if (fat_get(cluster, &value) != 0) return -1;
        if (value == FAT_ENTRY_FREE) {
            if (fat_set(cluster, FAT_ENTRY_EOC) != 0) return -1;
            if (clear_cluster(cluster) != 0) return -1;
            *cluster_out = cluster;
            return 0;
        }
    }
    return -1;
}

/*
 * Frees every cluster in a FAT chain starting at first_cluster.
 * Each entry is read before being cleared so the next link is preserved long enough
 * to continue walking the chain. The loop stops when it reaches an end-of-chain marker.
 */
static int free_chain(uint32_t first_cluster) {
    uint32_t cluster = first_cluster;
    uint32_t next;

    while (cluster >= 2 && cluster < volume.total_clusters + 2) {
        if (fat_get(cluster, &next) != 0) return -1;
        if (fat_set(cluster, FAT_ENTRY_FREE) != 0) return -1;
        if (is_eoc(next)) break;
        cluster = next;
    }
    return 0;
}

/* Extracts the FAT32 high and low cluster fields from a directory entry. */
static uint32_t entry_first_cluster(const uint8_t *entry) {
    return ((uint32_t)le16(&entry[20]) << 16) | le16(&entry[26]);
}

/* Stores a cluster number into the high and low cluster fields of a directory entry. */
static void set_entry_first_cluster(uint8_t *entry, uint32_t cluster) {
    put_le16(&entry[20], (uint16_t)(cluster >> 16));
    put_le16(&entry[26], (uint16_t)(cluster & 0xffff));
}

/* Loads the current on-disk contents of a directory entry location into the locator. */
static int read_entry(DirEntryLoc *loc) {
    if (read_sector(loc->sector, scratch) != 0) return -1;
    mem_copy(loc->data, &scratch[loc->offset], 32);
    return 0;
}

/* Writes a locator's cached directory entry bytes back to its disk location. */
static int write_entry(const DirEntryLoc *loc) {
    if (read_sector(loc->sector, scratch) != 0) return -1;
    mem_copy(&scratch[loc->offset], loc->data, 32);
    return write_sector(loc->sector, scratch);
}

/* Marks a directory entry as deleted by replacing its first byte with FAT's 0xe5 marker. */
static int mark_entry_deleted(const DirEntryLoc *loc) {
    if (read_sector(loc->sector, scratch) != 0) return -1;
    scratch[loc->offset] = 0xe5;
    return write_sector(loc->sector, scratch);
}

/* Builds a 32-byte short-name directory entry with the requested attributes and cluster. */
static void make_dir_entry(uint8_t *entry, const uint8_t name[11], uint8_t attr, uint32_t cluster, uint32_t size) {
    mem_set(entry, 0, 32);
    mem_copy(entry, name, 11);
    entry[11] = attr;
    set_entry_first_cluster(entry, cluster);
    put_le32(&entry[28], size);
}

/*
 * Searches a directory cluster chain for an entry matching an 8.3 short name.
 * Each sector is scanned in 32-byte directory-entry slots while deleted and long-name
 * entries are ignored. A zero first byte means the directory has no later live entries,
 * so lookup can stop early.
 */
static int dir_find(uint32_t dir_cluster, const uint8_t short_name[11], DirEntryLoc *out) {
    uint32_t cluster = dir_cluster;
    uint32_t next;
    uint8_t s;
    uint16_t off;

    while (1) {
        for (s = 0; s < volume.sectors_per_cluster; s++) {
            uint32_t sector = cluster_to_sector(cluster) + s;
            if (read_sector(sector, scratch) != 0) return -1;
            for (off = 0; off < FS_SECTOR_SIZE; off += 32) {
                uint8_t first = scratch[off];
                uint8_t attr = scratch[off + 11];
                if (first == 0x00) return 0;
                if (first == 0xe5) continue;
                if (attr == FAT_ATTR_LONG_NAME) continue;
                if (mem_eq(&scratch[off], short_name, 11)) {
                    out->sector = sector;
                    out->offset = off;
                    mem_copy(out->data, &scratch[off], 32);
                    return 1;
                }
            }
        }

        if (fat_get(cluster, &next) != 0) return -1;
        if (is_eoc(next)) return 0;
        cluster = next;
    }
}

/*
 * Finds a free directory-entry slot, extending the directory if every current slot is full.
 * Existing free or deleted slots are reused first. If the directory must grow, a new
 * cluster is allocated, linked to the previous end, and its first entry slot is returned.
 */
static int dir_find_free(uint32_t dir_cluster, DirEntryLoc *out) {
    uint32_t cluster = dir_cluster;
    uint32_t prev = 0;
    uint32_t next;
    uint32_t new_cluster;
    uint8_t s;
    uint16_t off;

    while (1) {
        for (s = 0; s < volume.sectors_per_cluster; s++) {
            uint32_t sector = cluster_to_sector(cluster) + s;
            if (read_sector(sector, scratch) != 0) return -1;
            for (off = 0; off < FS_SECTOR_SIZE; off += 32) {
                uint8_t first = scratch[off];
                if (first == 0x00 || first == 0xe5) {
                    out->sector = sector;
                    out->offset = off;
                    mem_set(out->data, 0, 32);
                    return 0;
                }
            }
        }

        if (fat_get(cluster, &next) != 0) return -1;
        if (is_eoc(next)) break;
        prev = cluster;
        cluster = next;
    }

    prev = cluster;
    if (alloc_cluster(&new_cluster) != 0) return -1;
    if (fat_set(prev, new_cluster) != 0) return -1;
    out->sector = cluster_to_sector(new_cluster);
    out->offset = 0;
    mem_set(out->data, 0, 32);
    return 0;
}

/*
 * Resolves a path to its parent directory and final 8.3 name.
 * Intermediate components must already exist and be directories. The final component is
 * looked up if present, allowing callers to distinguish create-new from already-existing.
 */
static int resolve_parent(const char *path, uint32_t *parent_cluster, uint8_t name[11], DirEntryLoc *existing) {
    uint32_t dir = volume.root_cluster;
    uint8_t part[11];
    int last = 0;
    int result;

    if (!path || *path == '\0') return -1;
    while (is_path_sep(*path)) path++;
    if (*path == '\0') return -1;

    while ((result = next_path_part(&path, part, &last)) > 0) {
        if (last) {
            mem_copy(name, part, 11);
            *parent_cluster = dir;
            result = dir_find(dir, part, existing);
            return result < 0 ? -1 : result;
        } else {
            DirEntryLoc entry;
            result = dir_find(dir, part, &entry);
            if (result <= 0) return -1;
            if ((entry.data[11] & FAT_ATTR_DIRECTORY) == 0) return -1;
            dir = entry_first_cluster(entry.data);
            if (dir < 2) return -1;
        }
    }

    return -1;
}

/*
 * Resolves a path that must name an existing directory and returns its cluster.
 * Empty paths and root paths resolve to the root cluster. Each component is looked up
 * as a directory before walking into the next component.
 */
static int resolve_directory(const char *path, uint32_t *dir_cluster) {
    uint32_t dir = volume.root_cluster;
    uint8_t part[11];
    int last = 0;
    int result;

    if (path == 0 || *path == '\0') {
        *dir_cluster = dir;
        return 0;
    }

    while (is_path_sep(*path)) path++;
    if (*path == '\0') {
        *dir_cluster = dir;
        return 0;
    }

    while ((result = next_path_part(&path, part, &last)) > 0) {
        DirEntryLoc entry;
        result = dir_find(dir, part, &entry);
        if (result <= 0) return -1;
        if ((entry.data[11] & FAT_ATTR_DIRECTORY) == 0) return -1;
        dir = entry_first_cluster(entry.data);
        if (dir < 2) return -1;
        if (last) {
            *dir_cluster = dir;
            return 0;
        }
    }

    return -1;
}

/*
 * Formats an 11-byte FAT short name into a printable NAME or NAME.EXT string.
 * Trailing spaces are suppressed and the dot is emitted only when the extension
 * field contains at least one non-space byte.
 */
static void format_entry_name(const uint8_t *raw, char out[13]) {
    int i;
    int pos = 0;
    int ext_start;
    int has_ext = 0;

    for (i = 0; i < 8 && raw[i] != ' '; i++) {
        out[pos++] = (char)raw[i];
    }

    for (i = 8; i < 11; i++) {
        if (raw[i] != ' ') has_ext = 1;
    }

    if (has_ext) {
        out[pos++] = '.';
        ext_start = pos;
        for (i = 8; i < 11 && raw[i] != ' '; i++) {
            out[pos++] = (char)raw[i];
        }
        if (pos == ext_start) pos--;
    }

    out[pos] = '\0';
}

/* Refreshes an open file's directory entry with its current first cluster and size. */
static int update_open_file_entry(OpenFile *file) {
    set_entry_first_cluster(file->entry.data, file->first_cluster);
    put_le32(&file->entry.data[28], file->size);
    return write_entry(&file->entry);
}

/*
 * Returns the cluster that contains a file offset, optionally extending the file chain.
 * Empty files allocate their first cluster only when allocation is allowed. When walking
 * past the current end, new clusters are linked one at a time so writes can grow files.
 */
static int cluster_for_offset(OpenFile *file, uint32_t offset, int allocate, uint32_t *cluster_out) {
    uint32_t cluster_size = (uint32_t)volume.sectors_per_cluster * FS_SECTOR_SIZE;
    uint32_t hops = offset / cluster_size;
    uint32_t cluster = file->first_cluster;
    uint32_t next;
    uint32_t new_cluster;

    if (cluster == 0) {
        if (!allocate) return -1;
        if (alloc_cluster(&cluster) != 0) return -1;
        file->first_cluster = cluster;
        if (update_open_file_entry(file) != 0) return -1;
    }

    while (hops--) {
        if (fat_get(cluster, &next) != 0) return -1;
        if (is_eoc(next)) {
            if (!allocate) return -1;
            if (alloc_cluster(&new_cluster) != 0) return -1;
            if (fat_set(cluster, new_cluster) != 0) return -1;
            cluster = new_cluster;
        } else {
            cluster = next;
        }
    }

    *cluster_out = cluster;
    return 0;
}

/* Extracts the read/write mode bits from open flags. */
static int access_mode(int flags) {
    return flags & 0x3;
}

/* Returns whether an open file's flags permit reads. */
static int can_read(int flags) {
    int mode = access_mode(flags);
    return mode == O_RDONLY || mode == O_RDWR;
}

/* Returns whether an open file's flags permit writes. */
static int can_write(int flags) {
    int mode = access_mode(flags);
    return mode == O_WRONLY || mode == O_RDWR;
}

/*
 * Parses and validates the FAT32 BIOS Parameter Block for the mounted volume.
 * The function accepts only 512-byte sectors and FAT32 geometry, then derives the
 * FAT start, data start, root cluster, and cluster count used by later operations.
 */
static int parse_bpb(uint32_t partition_lba) {
    uint16_t reserved;
    uint32_t total_sectors;
    uint32_t root_dir_sectors;
    uint32_t data_sectors;

    if (read_sector(partition_lba, scratch) != 0) return -20;
    if (scratch[510] != 0x55 || scratch[511] != 0xaa) return -21;

    volume.bytes_per_sector = le16(&scratch[11]);
    volume.sectors_per_cluster = scratch[13];
    reserved = le16(&scratch[14]);
    volume.fat_count = scratch[16];
    total_sectors = le16(&scratch[19]);
    if (total_sectors == 0) total_sectors = le32(&scratch[32]);
    volume.sectors_per_fat = le32(&scratch[36]);
    volume.root_cluster = le32(&scratch[44]);

    if (volume.bytes_per_sector != FS_SECTOR_SIZE) return -22;
    if (volume.sectors_per_cluster == 0) return -23;
    if (reserved == 0 || volume.fat_count == 0 || volume.sectors_per_fat == 0) return -24;
    if (volume.root_cluster < 2) return -25;

    root_dir_sectors = 0;
    volume.partition_lba = partition_lba;
    volume.first_fat_sector = partition_lba + reserved;
    volume.first_data_sector = volume.first_fat_sector + (volume.fat_count * volume.sectors_per_fat) + root_dir_sectors;
    data_sectors = total_sectors - (reserved + (volume.fat_count * volume.sectors_per_fat) + root_dir_sectors);
    volume.total_clusters = data_sectors / volume.sectors_per_cluster;
    volume.mounted = 1;
    return 0;
}

/*
 * Initializes the block device and mounts a FAT32 volume from the attached disk image.
 * Sector zero is treated as either a FAT32 boot sector or an MBR containing a FAT32
 * partition entry. Open file slots are reset before the BPB is parsed so each boot starts
 * with a clean descriptor table.
 */
int fs_init() {
    int i;
    uint32_t partition_lba = 0;

    volume.mounted = 0;
    for (i = 0; i < MAX_OPEN_FILES; i++) open_files[i].used = 0;

    {
        int disk_result = disk_mmio_init();
        if (disk_result != 0) return disk_result;
    }
    if (read_sector(0, scratch) != 0) return -3;

    if (!(scratch[82] == 'F' && scratch[83] == 'A' && scratch[84] == 'T' &&
          scratch[85] == '3' && scratch[86] == '2')) {
        uint8_t type = scratch[446 + 4];
        if ((type == 0x0b || type == 0x0c || type == 0x1b || type == 0x1c) &&
            scratch[510] == 0x55 && scratch[511] == 0xaa) {
            partition_lba = le32(&scratch[446 + 8]);
        }
    }

    return parse_bpb(partition_lba);
}

/*
 * Opens or creates a regular file and returns a kernel-local file descriptor.
 * The parent path is resolved first, then O_CREAT can allocate a new directory entry
 * while O_TRUNC frees any existing cluster chain for writable opens. The descriptor
 * records the file's directory entry, current size, first cluster, flags, and initial offset.
 */
int fs_open(const char *path, int flags) {
    uint32_t parent_cluster;
    uint8_t short_name[11];
    DirEntryLoc entry;
    int found;
    int fd;

    if (!volume.mounted) return -1;

    found = resolve_parent(path, &parent_cluster, short_name, &entry);
    if (found < 0) return -1;
    if (found == 0) {
        if ((flags & O_CREAT) == 0) return -1;
        if (dir_find_free(parent_cluster, &entry) != 0) return -1;
        mem_copy(entry.data, short_name, 11);
        entry.data[11] = FAT_ATTR_ARCHIVE;
        set_entry_first_cluster(entry.data, 0);
        put_le32(&entry.data[28], 0);
        if (write_entry(&entry) != 0) return -1;
    } else {
        if (entry.data[11] & FAT_ATTR_DIRECTORY) return -1;
        if ((flags & O_TRUNC) && can_write(flags)) {
            uint32_t first = entry_first_cluster(entry.data);
            if (first >= 2 && free_chain(first) != 0) return -1;
            set_entry_first_cluster(entry.data, 0);
            put_le32(&entry.data[28], 0);
            if (write_entry(&entry) != 0) return -1;
        }
    }

    for (fd = 0; fd < MAX_OPEN_FILES; fd++) {
        if (!open_files[fd].used) {
            open_files[fd].used = 1;
            open_files[fd].flags = flags;
            open_files[fd].entry.sector = entry.sector;
            open_files[fd].entry.offset = entry.offset;
            mem_copy(open_files[fd].entry.data, entry.data, 32);
            open_files[fd].first_cluster = entry_first_cluster(entry.data);
            open_files[fd].size = le32(&entry.data[28]);
            open_files[fd].offset = (flags & O_APPEND) ? open_files[fd].size : 0;
            return fd;
        }
    }

    return -1;
}

/* Closes an open file descriptor by releasing its descriptor-table slot. */
int fs_close(int fd) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_files[fd].used) return -1;
    open_files[fd].used = 0;
    return 0;
}

/*
 * Reads bytes from an open file at its current offset and advances that offset.
 * Reads are clipped at EOF, then each sector touched by the current offset is loaded
 * through the cluster chain and copied into the caller's buffer. Partial progress is
 * returned if a later sector read fails after some bytes were already copied.
 */
long fs_read(int fd, void *buf, unsigned long count) {
    OpenFile *file;
    uint8_t *out = (uint8_t *)buf;
    unsigned long done = 0;
    uint32_t cluster_size;

    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_files[fd].used) return -1;
    if (buf == 0 && count > 0) return -1;
    file = &open_files[fd];
    if (!can_read(file->flags)) return -1;
    if (file->offset >= file->size) return 0;
    if (count > file->size - file->offset) count = file->size - file->offset;

    cluster_size = (uint32_t)volume.sectors_per_cluster * FS_SECTOR_SIZE;
    while (done < count) {
        uint32_t cluster;
        uint32_t cluster_offset = file->offset % cluster_size;
        uint32_t sector_offset = cluster_offset % FS_SECTOR_SIZE;
        uint32_t sector_in_cluster = cluster_offset / FS_SECTOR_SIZE;
        uint32_t chunk = FS_SECTOR_SIZE - sector_offset;
        if (chunk > count - done) chunk = count - done;

        if (cluster_for_offset(file, file->offset, 0, &cluster) != 0) break;
        if (read_sector(cluster_to_sector(cluster) + sector_in_cluster, scratch) != 0) return done ? (long)done : -1;
        mem_copy(out + done, &scratch[sector_offset], chunk);
        done += chunk;
        file->offset += chunk;
    }

    return (long)done;
}

/*
 * Writes bytes to an open file at its current offset and advances that offset.
 * Append mode first moves the offset to EOF, while normal writes preserve untouched
 * bytes in partial sectors by reading before patching. If the write grows the file,
 * clusters are allocated as needed and the directory entry size is updated.
 */
long fs_write(int fd, const void *buf, unsigned long count) {
    OpenFile *file;
    const uint8_t *in = (const uint8_t *)buf;
    unsigned long done = 0;
    uint32_t cluster_size;

    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_files[fd].used) return -1;
    if (buf == 0 && count > 0) return -1;
    file = &open_files[fd];
    if (!can_write(file->flags)) return -1;
    if (file->flags & O_APPEND) file->offset = file->size;

    cluster_size = (uint32_t)volume.sectors_per_cluster * FS_SECTOR_SIZE;
    while (done < count) {
        uint32_t cluster;
        uint32_t cluster_offset = file->offset % cluster_size;
        uint32_t sector_offset = cluster_offset % FS_SECTOR_SIZE;
        uint32_t sector_in_cluster = cluster_offset / FS_SECTOR_SIZE;
        uint32_t chunk = FS_SECTOR_SIZE - sector_offset;
        uint32_t sector;
        if (chunk > count - done) chunk = count - done;

        if (cluster_for_offset(file, file->offset, 1, &cluster) != 0) return done ? (long)done : -1;
        sector = cluster_to_sector(cluster) + sector_in_cluster;
        if (sector_offset != 0 || chunk != FS_SECTOR_SIZE) {
            if (read_sector(sector, scratch) != 0) return done ? (long)done : -1;
        } else {
            mem_set(scratch, 0, FS_SECTOR_SIZE);
        }
        mem_copy(&scratch[sector_offset], in + done, chunk);
        if (write_sector(sector, scratch) != 0) return done ? (long)done : -1;

        done += chunk;
        file->offset += chunk;
        if (file->offset > file->size) {
            file->size = file->offset;
            if (update_open_file_entry(file) != 0) return done ? (long)done : -1;
        }
    }

    return (long)done;
}

/*
 * Creates a new directory at the requested path.
 * The parent directory must already exist and the final name must not. A fresh cluster
 * is allocated for the directory body, initialized with . and .. entries, then linked
 * from a new directory entry in the parent.
 */
int fs_mkdir(const char *path) {
    uint32_t parent_cluster;
    uint32_t new_cluster;
    uint8_t short_name[11];
    uint8_t dot_name[11];
    uint8_t dotdot_name[11];
    DirEntryLoc entry;
    int found;

    if (!volume.mounted) return -1;

    mem_set(dot_name, ' ', 11);
    dot_name[0] = '.';
    mem_set(dotdot_name, ' ', 11);
    dotdot_name[0] = '.';
    dotdot_name[1] = '.';

    found = resolve_parent(path, &parent_cluster, short_name, &entry);
    if (found != 0) return -1;
    if (dir_find_free(parent_cluster, &entry) != 0) return -1;
    if (alloc_cluster(&new_cluster) != 0) return -1;

    if (read_sector(cluster_to_sector(new_cluster), scratch) != 0) return -1;
    mem_set(scratch, 0, FS_SECTOR_SIZE);
    make_dir_entry(&scratch[0], dot_name, FAT_ATTR_DIRECTORY, new_cluster, 0);
    make_dir_entry(&scratch[32], dotdot_name, FAT_ATTR_DIRECTORY, parent_cluster, 0);
    if (write_sector(cluster_to_sector(new_cluster), scratch) != 0) {
        free_chain(new_cluster);
        return -1;
    }

    make_dir_entry(entry.data, short_name, FAT_ATTR_DIRECTORY, new_cluster, 0);
    if (write_entry(&entry) != 0) {
        free_chain(new_cluster);
        return -1;
    }

    return 0;
}

/*
 * Recursively removes every live child entry from a directory cluster chain.
 * File entries have their cluster chains freed, while directory entries recurse before
 * their own chain is freed. Entries are marked deleted after their contents are released,
 * and the current sector is reloaded so scanning can continue safely.
 */
static int remove_dir_contents(uint32_t dir_cluster) {
    uint32_t cluster = dir_cluster;
    uint32_t next;
    uint8_t s;
    uint16_t off;

    while (1) {
        for (s = 0; s < volume.sectors_per_cluster; s++) {
            uint32_t sector = cluster_to_sector(cluster) + s;
            if (read_sector(sector, scratch) != 0) return -1;

            for (off = 0; off < FS_SECTOR_SIZE; off += 32) {
                DirEntryLoc loc;
                uint8_t entry_data[32];
                uint8_t first = scratch[off];
                uint8_t attr = scratch[off + 11];
                uint32_t first_cluster;

                if (first == 0x00) return 0;
                if (first == 0xe5) continue;
                if (first == '.') continue;
                if (attr == FAT_ATTR_LONG_NAME) continue;
                if (attr & FAT_ATTR_VOLUME_ID) continue;

                loc.sector = sector;
                loc.offset = off;
                mem_copy(entry_data, &scratch[off], 32);
                mem_copy(loc.data, entry_data, 32);
                first_cluster = entry_first_cluster(entry_data);

                if (attr & FAT_ATTR_DIRECTORY) {
                    if (first_cluster < 2) return -1;
                    if (remove_dir_contents(first_cluster) != 0) return -1;
                    if (free_chain(first_cluster) != 0) return -1;
                } else {
                    if (first_cluster >= 2 && free_chain(first_cluster) != 0) return -1;
                }

                if (mark_entry_deleted(&loc) != 0) return -1;
                if (read_sector(sector, scratch) != 0) return -1;
            }
        }

        if (fat_get(cluster, &next) != 0) return -1;
        if (is_eoc(next)) return 0;
        cluster = next;
    }
}

/*
 * Removes a regular file and releases its cluster chain.
 * The path must resolve to an existing non-directory entry. Empty files may have no
 * cluster chain, but their directory entry is still marked deleted.
 */
int fs_remove(const char *path) {
    uint32_t parent_cluster;
    uint8_t short_name[11];
    DirEntryLoc entry;
    uint32_t first_cluster;
    int found;

    if (!volume.mounted) return -1;

    found = resolve_parent(path, &parent_cluster, short_name, &entry);
    (void)parent_cluster;
    (void)short_name;
    if (found != 1) return -1;
    if (entry.data[11] & FAT_ATTR_DIRECTORY) return -1;

    first_cluster = entry_first_cluster(entry.data);
    if (first_cluster >= 2 && free_chain(first_cluster) != 0) return -1;
    return mark_entry_deleted(&entry);
}

/*
 * Recursively removes a directory and releases its directory cluster chain.
 * The path must resolve to an existing directory. Children are removed first, then the
 * directory's own FAT chain is freed and its parent entry is marked deleted.
 */
int fs_rmdir(const char *path) {
    uint32_t parent_cluster;
    uint8_t short_name[11];
    DirEntryLoc entry;
    uint32_t first_cluster;
    int found;

    if (!volume.mounted) return -1;

    found = resolve_parent(path, &parent_cluster, short_name, &entry);
    (void)parent_cluster;
    (void)short_name;
    if (found != 1) return -1;
    if ((entry.data[11] & FAT_ATTR_DIRECTORY) == 0) return -1;

    first_cluster = entry_first_cluster(entry.data);
    if (first_cluster < 2) return -1;
    if (remove_dir_contents(first_cluster) != 0) return -1;
    if (free_chain(first_cluster) != 0) return -1;
    return mark_entry_deleted(&entry);
}

/*
 * Lists the live entries in an existing directory through a caller-provided callback.
 * Long-name, volume-label, deleted, and dot entries are skipped. Each returned entry
 * contains a printable short name, directory flag, and file size.
 */
int fs_list(const char *path, void (*emit)(const FsDirEntry *entry, void *ctx), void *ctx) {
    uint32_t cluster;
    uint32_t next;
    uint8_t s;
    uint16_t off;

    if (!volume.mounted || emit == 0) return -1;
    if (resolve_directory(path, &cluster) != 0) return -1;

    while (1) {
        for (s = 0; s < volume.sectors_per_cluster; s++) {
            uint32_t sector = cluster_to_sector(cluster) + s;
            if (read_sector(sector, scratch) != 0) return -1;
            for (off = 0; off < FS_SECTOR_SIZE; off += 32) {
                FsDirEntry listed;
                uint8_t first = scratch[off];
                uint8_t attr = scratch[off + 11];

                if (first == 0x00) return 0;
                if (first == 0xe5) continue;
                if (first == '.') continue;
                if (attr == FAT_ATTR_LONG_NAME) continue;
                if (attr & FAT_ATTR_VOLUME_ID) continue;

                format_entry_name(&scratch[off], listed.name);
                listed.is_dir = (attr & FAT_ATTR_DIRECTORY) ? 1 : 0;
                listed.size = le32(&scratch[off + 28]);
                emit(&listed, ctx);
            }
        }

        if (fat_get(cluster, &next) != 0) return -1;
        if (is_eoc(next)) return 0;
        cluster = next;
    }
}
