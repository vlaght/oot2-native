// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The oot2-native contributors
//
// Translation of a shader variant on demand (runtime/gpu/shader_translate.c): when the replay needs a module under answers
// that no pack holds, the translator child (oot2-xlate --serve) makes it from the program's job record, and the result stays
// in memory for the run. Without it a state the packs cannot serve stops the run by name.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "oot2/shader_cache.h"

// The packs' folder, from the path of the base pack (shaders/shader-base.bin): where split/jobs.jsonl and split/programs.bin
// lie. Called when the store is opened.
void sht_open(const char *base_pack_path);
// The variant of program `key` under `answers` (the canonical JSON object of the answers its module reads, as the ledger
// writes it: {"attr":{"6":"uint"}}) whose digest is `digest`: translated now, or found among those made. 1 and its index
// in *index, or 0 with a reason in why.
int sht_variant(const char *key, const uint8_t digest[32], const char *answers, int *index, char *why, size_t cap);
// The entry of variant `index`, as a pack row reads: valid for the run. 1 when there is one.
int sht_entry(int index, ShcEntry *out);
uint64_t sht_count(void);   // the variants made so far
