/*
*  myfs.c - Implementacao do sistema de arquivos MyFS
*
*  Autores: Isaac João - Ian Fernandes - Victor Gonçalves
*  Projeto: Trabalho Pratico II - Sistemas Operacionais
*  Organizacao: Universidade Federal de Juiz de Fora
*  Departamento: Dep. Ciencia da Computacao
*
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "myfs.h"
#include "vfs.h"
#include "inode.h"
#include "util.h"
#include "disk.h"

/* =========================
   Constantes / Layout do Disco
   ========================= */

#define MYFS_MAGIC 0x4D594653u /* 'MYFS' */
#define MYFS_ROOT_INODE 1u

#define SUPER_SECTOR 0ul
#define BITMAP_SECTOR 1ul
#define BITMAP_BITS (DISK_SECTORDATASIZE * 8u) /* 4096 bits */

/* Superbloco */
typedef struct {
    uint32_t magic;
    uint32_t blockSize;        /* em BYTES */
    uint32_t sectorsPerBlock;  /* em SETORES */
    uint32_t inodeSectors;
    uint32_t dataStartSector;
    uint32_t totalBlocks;      /* quantidade de blocos gerenciados pelo bitmap (<= 4096) */
} MyFSSuper;

/* Entrada do "diretório raiz" armazenado no inode 1 */
typedef struct {
    uint32_t inumber;
    char     name[MAX_FILENAME_LENGTH + 1];
    uint8_t  active;
    uint8_t  _pad[3];
} MyDirEntry;

/* Tabela de arquivos abertos */
typedef struct {
    int used;
    uint32_t inumber;
    uint32_t cursor;
} MyFD;

/* =========================
   Globais
   ========================= */

static MyFSSuper g_sb;
static uint8_t   g_bitmap[DISK_SECTORDATASIZE];
static int       g_mounted = 0;
static MyFD      g_fd[MAX_FDS];

extern Disk* rootDisk; /* vfs.c */

/* =========================
   Helpers: Bitmap / Superbloco / FD
   ========================= */

static void bitmap_clear_all(void) {
    memset(g_bitmap, 0, sizeof(g_bitmap));
}

static int bitmap_test(uint32_t b) {
    if (b >= BITMAP_BITS) return 1;
    return (g_bitmap[b >> 3] >> (b & 7)) & 1;
}

static void bitmap_set(uint32_t b, int val) {
    if (b >= BITMAP_BITS) return;
    if (val) g_bitmap[b >> 3] |=  (uint8_t)(1u << (b & 7));
    else     g_bitmap[b >> 3] &= (uint8_t)~(1u << (b & 7));
}

static int super_write(Disk *d) {
    unsigned char sec[DISK_SECTORDATASIZE];
    memset(sec, 0, sizeof(sec));
    ul2char(g_sb.magic,           &sec[0]);
    ul2char(g_sb.blockSize,       &sec[4]);
    ul2char(g_sb.sectorsPerBlock, &sec[8]);
    ul2char(g_sb.inodeSectors,    &sec[12]);
    ul2char(g_sb.dataStartSector, &sec[16]);
    ul2char(g_sb.totalBlocks,     &sec[20]);
    return diskWriteSector(d, SUPER_SECTOR, sec);
}

static int super_read(Disk *d) {
    unsigned char sec[DISK_SECTORDATASIZE];
    if (diskReadSector(d, SUPER_SECTOR, sec) < 0) return -1;

    char2ul(&sec[0],  &g_sb.magic);
    char2ul(&sec[4],  &g_sb.blockSize);
    char2ul(&sec[8],  &g_sb.sectorsPerBlock);
    char2ul(&sec[12], &g_sb.inodeSectors);
    char2ul(&sec[16], &g_sb.dataStartSector);
    char2ul(&sec[20], &g_sb.totalBlocks);

    if (g_sb.magic != MYFS_MAGIC) return -1;
    if (g_sb.blockSize == 0 || (g_sb.blockSize % DISK_SECTORDATASIZE) != 0) return -1;
    if (g_sb.sectorsPerBlock == 0) return -1;
    if (g_sb.totalBlocks == 0 || g_sb.totalBlocks > BITMAP_BITS) return -1;

    return 0;
}

