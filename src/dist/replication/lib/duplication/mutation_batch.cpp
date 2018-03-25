/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 *
 * -=- Robust Distributed System Nucleus (rDSN) -=-
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <dsn/dist/fmt_logging.h>

#include "mutation_batch.h"

namespace dsn {
namespace replication {

error_s mutation_batch::add(mutation_ptr mu)
{
    error_code ec = _mutation_buffer->prepare(mu, partition_status::PS_INACTIVE);
    if (ec != ERR_OK) {
        return FMT_ERR(ERR_INVALID_DATA,
                       "mutation_batch: failed to add mutation [err: {}, mutation decree: "
                       "{}, ballot: {}]",
                       ec,
                       mu->get_decree(),
                       mu->get_ballot());
    }

    dassert(_mutation_buffer->count() < PREPARE_LIST_NUM_ENTRIES,
            "impossible! prepare_list has reached the capacity");
    return error_s::ok();
}

mutation_batch::mutation_batch(on_committed_mutation_callback &&cb)
    : _on_committed_cb(std::move(cb))
{
    _mutation_buffer = dsn::make_unique<prepare_list>(
        0, PREPARE_LIST_NUM_ENTRIES, [](mutation_ptr &mu) { _loaded_listener->notify(mu); });
}

} // namespace replication
} // namespace dsn