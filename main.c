#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include "uthash.h"

/******************************************************************************
 * Label -> Address Map
 ******************************************************************************/
typedef struct {
    char label[50];
    int address;        // We'll store addresses like 0x1000 as int => e.g. 4096
    UT_hash_handle hh;  // UTHash
} LabelAddress;

static LabelAddress *hashmap = NULL;

// Add a label to the hash
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

// Find a label by name
static LabelAddress *find_label(const char *label) {
    LabelAddress *entry;
    HASH_FIND_STR(hashmap, label, entry);
    return entry;
}

// Free entire label->address map
static void free_hashmap(void) {
    LabelAddress *cur, *tmp;
    HASH_ITER(hh, hashmap, cur, tmp) {
        HASH_DEL(hashmap, cur);
        free(cur);
    }
}

/******************************************************************************
 * Trim leading/trailing whitespace in-place
 ******************************************************************************/
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
 * Pass 1 Helpers: robust detection of "ld", "push", "pop" ignoring extra spaces
 ******************************************************************************/

static int starts_with_ld(const char *line) {
    // skip leading spaces
    while (isspace((unsigned char)*line)) {
        line++;
    }
    if (strncmp(line, "ld", 2) == 0) {
        // next char must be end-of-string, whitespace, or comma
        char c = line[2];
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
    if (!strncmp(line, "push", 4)) {
        char c = line[4];
        if (c == '\0' || isspace((unsigned char)c)) {
            return 1;
        }
    }
    return 0;
}

static int starts_with_pop(const char *line) {
    while (isspace((unsigned char)*line)) {
        line++;
    }
    if (!strncmp(line, "pop", 3)) {
        char c = line[3];
        if (c == '\0' || isspace((unsigned char)c)) {
            return 1;
        }
    }
    return 0;
}

// minimal validity check of first token for pass1
static int is_valid_instruction_pass1(const char *line) {
    char op[32];
    if (sscanf(line, "%31s", op) != 1) {
        return 0;
    }
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
 * PASS 1: gather labels, track PC
 ******************************************************************************/
static void pass1(const char *filename) {
    FILE *fin = fopen(filename, "r");
    if (!fin) {
        perror("pass1: fopen");
        exit(1);
    }

    enum { NONE, CODE, DATA } section = NONE;
    int programCounter = 0x1000; // Tinker code starts at 0x1000

    char line[1024];
    while (fgets(line, sizeof(line), fin)) {
        line[strcspn(line, "\n")] = '\0';
        trim(line);
        if (!line[0] || line[0] == ';') {
            continue; // empty or comment
        }
        // check directives
        if (line[0] == '.') {
            if (!strncmp(line, ".code", 5)) {
                section = CODE;
            } else if (!strncmp(line, ".data", 5)) {
                section = DATA;
            }
            continue;
        }
        // label definition
        if (line[0] == ':') {
            char labelName[50];
            if (sscanf(line+1, "%49s", labelName) == 1) {
                add_label(labelName, programCounter);
            }
            continue;
        }

        // if code section
        if (section == CODE) {
            if (!is_valid_instruction_pass1(line)) {
                fprintf(stderr, "pass1 error: invalid line => %s\n", line);
                fclose(fin);
                exit(1);
            }
            if (starts_with_ld(line)) {
                // 12 instructions => 48 bytes
                programCounter += 48;
            }
            else if (starts_with_push(line)) {
                // push => 2 instructions => 8 bytes
                programCounter += 8;
            }
            else if (starts_with_pop(line)) {
                // pop => 2 instructions => 8 bytes
                programCounter += 8;
            }
            else {
                // normal => +4
                programCounter += 4;
            }
        }
        else if (section == DATA) {
            // each data item => 8 bytes
            programCounter += 8;
        }
        else {
            // line outside code/data => do nothing
        }
    }

    fclose(fin);
}

/******************************************************************************
 * PASS 2: 
 *   - merge multiple .code => single .code
 *   - keep each .data as is
 *   - re-emit label lines in final => "LABEL:"
 *   - expand macros
 *   - references to :LABEL => decimal address
 ******************************************************************************/

// Macros expansions
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
// push => subi r31, 8; mov (r31)(0), rD
static void expandPush(int rD, FILE *fout) {
    fprintf(fout, "\tsubi r31, 8\n");
    fprintf(fout, "\tmov (r31)(0), r%d\n", rD);
}
// pop => mov rD, (r31)(0); addi r31, 8
static void expandPop(int rD, FILE *fout) {
    fprintf(fout, "\tmov r%d, (r31)(0)\n", rD);
    fprintf(fout, "\taddi r31, 8\n");
}

/*
   ld => 12 instructions => 48 bytes
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
        fprintf(stderr, "parseMacro: cannot parse op from line: %s\n", line);
        return;
    }

    if (!strcmp(op, "in")) {
        if (sscanf(line, "in r%d , r%d", &rD, &rS) == 2 ||
            sscanf(line, "in r%d r%d", &rD, &rS) == 2)
        {
            expandIn(rD, rS, fout);
        } else {
            fprintf(stderr, "Error parsing in macro: %s\n", line);
        }
    }
    else if (!strcmp(op, "out")) {
        if (sscanf(line, "out r%d , r%d", &rD, &rS) == 2 ||
            sscanf(line, "out r%d r%d", &rD, &rS) == 2)
        {
            expandOut(rD, rS, fout);
        } else {
            fprintf(stderr, "Error parsing out macro: %s\n", line);
        }
    }
    else if (!strcmp(op, "clr")) {
        if (sscanf(line, "clr r%d", &rD) == 1) {
            expandClr(rD, fout);
        } else {
            fprintf(stderr, "Error parsing clr macro: %s\n", line);
        }
    }
    else if (!strcmp(op, "halt")) {
        expandHalt(fout);
    }
    else if (!strcmp(op, "push")) {
        if (sscanf(line, "push r%d", &rD) == 1) {
            expandPush(rD, fout);
        } else {
            fprintf(stderr, "Error parsing push macro: %s\n", line);
        }
    }
    else if (!strcmp(op, "pop")) {
        if (sscanf(line, "pop r%d", &rD) == 1) {
            expandPop(rD, fout);
        } else {
            fprintf(stderr, "Error parsing pop macro: %s\n", line);
        }
    }
    else if (!strcmp(op, "ld")) {
        if (sscanf(line, "ld r%d , %llu", &rD, &imm) == 2 ||
            sscanf(line, "ld r%d %llu", &rD, &imm) == 2)
        {
            expandLd(rD, imm, fout);
        } else {
            fprintf(stderr, "Error parsing ld macro: %s\n", line);
        }
    }
    else {
        // Not recognized => just pass as is
        fprintf(fout, "\t%s\n", line);
    }
}

// If the first token is one of the macros, we expand in pass2
static int is_macro_line(const char *line) {
    char op[16];
    if (sscanf(line, "%15s", op) != 1) {
        return 0;
    }
    if (!strcmp(op, "ld")   || !strcmp(op, "push") ||
        !strcmp(op, "pop")  || !strcmp(op, "in")   ||
        !strcmp(op, "out")  || !strcmp(op, "clr")  ||
        !strcmp(op, "halt"))
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

    int code_section_printed = 0; // to unify all .code

    char line[1024];
    while (fgets(line, sizeof(line), fin)) {
        line[strcspn(line, "\n")] = '\0';
        trim(line);

        if (!line[0] || line[0] == ';') {
            continue; // skip empty or comment
        }

        // .code => only print the first one
        if (!strcmp(line, ".code")) {
            if (!code_section_printed) {
                fprintf(fout, ".code\n");
                code_section_printed = 1;
            }
            // skip subsequent .code lines
            continue;
        }
        // .data => print as is
        if (!strcmp(line, ".data")) {
            fprintf(fout, ".data\n");
            continue;
        }
        // label definition => re-emit as "LABEL:" in final output
        if (line[0] == ':') {
            // e.g. ":LOOP"
            // parse out "LOOP"
            char labelName[50];
            if (sscanf(line+1, "%49s", labelName) == 1) {
                // output "LOOP:"
                fprintf(fout, "%s:\n", labelName);
                continue;
            }
        }

        // If there's a reference to :LABEL => replace with decimal address
        char *colon = strchr(line, ':');
        if (colon) {
            // we expect something like "br :LOOP" or "ld r5, :NUM"
            char lbl[50];
            if (sscanf(colon+1, "%49s", lbl) == 1) {
                LabelAddress *entry = find_label(lbl);
                if (entry) {
                    *colon = '\0'; // cut off the line at the colon
                    // e.g. "ld r5, " + "4096"
                    char buffer[1024];
                    snprintf(buffer, sizeof(buffer), "\t%s%d", line, entry->address);
                    fprintf(fout, "%s\n", buffer);
                    continue;
                } else {
                    // label not found
                    fprintf(stderr, "Warning: label '%s' not found.\n", lbl);
                    // just print line
                    fprintf(fout, "\t%s\n", line);
                    continue;
                }
            }
        }

        // If recognized macro => expand
        if (is_macro_line(line)) {
            parseMacro(line, fout);
        } else {
            // normal instruction => just print
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

    // Pass 2 => unify multiple .code => 1, keep .data, re-emit label lines
    pass2(argv[1], argv[2]);

    free_hashmap();
    return 0;
}
