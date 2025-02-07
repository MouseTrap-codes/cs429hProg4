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

LabelAddress *find_label(char *label) {
    LabelAddress *entry;
    HASH_FIND_STR(hashmap, label, entry);
    return entry;
}

void update_label(char *label, int new_address) {
    LabelAddress *entry = find_label(label);
    if (entry != NULL) {
        entry->address = new_address;
        printf("Updated label '%s' with new address: %d\n", label, new_address);
    } else {
        printf("Label '%s' not found. Add it first if needed.\n", label);
    }
}

void delete_label(char *label) {
    LabelAddress *entry = find_label(label);
    if (entry != NULL) {
        HASH_DEL(hashmap, entry);
        free(entry);
    }
}

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

    result = regcomp(&regex, pattern, REG_EXTENDED);
    if (result) {
        printf("Could not compile regex\n");
        return 0;
    }
    result = regexec(&regex, instruction, 0, NULL, 0);
    regfree(&regex);
    return !result;  // returns 1 if matches, 0 otherwise
}

int is_valid_instruction(const char *instruction) {
    regex_t regex;
    const char *pattern =
        "^(add|addi|sub|subi|mul|div|and|or|xor|not|shftr|shftri|shftl|shftli|"
        "br|brr|brr_l|brr_r|brnz|call|return|brgt|addf|subf|mulf|divf|mov|in|out|clr|ld|push|pop|halt|priv)"
        "([ \t]+r[0-9]+(,[ \t]*r[0-9]+(,[ \t]*r[0-9]+)?)?[ \t]*(,[ \t]*(:[A-Za-z][A-Za-z0-9]*|[0-9]+))?)?$";
    
    int result = regcomp(&regex, pattern, REG_EXTENDED);
    if (result) {
        printf("Could not compile regex\n");
        return 0;
    }
    result = regexec(&regex, instruction, 0, NULL, 0);
    regfree(&regex);
    return !result;
}

//---------------------------------------------------------------------
// Utility: trim leading and trailing whitespace (in–place)
//---------------------------------------------------------------------
void trim(char *s) {
    char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len-1])) {
        s[len-1] = '\0';
        len--;
    }
}

//---------------------------------------------------------------------
// Range-check helper functions
//---------------------------------------------------------------------
int check_unsigned12(uint32_t val) {
    return val <= 4095;
}

int check_signed12(int val) {
    return (val >= -2048 && val <= 2047);
}

//---------------------------------------------------------------------
// Pass 1: Compute label addresses.
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
        if (line[0] == ';')
            continue;
        
        // Directives: .code or .data
        if (line[0] == '.') {
            if (strncmp(line, ".code", 5) == 0)
                currentSection = CODE;
            else if (strncmp(line, ".data", 5) == 0)
                currentSection = DATA;
            continue;
        }
        
        // Process label definitions (lines starting with ':')
        if (line[0] == ':') {
            char label[50];
            sscanf(line + 1, "%s", label);
            add_label(label, programCounter);
            continue;
        }
        
        // Update program counter.
        if (currentSection == CODE) {
            if (is_valid_instruction(line)) {
                if (match_regex("ld\\s+r[0-9]+,\\s+[0-9]+", line))
                    programCounter += 48; // macro expands to 12 lines (48 bytes)
                else if (match_regex("push\\s+r[0-9]+", line))
                    programCounter += 8;  // 2-line macro (8 bytes)
                else if (match_regex("pop\\s+r[0-9]+", line))
                    programCounter += 8;  // 2-line macro (8 bytes)
                else
                    programCounter += 4;  // normal instruction: 4 bytes
            } else {
                exit(-1);
            }
        } else if (currentSection == DATA) {
            programCounter += 8;  // each data item is 8 bytes
        }
    }
    fclose(fin);
}

