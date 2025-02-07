#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include "uthash.h"

/******************************************************************************
 * Label -> Address Map (Hash)
 ******************************************************************************/
typedef struct {
    char label[50];
    int address;        // We'll store addresses like 0x1000 as an int (e.g. 4096).
    UT_hash_handle hh;  // UTHash
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

static LabelAddress *find_label(const char *label) {
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

/******************************************************************************
 * String Helpers
 ******************************************************************************/

// Trim leading/trailing whitespace in-place
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

/******************************************************************************
 * Pass 1 Helpers
 *
 * We'll robustly detect "ld", "push", "pop" lines by ignoring leading spaces
 * and checking the first token so that extra spaces or tabs won't break logic.
 ******************************************************************************/

// Check if line "starts with" the mnemonic "ld" ignoring leading whitespace
static int starts_with_ld(const char *line) {
    while (isspace((unsigned char)*line)) {
        line++;
    }
    // must begin with "ld"
    if (strncmp(line, "ld", 2) == 0) {
        char c = line[2];
        // after "ld", we want EOL or whitespace or a comma
        if (c == '\0' || isspace((unsigned char)c) || c == ',') {
            return 1;
        }
    }
    return 0;
}

static int starts_with_push(const char *line) {
    while (isspace((unsigned char)*line)) {
        line++;
    }
    return (strncmp(line, "push", 4) == 0 && isspace((unsigned char)line[4]));
}

static int starts_with_pop(const char *line) {
    while (isspace((unsigned char)*line)) {
        line++;
    }
    return (strncmp(line, "pop", 3) == 0 && isspace((unsigned char)line[3]));
}

// We'll also do a quick "is_valid_instruction" check for pass1
// to ensure we don't sum the PC for obviously invalid lines.
static int is_valid_instruction_pass1(const char *line) {
    // Quick parse of the first token
    char op[32];
    if (sscanf(line, "%31s", op) != 1) {
        return 0;
    }
    // We won't do an exhaustive check, just see if it's in a known set:
    // For the sake of pass1, let's accept these mnemonics:
    const char *ops[] = {
        // arithmetic
        "add","addi","sub","subi","mul","div",
        // logic
        "and","or","xor","not","shftr","shftri","shftl","shftli",
        // control
        "br","brr","brnz","call","return","brgt",
        // float
        "addf","subf","mulf","divf",
        // data movement
        "mov",
        // privileged
        "halt",
        // macros
        "in","out","clr","ld","push","pop"
    };
    int n = (int)(sizeof(ops)/sizeof(ops[0]));
    for (int i=0; i<n; i++){
        if (!strcmp(op, ops[i])) {
            return 1;
        }
    }
    return 0;
}

/******************************************************************************
 * Pass 1: Gather Labels, Track PC
 ******************************************************************************/
static void pass1(const char *filename) {
    FILE *fin = fopen(filename, "r");
    if (!fin) {
        perror("pass1: fopen");
        exit(1);
    }

    enum { SECTION_NONE, SECTION_CODE, SECTION_DATA } section = SECTION_NONE;
    int programCounter = 0x1000; // Tinker code starts at 0x1000

    char line[1024];
    while (fgets(line, sizeof(line), fin)) {
        line[strcspn(line, "\n")] = '\0';
        trim(line);
        if (line[0] == '\0') {
            continue; // empty
        }
        if (line[0] == ';') {
            continue; // comment
        }
        // Check directives .code / .data
        if (line[0] == '.') {
            if (!strncmp(line, ".code", 5)) {
                section = SECTION_CODE;
            } else if (!strncmp(line, ".data", 5)) {
                section = SECTION_DATA;
            }
            continue;
        }
        // Label definition: e.g. ":LABEL"
        if (line[0] == ':') {
            char labelName[50];
            if (sscanf(line+1, "%49s", labelName) == 1) {
                add_label(labelName, programCounter);
            }
            continue;
        }

        // If we're in CODE, each line is either a normal instruction (4 bytes),
        // or a macro that expands to multiple instructions
        if (section == SECTION_CODE) {
            if (!is_valid_instruction_pass1(line)) {
                fprintf(stderr, "pass1 error: invalid line: %s\n", line);
                fclose(fin);
                exit(1);
            }
            else {
                // If it's "ld ...", +48 bytes (12 instructions)
                if (starts_with_ld(line)) {
                    programCounter += 48;
                }
                // If it's "push rX", +8 bytes
                else if (starts_with_push(line)) {
                    programCounter += 8;
                }
                // If it's "pop rX", +8 bytes
                else if (starts_with_pop(line)) {
                    programCounter += 8;
                }
                else {
                    // normal => +4
                    programCounter += 4;
                }
            }
        }
        else if (section == SECTION_DATA) {
            // each data item => 8 bytes
            programCounter += 8;
        }
        else {
            // line outside .code/.data => do nothing or skip
        }
    }

    fclose(fin);
}

/******************************************************************************
 * Pass 2: Expand Macros, Replace Label References, Output Final
 ******************************************************************************/

// ----- Macro expansions -----
static void expandIn(int rD, int rS, FILE *fout) {
    fprintf(fout, "\tpriv r%d, r%d, r0, 3\n", rD, rS);
}

static void expandOut(int rD, int rS, FILE *fout) {
    fprintf(fout, "\tpriv r%d, r%d, r0, 4\n", rD, rS);
}

static void expandClr(int rD, FILE *fout) {
    fprintf(fout, "\txor r%d, r%d, r%d\n", rD, rD, rD);
}

static void expandHalt(FILE *fout) {
    fprintf(fout, "\tpriv r0, r0, r0, 0\n");
}

// push rD => subi r31, 8; mov (r31)(0), rD
static void expandPush(int rD, FILE *fout) {
    fprintf(fout, "\tsubi r31, 8\n");
    fprintf(fout, "\tmov (r31)(0), r%d\n", rD);
}

// pop rD => mov rD, (r31)(0); addi r31, 8
static void expandPop(int rD, FILE *fout) {
    fprintf(fout, "\tmov r%d, (r31)(0)\n", rD);
    fprintf(fout, "\taddi r31, 8\n");
}

/*
   ld rD, <64-bit literal> => 12-line expansion => +48 bytes
   We'll do the usual chunking:
     1) xor r0, r0, r0
     2) addi rD, r0, top12
     3) shftli rD, 12
     4) addi rD, rD, mid12a
     5) shftli rD, 12
     6) addi rD, rD, mid12b
     7) shftli rD, 12
     8) addi rD, rD, mid12c
     9) shftli rD, 4
     10) addi rD, rD, mid4
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

    fprintf(fout, "\taddi r%d, r0, %llu\n", rD, top12);
    fprintf(fout, "\tshftli r%d, 12\n", rD);

    fprintf(fout, "\taddi r%d, r%d, %llu\n", rD, rD, mid12a);
    fprintf(fout, "\tshftli r%d, 12\n", rD);

    fprintf(fout, "\taddi r%d, r%d, %llu\n", rD, rD, mid12b);
    fprintf(fout, "\tshftli r%d, 12\n", rD);

    fprintf(fout, "\taddi r%d, r%d, %llu\n", rD, rD, mid12c);
    fprintf(fout, "\tshftli r%d, 4\n", rD);

    fprintf(fout, "\taddi r%d, r%d, %llu\n", rD, rD, mid4);
    fprintf(fout, "\tshftli r%d, 4\n", rD);

    fprintf(fout, "\taddi r%d, r%d, %llu\n", rD, rD, last4);
}

// parseMacro => read an "ld"/"push"/"pop"/"in"/"out"/"clr"/"halt" line & expand
static void parseMacro(const char *line, FILE *fout) {
    char op[16];
    int rD, rS;
    uint64_t imm;

    if (sscanf(line, "%15s", op) != 1) {
        fprintf(stderr, "parseMacro: cannot read opcode: %s\n", line);
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
        // ld rD, <64-bit immediate>
        if (sscanf(line, "ld r%d , %llu", &rD, &imm) == 2 ||
            sscanf(line, "ld r%d %llu", &rD, &imm) == 2)
        {
            expandLd(rD, imm, fout);
        } else {
            fprintf(stderr, "Error parsing 'ld': %s\n", line);
        }
    }
    else {
        // Not recognized => just pass the line with a leading tab
        fprintf(fout, "\t%s\n", line);
    }
}

// A simpler check to see if line's first token is among recognized macros
// So we know to call parseMacro or not
static int is_macro_like(const char *line) {
    char op[16];
    if (sscanf(line, "%15s", op) != 1) {
        return 0;
    }
    if (!strcmp(op, "in")   || !strcmp(op, "out")  ||
        !strcmp(op, "clr")  || !strcmp(op, "halt") ||
        !strcmp(op, "push") || !strcmp(op, "pop")  ||
        !strcmp(op, "ld"))
    {
        return 1;
    }
    return 0;
}

static void pass2(const char *infile, const char *outfile) {
    FILE *fin = fopen(infile, "r");
    if (!fin) {
        perror("pass2: fopen input");
        exit(1);
    }
    FILE *fout = fopen(outfile, "w");
    if (!fout) {
        perror("pass2: fopen output");
        fclose(fin);
        exit(1);
    }

    char line[1024];
    while (fgets(line, sizeof(line), fin)) {
        line[strcspn(line, "\n")] = '\0';
        trim(line);

        if (!line[0] || line[0] == ';') {
            continue; // skip empty or comment
        }
        // Pass through .code/.data EXACTLY -> we do not unify them
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

        // Check for a reference to :LABEL => decimal replacement
        char *colon = strchr(line, ':');
        if (colon) {
            char labelName[50];
            if (sscanf(colon + 1, "%49s", labelName) == 1) {
                LabelAddress *entry = find_label(labelName);
                if (entry) {
                    *colon = '\0'; // cut the line at the colon
                    char buffer[1024];
                    snprintf(buffer, sizeof(buffer), "\t%s%d", line, entry->address);
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

        // If line is recognized as macro => expand
        if (is_macro_like(line)) {
            parseMacro(line, fout);
        } else {
            // otherwise just pass it as a normal instruction, with tab
            fprintf(fout, "\t%s\n", line);
        }
    }

    fclose(fin);
    fclose(fout);
}

/******************************************************************************
 * main
 ******************************************************************************/
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usagse: %s <inputfile> <outputfile>\n", argv[0]);
        return 1;
    }

    // Pass 1 => gather label addresses, compute program counters
    pass1(argv[1]);

    // Pass 2 => expand macros, fix label references, produce final .tk
    pass2(argv[1], argv[2]);

    free_hashmap();
    return 0;
}
