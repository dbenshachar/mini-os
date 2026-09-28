#include <stdint.h>

#include "src/io.h"
#include "src/commands.h"
#include "src/heap.h"
#include "src/files.h"

char cmdbuffer[256];
uint8_t cmdlen = 0;

void new_line() {
    cmdbuffer[0] = '\0';
    cmdlen = 0;
}

void backspace() {
    cmdbuffer[cmdlen] = '\0';
    cmdlen = (cmdlen > 0) ? cmdlen - 1 : 0;
}

void printint(int value) {
    char digits[12];
    int i = 0;
    unsigned int n;
    if (value < 0) {
        printchar('-');
        n = (unsigned int)(-value);
    } else {
        n = (unsigned int)value;
    }
    if (n == 0) {
        printchar('0');
        return;
    }
    while (n > 0) {
        digits[i++] = (char)('0' + (n % 10));
        n /= 10;
    }
    while (i > 0) printchar(digits[--i]);
}

void kmain(void){
    int fs_result;
    init_commands();
    init_malloc();
    fs_result = fs_init();
    if (fs_result != 0) {
        printstr("\nfs init failed: ");
        printint(fs_result);
    }

    printstr("\n> ");

    while (1)
    {
        const char c = readchar();
        if (c == '\n' || c == '\r') {
            cmdbuffer[cmdlen] = '\0';
            cmdlen++;

            int done = execute(cmdbuffer);
            if (done) { printstr("\nShutting down...\n"); return; }
            new_line();
            printstr("\n> ");
            continue;
        }
        if (c ==0x7F || c =='\b') {
            backspace();
            printstr("\b \b");
            continue;
        }
        cmdbuffer[cmdlen] = c;
        cmdlen++;
        printchar((long) c);
    }
}
