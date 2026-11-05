#pragma once
// saan_model_open が PSRAM に写した model 領域全体の先頭と大きさ（開く前は NULL）。
#include <stddef.h>
const void *sano_model_image(size_t *partition_bytes);
