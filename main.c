#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include "uthash.h"
#include <regex.h>
#include <ctype.h>
#include <string.h>



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

/// here we make some regex for the macros ///////////////////////////////////////////////////////////

// match an instruction line to a regex pattern
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

    return !result;  // Return 1 if it matches, 0 otherwise
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

//////////////////////////////////////////////////////////////////////////////////////////////////


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
        if (line[0] == ';') {
            continue;
        }

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
        
        // process label definitions (lines starting with ':')
        if (line[0] == ':') {
            char label[50];
            // extract the label name (ignoring the colon)
            sscanf(line + 1, "%s", label);
            // The label gets the current address based on the section
            int addr = programCounter;
            add_label(label, addr);
            // Label definitions do not increment the address counter.
            continue;
        }
        
        // If the line is not a label or a directive, then it is either an instruction (in code)
        // or a data item (in data), so update the corresponding address counter.
        if (currentSection == CODE) {
            if (is_valid_instruction(line)) { // first we check if the current line is a valid expansion
                // need to check for special macros with multiline expansions
                if (match_regex("ld\\s+r[0-9]+,\\s+[0-9]+", line)) {
                    programCounter += 48; // 12 line macro expansion
                } else if (match_regex("push\\s+r[0-9]+", line)) {
                    programCounter += 8; // 2 line macro expansion
                } else if (match_regex("pop\\s+r[0-9]+", line)) {
                    programCounter += 8; // 2 line macro expansion
                } else {
                    programCounter += 4; // normal instruction
                }
            } else {
                exit(-1); // this line is not a valid instruction -> exit program!
            }
        } else if (currentSection == DATA) {
            programCounter += 8;  // each data item is 8 bytes
        } 
    }
    
    fclose(fin);
}

//// ignore this for now
//--------------------------------------------------------------
// Macro expansion functions
//--------------------------------------------------------------
void expandIn(int rD, int rS, FILE* output) {
    fprintf(output, "\tpriv r%d, r%d, 0, 3\n", rD, rS);
}

