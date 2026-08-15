#ifndef WIFI_API_H
#define WIFI_API_H

#include "types.h"

int wifi_runtime_init(void);
void wifi_sdio1_irq_handler(void);
int wifi_sdio1_probe(uintptr_t crg_base, uintptr_t sysctrl_base,
                      uintptr_t rtcsys_ctrl_base, uintptr_t rtcsys_io_base,
                      uintptr_t sdio1_base);

#endif /* WIFI_API_H */
