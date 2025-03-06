#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <regex.h>
#include <errno.h>
#include "uthash.h"

/******************************************************************************
 * Label -> Address Map
 ******************************************************************************/
typedef struct {
    char label[50];
    int address;        // ie 0x1000 is stored as 4096 (in decimal)
    UT_hash_handle hh;  // UTHash handle
} LabelAddress;

static LabelAddress *hashmap = NULL;

// add a label to the hash map.
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

// find a label by name.
static LabelAddress *find_label(const char *label) {
    LabelAddress *entry;
    HASH_FIND_STR(hashmap, label, entry);
    return entry;
}

// free the hashmap
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
 * Helper functions to parse 12-bit ranges
 ******************************************************************************/
static int parse_signed_12_bit(const char *str, int *valueOut) {
    errno = 0;
    long val = strtol(str, NULL, 0); // allow 0x prefix
    if ((errno == ERANGE) || (val < -2048) || (val > 2047)) {
        return 0; // Out of range
    }
    *valueOut = (int)val;
    return 1;
}

static int parse_unsigned_12_bit(const char *str, int *valueOut) {
    errno = 0;
    unsigned long val = strtoul(str, NULL, 0); // allow 0x prefix
    if ((errno == ERANGE) || (val > 4095)) {
        return 0; // Out of range
    }
    *valueOut = (int)val;
    return 1;
}

/******************************************************************************
 * Macros used in PASS 1 to detect expansions (e.g., ld, push, pop)
 * so we can increment PC by the correct byte-count.
 ******************************************************************************/
static int starts_with_ld(const char *line) {
    while (isspace((unsigned char)*line)) { line++; }
    if (strncmp(line, "ld", 2) == 0) {
        char c = line[2];
        if (c == '\0' || isspace((unsigned char)c) || c == ',') {
            return 1;
        }
    }
    return 0;
}

static int starts_with_push(const char *line) {
    while (isspace((unsigned char)*line)) { line++; }
    if (!strncmp(line, "push", 4)) {
        char c = line[4];
        if (c == '\0' || isspace((unsigned char)c)) {
            return 1;
        }
    }
    return 0;
}

static int starts_with_pop(const char *line) {
    while (isspace((unsigned char)*line)) { line++; }
    if (!strncmp(line, "pop", 3)) {
        char c = line[3];
        if (c == '\0' || isspace((unsigned char)c)) {
            return 1;
        }
    }
    return 0;
}

/******************************************************************************
 * validate_brr: brr has two forms:
 *   brr rX  (pc ← pc + rX)  opcode 0x9
 *   brr L   (pc ← pc + L)   opcode 0xa, L can be negative
 ******************************************************************************/
static int validate_brr(const char *line) {
    char op[32], operand[64];
    int count = sscanf(line, "%31s %63[^\n]", op, operand);
    if (count < 2) {
        fprintf(stderr, "Error: 'brr' missing operand: %s\n", line);
        return 0;
    }
    if (operand[0] == 'r') {
        // brr rX form
        int rd = atoi(operand + 1);
        if (rd < 0 || rd > 31) {
            fprintf(stderr, "Error: 'brr r%d' invalid register.\n", rd);
            return 0;
        }
        return 1; // valid
    } else {
        // brr L form => L is signed 12-bit
        int val;
        if (!parse_signed_12_bit(operand, &val)) {
            fprintf(stderr, "Error: 'brr' literal out of [-2048..2047]: %s\n", operand);
            return 0;
        }
        return 1;
    }
}

/******************************************************************************
 * validate_mov: Tinker has 4 forms of mov:
 *  1) mov rD, (rS)(L)   => 0x10
 *  2) mov rD, rS        => 0x11
 *  3) mov rD, L         => 0x12
 *  4) mov (rD)(L), rS   => 0x13
 * We'll parse them in pass1 to ensure correct usage and immediate range.
 ******************************************************************************/
