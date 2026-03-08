/*
* myfs.c - Implementacao do sistema de arquivos MyFS
* ... (Cabeçalho original mantido) ...
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// >>> Inclusão das bibliotecas auxiliares fornecidas pelo professor
#include "myfs.h"
#include "vfs.h"
#include "inode.h"
#include "util.h"
#include "disk.h"

/* =========================
   Constantes / Layout do Disco
   ========================= */

// >>> "Assinatura" do sistema de arquivos. Se o setor 0 não tiver esse número, não é um MyFS.
#define MYFS_MAGIC 0x4D594653u /* 'MYFS' em hex */

// >>> O Inode 1 é reservado para ser a "pasta raiz" (/).
#define MYFS_ROOT_INODE 1u

// >>> Definição física: Onde as coisas ficam no disco.
#define SUPER_SECTOR 0ul    // >>> Superbloco fica no setor 0
#define BITMAP_SECTOR 1ul   // >>> Bitmap fica no setor 1
// >>> Tamanho máximo do bitmap em bits (4096 bits = 512 bytes * 8 bits).
#define BITMAP_BITS (DISK_SECTORDATASIZE * 8u) 

/* Superbloco */
// >>> Estrutura que define as propriedades globais da partição.
typedef struct {
    uint32_t magic;            // >>> Número mágico para identificação
    uint32_t blockSize;        // >>> Tamanho do bloco lógico em BYTES (ex: 1024, 2048)
    uint32_t sectorsPerBlock;  // >>> Quantos setores físicos compõem 1 bloco lógico
    uint32_t inodeSectors;     // >>> Quantos setores são reservados apenas para guardar Inodes
    uint32_t dataStartSector;  // >>> Em qual setor começa a área de dados (arquivos)
    uint32_t totalBlocks;      // >>> Total de blocos disponíveis para uso
} MyFSSuper;

/* Entrada do "diretório raiz" armazenado no inode 1 */
// >>> Estrutura que representa um arquivo dentro de uma pasta.
typedef struct {
    uint32_t inumber;               // >>> Qual é o Inode desse arquivo?
    char     name[MAX_FILENAME_LENGTH + 1]; // >>> Nome do arquivo (ex: "texto.txt")
    uint8_t  active;                // >>> 1 = arquivo existe, 0 = entrada apagada/livre
    uint8_t  _pad[3];               // >>> Padding para alinhar a memória (não usado)
} MyDirEntry;

/* Tabela de arquivos abertos */
// >>> Estrutura para controlar arquivos que estão abertos no momento (File Descriptors).
typedef struct {
    int used;           // >>> 1 se este slot da tabela está em uso
    uint32_t inumber;   // >>> Qual arquivo está aberto (número do inode)
    uint32_t cursor;    // >>> Onde está o cursor de leitura/escrita (offset em bytes)
} MyFD;

/* =========================
   Globais
   ========================= */

// >>> Variáveis globais carregadas na memória RAM enquanto o sistema roda.
static MyFSSuper g_sb;                         // >>> Cópia do Superbloco em RAM
static uint8_t   g_bitmap[DISK_SECTORDATASIZE]; // >>> Cópia do Bitmap em RAM (vetor de bytes)
static int       g_mounted = 0;                // >>> Flag: 1 se o disco está montado, 0 se não
static MyFD      g_fd[MAX_FDS];                // >>> Tabela de arquivos abertos
extern Disk* rootDisk; /* vfs.c */             // >>> Ponteiro para o disco físico (vem do vfs.c)

/* =========================
   Helpers: Bitmap / Superbloco / FD
   ========================= */

// >>> Zera todo o bitmap na memória (marca tudo como livre).
static void bitmap_clear_all(void) {
    memset(g_bitmap, 0, sizeof(g_bitmap));
}

// >>> Verifica se um bloco 'b' está ocupado (retorna 1) ou livre (retorna 0).
static int bitmap_test(uint32_t b) {
    if (b >= BITMAP_BITS) return 1; // >>> Proteção: se pedir bloco fora do limite, diz que tá ocupado.
    // >>> Lógica bitwise:
    // >>> b >> 3 divide por 8 para achar o BYTE certo no vetor.
    // >>> b & 7 faz o resto da divisão por 8 para achar o BIT certo.
    return (g_bitmap[b >> 3] >> (b & 7)) & 1;
}

