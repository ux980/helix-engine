#ifndef HELIX_CODEC_H
#define HELIX_CODEC_H

#include "helix/status.h"

helix_status_t helix_encode_file(const char *input_path, const char *output_path);
helix_status_t helix_decode_file(const char *input_path, const char *output_path);

#endif