static int validate_mov(const char *line) {
   
    // quick tokenize
    char op[32], part1[64], part2[64];
    int count = sscanf(line, "%31s %63[^,], %63[^\n]", op, part1, part2);

    // if count < 2 => invalid "mov"
    if (count < 2) {
        fprintf(stderr, "Error: incomplete 'mov' instruction: %s\n", line);
        return 0;
    }

    // if count=2
    if (count == 2) {
        char second[64] = {0};
        if (sscanf(part1, "%63s %63s", part1, second) == 2) {
            strcpy(part2, second);
            count = 3;
        }
    }

    // have possibly 3 tokens: op, part1, part2
    // remove trailing spaces or commas from part1, part2
    {
        // trim front
        char *p = part1; 
        while (isspace((unsigned char)*p) || *p == ',') p++;
        if (p != part1) memmove(part1, p, strlen(p)+1);
        p = part2;
        while (isspace((unsigned char)*p) || *p == ',') p++;
        if (p != part2) memmove(part2, p, strlen(p)+1);
        // trim end
        for (int i = (int)strlen(part1)-1; i >= 0; i--) {
            if (isspace((unsigned char)part1[i]) || part1[i] == ',') part1[i] = '\0'; else break;
        }
        for (int i = (int)strlen(part2)-1; i >= 0; i--) {
            if (isspace((unsigned char)part2[i]) || part2[i] == ',') part2[i] = '\0'; else break;
        }
    }

    // see which form it might be:
    //  (a) mov rD, rS
    //  (b) mov rD, L
    //  (c) mov rD, (rS)(L)
    //  (d) mov (rD)(L), rS
    //
    // detect (d) if part1 starts with '(' or we see "(". 
    // detect (c) if part2 starts with '('
    // detect (a) if both part1 and part2 are registers
    // detect (b) if part1 is register, part2 is immediate
    //

    // If "part1" starts with '(' => form (d)
    if (part1[0] == '(') {
        // (d) mov (rD)(L), rS
        // part1 => e.g. "(r5)(-8)"
        // part2 => e.g. "r6"
        // check part2 is register
        if (part2[0] != 'r') {
            fprintf(stderr, "Error: 'mov (rD)(L), rS' => 'rS' is not a register? %s\n", line);
            return 0;
        }
        int rS = atoi(part2+1);
        if (rS < 0 || rS > 31) {
            fprintf(stderr, "Error: register out of range in mov (rD)(L), rS => %s\n", line);
            return 0;
        }
        // parse part1 => e.g. "(r5)(-8)"
        // find rD in the parentheses
        char *p = strstr(part1, "r");
        if (!p) {
            fprintf(stderr, "Error: 'mov (rD)(L), rS': can't find register in %s\n", part1);
            return 0;
        }
        int rD = atoi(p+1);
        if (rD < 0 || rD > 31) {
            fprintf(stderr, "Error: 'mov (rD)(L), rS': register out of range => %s\n", line);
            return 0;
        }
        p = strchr(p, '(');
        if (!p) {
            fprintf(stderr, "Error: 'mov (rD)(L), rS': missing offset => %s\n", line);
            return 0;
        }
        // p should point to "(" that encloses the offset
        p++; // skip '('
        char offsetBuf[32];
        int i=0;
        while (*p && *p != ')' && i < 31) {
            offsetBuf[i++] = *p++;
        }
        offsetBuf[i] = '\0';
        // offsetBuf might be "-8" or "12"
        // assume signed 12-bit
        int offsetVal;
        if (!parse_signed_12_bit(offsetBuf, &offsetVal)) {
            fprintf(stderr, "Error: offset out of [-2048..2047] in 'mov (rD)(L), rS' => %s\n", offsetBuf);
            return 0;
        }
        // passes validation
        return 1; 
    }

    // otherwise, part1 is presumably "rD"
    if (part1[0] != 'r') {
        fprintf(stderr, "Error: mov => expected 'rD' or '(rD)(L)' => got: %s\n", part1);
        return 0;
    }
    int rD = atoi(part1+1);
    if (rD < 0 || rD > 31) {
        fprintf(stderr, "Error: register out of range in mov => %s\n", line);
        return 0;
    }

    // if count < 3 => incomplete
    if (count < 3) {
        fprintf(stderr, "Error: incomplete 'mov' => missing second operand: %s\n", line);
        return 0;
    }

    // check part2
    if (part2[0] == 'r') {
        // (a) mov rD, rS
        // ie "mov r5, r6"
        int rS = atoi(part2+1);
        if (rS < 0 || rS > 31) {
            fprintf(stderr, "Error: register out of range => %s\n", line);
            return 0;
        }
        // Valid
        return 1;
    }
    else if (part2[0] == '(') {
        // (c) mov rD, (rS)(L)
        // parse the memory form
        // ie part2="(r6)(-8)"
        char *p = strstr(part2, "r");
        if (!p) {
            fprintf(stderr, "Error: can't find register in 'mov rD, (rS)(L)' => %s\n", line);
            return 0;
        }
        int rS = atoi(p+1);
        if (rS < 0 || rS > 31) {
            fprintf(stderr, "Error: register out of range in 'mov rD, (rS)(L)' => %s\n", line);
            return 0;
        }
        p = strchr(p, '(');
        if (!p) {
            fprintf(stderr, "Error: missing offset => %s\n", line);
            return 0;
        }
        p++; // skip '('
        char offsetBuf[32];
        int i=0;
        while (*p && *p != ')' && i < 31) {
            offsetBuf[i++] = *p++;
        }
        offsetBuf[i] = '\0';
        // parse offset as signed 12-bit
        int offsetVal;
        if (!parse_signed_12_bit(offsetBuf, &offsetVal)) {
            fprintf(stderr, "Error: offset out of [-2048..2047] => %s\n", offsetBuf);
            return 0;
        }
        return 1; 
    }
    else {
        // (b) mov rD, L
        // We assume an unsigned 12-bit literal for bits 52..63
        // ie "mov r5, 100"
        int val;
        if (!parse_unsigned_12_bit(part2, &val)) {
            fprintf(stderr, "Error: mov rD, L => L out of [0..4095]: %s\n", part2);
            return 0;
        }
        return 1;
    }
}

