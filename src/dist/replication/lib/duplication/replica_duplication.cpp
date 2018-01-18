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

#include <boost/lexical_cast.hpp>
#include <dsn/dist/replication/duplication_common.h>

#include "replica_duplication.h"

namespace dsn {
namespace replication {

std::vector<duplication_confirm_entry>
replica::duplication_impl::get_duplication_confirms_to_update() const
{
    std::vector<duplication_confirm_entry> updates;
    for (auto &kv : _duplications) {
        const duplication_view &view = kv.second->view();
        if (view.last_decree != view.confirmed_decree) {
            duplication_confirm_entry entry;
            entry.dupid = view.id;
            entry.confirmed_decree = view.last_decree;
            updates.emplace_back(entry);
        }
    }
    return updates;
}

void replica::duplication_impl::sync_duplication(const duplication_entry &ent)
{
    dassert(_replica->status() == partition_status::PS_PRIMARY, "");

    dupid_t dupid = ent.dupid;
    duplication_status::type next_status = ent.status;

    auto it = _duplications.find(dupid);
    if (it == _duplications.end()) {
        auto dup = std::make_shared<mutation_duplicator>(ent, _replica);
        _duplications[ent.dupid] = std::move(dup);
    }

    update_duplication_status(dupid, next_status);
}

void replica::duplication_impl::update_duplication_status(dupid_t dupid,
                                                          duplication_status::type next_status)
{
    mutation_duplicator *dup = _duplications[dupid].get();

    if (dup->view().status == next_status) {
        return;
    }

    if (next_status == duplication_status::DS_START) {
        dup->start_duplication();
    } else if (next_status == duplication_status::DS_PAUSE) {
        dup->pause();
    } else {
        dassert("unexpected duplication status (%s)", duplication_status_to_string(next_status));
    }
}

void replica::duplication_impl::update_confirmed_points(
    const std::vector<duplication_confirm_entry> &confirmed_points)
{
    for (const duplication_confirm_entry &ce : confirmed_points) {
        auto it = _duplications.find(ce.dupid);
        if (it == _duplications.end()) {
            continue;
        }

        mutation_duplicator_s_ptr dup = it->second;
        dup->mutable_view()->confirmed_decree = ce.confirmed_decree;
    }
}

int64_t replica::duplication_impl::min_confirmed_decree() const
{
    int64_t min_decree = std::numeric_limits<int64_t>::max();
    if (_replica->status() == partition_status::PS_PRIMARY) {
        for (auto &kv : _duplications) {
            const duplication_view &view = kv.second.get()->view();
            if (view.status == duplication_status::type::DS_REMOVED) {
                continue;
            }
            min_decree = std::min(min_decree, view.confirmed_decree);
        }
    }
    dassert(min_decree >= 0, "invalid min_decree %" PRId64, min_decree);
    return min_decree;
}

} // namespace replication
} // namespace dsn
