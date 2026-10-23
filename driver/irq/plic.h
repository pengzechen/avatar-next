/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef DRIVER_IRQ_PLIC_H
#define DRIVER_IRQ_PLIC_H

#include "types.h"

void plic_init(void);
void plic_enable_irq(uint32_t irq, uint32_t priority);
void plic_disable_irq(uint32_t irq);

#endif /* DRIVER_IRQ_PLIC_H */