/******************************************************************************
 * For addi, subi, shftri, shftli => 0..4095
 * For everything else with literal => separate checks (brr, mov, etc.)
 ******************************************************************************/
static int validate_instruction_immediate(const char *line) {
    // do a quick parse: opcode, rD, immediate
    char op[32], rdPart[32], immPart[64];
    int count = sscanf(line, "%31s %31s %63[^\n]", op, rdPart, immPart);
    if (count < 2) return 1; // we won't call it invalid here

    if (!strcmp(op, "addi") || !strcmp(op, "subi") || 
        !strcmp(op, "shftri") || !strcmp(op, "shftli")) 
    {
        // must have an unsigned 12-bit immediate
        if (count < 3) {
            fprintf(stderr, "Error: missing immediate => %s\n", line);
            return 0;
        }
        // rdPart => "rD"
        if (rdPart[0] != 'r') return 0;
        int rD = atoi(rdPart+1);
        if (rD < 0 || rD > 31) {
            fprintf(stderr, "Error: register out of range => %s\n", line);
            return 0;
        }
        // immPart might have a leading comma
        char *p = immPart;
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        int val;
        if (!parse_unsigned_12_bit(p, &val)) {
            fprintf(stderr, "Error: %s => immediate out of [0..4095]: %s\n", op, p);
            return 0;
        }
        return 1;
    }
    // fallback => do nothing
    return 1;
}

/******************************************************************************
 * is_valid_instruction_pass1: check opcode and specialized forms (brr, mov, etc.)
 ******************************************************************************/
static int is_valid_instruction_pass1(const char *line) {
    char op[32];
    if (sscanf(line, "%31s", op) != 1) {
        return 0;
    }
    const char *ops[] = {
        "add","addi","sub","subi","mul","div",
        "and","or","xor","not","shftr","shftri","shftl","shftli",
        "br","brr","brnz","call","return","brgt",
        "addf","subf","mulf","divf",
        "mov",
        "halt",
        "in","out","clr","ld","push","pop"
    };
    int recognized = 0;
    int n = (int)(sizeof(ops)/sizeof(ops[0]));
    for (int i = 0; i < n; i++) {
        if (!strcmp(op, ops[i])) {
            recognized = 1;
            break;
        }
    }
    if (!recognized) {
        return 0;
    }

    if (!strcmp(op, "brr")) {
        if (!validate_brr(line)) {
            return 0;
        }
        return 1;
    }
    else if (!strcmp(op, "mov")) {
        // parse the line for any of the 4 forms
        if (!validate_mov(line)) {
            return 0;
        }
        return 1;
    }

    // for addi, subi, etc.
    if (!validate_instruction_immediate(line)) {
        return 0;
    }

    return 1;
}

/******************************************************************************
 * PASS 1: Gather labels, track the program counter, ensure valid instructions.
 ******************************************************************************/
