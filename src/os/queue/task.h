#ifndef TASK_H

#include <stdint.h>

struct Task {
  uint32_t priority;
  bool (*func)();
};

#endif /* ifndef TASK_H */
