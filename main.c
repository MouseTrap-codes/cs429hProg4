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

//---------------------------------------------------------------------
// Regex helper functions
//---------------------------------------------------------------------
int match_regex(const char *pattern, const char *instruction) {
    regex_t regex;
    int result;

    // Compile the regex
    result = regcomp(&regex, pattern, REG_EXTENDED);
    if (result) {
        printf("Could not compile regex\n");
        return 0;
    }
    // Execute the regex
    result = regexec(&regex, instruction, 0, NULL, 0);
    regfree(&regex);  // Free the compiled regex
    return !result;   // Return 1 if it matches, 0 otherwise
}

// check if any given line is a valid instruction
int is_valid_instruction(const char *instruction) {
    regex_t regex;
    const char *pattern =
        "^(add|addi|sub|subi|mul|div|and|or|xor|not|shftr|shftri|shftl|shftli|"
        "br|brr|brnz|call|return|brgt|addf|subf|mulf|divf|mov|in|out|clr|ld|push|pop|halt)"
        "([ \t]+r[0-9]+(,[ \t]*r[0-9]+(,[ \t]*r[0-9]+)?)?[ \t]*(,[ \t]*(:[A-Za-z][A-Za-z0-9]*|[0-9]+))?)?$";
    
    int result = regcomp(&regex, pattern, REG_EXTENDED);
    if (result) {
        printf("Could not compile regex\n");
        return 0;
    }
    result = regexec(&regex, instruction, 0, NULL, 0);
    regfree(&regex);
    return !result;  // Return 1 if matched, 0 otherwise.
}

//---------------------------------------------------------------------
// Trim leading and trailing whitespace (in–place)
//---------------------------------------------------------------------
void trim(char *s) {
    char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
    // Trim trailing whitespace
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }
}

//---------------------------------------------------------------------
// Pass 1: Calculate label addresses.
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
        if (strlen(line) == 0)
            continue;
        
        // Skip comment lines (those that start with a semicolon)
        if (line[0] == ';') {
            continue;
        }
        
        // Check for directives (.code and .data)
        if (line[0] == '.') {
            if (strncmp(line, ".code", 5) == 0) {
                currentSection = CODE;
            } else if (strncmp(line, ".data", 5) == 0) {
                currentSection = DATA;
            }
            continue;
        }
        
        // Process label definitions (lines starting with ':')
        if (line[0] == ':') {
            char label[50];
            sscanf(line + 1, "%s", label);
            int addr = programCounter;
            add_label(label, addr);
            continue;
        }
        
        // For instructions (or data items) update the program counter.
        if (currentSection == CODE) {
            if (is_valid_instruction(line)) {
                if (match_regex("ld\\s+r[0-9]+,\\s+[0-9]+", line))
                    programCounter += 48; // 12-line macro expansion
                else if (match_regex("push\\s+r[0-9]+", line))
                    programCounter += 8;  // 2-line macro expansion
                else if (match_regex("pop\\s+r[0-9]+", line))
                    programCounter += 8;  // 2-line macro expansion
                else
                    programCounter += 4;  // normal instruction
            } else {
                exit(-1); // Invalid instruction -> exit program!
            }
        } else if (currentSection == DATA) {
            programCounter += 8;  // Each data item is 8 bytes
        }
    }
    
    fclose(fin);
}

//---------------------------------------------------------------------
// Macro expansion functions (for useful macros)
//---------------------------------------------------------------------
void expandIn(int rD, int rS, FILE* output) {
    fprintf(output, "\tpriv r%d, r%d, 0, 3\n", rD, rS);
}

void expandOut(int rD, int rS, FILE* output) {
    fprintf(output, "\tpriv r%d, r%d, 0, 4\n", rD, rS);
}

void expandClr(int rD, FILE* output) {
    fprintf(output, "\txor r%d, r%d, r%d\n", rD, rD, rD);
}

void expandHalt(FILE* output) {
    fprintf(output, "\tpriv r0, r0, r0, 0\n");
}

void expandPush(int rD, FILE* output) {
    fprintf(output, "\tmov r%d, (r31)(0)\n", rD);
    fprintf(output, "\tsubi r31, r31, 8\n");
}

void expandPop(int rD, FILE* output) {
    fprintf(output, "\tmov r%d, (r31)(0)\n", rD);
    fprintf(output, "\taddi r31, r31, 8\n");
}

void expandLd(int rD, uint64_t L, FILE* output) {
    // Example expansion for ld (adjust if necessary)
    fprintf(output, "\txor r0, r0, r0\n");
    fprintf(output, "\taddi r%d, r0, %llu\n", rD, (L >> 52) & 0xFFF);
    fprintf(output, "\tshiftli r%d, 12\n", rD);
    fprintf(output, "\taddi r%d, r0, %llu\n", rD, (L >> 40) & 0xFFF);
    fprintf(output, "\tshftli r%d, 12\n", rD);
    fprintf(output, "\taddi r%d, r0, %llu\n", rD, (L >> 28) & 0xFFF);
    fprintf(output, "\tshftli r%d, 12\n", rD);
    fprintf(output, "\taddi r%d, r0, %llu\n", rD, (L >> 16) & 0xFFF);
    fprintf(output, "\tshftli r%d, 4\n", rD);
    fprintf(output, "\taddi r%d, r0, %llu\n", rD, (L >> 4) & 0xFFF);
    fprintf(output, "\tshftli r%d, 4\n", rD);
    fprintf(output, "\taddi r%d, r0, %llu\n", rD, L & 0xF);
}

