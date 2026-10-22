/* SPDX-License-Identifier: MIT */
#ifndef ZM_CODEC_H
#define ZM_CODEC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
size_t zm_compress(const uint8_t *,size_t,uint8_t *,size_t);
bool zm_decompress(const uint8_t *,size_t,uint8_t *,size_t);
#endif