// >>> Define um bloco 'b' como ocupado (val=1) ou livre (val=0).
static void bitmap_set(uint32_t b, int val) {
    if (b >= BITMAP_BITS) return;
    if (val) 
        g_bitmap[b >> 3] |=  (uint8_t)(1u << (b & 7)); // >>> Liga o bit usando OR
    else     
        g_bitmap[b >> 3] &= (uint8_t)~(1u << (b & 7)); // >>> Desliga o bit usando AND e NOT
}

// >>> Escreve a struct g_sb (memória) para o setor 0 do disco.
static int super_write(Disk *d) {
    unsigned char sec[DISK_SECTORDATASIZE]; // >>> Buffer temporário de 512 bytes
    memset(sec, 0, sizeof(sec));
    
    // >>> Serialização: Converte inteiros da struct para bytes no buffer.
    ul2char(g_sb.magic,           &sec[0]);
    ul2char(g_sb.blockSize,       &sec[4]);
    ul2char(g_sb.sectorsPerBlock, &sec[8]);
    ul2char(g_sb.inodeSectors,    &sec[12]);
    ul2char(g_sb.dataStartSector, &sec[16]);
    ul2char(g_sb.totalBlocks,     &sec[20]);
    
    // >>> Grava fisicamente no disco.
    return diskWriteSector(d, SUPER_SECTOR, sec);
}

// >>> Lê o setor 0 do disco e preenche a struct g_sb na memória.
static int super_read(Disk *d) {
    unsigned char sec[DISK_SECTORDATASIZE];
    if (diskReadSector(d, SUPER_SECTOR, sec) < 0) return -1; // >>> Falha de hardware

    // >>> Desserialização: Converte bytes do buffer para inteiros na struct.
    char2ul(&sec[0],  &g_sb.magic);
    char2ul(&sec[4],  &g_sb.blockSize);
    char2ul(&sec[8],  &g_sb.sectorsPerBlock);
    char2ul(&sec[12], &g_sb.inodeSectors);
    char2ul(&sec[16], &g_sb.dataStartSector);
    char2ul(&sec[20], &g_sb.totalBlocks);

    // >>> Validações de segurança para garantir que o disco não está corrompido.
    if (g_sb.magic != MYFS_MAGIC) return -1; // >>> Não é um disco MyFS
    if (g_sb.blockSize == 0 || (g_sb.blockSize % DISK_SECTORDATASIZE) != 0) return -1;
    if (g_sb.sectorsPerBlock == 0) return -1;
    if (g_sb.totalBlocks == 0 || g_sb.totalBlocks > BITMAP_BITS) return -1;

    return 0;
}

// >>> Salva o bitmap atual da memória para o setor 1 do disco.
static int bitmap_write(Disk *d) {
    return diskWriteSector(d, BITMAP_SECTOR, g_bitmap);
}

// >>> Carrega o bitmap do setor 1 do disco para a memória.
static int bitmap_read(Disk *d) {
    return diskReadSector(d, BITMAP_SECTOR, g_bitmap);
}

/* Procura inode livre APENAS na área reservada de inodes do MyFS */
// >>> Percorre a tabela de Inodes no disco procurando um vazio para criar um arquivo novo.
static unsigned int myfs_find_free_inode(Disk *d) {
    unsigned int perSector = inodeNumInodesPerSector();
    unsigned int maxInodes = g_sb.inodeSectors * perSector; // >>> Limite total de inodes

    // >>> Começa do 2, pois o 1 é a Raiz.
    for (unsigned int n = 2; n <= maxInodes; n++) {
        Inode *i = inodeLoad(n, d); // >>> Carrega inode do disco
        if (!i) return 0;

        // >>> Verifica se está zerado (livre)
        int isFree =
            (inodeGetFileType(i) == 0) &&
            (inodeGetRefCount(i) == 0) &&
            (inodeGetFileSize(i) == 0) &&
            (inodeGetNextNumber(i) == 0) &&
            (inodeGetBlockAddr(i, 0) == 0);
        free(i); // >>> Libera memória auxiliar

        if (isFree) return n; // >>> Achou! Retorna o número do Inode.
    }
    return 0; // >>> Nenhum inode livre (disco cheio de arquivos, mesmo se tiver espaço em blocos).
}

