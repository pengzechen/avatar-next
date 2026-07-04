#ifndef SHARED_PAGE_H
#define SHARED_PAGE_H

#include "types.h"

void shared_page_ref(uint64_t paddr);
void shared_page_unref(uint64_t paddr);

#endif /* SHARED_PAGE_H */
