#include <stdint.h>

#include "src/io.h"
#include "src/commands.h"
#include "src/heap.h"

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

void kmain(void){
    init_malloc();

    printstr("\n> ");

    while (1)
    {
        const char c = readchar();
        if (c == '\n' || c == '\r') {
            cmdbuffer[cmdlen] = '\0';
            cmdlen++;

            int done = !execute(cmdbuffer);
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