// >>> Encontra um slot livre na tabela de arquivos abertos (g_fd).
static int fd_alloc(void) {
    for (int i = 0; i < MAX_FDS; i++) {
        if (!g_fd[i].used) {
            g_fd[i].used = 1;
            g_fd[i].cursor = 0; // >>> Arquivo abre com cursor no início
            g_fd[i].inumber = 0;
            return i + 1; // >>> Retorna o File Descriptor (FD), que começa em 1.
        }
    }
    return -1; // >>> Tabela cheia (muitos arquivos abertos).
}

// >>> Converte o FD (usuário) para o índice do array g_fd (interno). Valida se existe.
static int fd_slot(int fd) {
    int s = fd - 1;
    if (s < 0 || s >= MAX_FDS) return -1;
    if (!g_fd[s].used) return -1;
    return s;
}

/* =========================
   Helpers: blocos e IO
   ========================= */

// >>> Procura um bit '0' no bitmap para alocar um novo bloco de dados.
static int alloc_block_index(void) {
    for (uint32_t b = 0; b < g_sb.totalBlocks && b < BITMAP_BITS; b++) {
        if (!bitmap_test(b)) { // >>> Se bit for 0 (livre)
            bitmap_set(b, 1);  // >>> Marca como ocupado
            return (int)b;     // >>> Retorna índice do bloco
        }
    }
    return -1; // >>> Disco cheio (sem blocos livres).
}

// >>> Converte índice de bloco lógico para o número do Setor Físico no disco.
// >>> Ex: Bloco 0 pode ser o setor 130 (após superbloco, bitmap e inodes).
static unsigned long block_to_sector(uint32_t blockIndex) {
    return (unsigned long)g_sb.dataStartSector +
           (unsigned long)blockIndex * (unsigned long)g_sb.sectorsPerBlock;
}

// >>> Limpa um bloco no disco (escreve zeros) para garantir que não haja lixo de arquivos antigos.
static int zero_block(Disk *d, uint32_t blockIndex) {
    unsigned char sec[DISK_SECTORDATASIZE];
    memset(sec, 0, sizeof(sec));
    unsigned long start = block_to_sector(blockIndex);
    
    // >>> Um bloco pode ter vários setores, tem que zerar todos.
    for (uint32_t s = 0; s < g_sb.sectorsPerBlock; s++) {
        if (diskWriteSector(d, start + s, sec) < 0) return -1;
    }
    return 0;
}

// >>> Garante que o arquivo tenha um bloco físico alocado para a posição lógica desejada.
static int ensure_file_block(Disk *d, Inode *ino, uint32_t logicalBlock) {
    unsigned int addr = inodeGetBlockAddr(ino, logicalBlock);
    if (addr != 0) return 0; // >>> Já existe bloco alocado, tudo certo.

    // >>> Precisa alocar novo bloco.
    int b = alloc_block_index();
    if (b < 0) return -1; // >>> Sem espaço.

    if (zero_block(d, (uint32_t)b) < 0) return -1;

    // >>> Associa o setor físico encontrado ao Inode.
    unsigned long startSector = block_to_sector((uint32_t)b);
    if (inodeAddBlock(ino, (unsigned int)startSector) < 0) return -1;

    return 0;
}

