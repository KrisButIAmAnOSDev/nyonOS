#include "preempt.h"

struct preempt_state preempt_state;

void preempt_init(void) {
    preempt_state.count = 0;
}
