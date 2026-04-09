#ifndef COMMANDS_H
#define COMMANDS_H

int deserialize_params(char *cmd, char **params, int max_params);
int execute(char* cmd);

#endif // COMMANDS_H