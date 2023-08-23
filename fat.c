#include "fat.h"
#include "ds.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <wchar.h>

#define SUPER 0
#define DIR 1
#define TABLE 2

#define SIZE 1024

// the superblock
#define MAGIC_N           0xAC0010DE
typedef struct{
	int magic;
	int number_blocks;
	int n_fat_blocks;
	char empty[BLOCK_SIZE-3*sizeof(int)];
} super;

super sb;

//item
#define MAX_LETTERS 6
#define OK 1
#define NON_OK 0
#define EMPTY -1
typedef struct{
	unsigned char used;
	char name[MAX_LETTERS+1];
	unsigned int length;
	unsigned int first;
} dir_item;

#define N_ITEMS (BLOCK_SIZE / sizeof(dir_item))
dir_item dir[N_ITEMS];

// table
#define FREE 0
#define EOFF 1
#define BUSY 2
/* fat foi implementado assim para que ela ocupasse o bloco inteiro
   apesar disso, em nenhum momento foi utilizado mais do que N elementos
   da fat já que todos os laços que a manipulam estão limitados por ds_size()
*/ 
#define N_FAT_ENTRIES (BLOCK_SIZE / sizeof(int))
unsigned int fat[N_FAT_ENTRIES];

// 0 = Não montado
// 1 = Montado
int mountState = 0;

#define NUM_BLOCKS (ds_size())

int fat_format(){
    if(mountState == 1) {
        return -1;
    }

    // Inicializa o superbloco
    sb.magic = MAGIC_N;
    sb.number_blocks = ds_size();
    sb.n_fat_blocks = 1;

    // Inicializa o bloco de diretório
    for(int i = 0; i < N_ITEMS; i++) 
        dir[i].used = 0;

    // Inicializa o bloco de FAT
    fat[0] = EOFF; // Superbloco
    fat[1] = EOFF; // Diretório
    fat[2] = EOFF; // Tabela FAT
    for(int i = 3; i < ds_size(); i++)
        fat[i] = FREE;

    // Grava as informações no disco
    ds_write(SUPER, (char *)&sb);
    ds_write(DIR, (char *)dir);
    ds_write(TABLE, (char *)fat);
    
    return 0;
}

void fat_debug(){
    char buff[BLOCK_SIZE];
    char magic_number_msg[] = "not ok";
    
    // Mostra as informações do superbloco
    ds_read(SUPER, buff);
    memcpy(&sb, buff, BLOCK_SIZE);

    if(sb.magic == MAGIC_N)
        strcpy(magic_number_msg, "ok");
    printf("superblock:");
    printf("  magic is %s\n", magic_number_msg);
    printf("  %d blocks\n", sb.number_blocks);
    printf("  %d block fat\n", sb.n_fat_blocks);

    // Mostra as informações de todos os arquivos
    ds_read(DIR, buff);
    memcpy(dir, buff, BLOCK_SIZE);
    ds_read(TABLE, buff);
    memcpy(fat, buff, BLOCK_SIZE);
    
    for(int i = 0; i < N_ITEMS; i++) {
        if(dir[i].used == NON_OK) continue;
        printf("file %s:\n", dir[i].name);
        printf("  size: %d\n", dir[i].length);
        printf("  blocks:");
        int block_num = dir[i].first;
        while(block_num != EOFF) {
            printf(" %d", block_num);
            block_num = fat[block_num];
        }
        printf("\n");
    }
}

int fat_mount(){
    char buff[BLOCK_SIZE];
    ds_read(SUPER, buff);
    memcpy(&sb, buff, BLOCK_SIZE);
    ds_read(DIR, buff);
    memcpy(dir, buff, BLOCK_SIZE);
    ds_read(TABLE, buff);
    memcpy(fat, buff, BLOCK_SIZE);
    mountState = 1;
  	return 0;
}

