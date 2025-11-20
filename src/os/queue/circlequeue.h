#ifndef CIRCLEQ_H

#include "task.h"
#include <stdint.h>

struct CircleQueue {
  uint32_t current_start;
  uint32_t current_end;
  uint32_t max_length;
  uint32_t current_length;
  struct Task *taskbuffer;
};

uint8_t cq_is_full(struct CircleQueue *queue);
void cq_add_to_back(struct CircleQueue *queue, struct Task task);
struct Task cq_pop(struct CircleQueue *queue);

#endif /* ifndef CIRCLEQ_H */