static void pass1(const char *filename) {
    FILE *fin = fopen(filename, "r");
    if (!fin) {
        perror("pass1: fopen");
        exit(1);
    }
    enum { NONE, CODE, DATA } section = NONE;
    int programCounter = 0x1000; // tinker code starts at address 0x1000

    char line[1024];
    while (fgets(line, sizeof(line), fin)) {
        line[strcspn(line, "\n")] = '\0';
        trim(line);
        if (!line[0] || line[0] == ';') {
            continue;
        }
        // check for disrectives.
        if (line[0] == '.') {
            if (!strncmp(line, ".code", 5)) {
                section = CODE;
            }
            else if (!strncmp(line, ".data", 5)) {
                section = DATA;
            }
            continue;
        }
        // label definition.
        if (line[0] == ':') {
            char labelName[50];
            if (sscanf(line + 1, "%49s", labelName) == 1) {
                add_label(labelName, programCounter);
            }
            continue;
        }
        // process instructions/data
        if (section == CODE) {
            // validate the instruction
            if (!is_valid_instruction_pass1(line)) {
                fprintf(stderr, "pass1 error: invalid line => %s\n", line);
                fclose(fin);
                exit(1);
            }
            // macros expansions for pass1 counting:
            if (starts_with_ld(line)) {
                // ld => expands to 48 bytes
                programCounter += 48;
            }
            else if (starts_with_push(line)) {
                // push => expands to 8 bytes
                programCounter += 8;
            }
            else if (starts_with_pop(line)) {
                // pop => expands to 8 bytes
                programCounter += 8;
            }
            else {
                // normal instruction => 4 bytes
                programCounter += 4;
            }
        } 
        else if (section == DATA) {
            // Each data item is 8 bytes
            programCounter += 8;
        }
    }
    fclose(fin);
}

/******************************************************************************
 * Macro expansions (Pass 2)
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
    fprintf(fout, "\tmov (r31)(-8), r%d\n", rD);
    fprintf(fout, "\tsubi r31, 8\n");
}
static void expandPop(int rD, FILE *fout) {
    fprintf(fout, "\tmov r%d, (r31)(0)\n", rD);
    fprintf(fout, "\taddi r31, 8\n");
}

/******************************************************************************
 * expandLd: Expand an ld macro into 12 instructions (48 bytes).
 ******************************************************************************/
