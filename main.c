/*****************************************************************************
 * tinker_assembler.c
 * 
 * A Tinker assembler that:
 *   1) In pass1, collects label addresses and increments PC correctly 
 *      (accounting for multi-instruction macros).
 *   2) In pass2, fully expands macros and replaces label references with 
 *      their decimal addresses (e.g., "4096").
 *   3) Uses the Tinker-specified syntax for push/pop/ld expansions.
 *   4) Starts code at 0x1000 but prints decimal addresses in final output.
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include "uthash.h"

// ----------------------------------------------------------------------
// Label -> Address Hash
// ----------------------------------------------------------------------
typedef struct {
    char label[50];
    int address;       // We'll store int for addresses like 0x1000, etc.
    UT_hash_handle hh; // uthash
} LabelAddress;

static LabelAddress *hashmap = NULL;

// Add a label
static void add_label(const char *label, int address) {
    LabelAddress *entry = (LabelAddress *)malloc(sizeof(LabelAddress));
    if (!entry) {
        fprintf(stderr, "add_label: malloc failed\n");
        return;
    }
    strncpy(entry->label, label, sizeof(entry->label) - 1);
    entry->label[sizeof(entry->label) - 1] = '\0';
    entry->address = address;
    HASH_ADD_STR(hashmap, label, entry);
}

// Find a label
static LabelAddress* find_label(const char *label) {
    LabelAddress *entry;
    HASH_FIND_STR(hashmap, label, entry);
    return entry;
}

// Free the entire hash
static void free_hashmap(void) {
    LabelAddress *cur, *tmp;
    HASH_ITER(hh, hashmap, cur, tmp) {
        HASH_DEL(hashmap, cur);
        free(cur);
    }
}

// ----------------------------------------------------------------------
// Trim leading/trailing whitespace in-place
// ----------------------------------------------------------------------
static void trim(char *s) {
    // leading
    char *p = s;
    while (isspace((unsigned char)*p)) { p++; }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
    // trailing
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }
}

// ----------------------------------------------------------------------
// Decide if a line (minus label definition / comment / directive)
// is a valid Tinker instruction or recognized macro.
// We'll just do a quick "first token in known set" approach.
// ----------------------------------------------------------------------
static int is_valid_instruction(const char *line) {
    // Extract first token
    char mnemonic[32];
    if (sscanf(line, "%31s", mnemonic) != 1) {
        return 0;
    }

    // List of recognized Tinker instructions + macros
    const char *ops[] = {
        // integer arithmetic
        "add", "addi", "sub", "subi", "mul", "div",
        // logic
        "and", "or", "xor", "not", "shftr", "shftri", "shftl", "shftli",
        // control
        "br", "brr", "brnz", "call", "return", "brgt",
        // floating
        "addf", "subf", "mulf", "divf",
        // data movement
        "mov",
        // privileged
        "halt",
        // recognized macros:
        "in", "out", "clr", "ld", "push", "pop"
    };

    int count = (int)(sizeof(ops) / sizeof(ops[0]));
    for (int i = 0; i < count; i++) {
        if (strcmp(mnemonic, ops[i]) == 0) {
            return 1; // recognized
        }
    }
    return 0; // not recognized
}

// ----------------------------------------------------------------------
// Macro expansions
// ----------------------------------------------------------------------

// in rD, rS => priv rD, rS, r0, 3
static void expandIn(int rD, int rS, FILE *fout) {
    fprintf(fout, "\tpriv r%d, r%d, r0, 3\n", rD, rS);
}

// out rD, rS => priv rD, rS, r0, 4
static void expandOut(int rD, int rS, FILE *fout) {
    fprintf(fout, "\tpriv r%d, r%d, r0, 4\n", rD, rS);
}

// clr rD => xor rD, rD, rD
static void expandClr(int rD, FILE *fout) {
    fprintf(fout, "\txor r%d, r%d, r%d\n", rD, rD, rD);
}

// halt => priv r0, r0, r0, 0
static void expandHalt(FILE *fout) {
    fprintf(fout, "\tpriv r0, r0, r0, 0\n");
}

// push rD => mov (r31)(0), rD; subi r31, 8
static void expandPush(int rD, FILE *fout) {
    fprintf(fout, "\tmov (r31)(0), r%d\n", rD);
    fprintf(fout, "\tsubi r31, 8\n");
}

// pop rD => addi r31, 8; mov rD, (r31)(0)
static void expandPop(int rD, FILE *fout) {
    fprintf(fout, "\taddi r31, 8\n");
    fprintf(fout, "\tmov r%d, (r31)(0)\n", rD);
}

// ld rD, <64-bit literal> => expand into ~12 instructions
static void expandLd(int rD, uint64_t L, FILE *fout) {
    // For example, do:
    fprintf(fout, "\txor r0, r0, r0\n");

    // top 12 bits => (L >> 52) & 0xFFF
    fprintf(fout, "\taddi r%d, %llu\n", rD, (L >> 52) & 0xFFF);
    fprintf(fout, "\tshftli r%d, 12\n", rD);

    // next 12 => (L >> 40) & 0xFFF
    fprintf(fout, "\taddi r%d, %llu\n", rD, (L >> 40) & 0xFFF);
    fprintf(fout, "\tshftli r%d, 12\n", rD);

    // next 12 => (L >> 28) & 0xFFF
    fprintf(fout, "\taddi r%d, %llu\n", rD, (L >> 28) & 0xFFF);
    fprintf(fout, "\tshftli r%d, 12\n", rD);

    // next 12 => (L >> 16) & 0xFFF
    fprintf(fout, "\taddi r%d, %llu\n", rD, (L >> 16) & 0xFFF);
    fprintf(fout, "\tshftli r%d, 4\n", rD);

    // next 12 => (L >> 4) & 0xFFF
    fprintf(fout, "\taddi r%d, %llu\n", rD, (L >> 4) & 0xFFF);
    fprintf(fout, "\tshftli r%d, 4\n", rD);

    // last 4 => (L & 0xF)
    fprintf(fout, "\taddi r%d, %llu\n", rD, (L & 0xF));
}

// Parse line to see if it's a known macro: e.g. "in rX, rY" => expandIn(...)
static void parseMacro(const char *line, FILE *fout) {
    char op[16];
    int rD, rS;
    uint64_t imm;

    // Attempt to read the first token (the operation)
    if (sscanf(line, "%15s", op) != 1) {
        // can't parse
        fprintf(stderr, "parseMacro: cannot read opcode from line: %s\n", line);
        return;
    }

    if (strcmp(op, "in") == 0) {
        // in rD, rS
        if (sscanf(line, "in r%d , r%d", &rD, &rS) == 2 ||
            sscanf(line, "in r%d r%d", &rD, &rS) == 2)
        {
            expandIn(rD, rS, fout);
        } else {
            fprintf(stderr, "Error parsing in macro: %s\n", line);
        }
    }
    else if (strcmp(op, "out") == 0) {
        // out rD, rS
        if (sscanf(line, "out r%d , r%d", &rD, &rS) == 2 ||
            sscanf(line, "out r%d r%d", &rD, &rS) == 2)
        {
            expandOut(rD, rS, fout);
        } else {
            fprintf(stderr, "Error parsing out macro: %s\n", line);
        }
    }
    else if (strcmp(op, "clr") == 0) {
        // clr rD
        if (sscanf(line, "clr r%d", &rD) == 1) {
            expandClr(rD, fout);
        } else {
            fprintf(stderr, "Error parsing clr macro: %s\n", line);
        }
    }
    else if (strcmp(op, "halt") == 0) {
        // no params
        expandHalt(fout);
    }
    else if (strcmp(op, "push") == 0) {
        // push rD
        if (sscanf(line, "push r%d", &rD) == 1) {
            expandPush(rD, fout);
        } else {
            fprintf(stderr, "Error parsing push macro: %s\n", line);
        }
    }
    else if (strcmp(op, "pop") == 0) {
        // pop rD
        if (sscanf(line, "pop r%d", &rD) == 1) {
            expandPop(rD, fout);
        } else {
            fprintf(stderr, "Error parsing pop macro: %s\n", line);
        }
    }
    else if (strcmp(op, "ld") == 0) {
        // ld rD, <64-bit immediate>
        // parse as decimal (or possibly detect "0x" yourself, if desired).
        if (sscanf(line, "ld r%d , %llu", &rD, &imm) == 2 ||
            sscanf(line, "ld r%d %llu", &rD, &imm) == 2)
        {
            expandLd(rD, imm, fout);
        } else {
            fprintf(stderr, "Error parsing ld macro (literal): %s\n", line);
        }
    }
    else {
        // Not recognized as a macro => just print as is, with a leading tab
        fprintf(fout, "\t%s\n", line);
    }
}

// ----------------------------------------------------------------------
// Pass 1: read file, identify labels => addresses, keep track of PC.
// Tinker typically starts at 0x1000. 
// Macros that expand to multiple instructions => we increment PC accordingly.
// ----------------------------------------------------------------------
static void pass1(const char *filename) {
    FILE *fin = fopen(filename, "r");
    if (!fin) {
        perror("pass1: fopen input");
        exit(EXIT_FAILURE);
    }

    enum { SECTION_NONE, SECTION_CODE, SECTION_DATA } section = SECTION_NONE;
    int programCounter = 0x1000;

    char line[1024];
    while (fgets(line, sizeof(line), fin)) {
        line[strcspn(line, "\n")] = '\0';
        trim(line);
        if (!line[0]) {
            continue; // empty
        }
        // skip comments
        if (line[0] == ';') {
            continue;
        }
        // check directives
        if (line[0] == '.') {
            if (strncmp(line, ".code", 5) == 0) {
                section = SECTION_CODE;
            } else if (strncmp(line, ".data", 5) == 0) {
                section = SECTION_DATA;
            }
            continue;
        }
        // if it's a label definition
        if (line[0] == ':') {
            // e.g. ":LOOP"
            char label[50];
            if (sscanf(line+1, "%49s", label) == 1) {
                add_label(label, programCounter);
            }
            continue;
        }

        // otherwise, must be code or data
        if (section == SECTION_CODE) {
            // check validity
            if (!is_valid_instruction(line)) {
                fprintf(stderr, "pass1 error: invalid instruction: %s\n", line);
                fclose(fin);
                exit(EXIT_FAILURE);
            } else {
                // macros expansions => multiple instructions => add more than 4
                if (strncmp(line, "ld ", 3) == 0) {
                    programCounter += 48; // 12 instructions * 4 bytes
                }
                else if (strncmp(line, "push", 4) == 0) {
                    programCounter += 8;  // 2 instructions => 8 bytes
                }
                else if (strncmp(line, "pop", 3) == 0) {
                    programCounter += 8;  // 2 instructions
                }
                else {
                    // normal => +4
                    programCounter += 4;
                }
            }
        }
        else if (section == SECTION_DATA) {
            // each data line => 8 bytes
            programCounter += 8;
        }
        else {
            // line is outside .code or .data => often a no-op or error 
        }
    }

    fclose(fin);
}

// ----------------------------------------------------------------------
// Pass 2: produce final file
//   - replicate .code/.data lines
//   - skip label definitions
//   - macros expanded
//   - references to :LABEL => decimal address
// ----------------------------------------------------------------------
static void pass2(const char *inputfile, const char *outputfile) {
    FILE *fin = fopen(inputfile, "r");
    if (!fin) {
        perror("pass2: fopen input");
        exit(EXIT_FAILURE);
    }
    FILE *fout = fopen(outputfile, "w");
    if (!fout) {
        perror("pass2: fopen output");
        fclose(fin);
        exit(EXIT_FAILURE);
    }

    int in_code_section = 0;
    char line[1024];

    while (fgets(line, sizeof(line), fin)) {
        line[strcspn(line, "\n")] = '\0';
        trim(line);

        if (!line[0] || line[0] == ';') {
            // skip empty or comment
            continue;
        }
        // pass through .code/.data
        if (strcmp(line, ".code") == 0) {
            // if we haven't printed .code yet, do so
            if (!in_code_section) {
                fprintf(fout, ".code\n");
                in_code_section = 1;
            }
            continue;
        }
        if (strcmp(line, ".data") == 0) {
            fprintf(fout, ".data\n");
            in_code_section = 0;
            continue;
        }
        // skip label definitions entirely
        if (line[0] == ':') {
            continue;
        }

        // if there's a reference to :LABEL, replace it with decimal
        char *colon = strchr(line, ':');
        if (colon) {
            char labelName[50];
            if (sscanf(colon + 1, "%49s", labelName) == 1) {
                LabelAddress *entry = find_label(labelName);
                if (entry) {
                    // cut off the line at the colon
                    *colon = '\0';
                    // then append the label's decimal address
                    // e.g. "... 4096"
                    char buffer[1024];
                    snprintf(buffer, sizeof(buffer),
                             "\t%s%d", line, entry->address);
                    fprintf(fout, "%s\n", buffer);
                    continue;
                } else {
                    // label not found
                    fprintf(stderr, "Warning: label '%s' not found.\n", labelName);
                    fprintf(fout, "\t%s\n", line);
                    continue;
                }
            }
        }

        // If line is recognized macro => expand
        if (is_valid_instruction(line)) {
            // check first token 
            char firstWord[16];
            if (sscanf(line, "%15s", firstWord) == 1) {
                if (!strcmp(firstWord, "in")   ||
                    !strcmp(firstWord, "out")  ||
                    !strcmp(firstWord, "clr")  ||
                    !strcmp(firstWord, "ld")   ||
                    !strcmp(firstWord, "push") ||
                    !strcmp(firstWord, "pop")  ||
                    !strcmp(firstWord, "halt"))
                {
                    parseMacro(line, fout);
                } else {
                    // normal instruction
                    fprintf(fout, "\t%s\n", line);
                }
            } else {
                // fallback
                fprintf(fout, "\t%s\n", line);
            }
        } else {
            // not recognized => print as is
            fprintf(fout, "\t%s\n", line);
        }
    }

    fclose(fin);
    fclose(fout);
}

// ----------------------------------------------------------------------
// main
// ----------------------------------------------------------------------
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <inputfile> <outputfile>\n", argv[0]);
        return 1;
    }

    // pass1 => gather labels / addresses
    pass1(argv[1]);

    // pass2 => expand macros, fix label references, produce final .tk
    pass2(argv[1], argv[2]);

    // optional: see what labels we found
    /*
    LabelAddress *e, *tmp;
    HASH_ITER(hh, hashmap, e, tmp) {
        printf("Label: %s => %d\n", e->label, e->address);
    }
    */

    free_hashmap();
    return 0;
}