int fat_create(char *name){
    if(mountState == 0) {
        return -1;
    }
    
    // Encontra a próxima entrada do diretório
    int free_item;
    for(free_item = 0; free_item < N_ITEMS; free_item++) {
        if(dir[free_item].used == NON_OK) {
            break;
        }
    }

    // Caso não tenha espaço no diretório retorna um erro
    if(free_item == N_ITEMS) {
        return -1;
    }

    // Procura por um espaço livre na FAT
    int free_fat;
    for(free_fat = 0; free_fat < ds_size(); free_fat++) {
        if(fat[free_fat] == FREE) {
            fat[free_fat] = EOFF;
            break;
        }
    }

    // Caso não tenha espaço na FAT retorna um erro
    if(free_fat == ds_size()) {
        return -1;
    }

    // Atualiza a memória primária
    dir[free_item].used = OK;
    dir[free_item].length = 0;
    dir[free_item].first = free_fat;
    strcpy(dir[free_item].name, name);

    // Atualiza na memória secundária
    ds_write(TABLE, (char *)fat);
    ds_write(DIR, (char *)dir);
    
  	return 0;
}

int fat_delete( char *name){
    if(mountState == 0) {
        return -1;
    }
    int next_block;
    int aux;

    // Muda a flag do diretório para 0 (a posição está livre)
    // E também sinaliza que os blocos estão livres na FAT
    int i;
    for(i = 0; i < N_ITEMS; i++) {
        if(dir[i].used == OK) {
            if(strcmp(dir[i].name, name) == 0) {
                dir[i].used = NON_OK;
                next_block = dir[i].first;
                while(next_block != EOFF) {
                    aux = fat[next_block];
                    fat[next_block] = FREE;
                    next_block = aux;
                }
            }
            break;
        }
    }

    // Caso o arquivo não exista retorna erro
    if(i == N_ITEMS) {
        return -1;
    }

    // Atualiza na memória secundária
    ds_write(DIR, (char *)dir);
    ds_write(TABLE, (char *)fat);
    return 0;
}

int fat_getsize(char *name){ 
    if(mountState == 0) {
        return -1;
    }
    for(int i = 0; i < N_ITEMS; i++) { 
        if(dir[i].used == OK) {
            if(strcmp(dir[i].name, name) == 0) {
                return dir[i].length;
            }
        }
    }
    // Caso o arquivo não exista retorna erro
    return -1;
}


int fat_read( char *name, char *buff, int length, int offset){
    if(mountState == 0) return -1;
    if(offset < 0) return -1;

    char aux_buff[length];
    char read_buff[BLOCK_SIZE];
    int i, block, read_bytes = 0, n, m, file_len;
    for(i = 0; i < N_ITEMS; i++) {
        // Encontrar o arquivo desejado
        if(dir[i].used == OK && (strcmp(dir[i].name, name) == 0)) {
            // Caso o offset seja maior que o tamanho do arquivo retorna -1
            if(offset >= dir[i].length) return -1;
            if(dir[i].length == 0) return 0;

            // Armazena o tamanho do arquivo
            file_len = dir[i].length;
            
            // Procura em qual bloco começar baseado no tamanho do offset
            // Ou seja, caso o offset seja maior do que um bloco, começa a leitura
            // a partir do próximo e assim por diante. 
            block = dir[i].first;
            while(offset % BLOCK_SIZE == 0 && offset != 0) {
                block = fat[block];
                offset -= BLOCK_SIZE;
            }
            if(offset < 0) return -1;

            // A partir do bloco descoberto no passo anterior, começa a leitura
            // `length` bytes do arquivo ou até ele chegar no final.
            do {
                ds_read(block, read_buff);
                
                // Concatena os bytes lidos com a saída
                n = fmin(BLOCK_SIZE, length);
                snprintf(aux_buff, read_bytes + n, "%s%s", buff, read_buff+offset);
                strncpy(buff, aux_buff, sizeof(aux_buff));
                m = fmin(n, file_len);
                read_bytes += m;
                file_len -= n;

                // Caso já tenha copiado todos os bytes do arquivo, retorna
                if(file_len <= 0) return read_bytes;
                
                // Após a primeira iteração do loop o offset é desnecessário
                if(offset > 0) offset = 0;
                
                // Atualiza o valor de length diminuindo o número de bytes lidos
                length -= BLOCK_SIZE;
                // Avança para o próximo bloco
                block = fat[block];
            } while(length > 0 && block != EOFF);
        }
    }
    // Caso não encontre o arquivo retorna erro
    if(i == N_ITEMS) return -1;
	return read_bytes;
}

