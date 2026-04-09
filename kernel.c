#include <stdint.h>

#include "src/io.h"

int done = 0;
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
    printstr("\n> ");
    
    int done = 0;

    while (!done)
    {
        const char c = readchar();
        if (c == '\n' || c == '\r') {
            cmdbuffer[cmdlen] = '\0';
            cmdlen++;

            // done = !execute(cmdbuffer);
            if (done) { printstr("\nTried to quit!"); }
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