// PS2 profile: get_number() table lookup (no Lua expression evaluator)
#ifndef PS2_SOC_NUMBERS_H
#define PS2_SOC_NUMBERS_H

#include <string.h>
#include "../doomtype.h"
#include "../m_fixed.h"

boolean PS2_SOCNumber(const char *word, fixed_t *value);

#endif