// >>> Função complexa de leitura. Lê 'nbytes' do arquivo 'inumber' a partir de 'offset'.
static int file_read_at(Disk *d, uint32_t inumber, uint32_t offset, char *buf, uint32_t nbytes) {
    Inode *ino = inodeLoad(inumber, d); // >>> Carrega metadados do arquivo
    if (!ino) return -1;

    uint32_t fsz = inodeGetFileSize(ino);
    // >>> Se tentar ler além do fim do arquivo, ajusta ou retorna.
    if (offset >= fsz) { free(ino); return 0; }
    uint32_t toRead = nbytes;
    if (offset + toRead > fsz) toRead = fsz - offset; // >>> Não lê lixo após fim do arquivo.

    uint32_t done = 0; // >>> Contados de bytes lidos
    while (done < toRead) {
        uint32_t pos = offset + done;
        // >>> Calcula em qual bloco lógico (0, 1, 2...) está o dado.
        uint32_t lblock = pos / g_sb.blockSize;
        // >>> Calcula o deslocamento dentro desse bloco.
        uint32_t inoff  = pos % g_sb.blockSize;

        // >>> Pega o endereço físico no disco através do Inode.
        unsigned int startSector = inodeGetBlockAddr(ino, lblock);
        if (startSector == 0) break; // >>> Buraco no arquivo (esparso)? Para.

        // >>> Buffer temporário para ler o bloco inteiro do disco.
        unsigned char blockBuf[4096]; 
        if (g_sb.blockSize > sizeof(blockBuf)) { free(ino); return -1; }

        // >>> Lê todos os setores que compõem esse bloco.
        for (uint32_t s = 0; s < g_sb.sectorsPerBlock; s++) {
            if (diskReadSector(d, (unsigned long)startSector + s,
                               &blockBuf[s * DISK_SECTORDATASIZE]) < 0) {
                free(ino);
                return -1;
            }
        }

        // >>> Copia apenas a parte útil do bloco para o buffer do usuário.
        uint32_t chunk = toRead - done;
        uint32_t room = g_sb.blockSize - inoff; // >>> Espaço restante neste bloco
        if (chunk > room) chunk = room;

        memcpy(buf + done, blockBuf + inoff, chunk);
        done += chunk;
    }

    free(ino);
    return (int)done;
}

// >>> Função complexa de escrita. Escreve 'nbytes' no arquivo.
static int file_write_at(Disk *d, uint32_t inumber, uint32_t offset, const char *buf, uint32_t nbytes) {
    Inode *ino = inodeLoad(inumber, d);
    if (!ino) return -1;

    uint32_t done = 0;
    while (done < nbytes) {
        uint32_t pos = offset + done;
        uint32_t lblock = pos / g_sb.blockSize;
        uint32_t inoff  = pos % g_sb.blockSize;

        // >>> Se o bloco não existe, aloca um novo (Write Allocation).
        if (ensure_file_block(d, ino, lblock) < 0) { free(ino); return -1; }
        
        unsigned int startSector = inodeGetBlockAddr(ino, lblock);
        if (startSector == 0) { free(ino); return -1; }

        unsigned char blockBuf[4096];
        if (g_sb.blockSize > sizeof(blockBuf)) { free(ino); return -1; }

        // >>> Read-Modify-Write: Lê o bloco atual do disco...
        for (uint32_t s = 0; s < g_sb.sectorsPerBlock; s++) {
            if (diskReadSector(d, (unsigned long)startSector + s,
                               &blockBuf[s * DISK_SECTORDATASIZE]) < 0) {
                free(ino);
                return -1;
            }
        }

        uint32_t chunk = nbytes - done;
        uint32_t room = g_sb.blockSize - inoff;
        if (chunk > room) chunk = room;

        // >>> ...Modifica apenas a parte necessária na memória...
        memcpy(blockBuf + inoff, buf + done, chunk);

        // >>> ...Escreve o bloco alterado de volta pro disco.
        for (uint32_t s = 0; s < g_sb.sectorsPerBlock; s++) {
            if (diskWriteSector(d, (unsigned long)startSector + s,
                                &blockBuf[s * DISK_SECTORDATASIZE]) < 0) {
                free(ino);
                return -1;
            }
        }
        done += chunk;
    }

    // >>> Atualiza o tamanho do arquivo no Inode se cresceu.
    uint32_t fsz = inodeGetFileSize(ino);
    if (offset + done > fsz) {
        inodeSetFileSize(ino, offset + done);
        if (inodeSave(ino) < 0) { free(ino); return -1; } // >>> Persiste metadados atualizados
    }

    free(ino);
    return (int)done;
}

/* =========================
   "Diretório raiz" interno
   ========================= */

// >>> Verifica se o nome do arquivo é válido (tamanho e sem barras).
static int valid_name(const char *name) {
    if (!name || name[0] == '\0') return 0;
    if (strlen(name) > MAX_FILENAME_LENGTH) return 0;
    for (const char *p = name; *p; p++) if (*p == '/') return 0; // >>> MyFS é "flat", não tem subpastas.
    return 1;
}

