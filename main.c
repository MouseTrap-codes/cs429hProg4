/*****************************************************************************
 * tinker_assembler.c
 *
 * Features:
 *   1) Code starts at 0x1000.
 *   2) "ld rD, L" => 12-instruction macro (48 bytes).
 *   3) "push rD" => subi r31, 8; mov (r31)(0), rD
 *   4) "pop rD" => mov rD, (r31)(0); addi r31, 8
 *   5) Label references replaced with decimal addresses (e.g., "4096").
 *   6) If multiple .code sections appear, all are printed as separate .code lines
 *      (i.e., we do NOT merge them into a single .code block).
 *   7) Same for multiple .data sections.
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
    int address;       // We'll store int for addresses, e.g. 4096
    UT_hash_handle hh; // uthash
} LabelAddress;

static LabelAddress *hashmap = NULL;

static void add_label(const char *label, int address) {
    LabelAddress *entry = (LabelAddress *)malloc(sizeof(LabelAddress));
    if (!entry) {
        fprintf(stderr, "Error: malloc failed in add_label.\n");
        return;
    }
    strncpy(entry->label, label, sizeof(entry->label) - 1);
    entry->label[sizeof(entry->label) - 1] = '\0';
    entry->address = address;
    HASH_ADD_STR(hashmap, label, entry);
}

static LabelAddress* find_label(const char *label) {
    LabelAddress *entry;
    HASH_FIND_STR(hashmap, label, entry);
    return entry;
}

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
    char *p = s;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }
}

// ----------------------------------------------------------------------
// is_valid_instruction: check if the line's first token is a known 
// Tinker instruction or recognized macro.
// ----------------------------------------------------------------------
static int is_valid_instruction(const char *line) {
    char mnemonic[32];
    if (sscanf(line, "%31s", mnemonic) != 1) {
        return 0;
    }
    // List of recognized ops/macros
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
        // macros
        "in", "out", "clr", "ld", "push", "pop"
    };
    int count = (int)(sizeof(ops) / sizeof(ops[0]));
    for (int i = 0; i < count; i++) {
        if (strcmp(mnemonic, ops[i]) == 0) {
            return 1;
        }
    }
    return 0;
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

// push rD => subi r31, 8; mov (r31)(0), rD
static void expandPush(int rD, FILE *fout) {
    fprintf(fout, "\tsubi r31, 8\n");
    fprintf(fout, "\tmov (r31)(0), r%d\n", rD);
}

// pop rD => mov rD,(r31)(0); addi r31, 8
static void expandPop(int rD, FILE *fout) {
    // as requested: mov first, then add
    fprintf(fout, "\tmov r%d, (r31)(0)\n", rD);
    fprintf(fout, "\taddi r31, 8\n");
}

/*
   ld rD, <64-bit literal> => 12-line expansion => 48 bytes

   1) xor r0, r0, r0
   2) addi rD, r0, top12
   3) shftli rD, 12
   4) addi rD, rD, next12
   5) shftli rD, 12
   6) addi rD, rD, next12
   7) shftli rD, 12
   8) addi rD, rD, next12
   9) shftli rD, 4
   10) addi rD, rD, next12
   11) shftli rD, 4
   12) addi rD, rD, last4
*/
static void expandLd(int rD, uint64_t L, FILE *fout) {
    fprintf(fout, "\txor r0, r0, r0\n");

    unsigned long long top12  = (L >> 52) & 0xFFF;
    unsigned long long mid12a = (L >> 40) & 0xFFF;
    unsigned long long mid12b = (L >> 28) & 0xFFF;
    unsigned long long mid12c = (L >> 16) & 0xFFF;
    unsigned long long mid4   = (L >>  4) & 0xFFF;
    unsigned long long last4  =  L       & 0xF;

    // Step 2
    fprintf(fout, "\taddi r%d, r0, %llu\n", rD, top12);
    // Step 3
    fprintf(fout, "\tshftli r%d, 12\n", rD);

    // Step 4
    fprintf(fout, "\taddi r%d, r%d, %llu\n", rD, rD, mid12a);
    // Step 5
    fprintf(fout, "\tshftli r%d, 12\n", rD);

    // Step 6
    fprintf(fout, "\taddi r%d, r%d, %llu\n", rD, rD, mid12b);
    // Step 7
    fprintf(fout, "\tshftli r%d, 12\n", rD);

    // Step 8
    fprintf(fout, "\taddi r%d, r%d, %llu\n", rD, rD, mid12c);
    // Step 9
    fprintf(fout, "\tshftli r%d, 4\n", rD);

    // Step 10
    fprintf(fout, "\taddi r%d, r%d, %llu\n", rD, rD, mid4);
    // Step 11
    fprintf(fout, "\tshftli r%d, 4\n", rD);

    // Step 12
    fprintf(fout, "\taddi r%d, r%d, %llu\n", rD, rD, last4);
}

