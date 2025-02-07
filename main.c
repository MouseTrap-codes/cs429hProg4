#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <regex.h>
#include <ctype.h>
#include <string.h>
#include "uthash.h"

//---------------------------------------------------------------------
// Hashmap of labels to addresses using uthash
//---------------------------------------------------------------------
typedef struct {
    char label[50];
    int address;
    UT_hash_handle hh;
} LabelAddress;

LabelAddress *hashmap = NULL;

// add a new label -> address pair to the hashmap
void add_label(char *label, int address) {
    LabelAddress *entry = (LabelAddress *)malloc(sizeof(LabelAddress));
    if (entry == NULL) {
        fprintf(stderr, "Memory allocation failed\n");
        return;
    }
    strncpy(entry->label, label, sizeof(entry->label) - 1);
    entry->label[sizeof(entry->label) - 1] = '\0';
    entry->address = address;

    HASH_ADD_STR(hashmap, label, entry);
}

// lookup a label
LabelAddress *find_label(char *label) {
    LabelAddress *entry;
    HASH_FIND_STR(hashmap, label, entry);
    return entry;
}

// update an existing label (if it was found)
void update_label(char *label, int new_address) {
    LabelAddress *entry = find_label(label);
    if (entry != NULL) {
        entry->address = new_address;
        printf("Updated label '%s' with new address: %d\n", label, new_address);
    } else {
        printf("Label '%s' not found. Add it first if needed.\n", label);
    }
}

// delete a label
void delete_label(char *label) {
    LabelAddress *entry = find_label(label);
    if (entry != NULL) {
        HASH_DEL(hashmap, entry);
        free(entry);
    }
}

// free the entire hashmap
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
        fprintf(stderr, "Could not compile regex pattern: %s\n", pattern);
        return 0;
    }
    // Execute the regex
    result = regexec(&regex, instruction, 0, NULL, 0);
    regfree(&regex);  // Free the compiled regex
    return !result;   // Return 1 if it matches, 0 otherwise
}

// check if any given line is a valid instruction
// (Add or remove mnemonics here to match your final spec.)
int is_valid_instruction(const char *instruction) {
    regex_t regex;
    const char *pattern =
        "^(add|addi|sub|subi|mul|div|and|or|xor|not|shftr|shftri|shftl|shftli|"
        "br|brr|brnz|call|return|brgt|addf|subf|mulf|divf|mov|in|out|clr|ld|push|pop|halt)"
        "([ \t]+r[0-9]+(,[ \t]*r[0-9]+(,[ \t]*r[0-9]+)?)?[ \t]*(,[ \t]*(:[A-Za-z][A-Za-z0-9]*|[0-9]+))?)?$";
    
    int result = regcomp(&regex, pattern, REG_EXTENDED);
    if (result) {
        fprintf(stderr, "Could not compile master instruction regex.\n");
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
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
    // Trim trailing whitespace
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }
}

//---------------------------------------------------------------------
// Pass 1: Calculate label addresses (PC).
//---------------------------------------------------------------------
void pass1(const char *input_filename) {
    FILE *fin = fopen(input_filename, "r");
    if (!fin) {
        perror("Error opening input file");
        exit(1);
    }
    
    typedef enum { NONE, CODE, DATA } Section;
    Section currentSection = NONE;
    
    // The example specs mention that code starts at 0x1000
    int programCounter = 0x1000;
    char line[1024];
    
    while (fgets(line, sizeof(line), fin)) {
        line[strcspn(line, "\n")] = '\0';  // remove newline
        trim(line);
        if (strlen(line) == 0) {
            continue;
        }
        
        // Skip comment lines
        if (line[0] == ';') {
            continue;
        }
        
        // Check for directives (.code/.data)
        if (line[0] == '.') {
            if (strncmp(line, ".code", 5) == 0) {
                currentSection = CODE;
            } else if (strncmp(line, ".data", 5) == 0) {
                currentSection = DATA;
            }
            continue;
        }
        
        // Process label definitions (e.g. ":LABEL")
        if (line[0] == ':') {
            // example: ":L1" -> store programCounter
            char label[50];
            sscanf(line + 1, "%49s", label);
            add_label(label, programCounter);
            continue;
        }
        
        // In CODE section, each instruction is 4 bytes
        // BUT if the line is a macro that expands into multiple lines, we must account for that
        if (currentSection == CODE) {
            // Validate the line as either a known instruction or a recognized macro
            if (is_valid_instruction(line)) {
                // Check if it is one of the macros that expand to multiple instructions
                if (match_regex("ld\\s+r[0-9]+,\\s+[0-9]+", line)) {
                    // "ld" expands into 12 instructions => 12 * 4 = 48 bytes
                    programCounter += 48;
                }
                else if (match_regex("push\\s+r[0-9]+", line)) {
                    // "push" expands into 2 instructions => 8 bytes
                    programCounter += 8;
                }
                else if (match_regex("pop\\s+r[0-9]+", line)) {
                    // "pop" expands into 2 instructions => 8 bytes
                    programCounter += 8;
                }
                else {
                    // Normal instruction => 4 bytes
                    programCounter += 4;
                }
            }
            else {
                // Not recognized as a valid instruction => error out
                fprintf(stderr, "Error: Invalid instruction or macro in pass1: %s\n", line);
                exit(EXIT_FAILURE);
            }
        }
        else if (currentSection == DATA) {
            // Each data item is 8 bytes
            programCounter += 8;
        }
    }
    
    fclose(fin);
}