// >>> Busca linear: Lê o arquivo da raiz (Inode 1) procurando uma entrada com o nome 'name'.
static int root_find_entry(Disk *d, const char *name, MyDirEntry *out) {
    Inode *root = inodeLoad(MYFS_ROOT_INODE, d);
    if (!root) return -1;

    uint32_t sz = inodeGetFileSize(root);
    uint32_t off = 0;
    MyDirEntry e;
    
    // >>> Loop que lê structs MyDirEntry uma a uma.
    while (off + sizeof(MyDirEntry) <= sz) {
        int r = file_read_at(d, MYFS_ROOT_INODE, off, (char*)&e, (uint32_t)sizeof(MyDirEntry));
        if (r != (int)sizeof(MyDirEntry)) { free(root); return -1; }

        if (e.active && strcmp(e.name, name) == 0) { // >>> Achou nome igual e ativo?
            if (out) *out = e;
            free(root);
            return 1; // >>> Sucesso: Encontrado.
        }
        off += (uint32_t)sizeof(MyDirEntry);
    }

    free(root);
    return 0; // >>> Não encontrado.
}

// >>> Adiciona um novo arquivo ao diretório raiz.
static int root_add_entry(Disk *d, const char *name, uint32_t inumber) {
    Inode *root = inodeLoad(MYFS_ROOT_INODE, d);
    if (!root) return -1;

    uint32_t sz = inodeGetFileSize(root);
    uint32_t off = 0;
    int32_t freeOff = -1; // >>> Guardará posição de uma entrada deletada (reuso)
    MyDirEntry e;

    // >>> Varre para ver se o nome já existe e procura buracos livres.
    while (off + sizeof(MyDirEntry) <= sz) {
        int r = file_read_at(d, MYFS_ROOT_INODE, off, (char*)&e, (uint32_t)sizeof(MyDirEntry));
        if (r != (int)sizeof(MyDirEntry)) { free(root); return -1; }

        if (e.active && strcmp(e.name, name) == 0) { free(root); return -1; } // >>> Erro: Já existe.
        if (!e.active && freeOff < 0) freeOff = (int32_t)off; // >>> Achou espaço vago.
        off += (uint32_t)sizeof(MyDirEntry);
    }

    // >>> Cria a nova entrada na memória.
    MyDirEntry ne;
    memset(&ne, 0, sizeof(ne));
    ne.inumber = inumber;
    strncpy(ne.name, name, MAX_FILENAME_LENGTH);
    ne.name[MAX_FILENAME_LENGTH] = '\0';
    ne.active = 1;

    // >>> Se achou buraco, escreve lá. Se não, escreve no final do arquivo (append).
    uint32_t writeOff = (freeOff >= 0) ? (uint32_t)freeOff : sz;
    int w = file_write_at(d, MYFS_ROOT_INODE, writeOff, (const char*)&ne, (uint32_t)sizeof(MyDirEntry));

    free(root);
    return (w == (int)sizeof(MyDirEntry)) ? 0 : -1;
}

/* =========================
   API MyFS - Funções chamadas pelo VFS
   ========================= */

// >>> Verifica se o sistema pode ser desmontado (nenhum arquivo aberto).
int myFSIsIdle (Disk *d) {
    (void)d;
    for (int i = 0; i < MAX_FDS; i++) {
        if (g_fd[i].used) return 0; // >>> Tem arquivo aberto, não pode desmontar.
    }
    return 1;
}