// ----------------------------------------------------------------------
// parseMacro: calls the above expansions if it matches a known macro
// ----------------------------------------------------------------------
static void parseMacro(const char *line, FILE *fout) {
    char op[16];
    int rD, rS;
    uint64_t imm;

    if (sscanf(line, "%15s", op) != 1) {
        fprintf(stderr, "Error: parseMacro could not read opcode: %s\n", line);
        return;
    }

    if (!strcmp(op, "in")) {
        if (sscanf(line, "in r%d , r%d", &rD, &rS) == 2 ||
            sscanf(line, "in r%d r%d", &rD, &rS) == 2)
        {
            expandIn(rD, rS, fout);
        } else {
            fprintf(stderr, "Error parsing 'in': %s\n", line);
        }
    }
    else if (!strcmp(op, "out")) {
        if (sscanf(line, "out r%d , r%d", &rD, &rS) == 2 ||
            sscanf(line, "out r%d r%d", &rD, &rS) == 2)
        {
            expandOut(rD, rS, fout);
        } else {
            fprintf(stderr, "Error parsing 'out': %s\n", line);
        }
    }
    else if (!strcmp(op, "clr")) {
        if (sscanf(line, "clr r%d", &rD) == 1) {
            expandClr(rD, fout);
        } else {
            fprintf(stderr, "Error parsing 'clr': %s\n", line);
        }
    }
    else if (!strcmp(op, "halt")) {
        expandHalt(fout);
    }
    else if (!strcmp(op, "push")) {
        if (sscanf(line, "push r%d", &rD) == 1) {
            expandPush(rD, fout);
        } else {
            fprintf(stderr, "Error parsing 'push': %s\n", line);
        }
    }
    else if (!strcmp(op, "pop")) {
        if (sscanf(line, "pop r%d", &rD) == 1) {
            expandPop(rD, fout);
        } else {
            fprintf(stderr, "Error parsing 'pop': %s\n", line);
        }
    }
    else if (!strcmp(op, "ld")) {
        if (sscanf(line, "ld r%d , %llu", &rD, &imm) == 2 ||
            sscanf(line, "ld r%d %llu", &rD, &imm) == 2)
        {
            expandLd(rD, imm, fout);
        } else {
            fprintf(stderr, "Error parsing 'ld': %s\n", line);
        }
    }
    else {
        // Not recognized => just output
        fprintf(fout, "\t%s\n", line);
    }
}

