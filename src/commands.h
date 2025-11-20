#ifndef COMMANDS_H
#define COMMANDS_H

#include "os/io/uart.h"
#include "os/util.h"
#include <stdint.h>

#define MAX_PARAMS 32
#define MAX_PARAM_LENGTH 16

int echo(char *string);

int quit();

uint8_t deserialize_params(char *cmd, char **params, uint8_t max_params); 

int execute(char *cmd); 

#endif
