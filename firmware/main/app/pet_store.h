// Pet save/load in NVS.
#pragma once

#include <stdbool.h>
#include "pet.h"

bool pet_store_load(pet_t *p);          // false if no valid save
void pet_store_save(const pet_t *p);
