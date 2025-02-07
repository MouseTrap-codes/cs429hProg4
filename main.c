#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <regex.h>
#include "uthash.h"

/******************************************************************************
 * Label -> Address Map
 ******************************************************************************/
typedef struct {
    char label[50];
    int address;        // e.g. 0x1000 is stored as 4096 (in decimal)
    UT_hash_handle hh;  // UTHash handle
} LabelAddress;

static LabelAddress *hashmap = NULL;

// Add a label to the hash map.
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

// Find a label by name.
static LabelAddress *find_label(const char *label) {
    LabelAddress *entry;
    HASH_FIND_STR(hashmap, label, entry);
    return entry;
}

// Free the entire label->address map.
static void free_hashmap(void) {
    LabelAddress *cur, *tmp;
    HASH_ITER(hh, hashmap, cur, tmp) {
        HASH_DEL(hashmap, cur);
        free(cur);
    }
}

/******************************************************************************
 * Trim leading/trailing whitespace in-place.
 ******************************************************************************/
static void trim(char *s) {
    char *p = s;
    while (isspace((unsigned char)*p)) { p++; }
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
 ******************************************************************************/
static int starts_with_ld(const char *line) {
    while (isspace((unsigned char)*line)) { line++; }
    if (strncmp(line, "ld", 2) == 0) {
        char c = line[2];
        if (c == '\0' || isspace((unsigned char)c) || c == ',') { return 1; }
    }
    return 0;
}

static int starts_with_push(const char *line) {
    while (isspace((unsigned char)*line)) { line++; }
    if (!strncmp(line, "push", 4)) {
        char c = line[4];
        if (c == '\0' || isspace((unsigned char)c)) { return 1; }
    }
    return 0;
}

static int starts_with_pop(const char *line) {
    while (isspace((unsigned char)*line)) { line++; }
    if (!strncmp(line, "pop", 3)) {
        char c = line[3];
        if (c == '\0' || isspace((unsigned char)c)) { return 1; }
    }
    return 0;
}

// Minimal validity check for the first token in pass1.
static int is_valid_instruction_pass1(const char *line) {
    char op[32];
    if (sscanf(line, "%31s", op) != 1) { return 0; }
    const char *ops[] = {
        "add","addi","sub","subi","mul","div",
        "and","or","xor","not","shftr","shftri","shftl","shftli",
        "br","brr","brnz","call","return","brgt",
        "addf","subf","mulf","divf",
        "mov",
        "halt",
        "in","out","clr","ld","push","pop"
    };
    int n = (int)(sizeof(ops)/sizeof(ops[0]));
    for (int i = 0; i < n; i++) {
        if (!strcmp(op, ops[i])) { return 1; }
    }
    return 0;
}

/******************************************************************************
 * PASS 1: Gather labels and track the program counter.
 ******************************************************************************/
static void pass1(const char *filename) {
    FILE *fin = fopen(filename, "r");
    if (!fin) {
        perror("pass1: fopen");
        exit(1);
    }
    enum { NONE, CODE, DATA } section = NONE;
    int programCounter = 0x1000; // Tinker code starts at address 0x1000

    char line[1024];
    while (fgets(line, sizeof(line), fin)) {
        line[strcspn(line, "\n")] = '\0';
        trim(line);
        if (!line[0] || line[0] == ';') { continue; }
        // Check for directives.
        if (line[0] == '.') {
            if (!strncmp(line, ".code", 5)) { section = CODE; }
            else if (!strncmp(line, ".data", 5)) { section = DATA; }
            continue;
        }
        // Label definition.
        if (line[0] == ':') {
            char labelName[50];
            if (sscanf(line + 1, "%49s", labelName) == 1) {
                add_label(labelName, programCounter);
            }
            continue;
        }
        // Process instructions or data items.
        if (section == CODE) {
            if (!is_valid_instruction_pass1(line)) {
                fprintf(stderr, "pass1 error: invalid line => %s\n", line);
                fclose(fin);
                exit(1);
            }
            if (starts_with_ld(line)) { programCounter += 48; }
            else if (starts_with_push(line)) { programCounter += 8; }
            else if (starts_with_pop(line)) { programCounter += 8; }
            else { programCounter += 4; }
        } else if (section == DATA) {
            programCounter += 8;
        }
    }
    fclose(fin);
}

/******************************************************************************
 * Macro expansion functions for non-regex macros.
 ******************************************************************************/
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
static void expandPush(int rD, FILE *fout) {
    fprintf(fout, "\tmov (r31)(0), r%d\n", rD);
    fprintf(fout, "\tsubi r31, 8\n");
}
static void expandPop(int rD, FILE *fout) {
    fprintf(fout, "\tmov r%d, (r31)(0)\n", rD);
    fprintf(fout, "\taddi r31, 8\n");
}

/******************************************************************************
 * expandLd: Expand an ld macro into 12 instructions that build a 64-bit immediate.
 * The expansion produces:
 *
 *   xor rX, rX, rX
 *   addi rX, <top12>
 *   shftli rX, 12
 *   addi rX, <mid12a>
 *   shftli rX, 12
 *   addi rX, <mid12b>
 *   shftli rX, 12
 *   addi rX, <mid12c>
 *   shftli rX, 12
 *   addi rX, <mid4>
 *   shftli rX, 4
 *   addi rX, <last4>
 *
 * All immediates are printed in decimal.
 ******************************************************************************/
static void expandLd(int rD, uint64_t L, FILE *fout) {
    // Zero the target register.
    fprintf(fout, "\txor r%d, r%d, r%d\n", rD, rD, rD);

    unsigned long long top12  = (L >> 52) & 0xFFF;
    unsigned long long mid12a = (L >> 40) & 0xFFF;
    unsigned long long mid12b = (L >> 28) & 0xFFF;
    unsigned long long mid12c = (L >> 16) & 0xFFF;
    unsigned long long mid4   = (L >> 4) & 0xFFF;
    unsigned long long last4  = L & 0xF;

    fprintf(fout, "\taddi r%d, %llu\n", rD, top12);
    fprintf(fout, "\tshftli r%d, 12\n", rD);
    fprintf(fout, "\taddi r%d, %llu\n", rD, mid12a);
    fprintf(fout, "\tshftli r%d, 12\n", rD);
    fprintf(fout, "\taddi r%d, %llu\n", rD, mid12b);
    fprintf(fout, "\tshftli r%d, 12\n", rD);
    fprintf(fout, "\taddi r%d, %llu\n", rD, mid12c);
    fprintf(fout, "\tshftli r%d, 12\n", rD);
    fprintf(fout, "\taddi r%d, %llu\n", rD, mid4);
    fprintf(fout, "\tshftli r%d, 4\n", rD);
    fprintf(fout, "\taddi r%d, %llu\n", rD, last4);
}

/******************************************************************************
 * parseMacro using regex to match macro expressions.
 * For Stage 1, the ld macro is expanded into 12 instructions.
 ******************************************************************************/
static void parseMacro(const char *line, FILE *fout) {
    regex_t regex;
    regmatch_t matches[3]; // Capture register and immediate.
    char op[16];

    if (sscanf(line, "%15s", op) != 1) {
        fprintf(stderr, "parseMacro: cannot parse op from line: %s\n", line);
        return;
    }

    if (strcmp(op, "ld") == 0) {
        /* The regex below accepts an immediate operand that may optionally begin with
           a colon (indicating a label reference) or not. */
        const char *pattern = "^[[:space:]]*ld[[:space:]]+r([0-9]+)[[:space:]]*,?[[:space:]]*(:?)([0-9a-fA-FxX:]+)[[:space:]]*$";
        if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
            fprintf(stderr, "Could not compile regex for ld\n");
            return;
        }
        if (regexec(&regex, line, 4, matches, 0) == 0) {
            char regBuf[16], immBuf[32];
            int rD;
            uint64_t imm;
            int len = matches[1].rm_eo - matches[1].rm_so;
            strncpy(regBuf, line + matches[1].rm_so, len);
            regBuf[len] = '\0';
            rD = atoi(regBuf);
            /* The second capture group (matches[2]) captures an optional colon.
               The actual immediate string is in capture group 3. */
            len = matches[3].rm_eo - matches[3].rm_so;
            strncpy(immBuf, line + matches[3].rm_so, len);
            immBuf[len] = '\0';
            /* If the immediate string starts with a colon, then it is a label reference.
               Remove the colon and look up the label's address. */
            if (immBuf[0] == ':') {
                LabelAddress *entry = find_label(immBuf+1);
                if (entry) {
                    imm = entry->address;
                } else {
                    fprintf(stderr, "Error: label %s not found\n", immBuf+1);
                    regfree(&regex);
                    return;
                }
            } else {
                imm = strtoull(immBuf, NULL, 0);
            }
            expandLd(rD, imm, fout);
        } else {
            fprintf(stderr, "Error parsing ld macro: %s\n", line);
        }
        regfree(&regex);
    }
    else if (strcmp(op, "push") == 0) {
        const char *pattern = "^[[:space:]]*push[[:space:]]+r([0-9]+)[[:space:]]*,?[[:space:]]*$";
        if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
            fprintf(stderr, "Could not compile regex for push\n");
            return;
        }
        if (regexec(&regex, line, 2, matches, 0) == 0) {
            char regBuf[16];
            int rD;
            int len = matches[1].rm_eo - matches[1].rm_so;
            strncpy(regBuf, line + matches[1].rm_so, len);
            regBuf[len] = '\0';
            rD = atoi(regBuf);
            expandPush(rD, fout);
        } else {
            fprintf(stderr, "Error parsing push macro: %s\n", line);
        }
        regfree(&regex);
    }
    else if (strcmp(op, "pop") == 0) {
        const char *pattern = "^[[:space:]]*pop[[:space:]]+r([0-9]+)[[:space:]]*,?[[:space:]]*$";
        if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
            fprintf(stderr, "Could not compile regex for pop\n");
            return;
        }
        if (regexec(&regex, line, 2, matches, 0) == 0) {
            char regBuf[16];
            int rD;
            int len = matches[1].rm_eo - matches[1].rm_so;
            strncpy(regBuf, line + matches[1].rm_so, len);
            regBuf[len] = '\0';
            rD = atoi(regBuf);
            expandPop(rD, fout);
        } else {
            fprintf(stderr, "Error parsing pop macro: %s\n", line);
        }
        regfree(&regex);
    }
    else if (strcmp(op, "in") == 0) {
        const char *pattern = "^[[:space:]]*in[[:space:]]+r([0-9]+)[[:space:]]*,?[[:space:]]*r([0-9]+)[[:space:]]*$";
        if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
            fprintf(stderr, "Could not compile regex for in\n");
            return;
        }
        if (regexec(&regex, line, 3, matches, 0) == 0) {
            char regBuf[16], regBuf2[16];
            int rD, rS;
            int len = matches[1].rm_eo - matches[1].rm_so;
            strncpy(regBuf, line + matches[1].rm_so, len);
            regBuf[len] = '\0';
            len = matches[2].rm_eo - matches[2].rm_so;
            strncpy(regBuf2, line + matches[2].rm_so, len);
            regBuf2[len] = '\0';
            rD = atoi(regBuf);
            rS = atoi(regBuf2);
            expandIn(rD, rS, fout);
        } else {
            fprintf(stderr, "Error parsing in macro: %s\n", line);
        }
        regfree(&regex);
    }
    else if (strcmp(op, "out") == 0) {
        const char *pattern = "^[[:space:]]*out[[:space:]]+r([0-9]+)[[:space:]]*,?[[:space:]]*r([0-9]+)[[:space:]]*$";
        if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
            fprintf(stderr, "Could not compile regex for out\n");
            return;
        }
        if (regexec(&regex, line, 3, matches, 0) == 0) {
            char regBuf[16], regBuf2[16];
            int rD, rS;
            int len = matches[1].rm_eo - matches[1].rm_so;
            strncpy(regBuf, line + matches[1].rm_so, len);
            regBuf[len] = '\0';
            len = matches[2].rm_eo - matches[2].rm_so;
            strncpy(regBuf2, line + matches[2].rm_so, len);
            regBuf2[len] = '\0';
            rD = atoi(regBuf);
            rS = atoi(regBuf2);
            expandOut(rD, rS, fout);
        } else {
            fprintf(stderr, "Error parsing out macro: %s\n", line);
        }
        regfree(&regex);
    }
    else if (strcmp(op, "clr") == 0) {
        const char *pattern = "^[[:space:]]*clr[[:space:]]+r([0-9]+)[[:space:]]*$";
        if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
            fprintf(stderr, "Could not compile regex for clr\n");
            return;
        }
        if (regexec(&regex, line, 2, matches, 0) == 0) {
            char regBuf[16];
            int rD;
            int len = matches[1].rm_eo - matches[1].rm_so;
            strncpy(regBuf, line + matches[1].rm_so, len);
            regBuf[len] = '\0';
            rD = atoi(regBuf);
            expandClr(rD, fout);
        } else {
            fprintf(stderr, "Error parsing clr macro: %s\n", line);
        }
        regfree(&regex);
    }
    else if (strcmp(op, "halt") == 0) {
        const char *pattern = "^[[:space:]]*halt[[:space:]]*$";
        if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
            fprintf(stderr, "Could not compile regex for halt\n");
            return;
        }
        if (regexec(&regex, line, 0, NULL, 0) == 0) {
            expandHalt(fout);
        } else {
            fprintf(stderr, "Error parsing halt macro: %s\n", line);
        }
        regfree(&regex);
    }
    else {
        // Not recognized – simply print the line as-is.
        fprintf(fout, "\t%s\n", line);
    }
}