/* 4 setores => 2048 bytes (exemplo de formatação) */
int myFSFormat (Disk *d, unsigned int blockSize) {
    if (!d) return -1;
    if (blockSize == 0) return -1;

    // >>> Valida se o bloco é múltiplo do setor (512 bytes).
    if (blockSize % DISK_SECTORDATASIZE != 0) return -1;

    uint32_t inodeSectors = 128; // >>> Reserva fixa de 128 setores para Inodes.
    unsigned long totalSectors = diskGetNumSectors(d);

    uint32_t blockSizeBytes  = (uint32_t)blockSize;
    uint32_t sectorsPerBlock = blockSizeBytes / DISK_SECTORDATASIZE;
    if (sectorsPerBlock == 0) return -1;

    if (blockSizeBytes > 4096) return -1; // >>> Limite de buffer interno.

    // >>> Calcula onde começam os dados: Superbloco + Bitmap + Inodes.
    uint32_t dataStart = 2u + inodeSectors;
    if ((unsigned long)dataStart >= totalSectors) return -1;

    // >>> Calcula quantos blocos cabem no espaço restante.
    uint32_t totalBlocks = (uint32_t)((totalSectors - dataStart) / sectorsPerBlock);
    if (totalBlocks == 0) return -1;
    if (totalBlocks > BITMAP_BITS) totalBlocks = BITMAP_BITS; // >>> Clampa no limite do bitmap.

    // >>> Preenche a struct do Superbloco.
    g_sb.magic           = MYFS_MAGIC;
    g_sb.blockSize       = blockSizeBytes;
    g_sb.sectorsPerBlock = sectorsPerBlock;
    g_sb.inodeSectors    = inodeSectors;
    g_sb.dataStartSector = dataStart;
    g_sb.totalBlocks     = totalBlocks;

    // >>> Grava Superbloco no disco.
    if (super_write(d) < 0) return -1;

    // >>> Prepara o bitmap: Zera tudo, mas marca como ocupado o que excede a capacidade do disco.
    bitmap_clear_all();
    for (uint32_t b = totalBlocks; b < BITMAP_BITS; b++) bitmap_set(b, 1);
    if (bitmap_write(d) < 0) return -1;

    // >>> Zera a área de inodes no disco para garantir "limpeza".
    unsigned char zero[DISK_SECTORDATASIZE];
    memset(zero, 0, sizeof(zero));
    for (uint32_t s = 0; s < inodeSectors; s++) {
        if (diskWriteSector(d, 2u + s, zero) < 0) return -1;
    }

    // >>> Cria o Inode Raiz (Inode 1) obrigatoriamente.
    Inode *root = inodeCreate(MYFS_ROOT_INODE, d);
    if (!root) return -1;
    inodeSetFileType(root, FILETYPE_DIR); // >>> É um diretório.
    inodeSetFileSize(root, 0);
    inodeSetRefCount(root, 1);
    if (inodeSave(root) < 0) { free(root); return -1; }
    free(root);

    return (int)totalBlocks;
}

// >>> Função de Montagem/Desmontagem. x=1 monta, x=0 desmonta.
int myFSxMount (Disk *d, int x) {
    if (x == 1) { // >>> Mount
        if (!d) return 0;
        // >>> Lê metadados vitais do disco para a RAM.
        if (super_read(d) < 0) return 0;
        if (bitmap_read(d) < 0) return 0;

        // >>> Limpa tabela de FDs.
        for (int i=0; i<MAX_FDS; i++) memset(&g_fd[i], 0, sizeof(g_fd[i]));
        g_mounted = 1;

        // >>> Teste se consegue ler o root.
        Inode *root = inodeLoad(MYFS_ROOT_INODE, d);
        if (!root) { g_mounted = 0; return 0; }
        free(root);
        return 1;
    } else { // >>> Unmount
        if (!g_mounted) return 0;
        if (!myFSIsIdle(d)) return 0; // >>> Recusa se tiver arquivos abertos.
        
        // >>> Salva bitmap atualizado antes de sair.
        if (bitmap_write(d) < 0) return 0;
        g_mounted = 0;
        return 1;
    }
}