//---------------------------------------------------------------------
// parseMacro(): Extract parameters from a macro line and call the expansion function.
//---------------------------------------------------------------------
void parseMacro(const char *line, FILE *output) {
    char op[16];
    int rD, rS;
    uint64_t immediate;

    // Read the operation mnemonic (e.g. "in", "out", etc.)
    if (sscanf(line, "%15s", op) != 1) {
        fprintf(stderr, "Error: Could not read macro operation from line: %s\n", line);
        return;
    }

    if (strcmp(op, "in") == 0) {
        // Expected format: in r<dest>, r<src>
        if (sscanf(line, "in%*[ \t]r%d%*[ \t,]r%d", &rD, &rS) == 2)
            expandIn(rD, rS, output);
        else
            fprintf(stderr, "Error parsing 'in' macro: %s\n", line);
    }
    else if (strcmp(op, "out") == 0) {
        // Expected format: out r<dest>, r<src>
        if (sscanf(line, "out%*[ \t]r%d%*[ \t,]r%d", &rD, &rS) == 2)
            expandOut(rD, rS, output);
        else
            fprintf(stderr, "Error parsing 'out' macro: %s\n", line);
    }
    else if (strcmp(op, "clr") == 0) {
        // Expected format: clr r<dest>
        if (sscanf(line, "clr%*[ \t]r%d", &rD) == 1)
            expandClr(rD, output);
        else
            fprintf(stderr, "Error parsing 'clr' macro: %s\n", line);
    }
    else if (strcmp(op, "halt") == 0) {
        // halt has no parameters.
        expandHalt(output);
    }
    else if (strcmp(op, "push") == 0) {
        // Expected format: push r<reg>
        if (sscanf(line, "push%*[ \t]r%d", &rD) == 1)
            expandPush(rD, output);
        else
            fprintf(stderr, "Error parsing 'push' macro: %s\n", line);
    }
    else if (strcmp(op, "pop") == 0) {
        // Expected format: pop r<reg>
        if (sscanf(line, "pop%*[ \t]r%d", &rD) == 1)
            expandPop(rD, output);
        else
            fprintf(stderr, "Error parsing 'pop' macro: %s\n", line);
    }
    else if (strcmp(op, "ld") == 0) {
        // Expected format: ld r<dest>, L
        // Here L is a literal (unsigned 64-bit immediate)
        if (sscanf(line, "ld%*[ \t]r%d%*[ \t,]%llu", &rD, &immediate) == 2)
            expandLd(rD, immediate, output);
        else
            fprintf(stderr, "Error parsing 'ld' macro: %s\n", line);
    }
    else {
        // If the macro is not recognized, just print the line unchanged.
        fprintf(output, "\t%s\n", line);
    }
}

//---------------------------------------------------------------------
// Pass 2: Process the input file, resolve labels, and expand macros.
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
        // Remove newline and trim the line
        line[strcspn(line, "\n")] = '\0';
        trim(line);
        if (strlen(line) == 0 || line[0] == ';')
            continue;
        
        // Handle .code directive
        if (strcmp(line, ".code") == 0) {
            if (!in_code_section) {
                fprintf(fout, ".code\n");
                in_code_section = 1;
            }
            continue;
        }
        
        // Handle .data directive (reset in_code_section flag)
        if (strcmp(line, ".data") == 0) {
            fprintf(fout, ".data\n");
            in_code_section = 0;
            continue;
        }
        
        // Skip lines that are just labels (e.g., :LABEL)
        if (line[0] == ':' && strlen(line) > 1 &&
            strspn(line + 1, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") == strlen(line + 1)) {
            continue;
        }
        
        // Check for label references and replace them with their decimal addresses.
        char *label_start = strstr(line, ":");
        if (label_start) {
            char label[50];
            sscanf(label_start + 1, "%s", label);
            LabelAddress *entry = find_label(label);
            if (entry) {
                char buffer[1024];
                *label_start = '\0';  // Split the line before the label
                snprintf(buffer, sizeof(buffer), "\t%s%d", line, entry->address);
                fprintf(fout, "%s\n", buffer);
            } else {
                printf("Warning: Label '%s' not found.\n", label);
                fprintf(fout, "\t%s\n", line);
            }
        }
        else {
            // No label reference: if the line is a macro, expand it using parseMacro().
            if (match_regex("^(in|out|clr|halt|push|pop|ld)\\b", line))
                parseMacro(line, fout);
            else
                fprintf(fout, "\t%s\n", line);
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
    
    // For demonstration, you can print the label-address pairs:
    // LabelAddress *entry, *tmp;
    // HASH_ITER(hh, hashmap, entry, tmp) {
    //     printf("Label: %s, Address: %d\n", entry->label, entry->address);
    // }
    
    free_hashmap();
    return 0;
}