// ----------------------------------------------------------------------
// pass1: collect label addresses, track PC
//   - "ld" => +48 bytes
//   - "push"/"pop" => +8 bytes
//   - normal => +4
//   - data => +8
// ----------------------------------------------------------------------
static void pass1(const char *filename) {
    FILE *fin = fopen(filename, "r");
    if (!fin) {
        perror("pass1: fopen");
        exit(1);
    }

    enum { NONE, CODE, DATA } section = NONE;
    int programCounter = 0x1000;

    char line[1024];
    while (fgets(line, sizeof(line), fin)) {
        line[strcspn(line, "\n")] = '\0';
        trim(line);
        if (!line[0]) {
            continue;
        }
        if (line[0] == ';') {
            continue; // comment
        }
        if (line[0] == '.') {
            if (!strncmp(line, ".code", 5)) {
                section = CODE;
            } else if (!strncmp(line, ".data", 5)) {
                section = DATA;
            }
            continue;
        }
        if (line[0] == ':') {
            // label
            char label[50];
            if (sscanf(line + 1, "%49s", label) == 1) {
                add_label(label, programCounter);
            }
            continue;
        }

        // otherwise, it's either code or data line
        if (section == CODE) {
            if (!is_valid_instruction(line)) {
                fprintf(stderr, "Error in pass1: invalid instruction: %s\n", line);
                fclose(fin);
                exit(1);
            } else {
                // macros that expand to multiple lines
                if (!strncmp(line, "ld ", 3)) {
                    // 12 instructions => 48 bytes
                    programCounter += 48;
                }
                else if (!strncmp(line, "push", 4)) {
                    programCounter += 8; // 2 instructions
                }
                else if (!strncmp(line, "pop", 3)) {
                    programCounter += 8;
                }
                else {
                    // single instruction => 4 bytes
                    programCounter += 4;
                }
            }
        }
        else if (section == DATA) {
            // each data item => 8 bytes
            programCounter += 8;
        }
        else {
            // line outside of code/data => do nothing
        }
    }

    fclose(fin);
}

// ----------------------------------------------------------------------
// pass2: produce final .tk
//   - skip label definitions
//   - replicate .code/.data lines as they appear (no merging).
//   - expand macros
//   - references to :LABEL => replaced with decimal address
// ----------------------------------------------------------------------
static void pass2(const char *infile, const char *outfile) {
    FILE *fin = fopen(infile, "r");
    if (!fin) {
        perror("pass2: fopen in");
        exit(1);
    }
    FILE *fout = fopen(outfile, "w");
    if (!fout) {
        perror("pass2: fopen out");
        fclose(fin);
        exit(1);
    }

    char line[1024];
    while (fgets(line, sizeof(line), fin)) {
        line[strcspn(line, "\n")] = '\0';
        trim(line);
        if (!line[0] || line[0] == ';') {
            continue;
        }

        // replicate .code/.data exactly, do not merge
        if (!strcmp(line, ".code")) {
            fprintf(fout, ".code\n");
            continue;
        }
        if (!strcmp(line, ".data")) {
            fprintf(fout, ".data\n");
            continue;
        }

        // skip label lines
        if (line[0] == ':') {
            continue;
        }

        // check for :LABEL reference => decimal replacement
        char *colon = strchr(line, ':');
        if (colon) {
            char labelName[50];
            if (sscanf(colon + 1, "%49s", labelName) == 1) {
                LabelAddress *ent = find_label(labelName);
                if (ent) {
                    *colon = '\0'; // cut the line at the colon
                    char buffer[1024];
                    snprintf(buffer, sizeof(buffer), "\t%s%d", line, ent->address);
                    fprintf(fout, "%s\n", buffer);
                    continue;
                } else {
                    fprintf(stderr, "Warning: label '%s' not found.\n", labelName);
                    fprintf(fout, "\t%s\n", line);
                    continue;
                }
            }
        }

        // If recognized macro => expand
        if (is_valid_instruction(line)) {
            char first[16];
            if (sscanf(line, "%15s", first) == 1) {
                if (!strcmp(first, "in")   ||
                    !strcmp(first, "out")  ||
                    !strcmp(first, "clr")  ||
                    !strcmp(first, "ld")   ||
                    !strcmp(first, "push") ||
                    !strcmp(first, "pop")  ||
                    !strcmp(first, "halt"))
                {
                    parseMacro(line, fout);
                } else {
                    // normal instruction => just print
                    fprintf(fout, "\t%s\n", line);
                }
            } else {
                // fallback
                fprintf(fout, "\t%s\n", line);
            }
        } else {
            // not recognized => just pass through
            fprintf(fout, "\t%s\n", line);
        }
    }

    fclose(fin);
    fclose(fout);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <inputfile> <outputfile>\n", argv[0]);
        return 1;
    }

    // Pass 1 => gather label addresses
    pass1(argv[1]);

    // Pass 2 => expand macros, replace label references
    pass2(argv[1], argv[2]);

    free_hashmap();
    return 0;
}
