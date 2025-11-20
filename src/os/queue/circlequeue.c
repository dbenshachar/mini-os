#include "circlequeue.h"

uint8_t cq_is_full(struct CircleQueue *queue) {
  return queue->current_length >= queue->max_length;
}

void cq_add_to_back(struct CircleQueue *queue, struct Task task) {
  queue->taskbuffer[queue->current_end] = task;
  queue->current_end += 1;
  if (queue->current_end >= queue->max_length) {
    queue->current_end -= queue->max_length;
  }
  queue->current_length += 1;
}

struct Task cq_pop(struct CircleQueue *queue) {
  queue->current_start += 1;
  if (queue->current_start >= queue->max_length) {
    queue->current_start -= queue->max_length;
  }
  queue->current_length -= 1;
  return queue->taskbuffer[queue->current_start - 1];
}

struct CircleQueue cq_create(uint32_t maxlength, struct Task *taskbuffer) {
    struct CircleQueue r = {0, 0, maxlength, 0, taskbuffer};
    return r;

}
