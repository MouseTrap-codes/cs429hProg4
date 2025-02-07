#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include "uthash.h"
#include <regex.h>
#include <ctype.h>
#include <string.h>

//---------------------------------------------------------------------
// Hashmap of labels to addresses using uthash
//---------------------------------------------------------------------
typedef struct {
    char label[50];
    int address;
    UT_hash_handle hh;
} LabelAddress;

LabelAddress *hashmap = NULL;

// add a new label address pair to the hashmap
void add_label(char *label, int address) {
    LabelAddress *entry = (LabelAddress *)malloc(sizeof(LabelAddress));
    if (entry == NULL) {
        printf("Memory allocation failed\n");
        return;
    }
    strncpy(entry->label, label, sizeof(entry->label) - 1);
    entry->label[sizeof(entry->label) - 1] = '\0';
    entry->address = address;
    HASH_ADD_STR(hashmap, label, entry);
}

// lookup a label address pair
LabelAddress *find_label(char *label) {
    LabelAddress *entry;
    HASH_FIND_STR(hashmap, label, entry);
    return entry;
}

// free the hashmap after the program is completed to avoid memory leaks
void free_hashmap() {
    LabelAddress *current, *tmp;
    HASH_ITER(hh, hashmap, current, tmp) {
        HASH_DEL(hashmap, current);
        free(current);
    }
}

//---------------------------------------------------------------------
// Trim leading and trailing whitespace (in-place)
//---------------------------------------------------------------------
void trim(char *s) {
    char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }
}

//---------------------------------------------------------------------
// Pass 1: Calculate label addresses
//---------------------------------------------------------------------
void pass1(const char *input_filename) {
    FILE *fin = fopen(input_filename, "r");
    if (!fin) {
        perror("Error opening input file");
        exit(1);
    }

    typedef enum { NONE, CODE, DATA } Section;
    Section currentSection = NONE;

    int programCounter = 4096;
    char line[1024];

    while (fgets(line, sizeof(line), fin)) {
        line[strcspn(line, "\n")] = '\0';
        trim(line);
        if (strlen(line) == 0) continue;
        if (line[0] == ';') continue;

        if (line[0] == '.') {
            if (strncmp(line, ".code", 5) == 0) currentSection = CODE;
            else if (strncmp(line, ".data", 5) == 0) currentSection = DATA;
            continue;
        }

        if (line[0] == ':') {
            char label[50];
            sscanf(line + 1, "%49s", label);
            add_label(label, programCounter);
            continue;
        }

        if (currentSection == CODE) {
            if (strstr(line, "push")) programCounter += 8;
            else if (strstr(line, "pop")) programCounter += 8;
            else programCounter += 4;
        } else if (currentSection == DATA) {
            programCounter += 8;
        }
    }
    fclose(fin);
}

//---------------------------------------------------------------------
// Pass 2: Generate expanded and resolved assembly output
//---------------------------------------------------------------------
void pass2(const char *input_filename, const char *output_filename) {
    FILE *fin = fopen(input_filename, "r");
    if (!fin) {
        perror("Error opening input file");
        exit(1);
    }
    FILE *fout = fopen(output_filename, "w");
    if (!fout) {
        perror("Error opening output file");
        fclose(fin);
        exit(1);
    }

    char line[1024];
    int in_code_section = 0;

    while (fgets(line, sizeof(line), fin)) {
        line[strcspn(line, "\n")] = '\0';
        trim(line);
        if (strlen(line) == 0 || line[0] == ';') continue;

        if (strcmp(line, ".code") == 0) {
            fprintf(fout, ".code\n");
            in_code_section = 1;
            continue;
        }

        if (strcmp(line, ".data") == 0) {
            fprintf(fout, ".data\n");
            in_code_section = 0;
            continue;
        }

        if (line[0] == ':' && strlen(line) > 1) continue;

        char *label_start = strstr(line, ":");
        if (label_start) {
            char label[50];
            sscanf(label_start + 1, "%49s", label);
            LabelAddress *entry = find_label(label);
            if (entry) {
                *label_start = '\0';
                fprintf(fout, "%s%d\n", line, entry->address);
            } else {
                printf("Warning: Label '%s' not found.\n", label);
                fprintf(fout, "%s\n", line);
            }
        } else {
            fprintf(fout, "%s\n", line);
        }
    }

    fclose(fin);
    fclose(fout);
}

//---------------------------------------------------------------------
// Main
//---------------------------------------------------------------------
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <inputfile> <outputfile>\n", argv[0]);
        return 1;
    }

    pass1(argv[1]);
    pass2(argv[1], argv[2]);
    free_hashmap();
    return 0;
}