static int bitmap_write(Disk *d) {
    return diskWriteSector(d, BITMAP_SECTOR, g_bitmap);
}

static int bitmap_read(Disk *d) {
    return diskReadSector(d, BITMAP_SECTOR, g_bitmap);
}

/* Procura inode livre APENAS na área reservada de inodes do MyFS */
static unsigned int myfs_find_free_inode(Disk *d) {
    unsigned int perSector = inodeNumInodesPerSector();
    unsigned int maxInodes = g_sb.inodeSectors * perSector;

    for (unsigned int n = 2; n <= maxInodes; n++) {
        Inode *i = inodeLoad(n, d);
        if (!i) return 0;

        int isFree =
            (inodeGetFileType(i) == 0) &&
            (inodeGetRefCount(i) == 0) &&
            (inodeGetFileSize(i) == 0) &&
            (inodeGetNextNumber(i) == 0) &&
            (inodeGetBlockAddr(i, 0) == 0);

        free(i);

        if (isFree) return n;
    }
    return 0;
}

static int fd_alloc(void) {
    for (int i = 0; i < MAX_FDS; i++) {
        if (!g_fd[i].used) {
            g_fd[i].used = 1;
            g_fd[i].cursor = 0;
            g_fd[i].inumber = 0;
            return i + 1;
        }
    }
    return -1;
}

static int fd_slot(int fd) {
    int s = fd - 1;
    if (s < 0 || s >= MAX_FDS) return -1;
    if (!g_fd[s].used) return -1;
    return s;
}

/* =========================
   Helpers: blocos e IO
   ========================= */

static int alloc_block_index(void) {
    for (uint32_t b = 0; b < g_sb.totalBlocks && b < BITMAP_BITS; b++) {
        if (!bitmap_test(b)) {
            bitmap_set(b, 1);
            return (int)b;
        }
    }
    return -1;
}

static unsigned long block_to_sector(uint32_t blockIndex) {
    return (unsigned long)g_sb.dataStartSector +
           (unsigned long)blockIndex * (unsigned long)g_sb.sectorsPerBlock;
}

static int zero_block(Disk *d, uint32_t blockIndex) {
    unsigned char sec[DISK_SECTORDATASIZE];
    memset(sec, 0, sizeof(sec));
    unsigned long start = block_to_sector(blockIndex);
    for (uint32_t s = 0; s < g_sb.sectorsPerBlock; s++) {
        if (diskWriteSector(d, start + s, sec) < 0) return -1;
    }
    return 0;
}

static int ensure_file_block(Disk *d, Inode *ino, uint32_t logicalBlock) {
    unsigned int addr = inodeGetBlockAddr(ino, logicalBlock);
    if (addr != 0) return 0;

    int b = alloc_block_index();
    if (b < 0) return -1;

    if (zero_block(d, (uint32_t)b) < 0) return -1;

    unsigned long startSector = block_to_sector((uint32_t)b);
    if (inodeAddBlock(ino, (unsigned int)startSector) < 0) return -1;

    return 0;
}

static int file_read_at(Disk *d, uint32_t inumber, uint32_t offset, char *buf, uint32_t nbytes) {
    Inode *ino = inodeLoad(inumber, d);
    if (!ino) return -1;

    uint32_t fsz = inodeGetFileSize(ino);
    if (offset >= fsz) { free(ino); return 0; }

    uint32_t toRead = nbytes;
    if (offset + toRead > fsz) toRead = fsz - offset;

    uint32_t done = 0;
    while (done < toRead) {
        uint32_t pos = offset + done;
        uint32_t lblock = pos / g_sb.blockSize;
        uint32_t inoff  = pos % g_sb.blockSize;

        unsigned int startSector = inodeGetBlockAddr(ino, lblock);
        if (startSector == 0) break;

        unsigned char blockBuf[4096];
        if (g_sb.blockSize > sizeof(blockBuf)) { free(ino); return -1; }

        for (uint32_t s = 0; s < g_sb.sectorsPerBlock; s++) {
            if (diskReadSector(d, (unsigned long)startSector + s,
                               &blockBuf[s * DISK_SECTORDATASIZE]) < 0) {
                free(ino); return -1;
            }
        }

        uint32_t chunk = toRead - done;
        uint32_t room = g_sb.blockSize - inoff;
        if (chunk > room) chunk = room;

        memcpy(buf + done, blockBuf + inoff, chunk);
        done += chunk;
    }

    free(ino);
    return (int)done;
}