//---------------------------------------------------------------------
// Macro expansion helpers
//---------------------------------------------------------------------
void expandIn(int rD, int rS, FILE* output) {
    // in rD, rS -> priv rD, rS, 0, 3
    fprintf(output, "\tpriv r%d, r%d, r0, 3\n", rD, rS);
}

void expandOut(int rD, int rS, FILE* output) {
    // out rD, rS -> priv rD, rS, 0, 4
    fprintf(output, "\tpriv r%d, r%d, r0, 4\n", rD, rS);
}

void expandClr(int rD, FILE* output) {
    // clr rD -> xor rD, rD, rD
    fprintf(output, "\txor r%d, r%d, r%d\n", rD, rD, rD);
}

void expandHalt(FILE* output) {
    // halt -> priv r0, r0, r0, 0
    fprintf(output, "\tpriv r0, r0, r0, 0\n");
}

void expandPush(int rD, FILE* output) {
    // push rD -> mov rD, (r31)(0); subi r31, r31, 8
    fprintf(output, "\tmov (r31)(0), r%d\n", rD);  // store rD at [r31+0]
    fprintf(output, "\tsubi r31, 8\n");           // decrement stack pointer by 8
}

void expandPop(int rD, FILE* output) {
    // pop rD -> addi r31, 8; mov rD, (r31)(0)
    fprintf(output, "\taddi r31, 8\n");
    fprintf(output, "\tmov r%d, (r31)(0)\n", rD);
}

void expandLd(int rD, uint64_t L, FILE* output) {
    // Example approach: break the 64-bit immediate L into chunks and shift them in.
    // Adjust if your actual Tinker microcode or instructions differ.
    //
    // For demonstration, we do a 12-instruction expansion:
    //  1) xor r0, r0, r0   (just to have a zero in r0)
    //  2) addi rD, r0, (top bits)
    //  3) shftli rD, #bits
    //  4) addi rD, rD, (next bits)
    //  5) shftli rD, #bits
    //  ...
    // This depends on your exact Tinker instructions and how you'd prefer to do it.
    //
    fprintf(output, "\txor r0, r0, r0\n");

    // Top 12 bits => ((L >> 52) & 0xFFF)
    fprintf(output, "\taddi r%d, r0, %llu\n", rD, (L >> 52) & 0xFFF);
    fprintf(output, "\tshftli r%d, 12\n", rD);

    // Next 12 bits => ((L >> 40) & 0xFFF)
    fprintf(output, "\taddi r%d, r%d, %llu\n", rD, rD, (L >> 40) & 0xFFF);
    fprintf(output, "\tshftli r%d, 12\n", rD);

    // Next 12 bits => ((L >> 28) & 0xFFF)
    fprintf(output, "\taddi r%d, r%d, %llu\n", rD, rD, (L >> 28) & 0xFFF);
    fprintf(output, "\tshftli r%d, 12\n", rD);

    // Next 12 bits => ((L >> 16) & 0xFFF)
    fprintf(output, "\taddi r%d, r%d, %llu\n", rD, rD, (L >> 16) & 0xFFF);
    fprintf(output, "\tshftli r%d, 4\n", rD);

    // Next 12 bits => ((L >> 4) & 0xFFF)
    fprintf(output, "\taddi r%d, r%d, %llu\n", rD, rD, (L >> 4) & 0xFFF);
    fprintf(output, "\tshftli r%d, 4\n", rD);

    // Last 4 bits => (L & 0xF)
    fprintf(output, "\taddi r%d, r%d, %llu\n", rD, rD, (L & 0xF));
}