static void expandLd(int rD, uint64_t L, FILE *fout) {
    fprintf(fout, "\txor r%d, r%d, r%d\n", rD, rD, rD);

    unsigned long long top12  = (L >> 52) & 0xFFF;
    unsigned long long mid12a = (L >> 40) & 0xFFF;
    unsigned long long mid12b = (L >> 28) & 0xFFF;
    unsigned long long mid12c = (L >> 16) & 0xFFF;
    unsigned long long mid4   = (L >> 4)  & 0xFFF;
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
 * parseMacro (Pass 2):
 * Handle macros: ld, push, pop, in, out, clr, halt
 ******************************************************************************/
static void parseMacro(const char *line, FILE *fout) {
    regex_t regex;
    regmatch_t matches[4];
    char op[16];

    if (sscanf(line, "%15s", op) != 1) {
        fprintf(stderr, "parseMacro: cannot parse op from line: %s\n", line);
        return;
    }

    if (!strcmp(op, "ld")) {
        const char *pattern =
          "^[[:space:]]*ld[[:space:]]+r([0-9]+)[[:space:]]*,?[[:space:]]*(:?)([0-9a-fA-FxX:]+)[[:space:]]*$";
        if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
            fprintf(stderr, "Could not compile regex for ld\n");
            return;
        }
        if (regexec(&regex, line, 4, matches, 0) == 0) {
            char regBuf[16], immBuf[64];
            int rD;
            uint64_t imm;
            int len = matches[1].rm_eo - matches[1].rm_so;
            strncpy(regBuf, line + matches[1].rm_so, len);
            regBuf[len] = '\0';
            rD = atoi(regBuf);
            if (rD < 0 || rD > 31) {
                fprintf(stderr, "Error: register out of range in ld => r%d\n", rD);
                regfree(&regex);
                return;
            }
            len = matches[3].rm_eo - matches[3].rm_so;
            strncpy(immBuf, line + matches[3].rm_so, len);
            immBuf[len] = '\0';

            if (immBuf[0] == ':') {
                LabelAddress *entry = find_label(immBuf + 1);
                if (!entry) {
                    fprintf(stderr, "Error: label %s not found\n", immBuf+1);
                    regfree(&regex);
                    return;
                }
                imm = entry->address;
            } else {
                errno = 0;
                char *endptr = NULL;
                uint64_t tmpVal = strtoull(immBuf, &endptr, 0);
                if (errno == ERANGE) {
                    fprintf(stderr, "Error: 'ld' immediate out of 64-bit range => %s\n", immBuf);
                    regfree(&regex);
                    return;
                }
                imm = tmpVal;
            }
            expandLd(rD, imm, fout);
        } else {
            fprintf(stderr, "Error parsing ld macro: %s\n", line);
        }
        regfree(&regex);
    }
    else if (!strcmp(op, "push")) {
        const char *pattern =
          "^[[:space:]]*push[[:space:]]+r([0-9]+)[[:space:]]*,?[[:space:]]*$";
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
            if (rD < 0 || rD > 31) {
                fprintf(stderr, "Error: register out of range in push => %s\n", line);
                regfree(&regex);
                return;
            }
            expandPush(rD, fout);
        } else {
            fprintf(stderr, "Error parsing push macro: %s\n", line);
        }
        regfree(&regex);
    }
    else if (!strcmp(op, "pop")) {
        const char *pattern =
          "^[[:space:]]*pop[[:space:]]+r([0-9]+)[[:space:]]*,?[[:space:]]*$";
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
            if (rD < 0 || rD > 31) {
                fprintf(stderr, "Error: register out of range in pop => %s\n", line);
                regfree(&regex);
                return;
            }
            expandPop(rD, fout);
        } else {
            fprintf(stderr, "Error parsing pop macro: %s\n", line);
        }
        regfree(&regex);
    }
    else if (!strcmp(op, "in")) {
        const char *pattern =
          "^[[:space:]]*in[[:space:]]+r([0-9]+)[[:space:]]*,?[[:space:]]*r([0-9]+)[[:space:]]*$";
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
            if (rD < 0 || rD > 31 || rS < 0 || rS > 31) {
                fprintf(stderr, "Error: register out of range in 'in': %s\n", line);
                regfree(&regex);
                return;
            }
            expandIn(rD, rS, fout);
        } else {
            fprintf(stderr, "Error parsing in macro: %s\n", line);
        }
        regfree(&regex);
    }
    else if (!strcmp(op, "out")) {
        const char *pattern =
          "^[[:space:]]*out[[:space:]]+r([0-9]+)[[:space:]]*,?[[:space:]]*r([0-9]+)[[:space:]]*$";
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
            if (rD < 0 || rD > 31 || rS < 0 || rS > 31) {
                fprintf(stderr, "Error: register out of range in 'out': %s\n", line);
                regfree(&regex);
                return;
            }
            expandOut(rD, rS, fout);
        } else {
            fprintf(stderr, "Error parsing out macro: %s\n", line);
        }
        regfree(&regex);
    }
    else if (!strcmp(op, "clr")) {
        const char *pattern =
          "^[[:space:]]*clr[[:space:]]+r([0-9]+)[[:space:]]*$";
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
            if (rD < 0 || rD > 31) {
                fprintf(stderr, "Error: register out of range in clr => %s\n", line);
                regfree(&regex);
                return;
            }
            expandClr(rD, fout);
        } else {
            fprintf(stderr, "Error parsing clr macro: %s\n", line);
        }
        regfree(&regex);
    }
    else if (!strcmp(op, "halt")) {
        const char *pattern = 
          "^[[:space:]]*halt[[:space:]]*$";
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
        // not recognized – print the line as-is.
        fprintf(fout, "\t%s\n", line);
    }
}

// check if line is one of the recognized macros
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

/******************************************************************************
 * PASS 2: Output file generation (macro expansion + label substitution)
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

    char line[1024];
    while (fgets(line, sizeof(line), fin)) {
        line[strcspn(line, "\n")] = '\0';
        trim(line);
        if (!line[0] || line[0] == ';') {
            continue;
        }
        // Directives
        if (!strcmp(line, ".code")) {
            fprintf(fout, ".code\n");
            continue;
        }
        if (!strcmp(line, ".data")) {
            fprintf(fout, ".data\n");
            continue;
        }
        // skip label definitions
        if (line[0] == ':') {
            continue;
        }
        // replace label references with it's address
        char *colon = strchr(line, ':');
        if (colon) {
            char lbl[50];
            if (sscanf(colon + 1, "%49s", lbl) == 1) {
                LabelAddress *entry = find_label(lbl);
                if (entry) {
                    *colon = '\0'; // cut at colon
                    char buffer[1024];
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
        // if the line is a macro, expand; otherwise output as-is
        if (is_macro_line(line)) {
            parseMacro(line, fout);
        } else {
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
        fprintf(stderr, "Usage: %s <inputfile> <outputfile>\n", argv[0]);
        return 1;
    }
    // Pass 1: validate instructions + populate label -> addresses hashmap
    pass1(argv[1]);
    // Pass 2: expand macros + replace labels with addreses
    pass2(argv[1], argv[2]);
    // free memory!
    free_hashmap();
    return 0;
}