static int file_write_at(Disk *d, uint32_t inumber, uint32_t offset, const char *buf, uint32_t nbytes) {
    Inode *ino = inodeLoad(inumber, d);
    if (!ino) return -1;

    uint32_t done = 0;
    while (done < nbytes) {
        uint32_t pos = offset + done;
        uint32_t lblock = pos / g_sb.blockSize;
        uint32_t inoff  = pos % g_sb.blockSize;

        if (ensure_file_block(d, ino, lblock) < 0) { free(ino); return -1; }
        unsigned int startSector = inodeGetBlockAddr(ino, lblock);
        if (startSector == 0) { free(ino); return -1; }

        unsigned char blockBuf[4096];
        if (g_sb.blockSize > sizeof(blockBuf)) { free(ino); return -1; }

        for (uint32_t s = 0; s < g_sb.sectorsPerBlock; s++) {
            if (diskReadSector(d, (unsigned long)startSector + s,
                               &blockBuf[s * DISK_SECTORDATASIZE]) < 0) {
                free(ino); return -1;
            }
        }

        uint32_t chunk = nbytes - done;
        uint32_t room = g_sb.blockSize - inoff;
        if (chunk > room) chunk = room;

        memcpy(blockBuf + inoff, buf + done, chunk);

        for (uint32_t s = 0; s < g_sb.sectorsPerBlock; s++) {
            if (diskWriteSector(d, (unsigned long)startSector + s,
                                &blockBuf[s * DISK_SECTORDATASIZE]) < 0) {
                free(ino); return -1;
            }
        }
        done += chunk;
    }

    uint32_t fsz = inodeGetFileSize(ino);
    if (offset + done > fsz) {
        inodeSetFileSize(ino, offset + done);
        if (inodeSave(ino) < 0) { free(ino); return -1; }
    }

    free(ino);
    return (int)done;
}

/* =========================
   "Diretório raiz" interno (usado só por myFSOpen)
   ========================= */

static int valid_name(const char *name) {
    if (!name || name[0] == '\0') return 0;
    if (strlen(name) > MAX_FILENAME_LENGTH) return 0;
    for (const char *p = name; *p; p++) if (*p == '/') return 0;
    return 1;
}

static int root_find_entry(Disk *d, const char *name, MyDirEntry *out) {
    Inode *root = inodeLoad(MYFS_ROOT_INODE, d);
    if (!root) return -1;

    uint32_t sz = inodeGetFileSize(root);
    uint32_t off = 0;
    MyDirEntry e;

    while (off + sizeof(MyDirEntry) <= sz) {
        int r = file_read_at(d, MYFS_ROOT_INODE, off, (char*)&e, (uint32_t)sizeof(MyDirEntry));
        if (r != (int)sizeof(MyDirEntry)) { free(root); return -1; }

        if (e.active && strcmp(e.name, name) == 0) {
            if (out) *out = e;
            free(root);
            return 1;
        }
        off += (uint32_t)sizeof(MyDirEntry);
    }

    free(root);
    return 0;
}