//---------------------------------------------------------------------
// Macro expansion functions
//---------------------------------------------------------------------
void expandIn(int rD, int rS, FILE* output) {
    // The following expansion uses a fixed L value of 3.
    // (L is later checked in pass2 if a "priv" instruction is encountered.)
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
    fprintf(output, "\tmov r%d, r31(0)\n", rD);
    fprintf(output, "\tsubi r31, r31, 8\n");
}

void expandPop(int rD, FILE* output) {
    fprintf(output, "\tmov r%d, r31(0)\n", rD);
    fprintf(output, "\taddi r31, r31, 8\n");
}

void expandLd(int rD, uint64_t L, FILE* output) {
    // Example expansion for ld.
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
    // mov from memory to register (load)
    fprintf(output, "\tld r%d, (r%d)\n", rD, rS);
}

void expandMov_rm(int rD, int rS, FILE* output) {
    // mov from register to memory (store)
    fprintf(output, "\tst r%d, (r%d)\n", rS, rD);
}

//---------------------------------------------------------------------
// parseMacro(): Extract parameters from a macro line and call expansion
//---------------------------------------------------------------------
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
        /* Try reading an unsigned 64-bit immediate.
           Otherwise, try reading a label (prefixed with a colon). */
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
        // If the instruction contains parentheses, treat it as a memory move.
        if (strchr(line, '(') && strchr(line, ')')) {
            char *afterOp = line + 3;
            while(*afterOp && isspace((unsigned char)*afterOp))
                afterOp++;
            if (*afterOp == '(') {
                // mov_rm: syntax: mov (rX) , rY
                int rMem, rReg;
                if (sscanf(line, "mov (r%d)%*[ \t,]r%d", &rMem, &rReg) == 2)
                    expandMov_rm(rMem, rReg, output);
                else
                    fprintf(stderr, "Error parsing 'mov' macro (mov_rm): %s\n", line);
            } else {
                // mov_mr: syntax: mov rX , (rY)
                int rReg, rMem;
                if (sscanf(line, "mov r%d%*[ \t,](r%d)", &rReg, &rMem) == 2)
                    expandMov_mr(rReg, rMem, output);
                else
                    fprintf(stderr, "Error parsing 'mov' macro (mov_mr): %s\n", line);
            }
        } else {
            // If no parentheses, assume a register-to-immediate move.
            int reg, imm;
            if (sscanf(line, "mov r%d%*[ \t,]%d", &reg, &imm) == 2) {
                if (!check_signed12(imm)) {
                    fprintf(stderr, "Error: Immediate for mov must be signed 12-bit, got %d\n", imm);
                    exit(1);
                }
                fprintf(output, "\t%s\n", line);
            } else {
                // Fall back: output line unchanged.
                fprintf(output, "\t%s\n", line);
            }
        }
    } else {
        fprintf(output, "\t%s\n", line);
    }
}