//Retorna a quantidade de caracteres escritos
int fat_write(char *name, const char *buff, int length, int offset){
    if(mountState == 0) return -1;
    if(offset < 0) return -1;
    if(length < 0) return -1;
    if(offset >= strlen(buff)) return 0;

    char aux_buff[BLOCK_SIZE];
    int i, block, last_block, written_bytes = 0;
    for(i = 0; i < N_ITEMS; i++) {
        // Encontrar o arquivo desejado
        if(dir[i].used == OK && (strcmp(dir[i].name, name) == 0)) { 
            // Navega pela FAT até o primeiro bloco com espaço
            // entre os blocos que o arquivo já ocupa
            block = dir[i].first;
            while(block != EOFF) {
                last_block = block; // Salva a posição da FAT
                block = fat[block];
            }

            int last_block_used_bytes = dir[i].length % BLOCK_SIZE;
            int n = fmin(BLOCK_SIZE, last_block_used_bytes + length);
            
            // Salva o máximo possível no primeiro bloco caso o arquivo seja vazio
            if(dir[i].length == 0) {
                ds_read(last_block, aux_buff);
                snprintf(aux_buff, n, "%s%s", aux_buff, buff+offset);
                written_bytes = n;
                ds_write(last_block, aux_buff);
            }
            // Caso não seja o primeiro bloco, salva o máximo possível no último bloco
            // já alocado caso tenha espaço livre
            else if(last_block_used_bytes != 0) { 
                ds_read(last_block, aux_buff);
                snprintf(aux_buff, n, "%s%s", aux_buff, buff+offset);
                written_bytes = n - last_block_used_bytes;
                ds_write(last_block, aux_buff);
            }

            // Utilizados para depuraçãos
            /* printf("length: %d\n", length); */
            /* printf("written_bytes: %d\n", written_bytes); */
            /* printf("l - w: %d\n", length - written_bytes); */
            
            // Caso já tenha escrito tudo, encerra
            if((length - written_bytes) <= 0) {
                dir[i].length = written_bytes;
                ds_write(DIR, (char *)dir);
                ds_write(TABLE, (char *)fat);
                return written_bytes;
            }

            // Faz um laço que escreve bytes na memória até que não tenha mais blocos
            // disponíveis ou que tenha acabado a escrita
            while(1) {
                // Procura por um bloco livre
                for(int i = 3; i < ds_size(); i++) {
                    if(fat[i] == FREE) {
                        fat[last_block] = i;
                        block = i;
                        break;
                    }
                }
                // Caso 'block == EOFF' significa que não encontrou mais blocos vazios
                if(block == EOFF) {
                    dir[i].length = written_bytes;
                    ds_write(DIR, (char *)dir);
                    ds_write(TABLE, (char *)fat);
                    return written_bytes;
                }

                // Salva o valor no bloco
                int n = fmin(BLOCK_SIZE, length - written_bytes);
                ds_read(block, aux_buff);
                snprintf(aux_buff, n, "%s%s", aux_buff, buff+offset+written_bytes);
                written_bytes += n;
                ds_write(block, aux_buff);
                fat[block] = EOFF;

                // Verifica se todos os bytes foram escritos
                if((length - written_bytes) <= 0) {
                    dir[i].length = written_bytes;
                    ds_write(DIR, (char *)dir);
                    ds_write(TABLE, (char *)fat);
                    return written_bytes;
                }
            }
        }
    }
    dir[i].length = written_bytes;
    ds_write(DIR, (char *)dir);
    ds_write(TABLE, (char *)fat);
    return written_bytes;
}
