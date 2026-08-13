#ifndef FAULT_INJECTOR_H
#define FAULT_INJECTOR_H

#include <stdint.h>

#include "fault_config.h"

void fault_injector_init(uint32_t seed);
void fault_injector_step(void);
void fault_injector_set_profile(FaultProfile_t profile);
FaultProfile_t fault_injector_get_profile(void);

#endif