// >>> Abre arquivo pelo caminho (ex: /arquivo.txt).
int myFSOpen (Disk *d, const char *path) {
    if (!g_mounted || !d || !path) return -1;
    if (path[0] != '/') return -1; // >>> Caminho deve ser absoluto.

    const char *name = path + 1; // >>> Pula a barra '/'
    if (!valid_name(name)) return -1;

    MyDirEntry e;
    // >>> Procura no diretório raiz.
    int f = root_find_entry(d, name, &e);
    uint32_t inum = 0;

    if (f == 1) { 
        inum = e.inumber; // >>> Arquivo já existe, pega o Inode dele.
    } else if (f == 0) { // >>> Arquivo não existe, vamos criar.
        uint32_t maxInodes = g_sb.inodeSectors * inodeNumInodesPerSector();
        uint32_t newNum = myfs_find_free_inode(d); // >>> Acha Inode livre.
        if (newNum == 0 || newNum > maxInodes) return -1;

        Inode *ni = inodeCreate(newNum, d);
        if (!ni) return -1;

        inodeSetFileType(ni, FILETYPE_REGULAR); // >>> Cria como arquivo regular.
        inodeSetFileSize(ni, 0);
        inodeSetRefCount(ni, 1);

        if (inodeSave(ni) < 0) { free(ni); return -1; }
        free(ni);

        // >>> Adiciona nome e número no diretório raiz.
        if (root_add_entry(d, name, newNum) < 0) return -1;
        inum = newNum;
    } else {
        return -1; // >>> Erro na leitura do diretório.
    }

    // >>> Aloca um descritor de arquivo para o usuário.
    int fd = fd_alloc();
    if (fd < 0) return -1;

    int s = fd_slot(fd);
    if (s < 0) return -1;

    g_fd[s].inumber = inum; // >>> Associa FD ao Inode.
    g_fd[s].cursor  = 0;

    return fd;
}

// >>> Lê dados do arquivo aberto pelo FD.
int myFSRead (int fd, char *buf, unsigned int nbytes) {
    if (!g_mounted || !buf) return -1;
    int s = fd_slot(fd);
    if (s < 0) return -1;

    // >>> Chama a função complexa que mapeia blocos lógicos -> físicos.
    int r = file_read_at(rootDisk, g_fd[s].inumber, g_fd[s].cursor, buf, nbytes);
    
    // >>> Avança o cursor (ponteiro de leitura).
    if (r >= 0) g_fd[s].cursor += (uint32_t)r;
    return r;
}

// >>> Escreve dados no arquivo aberto.
int myFSWrite (int fd, const char *buf, unsigned int nbytes) {
    if (!g_mounted || !buf) return -1;
    int s = fd_slot(fd);
    if (s < 0) return -1;

    // >>> Chama a função complexa de escrita.
    int w = file_write_at(rootDisk, g_fd[s].inumber, g_fd[s].cursor, buf, nbytes);
    
    // >>> Avança o cursor.
    if (w >= 0) g_fd[s].cursor += (uint32_t)w;
    return w;
}

// >>> Fecha o arquivo (libera o FD).
int myFSClose (int fd) {
    int s = fd_slot(fd);
    if (s < 0) return -1;
    memset(&g_fd[s], 0, sizeof(g_fd[s])); // >>> Zera a entrada na tabela.
    return 0;
}

/* =========================
   Stubs (Funções vazias para completar a API do VFS)
   ========================= */
// >>> O MyFS deste projeto é simples e não suporta subdiretórios ou links simbólicos,
// >>> então estas funções retornam erro (-1) para satisfazer o compilador/VFS.

int myFSOpenDir (Disk *d, const char *path) { (void)d; (void)path; return -1; }
int myFSReadDir (int fd, char *filename, unsigned int *inumber) { (void)fd; (void)filename; (void)inumber; return -1; }
int myFSLink (int fd, const char *filename, unsigned int inumber) { (void)fd; (void)filename; (void)inumber; return -1; }
int myFSUnlink (int fd, const char *filename) { (void)fd; (void)filename; return -1; }
int myFSCloseDir (int fd) { (void)fd; return -1; }

/* =========================
   Instalacao no VFS
   ========================= */

// >>> Função chamada pelo main.c para registrar o MyFS no sistema.
int installMyFS (void) {
    static FSInfo info;
    memset(&info, 0, sizeof(info));
    info.fsid = 'M';           // >>> ID único do FS
    info.fsname = "MyFS";      // >>> Nome legível

    // >>> Conecta as funções que criamos às chamadas genéricas do VFS.
    info.isidleFn = myFSIsIdle;
    info.formatFn = myFSFormat;
    info.xMountFn = myFSxMount;
    info.openFn   = myFSOpen;
    info.readFn   = myFSRead;
    info.writeFn  = myFSWrite;
    info.closeFn  = myFSClose;

    // >>> Conecta os stubs de diretório.
    info.opendirFn  = myFSOpenDir;
    info.readdirFn  = myFSReadDir;
    info.linkFn     = myFSLink;
    info.unlinkFn   = myFSUnlink;
    info.closedirFn = myFSCloseDir;

    return vfsRegisterFS(&info);
}