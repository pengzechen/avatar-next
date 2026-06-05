#ifndef DRIVER_IRQ_PLIC_H
#define DRIVER_IRQ_PLIC_H

#include "types.h"

typedef void (*plic_irq_handler_t)(uint32_t irq, void *ctx);

void plic_init(void);
void plic_enable_irq(uint32_t irq, uint32_t priority);
void plic_disable_irq(uint32_t irq);
void plic_install(uint32_t irq, plic_irq_handler_t handler, void *ctx);

#endif /* DRIVER_IRQ_PLIC_H */