static int root_add_entry(Disk *d, const char *name, uint32_t inumber) {
    Inode *root = inodeLoad(MYFS_ROOT_INODE, d);
    if (!root) return -1;

    uint32_t sz = inodeGetFileSize(root);
    uint32_t off = 0;
    int32_t freeOff = -1;
    MyDirEntry e;

    while (off + sizeof(MyDirEntry) <= sz) {
        int r = file_read_at(d, MYFS_ROOT_INODE, off, (char*)&e, (uint32_t)sizeof(MyDirEntry));
        if (r != (int)sizeof(MyDirEntry)) { free(root); return -1; }

        if (e.active && strcmp(e.name, name) == 0) { free(root); return -1; }
        if (!e.active && freeOff < 0) freeOff = (int32_t)off;
        off += (uint32_t)sizeof(MyDirEntry);
    }

    MyDirEntry ne;
    memset(&ne, 0, sizeof(ne));
    ne.inumber = inumber;
    strncpy(ne.name, name, MAX_FILENAME_LENGTH);
    ne.name[MAX_FILENAME_LENGTH] = '\0';
    ne.active = 1;

    uint32_t writeOff = (freeOff >= 0) ? (uint32_t)freeOff : sz;
    int w = file_write_at(d, MYFS_ROOT_INODE, writeOff, (const char*)&ne, (uint32_t)sizeof(MyDirEntry));

    free(root);
    return (w == (int)sizeof(MyDirEntry)) ? 0 : -1;
}

/* =========================
   API MyFS
   ========================= */

int myFSIsIdle (Disk *d) {
    (void)d;
    for (int i = 0; i < MAX_FDS; i++) {
        if (g_fd[i].used) return 0;
    }
    return 1;
}

/* 4 setores => 2048 bytes */
int myFSFormat (Disk *d, unsigned int blockSize) {
    if (!d) return -1;
    if (blockSize == 0) return -1;

    /* blockSize vem em BYTES */
    if (blockSize % DISK_SECTORDATASIZE != 0) return -1;

    uint32_t inodeSectors = 128;
    unsigned long totalSectors = diskGetNumSectors(d);

    uint32_t blockSizeBytes  = (uint32_t)blockSize;
    uint32_t sectorsPerBlock = blockSizeBytes / DISK_SECTORDATASIZE;

    if (sectorsPerBlock == 0) return -1;

    /* Seus buffers locais são de 4096. Limita para evitar overflow. */
    if (blockSizeBytes > 4096) return -1;

    uint32_t dataStart = 2u + inodeSectors;
    if ((unsigned long)dataStart >= totalSectors) return -1;

    uint32_t totalBlocks = (uint32_t)((totalSectors - dataStart) / sectorsPerBlock);
    if (totalBlocks == 0) return -1;

    if (totalBlocks > BITMAP_BITS) totalBlocks = BITMAP_BITS;

    g_sb.magic           = MYFS_MAGIC;
    g_sb.blockSize       = blockSizeBytes;
    g_sb.sectorsPerBlock = sectorsPerBlock;
    g_sb.inodeSectors    = inodeSectors;
    g_sb.dataStartSector = dataStart;
    g_sb.totalBlocks     = totalBlocks;

    if (super_write(d) < 0) return -1;

    /* bitmap: marca como "usado" tudo acima de totalBlocks */
    bitmap_clear_all();
    for (uint32_t b = totalBlocks; b < BITMAP_BITS; b++) bitmap_set(b, 1);
    if (bitmap_write(d) < 0) return -1;

    /* zera a área de inodes (setores 2..2+inodeSectors-1) */
    unsigned char zero[DISK_SECTORDATASIZE];
    memset(zero, 0, sizeof(zero));
    for (uint32_t s = 0; s < inodeSectors; s++) {
        if (diskWriteSector(d, 2u + s, zero) < 0) return -1;
    }

    /* cria inode raiz */
    Inode *root = inodeCreate(MYFS_ROOT_INODE, d);
    if (!root) return -1;
    inodeSetFileType(root, FILETYPE_DIR);
    inodeSetFileSize(root, 0);
    inodeSetRefCount(root, 1);
    if (inodeSave(root) < 0) { free(root); return -1; }
    free(root);

    return (int)totalBlocks;
}

int myFSxMount (Disk *d, int x) {
    if (x == 1) {
        if (!d) return 0;
        if (super_read(d) < 0) return 0;
        if (bitmap_read(d) < 0) return 0;

        for (int i=0; i<MAX_FDS; i++) memset(&g_fd[i], 0, sizeof(g_fd[i]));
        g_mounted = 1;

        Inode *root = inodeLoad(MYFS_ROOT_INODE, d);
        if (!root) { g_mounted = 0; return 0; }
        free(root);
        return 1;
    } else {
        if (!g_mounted) return 0;
        if (!myFSIsIdle(d)) return 0;
        if (bitmap_write(d) < 0) return 0;
        g_mounted = 0;
        return 1;
    }
}

