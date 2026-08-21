/**
 * @file aesd-circular-buffer.c
 * @brief Functions and data related to a circular buffer implementation
 *
 * @author Dan Walkes
 * @date 2020-03-01
 * @copyright Copyright (c) 2020
 *
 */

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#endif

#include "aesd-circular-buffer.h"


struct aesd_buffer_entry *aesd_circular_buffer_find_entry_offset_for_fpos(struct aesd_circular_buffer *buffer,
            size_t char_offset, size_t *entry_offset_byte_rtn )
{
    if (buffer == NULL || entry_offset_byte_rtn == NULL) {
        return NULL;
    }

    uint8_t index = buffer->out_offs;
    size_t cumulative_chars = 0;
    uint8_t count = 0;

    // Loop through the valid entries in the circular buffer
    while (count < AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED) {
        // If buffer is empty and we've caught up to in_offs, break
        if (!buffer->full && (index == buffer->in_offs) && (count == 0)) {
            break;
        }

        size_t entry_size = buffer->entry[index].size;

        // Check if the requested char_offset falls within the current entry
        if (char_offset < (cumulative_chars + entry_size)) {
            *entry_offset_byte_rtn = char_offset - cumulative_chars;
            return &buffer->entry[index];
        }

        cumulative_chars += entry_size;
        index = (index + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
        count++;

        if (!buffer->full && (index == buffer->in_offs)) {
            break;
        }
    }

    return NULL;
}

void aesd_circular_buffer_add_entry(struct aesd_circular_buffer *buffer, const struct aesd_buffer_entry *add_entry)
{
    if (buffer == NULL || add_entry == NULL) {
        return;
    }

    // Store the entry at the current in_offs location
    buffer->entry[buffer->in_offs] = *add_entry;

    // If buffer was already full, advance out_offs as well (overwriting oldest)
    if (buffer->full) {
        buffer->out_offs = (buffer->out_offs + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }

    // Advance in_offs
    buffer->in_offs = (buffer->in_offs + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

    // Check if buffer is now full
    if (buffer->in_offs == buffer->out_offs) {
        buffer->full = true;
    }
}

void aesd_circular_buffer_init(struct aesd_circular_buffer *buffer)
{
    memset(buffer, 0, sizeof(struct aesd_circular_buffer));
}