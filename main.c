#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include "uthash.h"

//// here we implement the hashmap of labels to addresses using uthash ////////////////////////////////////////////////////////////////////////////////////////////////////////////
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

// update a label address pair -> VERY IMPORTANT as this is how we end up matching the addresses to the labels
void update_label(char *label, int new_address) {
    LabelAddress *entry = find_label(label);
    if (entry != NULL) {
        entry->address = new_address;
        printf("Updated label '%s' with new address: %d\n", label, new_address);
    } else {
        printf("Label '%s' not found. Add it first if needed.\n", label);
    }
}

// delete a label - cause why not
void delete_label(char *label) {
    LabelAddress *entry = find_label(label);
    if (entry != NULL) {
        HASH_DEL(hashmap, entry);
        free(entry);
    }
}

// we free the hashmap after the program is completed so we don't get any memory leaks
void free_hashmap() {
    LabelAddress *current, *tmp;
    HASH_ITER(hh, hashmap, current, tmp) {
        HASH_DEL(hashmap, current);
        free(current);
    }
}


// trim leading and trailing whitespace (in–place)
void trim(char *s) {
    char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
    // trim trailing whitespace
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len-1])) {
        s[len-1] = '\0';
        len--;
    }
}

void pass1(const char *input_filename) {
    FILE *fin = fopen(input_filename, "r");
    if (!fin) {
        perror("Error opening input file");
        exit(1);
    }
    
    // keep track of track of the current section.
    typedef enum { NONE, CODE, DATA } Section;
    Section currentSection = NONE;
    
    int programCounter = 4096;
  
    
    char line[1024];
    while (fgets(line, sizeof(line), fin)) {
        // Remove the newline and trim the line
        line[strcspn(line, "\n")] = '\0';
        trim(line);
        if (strlen(line) == 0)
            continue;
        
        // skip comment lines (those that start with a semicolon)
        if (line[0] == ';')
            continue;
        
        // check for directives (.code and .data)
        if (line[0] == '.') {
            if (strncmp(line, ".code", 5) == 0) {
                currentSection = CODE;
            } else if (strncmp(line, ".data", 5) == 0) {
                currentSection = DATA;
            }
            // directives do not update the address counters.
            continue;
        }
        
        // Process label definitions (lines starting with ':')
        if (line[0] == ':') {
            char label[50];
            // extract the label name (ignoring the colon)
            sscanf(line + 1, "%s", label);
            // The label gets the current address based on the section
            int addr = (currentSection == CODE) ? codeAddress : dataAddress;
            add_label(label, addr);
            // Label definitions do not increment the address counter.
            continue;
        }
        
        // If the line is not a label or a directive, then it is either an instruction (in code)
        // or a data item (in data), so update the corresponding address counter.
        if (currentSection == CODE) {
            programCounter += 4;  // each code instruction is 4 bytes
        } else if (currentSection == DATA) {
            dataAddress += 8;  // each data item is 8 bytes
        }
    }
    
    fclose(fin);
}

    





int main(int argc, char *argv[]) {
    char buffer[1024];
    File *file = fopen ("data.txt". "r");

}