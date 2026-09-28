#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_BYTES (16u * 1024u * 1024u)
#define SECTOR_SIZE 512u
#define TOTAL_SECTORS (IMAGE_BYTES / SECTOR_SIZE)
#define RESERVED_SECTORS 32u
#define FAT_COUNT 2u
#define SECTORS_PER_CLUSTER 1u
#define ROOT_CLUSTER 2u
#define MEDIA_DESCRIPTOR 0xf8u
#define EOC 0x0fffffffu

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static uint32_t fat_sectors_for_image(void) {
    uint32_t fat_sectors = 1;

    for (;;) {
        uint32_t data_sectors = TOTAL_SECTORS - RESERVED_SECTORS - (FAT_COUNT * fat_sectors);
        uint32_t clusters = data_sectors / SECTORS_PER_CLUSTER;
        uint32_t needed = ((clusters + 2) * 4 + SECTOR_SIZE - 1) / SECTOR_SIZE;
        if (needed == fat_sectors) return fat_sectors;
        fat_sectors = needed;
    }
}

static uint32_t first_data_sector(uint32_t fat_sectors) {
    return RESERVED_SECTORS + (FAT_COUNT * fat_sectors);
}

static uint32_t cluster_sector(uint32_t fat_sectors, uint32_t cluster) {
    return first_data_sector(fat_sectors) + ((cluster - 2) * SECTORS_PER_CLUSTER);
}

static void write_boot_sector(uint8_t *img, uint32_t fat_sectors) {
    uint8_t *b = img;
    uint32_t data_sectors = TOTAL_SECTORS - RESERVED_SECTORS - (FAT_COUNT * fat_sectors);
    uint32_t clusters = data_sectors / SECTORS_PER_CLUSTER;

    memset(b, 0, SECTOR_SIZE);
    b[0] = 0xeb;
    b[1] = 0x58;
    b[2] = 0x90;
    memcpy(&b[3], "MINIOS  ", 8);
    put16(&b[11], SECTOR_SIZE);
    b[13] = SECTORS_PER_CLUSTER;
    put16(&b[14], RESERVED_SECTORS);
    b[16] = FAT_COUNT;
    put16(&b[17], 0);
    put16(&b[19], 0);
    b[21] = MEDIA_DESCRIPTOR;
    put16(&b[22], 0);
    put16(&b[24], 63);
    put16(&b[26], 255);
    put32(&b[28], 0);
    put32(&b[32], TOTAL_SECTORS);
    put32(&b[36], fat_sectors);
    put16(&b[40], 0);
    put16(&b[42], 0);
    put32(&b[44], ROOT_CLUSTER);
    put16(&b[48], 1);
    put16(&b[50], 6);
    b[64] = 0x80;
    b[66] = 0x29;
    put32(&b[67], 0x20260409u);
    memcpy(&b[71], "MINI_OS    ", 11);
    memcpy(&b[82], "FAT32   ", 8);
    b[510] = 0x55;
    b[511] = 0xaa;

    memcpy(img + (6 * SECTOR_SIZE), b, SECTOR_SIZE);

    memset(img + SECTOR_SIZE, 0, SECTOR_SIZE);
    put32(img + SECTOR_SIZE, 0x41615252u);
    put32(img + SECTOR_SIZE + 484, 0x61417272u);
    put32(img + SECTOR_SIZE + 488, clusters - 4);
    put32(img + SECTOR_SIZE + 492, 6);
    put16(img + SECTOR_SIZE + 510, 0xaa55u);
}

static void fat_put(uint8_t *img, uint32_t fat_sectors, uint32_t cluster, uint32_t value) {
    uint32_t fat;
    for (fat = 0; fat < FAT_COUNT; fat++) {
        uint32_t fat_start = RESERVED_SECTORS + (fat * fat_sectors);
        put32(img + (fat_start * SECTOR_SIZE) + (cluster * 4), value);
    }
}

static void set_entry_cluster(uint8_t *entry, uint32_t cluster) {
    put16(&entry[20], (uint16_t)(cluster >> 16));
    put16(&entry[26], (uint16_t)(cluster & 0xffff));
}

static void make_entry(uint8_t *entry, const char name[11], uint8_t attr, uint32_t cluster, uint32_t size) {
    memset(entry, 0, 32);
    memcpy(entry, name, 11);
    entry[11] = attr;
    set_entry_cluster(entry, cluster);
    put32(&entry[28], size);
}

static void write_file_content(uint8_t *img, uint32_t fat_sectors, uint32_t cluster, const char *text) {
    uint8_t *sector = img + (cluster_sector(fat_sectors, cluster) * SECTOR_SIZE);
    memset(sector, 0, SECTOR_SIZE);
    memcpy(sector, text, strlen(text));
}

static int write_image(const char *path) {
    uint32_t fat_sectors = fat_sectors_for_image();
    uint8_t *img = calloc(1, IMAGE_BYTES);
    FILE *file;
    const char *root_text = "Hello from MINI_OS FAT32.\n";
    const char *nested_text = "Nested FAT32 reads work.\n";
    uint8_t *root_dir;
    uint8_t *sub_dir;

    if (!img) return 1;

    write_boot_sector(img, fat_sectors);
    fat_put(img, fat_sectors, 0, 0x0ffffff0u | MEDIA_DESCRIPTOR);
    fat_put(img, fat_sectors, 1, 0xffffffffu);
    fat_put(img, fat_sectors, 2, EOC);
    fat_put(img, fat_sectors, 3, EOC);
    fat_put(img, fat_sectors, 4, EOC);
    fat_put(img, fat_sectors, 5, EOC);

    root_dir = img + (cluster_sector(fat_sectors, ROOT_CLUSTER) * SECTOR_SIZE);
    make_entry(root_dir, "TEST    TXT", 0x20, 3, (uint32_t)strlen(root_text));
    make_entry(root_dir + 32, "DIR        ", 0x10, 4, 0);

    sub_dir = img + (cluster_sector(fat_sectors, 4) * SECTOR_SIZE);
    make_entry(sub_dir, ".          ", 0x10, 4, 0);
    make_entry(sub_dir + 32, "..         ", 0x10, ROOT_CLUSTER, 0);
    make_entry(sub_dir + 64, "FILE    TXT", 0x20, 5, (uint32_t)strlen(nested_text));

    write_file_content(img, fat_sectors, 3, root_text);
    write_file_content(img, fat_sectors, 5, nested_text);

    file = fopen(path, "wb");
    if (!file) {
        free(img);
        return 1;
    }
    if (fwrite(img, 1, IMAGE_BYTES, file) != IMAGE_BYTES) {
        fclose(file);
        free(img);
        return 1;
    }

    fclose(file);
    free(img);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s disk.img\n", argv[0]);
        return 2;
    }
    return write_image(argv[1]);
}
