/* Copyright (c) 2015 The Squash Authors
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *   Evan Nemerson <evan@nemerson.com>
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>

#include <squash/squash.h>

#include "density/src/density_api.h"

#define SQUASH_DENSITY_DEFAULT_ALGORITHM DENSITY_COMPRESSION_MODE_CHAMELEON_ALGORITHM

typedef enum {
  SQUASH_DENSITY_ACTION_INIT,
  SQUASH_DENSITY_ACTION_CONTINUE_OR_FINISH,
  SQUASH_DENSITY_ACTION_CONTINUE,
  SQUASH_DENSITY_ACTION_FINISH,
  SQUASH_DENSITY_ACTION_FINISHED
} SquashDensityAction;

static size_t
squash_density_get_max_compressed_size (SquashCodec* codec, size_t uncompressed_length) {
  return DENSITY_MINIMUM_OUT_BUFFER_SIZE + uncompressed_length;
}

typedef struct SquashDensityOptions_s {
  SquashOptions base_object;

  DENSITY_COMPRESSION_MODE mode;
  DENSITY_BLOCK_TYPE block_type;
} SquashDensityOptions;

typedef struct SquashDensityStream_s {
  SquashStream base_object;

  density_stream* stream;
  SquashDensityAction next;
  DENSITY_STREAM_STATE state;

  uint8_t buffer[DENSITY_MINIMUM_OUT_BUFFER_SIZE];
  size_t buffer_length;
  size_t buffer_pos;
  bool buffer_active;

  bool output_invalid;
} SquashDensityStream;

SquashStatus                 squash_plugin_init_codec      (SquashCodec* codec,
                                                            SquashCodecFuncs* funcs);

static void                  squash_density_options_init   (SquashDensityOptions* options,
                                                            SquashCodec* codec,
                                                            SquashDestroyNotify destroy_notify);
static SquashDensityOptions* squash_density_options_new     (SquashCodec* codec);
static void                  squash_density_options_destroy (void* options);
static void                  squash_density_options_free    (void* options);

static void                  squash_density_stream_init     (SquashDensityStream* stream,
                                                             SquashCodec* codec,
                                                             SquashStreamType stream_type,
                                                             SquashDensityOptions* options,
                                                             SquashDestroyNotify destroy_notify);
static SquashDensityStream*  squash_density_stream_new      (SquashCodec* codec, SquashStreamType stream_type, SquashDensityOptions* options);
static void                  squash_density_stream_destroy  (void* stream);
static void                  squash_density_stream_free     (void* stream);

static void
squash_density_options_init (SquashDensityOptions* options, SquashCodec* codec, SquashDestroyNotify destroy_notify) {
  assert (options != NULL);

  squash_options_init ((SquashOptions*) options, codec, destroy_notify);

  options->mode = DENSITY_COMPRESSION_MODE_CHAMELEON_ALGORITHM;
  options->block_type = DENSITY_BLOCK_TYPE_DEFAULT;
}

static SquashDensityOptions*
squash_density_options_new (SquashCodec* codec) {
  SquashDensityOptions* options;

  options = (SquashDensityOptions*) malloc (sizeof (SquashDensityOptions));
  squash_density_options_init (options, codec, squash_density_options_free);

  return options;
}

static void
squash_density_options_destroy (void* options) {
  squash_options_destroy ((SquashOptions*) options);
}

static void
squash_density_options_free (void* options) {
  squash_density_options_destroy ((SquashDensityOptions*) options);
  free (options);
}

static SquashOptions*
squash_density_create_options (SquashCodec* codec) {
  return (SquashOptions*) squash_density_options_new (codec);
}

static bool
string_to_bool (const char* value, bool* result) {
  if (strcasecmp (value, "true") == 0) {
    *result = true;
  } else if (strcasecmp (value, "false")) {
    *result = false;
  } else {
    return false;
  }
  return true;
}

static SquashStatus
squash_density_parse_option (SquashOptions* options, const char* key, const char* value) {
  SquashDensityOptions* opts = (SquashDensityOptions*) options;
  char* endptr = NULL;

  assert (opts != NULL);

  if (strcasecmp (key, "level") == 0) {
    const int level = (int) strtol (value, &endptr, 0);
    if (*endptr == '\0') {
      switch (level) {
        case 1:
          opts->mode = DENSITY_COMPRESSION_MODE_CHAMELEON_ALGORITHM;
          break;
        case 7:
          opts->mode = DENSITY_COMPRESSION_MODE_CHEETAH_ALGORITHM;
          break;
        case 9:
          opts->mode = DENSITY_COMPRESSION_MODE_LION_ALGORITHM;
          break;
        default:
          return SQUASH_BAD_VALUE;
      }
    } else {
      return SQUASH_BAD_VALUE;
    }
  } else if (strcasecmp (key, "checksum") == 0) {
    bool checksum;
    if (string_to_bool (value, &checksum)) {
      opts->block_type = checksum ? DENSITY_BLOCK_TYPE_WITH_HASHSUM_INTEGRITY_CHECK : DENSITY_BLOCK_TYPE_DEFAULT;
    } else {
      return SQUASH_BAD_VALUE;
    }
  } else {
    return SQUASH_BAD_PARAM;
  }

  return SQUASH_OK;
}

static SquashDensityStream*
squash_density_stream_new (SquashCodec* codec, SquashStreamType stream_type, SquashDensityOptions* options) {
  SquashDensityStream* stream;

  assert (codec != NULL);
  assert (stream_type == SQUASH_STREAM_COMPRESS || stream_type == SQUASH_STREAM_DECOMPRESS);

  stream = (SquashDensityStream*) malloc (sizeof (SquashDensityStream));
  squash_density_stream_init (stream, codec, stream_type, options, squash_density_stream_free);

  return stream;
}

static void
squash_density_stream_init (SquashDensityStream* stream,
                            SquashCodec* codec,
                            SquashStreamType stream_type,
                            SquashDensityOptions* options,
                            SquashDestroyNotify destroy_notify) {
  squash_stream_init ((SquashStream*) stream, codec, stream_type, (SquashOptions*) options, destroy_notify);

  stream->stream = density_stream_create (NULL, NULL);
  stream->next = SQUASH_DENSITY_ACTION_INIT;

  stream->buffer_length = 0;
  stream->buffer_pos = 0;
  stream->buffer_active = false;

  stream->output_invalid = false;
}

static void
squash_density_stream_destroy (void* stream) {
  SquashDensityStream* s = (SquashDensityStream*) stream;

  density_stream_destroy (s->stream);

  squash_stream_destroy (stream);
}

static void
squash_density_stream_free (void* stream) {
  squash_density_stream_destroy (stream);
  free (stream);
}

static SquashStream*
squash_density_create_stream (SquashCodec* codec, SquashStreamType stream_type, SquashOptions* options) {
  return (SquashStream*) squash_density_stream_new (codec, stream_type, (SquashDensityOptions*) options);
}

static bool
squash_density_flush_internal_buffer (SquashStream* stream) {
  SquashDensityStream* s = (SquashDensityStream*) stream;
  const size_t buffer_remaining = s->buffer_length - s->buffer_pos;
  const size_t cp_size = (stream->avail_out < buffer_remaining) ? stream->avail_out : buffer_remaining;

  if (cp_size > 0) {
    memcpy (stream->next_out, s->buffer + s->buffer_pos, cp_size);
    stream->next_out += cp_size;
    stream->avail_out -= cp_size;
    s->buffer_pos += cp_size;

    if (s->buffer_pos == s->buffer_length) {
      s->buffer_length = 0;
      s->buffer_pos = 0;
      return true;
    } else {
      return false;
    }
  }

  assert (false);
}

static size_t total_bytes_written = 0;

static SquashStatus
squash_density_process_stream (SquashStream* stream, SquashOperation operation) {
  SquashDensityStream* s = (SquashDensityStream*) stream;

  if (s->buffer_length > 0) {
    squash_density_flush_internal_buffer (stream);
    return SQUASH_PROCESSING;
  }

  if (s->next == SQUASH_DENSITY_ACTION_INIT) {
    if (stream->avail_out < DENSITY_MINIMUM_OUT_BUFFER_SIZE) {
      s->buffer_active = true;
      s->state = density_stream_prepare (s->stream, (uint8_t*) stream->next_in, stream->avail_in, s->buffer, DENSITY_MINIMUM_OUT_BUFFER_SIZE);
    } else {
      s->buffer_active = false;
      s->state = density_stream_prepare (s->stream, (uint8_t*) stream->next_in, stream->avail_in, stream->next_out, stream->avail_out);
    }
    if (s->state != DENSITY_STREAM_STATE_READY)
      return SQUASH_FAILED;
  }

  switch (s->state) {
    case DENSITY_STREAM_STATE_STALL_ON_INPUT:
      density_stream_update_input (s->stream, (uint8_t*) stream->next_in, stream->avail_in);
      s->state = DENSITY_STREAM_STATE_READY;
      break;
    case DENSITY_STREAM_STATE_STALL_ON_OUTPUT:
      {
        if (!s->output_invalid) {
          const size_t written = density_stream_output_available_for_use (s->stream);
          total_bytes_written += written;

          if (s->buffer_active) {
            s->buffer_length = written;
            s->buffer_pos = 0;

            const size_t cp_size = s->buffer_length < stream->avail_out ? s->buffer_length : stream->avail_out;
            memcpy (stream->next_out, s->buffer, cp_size);
            stream->next_out += cp_size;
            stream->avail_out -= cp_size;
            s->buffer_pos += cp_size;
            if (s->buffer_pos == s->buffer_length) {
              s->buffer_pos = 0;
              s->buffer_length = 0;
            }
          } else {
            assert (written <= stream->avail_out);
            stream->next_out += written;
            stream->avail_out -= written;
          }

          s->output_invalid = true;
          return SQUASH_PROCESSING;
        } else {
          if (stream->avail_out < DENSITY_MINIMUM_OUT_BUFFER_SIZE) {
            s->buffer_active = true;
            density_stream_update_output (s->stream, s->buffer, DENSITY_MINIMUM_OUT_BUFFER_SIZE);
          } else {
            s->buffer_active = false;
            density_stream_update_output (s->stream, stream->next_out, stream->avail_out);
          }
          s->output_invalid = false;
          s->state = DENSITY_STREAM_STATE_READY;
        }
      }
      break;
    case DENSITY_STREAM_STATE_READY:
      break;
    case DENSITY_STREAM_STATE_ERROR_OUTPUT_BUFFER_TOO_SMALL:
    case DENSITY_STREAM_STATE_ERROR_INVALID_INTERNAL_STATE:
    case DENSITY_STREAM_STATE_ERROR_INTEGRITY_CHECK_FAIL:
      return SQUASH_FAILED;
  }

  assert (s->output_invalid == false);

  while (s->state == DENSITY_STREAM_STATE_READY && s->next != SQUASH_DENSITY_ACTION_FINISHED) {
    switch (s->next) {
      case SQUASH_DENSITY_ACTION_INIT:
        if (stream->stream_type == SQUASH_STREAM_COMPRESS) {
          DENSITY_COMPRESSION_MODE compression_mode = SQUASH_DENSITY_DEFAULT_ALGORITHM;
          DENSITY_BLOCK_TYPE block_type = DENSITY_BLOCK_TYPE_DEFAULT;
          {
            SquashDensityOptions* opts = (SquashDensityOptions*) stream->options;
            if (opts != NULL) {
              compression_mode = opts->mode;
              block_type = opts->block_type;
            }
          }
          s->state = density_stream_compress_init (s->stream, compression_mode, block_type);
        } else {
          s->state = density_stream_decompress_init (s->stream, NULL);
        }
        assert (s->state == DENSITY_STREAM_STATE_READY);
        s->next = SQUASH_DENSITY_ACTION_CONTINUE;
        break;
      case SQUASH_DENSITY_ACTION_CONTINUE_OR_FINISH:
        s->next = (operation == SQUASH_OPERATION_PROCESS) ? SQUASH_DENSITY_ACTION_CONTINUE : SQUASH_DENSITY_ACTION_FINISH;
        break;
      case SQUASH_DENSITY_ACTION_CONTINUE:
        if (stream->stream_type == SQUASH_STREAM_COMPRESS) {
          s->state = density_stream_compress_continue (s->stream);
        } else {
          s->state = density_stream_decompress_continue (s->stream);
        }

        if (s->state == DENSITY_STREAM_STATE_STALL_ON_INPUT)
          s->next = SQUASH_DENSITY_ACTION_CONTINUE_OR_FINISH;

        break;
      case SQUASH_DENSITY_ACTION_FINISH:
        if (stream->stream_type == SQUASH_STREAM_COMPRESS) {
          s->state = density_stream_compress_finish (s->stream);
        } else {
          s->state = density_stream_decompress_finish (s->stream);
        }
        if (s->state == DENSITY_STREAM_STATE_READY) {
          s->state = DENSITY_STREAM_STATE_STALL_ON_OUTPUT;
          s->output_invalid = false;
          s->next = SQUASH_DENSITY_ACTION_FINISHED;
        }
        break;
      case SQUASH_DENSITY_ACTION_FINISHED:
      default:
        assert (0);
    }
  }

  if (s->state == DENSITY_STREAM_STATE_STALL_ON_INPUT) {
    stream->next_in += stream->avail_in;
    stream->avail_in = 0;
  } else if (s->state == DENSITY_STREAM_STATE_STALL_ON_OUTPUT) {
    {
      if (!s->output_invalid) {
        const size_t written = density_stream_output_available_for_use (s->stream);
        total_bytes_written += written;

        if (s->buffer_active) {
          s->buffer_length = written;
          s->buffer_pos = 0;

          const size_t cp_size = s->buffer_length < stream->avail_out ? s->buffer_length : stream->avail_out;
          memcpy (stream->next_out, s->buffer, cp_size);
          stream->next_out += cp_size;
          stream->avail_out -= cp_size;
          s->buffer_pos += cp_size;
          if (s->buffer_pos == s->buffer_length) {
            s->buffer_pos = 0;
            s->buffer_length = 0;
          }
        } else {
          assert (written <= stream->avail_out);
          stream->next_out += written;
          stream->avail_out -= written;
        }

        s->output_invalid = true;
        return SQUASH_PROCESSING;
      } else {
        assert (0);
      }
    }
  }

  if (operation == SQUASH_OPERATION_FINISH)
    total_bytes_written = 0;

  if (stream->avail_in == 0) {
    return SQUASH_OK;
  } else {
    return SQUASH_PROCESSING;
  }
}

SquashStatus
squash_plugin_init_codec (SquashCodec* codec, SquashCodecFuncs* funcs) {
  const char* name = squash_codec_get_name (codec);

  if (strcmp ("density", name) == 0) {
    funcs->create_options = squash_density_create_options;
    funcs->parse_option = squash_density_parse_option;
    funcs->create_stream = squash_density_create_stream;
    funcs->process_stream = squash_density_process_stream;
    funcs->get_max_compressed_size = squash_density_get_max_compressed_size;
  } else {
    return SQUASH_UNABLE_TO_LOAD;
  }

  return SQUASH_OK;
}
