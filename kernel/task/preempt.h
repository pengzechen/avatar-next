#ifndef KERNEL_TASK_PREEMPT_H
#define KERNEL_TASK_PREEMPT_H

#include "types.h"

void preempt_disable(void);
void preempt_enable(void);
bool preemptible(void);

#endif /* KERNEL_TASK_PREEMPT_H */
