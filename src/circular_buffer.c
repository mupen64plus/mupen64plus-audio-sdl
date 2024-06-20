/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-ui-console - circular_buffer.c                            *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2015 Bobby Smiles                                       *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "circular_buffer.h"


int init_cbuff(struct circular_buffer* cbuff, size_t capacity)
{
    void* data = malloc(capacity);

    if (data == NULL)
    {
        return -1;
    }

    cbuff->data = data;
    cbuff->size = capacity;
    cbuff->head = 0;

    return 0;
}

void release_cbuff(struct circular_buffer* cbuff)
{
    free(cbuff->data);
    memset(cbuff, 0, sizeof(*cbuff));
}

void grow_cbuff(struct circular_buffer* cbuff, size_t new_size)
{
    if (new_size >= cbuff->size) {

        cbuff->data = realloc(cbuff->data, new_size);

        size_t delta = new_size - cbuff->size;
        size_t delta_max = (cbuff->head >= cbuff->tail) ? cbuff->tail : cbuff->head;

        if (delta_max <= delta) {
            memcpy((unsigned char*)cbuff->data + cbuff->size, cbuff->data, delta_max);
            memset((unsigned char*)cbuff->data + cbuff->size + delta_max, 0, delta - delta_max);
        }
        else {
            memcpy((unsigned char*)cbuff->data + cbuff->size, cbuff->data, delta);
            memmove(cbuff->data, (const unsigned char*)cbuff->data + delta, delta_max - delta);
        }

        cbuff->size = new_size;
    }
}


void* cbuff_head(const struct circular_buffer* cbuff, size_t* available, size_t* extra)
{
    assert(cbuff->head <= cbuff->size);
    assert(cbuff->tail <= cbuff->size);

    if (cbuff->head >= cbuff->tail) {
        *available = cbuff->size - cbuff->head;
        *extra = cbuff->tail;
    }
    else {
        *available = cbuff->tail - cbuff->head;
        *extra = 0;
    }

    return (unsigned char*)cbuff->data + cbuff->head;
}


void* cbuff_tail(const struct circular_buffer* cbuff, size_t* available, size_t* extra)
{
    assert(cbuff->head <= cbuff->size);
    assert(cbuff->tail <= cbuff->size);

    if (cbuff->head >= cbuff->tail) {
        *available = cbuff->head - cbuff->tail;
        *extra = 0;
    }
    else {
        *available = cbuff->size - cbuff->tail;
        *extra = cbuff->head;
    }

    return (unsigned char*)cbuff->data + cbuff->tail;
}


void produce_cbuff_data(struct circular_buffer* cbuff, size_t amount)
{
    //assert((cbuff->head + amount) % cbuff->size <= cbuff->tail);

    cbuff->head += amount;
    if (cbuff->head > cbuff->size) {
        cbuff->head -= cbuff->size;
    }
}


void consume_cbuff_data(struct circular_buffer* cbuff, size_t amount)
{
    //assert((cbuff->tail + amount) % cbuff->size <= cbuff->tail);

    cbuff->tail += amount;
    if (cbuff->tail > cbuff->size) {
        cbuff->tail -= cbuff->size;
    }
}