void expandOut(int rD, int rS, FILE* output) {
    fprintf(output, "\tpriv r%d, r%d, 0, 3\n", rD, rS);
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
    // Example expansion for ld (you may need to adjust if your ISA differs)
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

void expandMov_mr(int rD, int rS, FILE* output) {
    // Expand move from memory to register: assume this becomes a load
    fprintf(output, "\tld r%d, (r%d)\n", rD, rS);
}

void expandMov_rm(int rD, int rS, FILE* output) {
    // Expand move from register to memory: assume this becomes a store.
    // (Here rD is the register holding the memory address.)
    fprintf(output, "\tst r%d, (r%d)\n", rS, rD);
}

//--------------------------------------------------------------
// parseMacro(): Extract parameters from a macro line and call the expansion function.
//--------------------------------------------------------------
void parseMacro(const char *line, FILE *output) {
    char op[16];
    int rD, rS;
    uint64_t immediate;
    
    if (sscanf(line, "%15s", op) != 1) {
        fprintf(stderr, "Error: Could not read macro operation from line: %s\n", line);
        return;
    }
    
    if (strcmp(op, "in") == 0) {
        if (sscanf(line, "in%*[ \t]r%d%*[ \t,]r%d", &rD, &rS) == 2)
            expandIn(rD, rS, output);
        else
            fprintf(stderr, "Error parsing 'in' macro: %s\n", line);
    } else if (strcmp(op, "out") == 0) {
        if (sscanf(line, "out%*[ \t]r%d%*[ \t,]r%d", &rD, &rS) == 2)
            expandOut(rD, rS, output);
        else
            fprintf(stderr, "Error parsing 'out' macro: %s\n", line);
    } else if (strcmp(op, "clr") == 0) {
        if (sscanf(line, "clr%*[ \t]r%d", &rD) == 1)
            expandClr(rD, output);
        else
            fprintf(stderr, "Error parsing 'clr' macro: %s\n", line);
    } else if (strcmp(op, "halt") == 0) {
        expandHalt(output);
    } else if (strcmp(op, "push") == 0) {
        if (sscanf(line, "push%*[ \t]r%d", &rD) == 1)
            expandPush(rD, output);
        else
            fprintf(stderr, "Error parsing 'push' macro: %s\n", line);
    } else if (strcmp(op, "pop") == 0) {
        if (sscanf(line, "pop%*[ \t]r%d", &rD) == 1)
            expandPop(rD, output);
        else
            fprintf(stderr, "Error parsing 'pop' macro: %s\n", line);
    } else if (strcmp(op, "ld") == 0) {
        /* First try reading a numeric immediate.
           If that fails, try reading a label operand (prefixed by a colon)
        */
        if (sscanf(line, "ld%*[ \t]r%d%*[ \t,]%llu", &rD, &immediate) == 2) {
            expandLd(rD, immediate, output);
        } else {
            char label[50];
            if (sscanf(line, "ld%*[ \t]r%d%*[ \t,]:%49s", &rD, label) == 2) {
                LabelAddress *entry = find_label(label);
                if (entry) {
                    expandLd(rD, entry->address, output);
                } else {
                    fprintf(stderr, "Label not found in 'ld' macro: %s\n", line);
                }
            } else {
                fprintf(stderr, "Error parsing 'ld' macro: %s\n", line);
            }
        }
    } else if (strcmp(op, "mov") == 0) {
        if (strchr(line, '(') && strchr(line, ')')) {
            char *afterOp = line + 3;
            while(*afterOp && isspace((unsigned char)*afterOp)) afterOp++;
            if (*afterOp == '(') {
                // mov_rm: syntax: mov (rX) , rY
                int rMem, rReg;
                if (sscanf(line, "mov (r%d)%*[, \t]r%d", &rMem, &rReg) != 2 &&
                    sscanf(line, "mov (r%d) , r%d", &rMem, &rReg) != 2)
                {
                    fprintf(stderr, "Error parsing 'mov' macro (mov_rm): %s\n", line);
                } else {
                    expandMov_rm(rMem, rReg, output);
                }
            } else {
                // mov_mr: syntax: mov rX , (rY)
                int rReg, rMem;
                if (sscanf(line, "mov r%d%*[, \t](r%d)", &rReg, &rMem) != 2) {
                    fprintf(stderr, "Error parsing 'mov' macro (mov_mr): %s\n", line);
                } else {
                    expandMov_mr(rReg, rMem, output);
                }
            }
        } else {
            fprintf(output, "\t%s\n", line);
        }
    } else {
        fprintf(output, "\t%s\n", line);
    }
}
/////// ignore this for now

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
        // Trim the line and skip empty or comment lines
        line[strcspn(line, "\n")] = '\0';
        trim(line);
        if (strlen(line) == 0 || line[0] == ';') {
            continue;
        }

        // Handle .code directive
        if (strcmp(line, ".code") == 0) {
            if (in_code_section) {
                // Skip this .code since we are already in one
                continue;
            } else {
                // Start a new .code section
                fprintf(fout, ".code\n");
                in_code_section = 1;
                continue;
            }
        }

        // Handle .data directive (reset in_code_section flag)
        if (strcmp(line, ".data") == 0) {
            fprintf(fout, ".data\n");
            in_code_section = 0;
            continue;
        }

        // Skip lines that are just labels (e.g., :LABEL)
        if (line[0] == ':' && strlen(line) > 1 && strspn(line + 1, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") == strlen(line + 1)) {
            continue;
        }

        // Check for label references and replace them with decimal addresses
        char *label_start = strstr(line, ":");
        if (label_start) {
            char label[50];
            sscanf(label_start + 1, "%s", label);

            // Look up the label in the hashmap
            LabelAddress *entry = find_label(label);
            if (entry) {
                // Replace the label with its decimal address
                char buffer[1024];
                *label_start = '\0';  // Split the line before the label
                snprintf(buffer, sizeof(buffer), "\t%s%d", line, entry->address);
                fprintf(fout, "%s\n", buffer);
            } else {
                printf("Warning: Label '%s' not found.\n", label);
                fprintf(fout, "\t%s\n", line);  // Preserve original if not found
            }
        } else {
            // No label reference, just write the line as is
            fprintf(fout, "\t%s\n", line);
        }
    }

    fclose(fin);
    fclose(fout);
}


    





int main(int argc, char *argv[]) {
    // test pass 1
    // Run Pass 1 to calculate label addresses.
    pass1(argv[1]);
    pass2(argv[1], argv[2]);
    
    // // For demonstration, print out the label-address pairs.
    // LabelAddress *entry, *tmp;
    // HASH_ITER(hh, hashmap, entry, tmp) {
    //     printf("Label: %s, Address: %d\n", entry->label, entry->address);
    // }
    
    free_hashmap();
    return 0;

}