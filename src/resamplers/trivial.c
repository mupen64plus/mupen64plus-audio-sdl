/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-sdl-audio - trivial.c                                     *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2017 Bobby Smiles                                       *
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

#include "resamplers/resamplers.h"

#include <stddef.h>
#include <stdint.h>

static void* trivial_init_from_id(const char* resampler_id)
{
    /* nothing to do */
    return NULL;
}

static void trivial_release(void* resampler)
{
    /* nothing to do */
}

static void trivial_resample(void* resampler,
                             const void* src, size_t src_size, unsigned int src_freq, size_t* consumed,
                             void* dst, size_t dst_size, unsigned int dst_freq, size_t* produced)
{
    enum { BYTES_PER_SAMPLE = 4 };
    size_t i;
    size_t j = 0;

    if (dst_freq >= src_freq) {
        const int dpos = 2*src_freq;
        const int dneg = dpos - 2*dst_freq;

        int criteria = dpos - dst_freq;

        for (i = 0; i < dst_size/BYTES_PER_SAMPLE; ++i) {

            ((uint32_t*)dst)[i] = ((const uint32_t*)src)[j];

            if (criteria >= 0) {
                ++j;
                criteria += dneg;
            }
            else {
                criteria += dpos;
            }
        }
    }
    else {
        /* Can happen when speed_factor > 1 */
        for (i = 0; i < dst_size/BYTES_PER_SAMPLE; ++i) {

            j = i * src_freq / dst_freq;
            ((uint32_t*)dst)[i] = ((const uint32_t*)src)[j];
        }
    }

    *consumed = j * 4;
    *produced = i * 4;
}


const struct resampler_interface g_trivial_iresampler = {
    "trivial",
    trivial_init_from_id,
    trivial_release,
    trivial_resample
};