//---------------------------------------------------------------------
// Pass 2: Process the input file, expand macros, resolve labels,
// and perform immediate-range checks.
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
    int currentAddress = 0;  // Will be set to 4096 when .code is seen.
    
    while (fgets(line, sizeof(line), fin)) {
        line[strcspn(line, "\n")] = '\0';
        trim(line);
        if (strlen(line) == 0 || line[0] == ';')
            continue;
        
        // If the line is a "priv" instruction, check that its fourth operand (L) is 0-4.
        if (strncmp(line, "priv", 4) == 0) {
            int r0, r1, r2, L;
            if (sscanf(line, "priv r%d, r%d, r%d, %d", &r0, &r1, &r2, &L) == 4) {
                if (L < 0 || L > 4) {
                    fprintf(stderr, "Error: L in priv must be 0, 1, 2, 3, or 4. Got %d\n", L);
                    exit(1);
                }
            } else {
                fprintf(stderr, "Error parsing priv instruction: %s\n", line);
                exit(1);
            }
        }
        
        // Check unsigned 12-bit immediates for addi, subi, shftri, shftli.
        if (strncmp(line, "addi", 4) == 0 || strncmp(line, "subi", 4) == 0 ||
            strncmp(line, "shftri", 6) == 0 || strncmp(line, "shftli", 6) == 0) {
            unsigned int imm;
            if (sscanf(line, "%*s r%*d%*[ ,\t]r%*d%*[ ,\t]%u", &imm) == 1) {
                if (!check_unsigned12(imm)) {
                    fprintf(stderr, "Error: Immediate for instruction '%s' must be unsigned 12-bit, got %u\n", line, imm);
                    exit(1);
                }
            }
        }
        
        // Handle section directives.
        if (strcmp(line, ".code") == 0) {
            in_code_section = 1;
            currentAddress = 4096;
            fprintf(fout, ".code\n");
            continue;
        }
        if (strcmp(line, ".data") == 0) {
            in_code_section = 0;
            fprintf(fout, ".data\n");
            continue;
        }
        
        // Skip label definitions.
        if (line[0] == ':' &&
            strlen(line) > 1 &&
            strspn(line + 1, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") == strlen(line + 1))
        {
            continue;
        }
        
        if (in_code_section) {
            int lineAddress = currentAddress;  // starting address for this instruction
            
            // If the instruction is a macro that we expand, do so.
            if (match_regex("^(in|out|clr|halt|push|pop|ld|mov)\\b", line)) {
                parseMacro(line, fout);
            }
            // Otherwise, if the line contains a label reference (e.g. "opcode ... :LABEL")
            else if (strchr(line, ':') != NULL) {
                char mnemonic[16];
                sscanf(line, "%15s", mnemonic);
                char *colon = strchr(line, ':');
                char label[50];
                sscanf(colon + 1, "%s", label);
                LabelAddress *entry = find_label(label);
                if (entry) {
                    if (strcmp(mnemonic, "brr") == 0 ||
                        strcmp(mnemonic, "brr_l") == 0 ||
                        strcmp(mnemonic, "brr_r") == 0)
                    {
                        int offset = entry->address - lineAddress;
                        if (strcmp(mnemonic, "brr_l") == 0 && offset > 0)
                            offset = -offset;
                        else if (strcmp(mnemonic, "brr_r") == 0 && offset < 0)
                            offset = -offset;
                        if (!check_signed12(offset)) {
                            fprintf(stderr, "Error: Branch offset for %s not in signed 12-bit range: %d\n", mnemonic, offset);
                            exit(1);
                        }
                        *colon = '\0';
                        fprintf(fout, "\t%s %d\n", line, offset);
                    } else {
                        *colon = '\0';
                        fprintf(fout, "\t%s %d\n", line, entry->address);
                    }
                } else {
                    fprintf(fout, "\t%s\n", line);
                }
            }
            // If this is a plain instruction (possibly a mov with immediate)
            else {
                // Special handling for a plain mov with an immediate (no parentheses).
                if (strncmp(line, "mov", 3) == 0 && (strchr(line, '(') == NULL)) {
                    int reg, imm;
                    if (sscanf(line, "mov r%d%*[ ,\t]%d", &reg, &imm) == 2) {
                        if (!check_signed12(imm)) {
                            fprintf(stderr, "Error: Immediate for mov must be signed 12-bit, got %d\n", imm);
                            exit(1);
                        }
                    }
                }
                // Output the instruction unchanged.
                fprintf(fout, "\t%s\n", line);
            }
            
            // Update currentAddress following the same rules as in pass1.
            if (is_valid_instruction(line)) {
                if (match_regex("ld\\s+r[0-9]+,\\s+[0-9]+", line))
                    currentAddress += 48;
                else if (match_regex("push\\s+r[0-9]+", line))
                    currentAddress += 8;
                else if (match_regex("pop\\s+r[0-9]+", line))
                    currentAddress += 8;
                else
                    currentAddress += 4;
            }
        } else {
            // In data section, output the line as is.
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
    free_hashmap();
    return 0;
}