// Checks if the line begins with one of the recognized macro opcodes.
static int is_macro_line(const char *line) {
    char op[16];
    if (sscanf(line, "%15s", op) != 1) { return 0; }
    if (!strcmp(op, "ld")   || !strcmp(op, "push") ||
        !strcmp(op, "pop")  || !strcmp(op, "in")   ||
        !strcmp(op, "out")  || !strcmp(op, "clr")  ||
        !strcmp(op, "halt"))
    {
        return 1;
    }
    return 0;
}

/******************************************************************************
 * PASS 2: Process the input file, performing label replacement and macro expansion.
 ******************************************************************************/
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

    int code_section_printed = 0; // Unify all .code sections.
    char line[1024];
    while (fgets(line, sizeof(line), fin)) {
        line[strcspn(line, "\n")] = '\0';
        trim(line);
        if (!line[0] || line[0] == ';') { continue; }
        // Handle directives.
        if (!strcmp(line, ".code")) {
            if (!code_section_printed) {
                fprintf(fout, ".code\n");
                code_section_printed = 1;
            }
            continue;
        }
        if (!strcmp(line, ".data")) {
            fprintf(fout, ".data\n");
            continue;
        }
        // Skip label definitions.
        if (line[0] == ':') {
            continue;
        }
        // If there is a label reference (a colon) in the line, replace it with its resolved address.
        char *colon = strchr(line, ':');
        if (colon) {
            char lbl[50];
            if (sscanf(colon+1, "%49s", lbl) == 1) {
                LabelAddress *entry = find_label(lbl);
                if (entry) {
                    *colon = '\0'; // Cut off at the colon.
                    char buffer[1024];
                    // Output the label's address in decimal.
                    snprintf(buffer, sizeof(buffer), "\t%s%d", line, entry->address);
                    if (is_macro_line(buffer)) {
                        parseMacro(buffer, fout);
                    } else {
                        fprintf(fout, "%s\n", buffer);
                    }
                    continue;
                } else {
                    fprintf(stderr, "Warning: label '%s' not found.\n", lbl);
                    fprintf(fout, "\t%s\n", line);
                    continue;
                }
            }
        }
        // If the line is a macro, expand it; otherwise, output it as-is.
        if (is_macro_line(line)) {
            parseMacro(line, fout);
        } else {
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
    // Pass 1: Gather label addresses.
    pass1(argv[1]);
    // Pass 2: Process the file (macro expansion and label replacement).
    pass2(argv[1], argv[2]);
    // Free the label->address map.
    free_hashmap();
    return 0;
}
