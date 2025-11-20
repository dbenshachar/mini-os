#include "commands.h"

int echo(char *string) {
  puts("\n");
  puts(string);
  return 1;
}

int quit() { return 0; }

uint8_t deserialize_params(char *cmd, char **params, uint8_t max_params) {
  uint8_t count = 0;
  char *p = cmd;

  while (*p != '\0') {
    while (*p == ' ')
      p++;
    if (*p == '\0')
      break;

    if (count >= max_params)
      break;
    params[count++] = p;

    while (*p != '\0' && *p != ' ')
      p++;
    if (*p == '\0')
      break;

    *p = '\0';
    p++;
  }
  return count;
}

int execute(char *cmd) {
  char *params[MAX_PARAMS];
  int param_count = deserialize_params(cmd, params, MAX_PARAMS);
  if (param_count == 0) {
    return 0;
  }
  const char *exec_command = params[0];
  if (str_eq(exec_command, "quit")) {
    return quit();
  }
  if (str_eq(exec_command, "echo")) {
    return echo(params[1]);
  }
  if (str_eq(exec_command, "echov")) {
  }
  puts("\nInvalid command!");
  return 1;
}