//---------------------------------------------------------------------
// parseMacro(): Extract parameters from a macro line and call expansions.
//---------------------------------------------------------------------
void parseMacro(const char *line, FILE *output) {
    char op[16];
    int rD, rS;
    uint64_t immediate;

    // Grab the first token (the operation: in/out/clr/halt/etc.)
    if (sscanf(line, "%15s", op) != 1) {
        fprintf(stderr, "Error: Could not read macro operation from line: %s\n", line);
        return;
    }

    if (strcmp(op, "in") == 0) {
        // in rD, rS
        if (sscanf(line, "in r%d , r%d", &rD, &rS) == 2 ||
            sscanf(line, "in r%d r%d", &rD, &rS) == 2)
        {
            expandIn(rD, rS, output);
        } else {
            fprintf(stderr, "Error parsing 'in' macro: %s\n", line);
        }
    }
    else if (strcmp(op, "out") == 0) {
        // out rD, rS
        if (sscanf(line, "out r%d , r%d", &rD, &rS) == 2 ||
            sscanf(line, "out r%d r%d", &rD, &rS) == 2)
        {
            expandOut(rD, rS, output);
        } else {
            fprintf(stderr, "Error parsing 'out' macro: %s\n", line);
        }
    }
    else if (strcmp(op, "clr") == 0) {
        // clr rD
        if (sscanf(line, "clr r%d", &rD) == 1) {
            expandClr(rD, output);
        } else {
            fprintf(stderr, "Error parsing 'clr' macro: %s\n", line);
        }
    }
    else if (strcmp(op, "halt") == 0) {
        // halt has no parameters
        expandHalt(output);
    }
    else if (strcmp(op, "push") == 0) {
        // push rD
        if (sscanf(line, "push r%d", &rD) == 1) {
            expandPush(rD, output);
        } else {
            fprintf(stderr, "Error parsing 'push' macro: %s\n", line);
        }
    }
    else if (strcmp(op, "pop") == 0) {
        // pop rD
        if (sscanf(line, "pop r%d", &rD) == 1) {
            expandPop(rD, output);
        } else {
            fprintf(stderr, "Error parsing 'pop' macro: %s\n", line);
        }
    }
    else if (strcmp(op, "ld") == 0) {
        // ld rD, <imm64>
        // can be either decimal or something else
        if (sscanf(line, "ld r%d , %llu", &rD, &immediate) == 2) {
            expandLd(rD, immediate, output);
        } else {
            fprintf(stderr, "Error parsing 'ld' macro: %s\n", line);
        }
    }
    else {
        // If it's not recognized as one of the macros above, just emit it "as is".
        fprintf(output, "\t%s\n", line);
    }
}

//---------------------------------------------------------------------
// Pass 2: Process the input again, do final expansions, label fixups, etc.
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
        // Remove newline and trim
        line[strcspn(line, "\n")] = '\0';
        trim(line);

        // Skip empty or purely comment lines
        if (strlen(line) == 0 || line[0] == ';') {
            continue;
        }
        
        // If we see .code, replicate in the output (once) and note we are in code
        if (strcmp(line, ".code") == 0) {
            if (!in_code_section) {
                fprintf(fout, ".code\n");
                in_code_section = 1;
            }
            continue;
        }
        
        // If we see .data, replicate in the output and note we left code
        if (strcmp(line, ".data") == 0) {
            fprintf(fout, ".data\n");
            in_code_section = 0;
            continue;
        }
        
        // If line is just a label definition, skip directly outputting it
        // because we want the label to be "absorbed" into final expansions,
        // or we might want to replicate it in output. In some specs, you might
        // want to preserve the label as an assembly label. 
        if (line[0] == ':' && strlen(line) > 1) {
            // e.g. ":L1"
            // We'll skip writing it out directly as text. 
            continue;
        }
        
        // If there's a label reference in the line, e.g. "ld r5, :LABEL"
        // we want to replace ":LABEL" with the actual address in hex.
        char *colon_pos = strstr(line, ":");
        if (colon_pos) {
            // We expect something like "... :LABEL"
            // Extract label text:
            char label[50];
            if (sscanf(colon_pos + 1, "%49s", label) == 1) {
                LabelAddress *entry = find_label(label);
                if (entry) {
                    // Overwrite the colon with a null terminator
                    // so we can keep the portion before the label, then re-append
                    // the resolved address in hex.
                    *colon_pos = '\0';

                    // Build output with address. Use 0x%X or 0x%08X, etc.
                    char buffer[1024];
                    snprintf(buffer, sizeof(buffer), "\t%s0x%X", line, entry->address);

                    fprintf(fout, "%s\n", buffer);
                } else {
                    // If label not found, just warn and emit line unchanged
                    fprintf(stderr, "Warning: Label '%s' not found.\n", label);
                    fprintf(fout, "\t%s\n", line);
                }
            }
            else {
                // Could not parse label after the colon
                fprintf(fout, "\t%s\n", line);
            }
        }
        else {
            // If the line is recognized as a macro that we expand, expand it
            if (match_regex("^(in|out|clr|halt|push|pop|ld)\\b", line)) {
                parseMacro(line, fout);
            } else {
                // Otherwise just emit the line as is
                fprintf(fout, "\t%s\n", line);
            }
        }
    }
    
    fclose(fin);
    fclose(fout);
}

//---------------------------------------------------------------------
// main()
//---------------------------------------------------------------------
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <inputfile> <outputfile>\n", argv[0]);
        return 1;
    }
    // First pass -> compute addresses of labels
    pass1(argv[1]);

    // Second pass -> expand macros, resolve label references
    pass2(argv[1], argv[2]);

    // Optional: debugging print of label-address pairs
    // LabelAddress *entry, *tmp;
    // HASH_ITER(hh, hashmap, entry, tmp) {
    //     printf("Label: %s, Address: 0x%X\n", entry->label, entry->address);
    // }

    free_hashmap();
    return 0;
}