int myFSOpen (Disk *d, const char *path) {
    if (!g_mounted || !d || !path) return -1;
    if (path[0] != '/') return -1;

    const char *name = path + 1;
    if (!valid_name(name)) return -1;

    MyDirEntry e;
    int f = root_find_entry(d, name, &e);
    uint32_t inum = 0;

    if (f == 1) {
        inum = e.inumber;
    } else if (f == 0) {
        uint32_t maxInodes = g_sb.inodeSectors * inodeNumInodesPerSector();
        uint32_t newNum = myfs_find_free_inode(d);
        if (newNum == 0 || newNum > maxInodes) return -1;

        Inode *ni = inodeCreate(newNum, d);
        if (!ni) return -1;

        inodeSetFileType(ni, FILETYPE_REGULAR);
        inodeSetFileSize(ni, 0);
        inodeSetRefCount(ni, 1);

        if (inodeSave(ni) < 0) { free(ni); return -1; }
        free(ni);

        if (root_add_entry(d, name, newNum) < 0) return -1;

        inum = newNum;
    } else {
        return -1;
    }

    int fd = fd_alloc();
    if (fd < 0) return -1;

    int s = fd_slot(fd);
    if (s < 0) return -1;

    g_fd[s].inumber = inum;
    g_fd[s].cursor  = 0;

    return fd;
}

int myFSRead (int fd, char *buf, unsigned int nbytes) {
    if (!g_mounted || !buf) return -1;
    int s = fd_slot(fd);
    if (s < 0) return -1;

    int r = file_read_at(rootDisk, g_fd[s].inumber, g_fd[s].cursor, buf, nbytes);
    if (r >= 0) g_fd[s].cursor += (uint32_t)r;
    return r;
}

int myFSWrite (int fd, const char *buf, unsigned int nbytes) {
    if (!g_mounted || !buf) return -1;
    int s = fd_slot(fd);
    if (s < 0) return -1;

    int w = file_write_at(rootDisk, g_fd[s].inumber, g_fd[s].cursor, buf, nbytes);
    if (w >= 0) g_fd[s].cursor += (uint32_t)w;
    return w;
}

int myFSClose (int fd) {
    int s = fd_slot(fd);
    if (s < 0) return -1;
    memset(&g_fd[s], 0, sizeof(g_fd[s]));
    return 0;
}

/* =========================
   Stubs de diretorio (para não crashar o simulador)
   ========================= */

int myFSOpenDir (Disk *d, const char *path) {
    (void)d; (void)path;
    return -1;
}

int myFSReadDir (int fd, char *filename, unsigned int *inumber) {
    (void)fd; (void)filename; (void)inumber;
    return -1;
}

int myFSLink (int fd, const char *filename, unsigned int inumber) {
    (void)fd; (void)filename; (void)inumber;
    return -1;
}

int myFSUnlink (int fd, const char *filename) {
    (void)fd; (void)filename;
    return -1;
}

int myFSCloseDir (int fd) {
    (void)fd;
    return -1;
}

/* =========================
   Instalacao no VFS
   ========================= */

int installMyFS (void) {
    static FSInfo info;
    memset(&info, 0, sizeof(info));
    info.fsid = 'M';           /* 77 */
    info.fsname = "MyFS";

    info.isidleFn = myFSIsIdle;
    info.formatFn = myFSFormat;
    info.xMountFn = myFSxMount;
    info.openFn   = myFSOpen;
    info.readFn   = myFSRead;
    info.writeFn  = myFSWrite;
    info.closeFn  = myFSClose;

    info.opendirFn  = myFSOpenDir;
    info.readdirFn  = myFSReadDir;
    info.linkFn     = myFSLink;
    info.unlinkFn   = myFSUnlink;
    info.closedirFn = myFSCloseDir;

    return vfsRegisterFS(&info);
}
