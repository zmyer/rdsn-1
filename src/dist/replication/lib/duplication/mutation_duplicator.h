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

#pragma once

#include <dsn/cpp/clientlet.h>
#include <dsn/dist/replication/fmt_logging.h>
#include <dsn/dist/replication/replication_types.h>
#include <dsn/dist/replication/replication_app_base.h>
#include <dsn/dist/replication/duplication_common.h>

#include "dist/replication/lib/replica.h"
#include "dist/replication/lib/prepare_list.h"
#include "dist/replication/lib/replica.h"
#include "dist/replication/lib/mutation_log.h"
#include "dist/replication/lib/mutation_log_utils.h"
#include "dist/replication/lib/duplication/fmt_utils.h"

#ifdef __TITLE__
#undef __TITLE__
#endif
#define __TITLE__ "mutation_duplicator"

namespace dsn {
namespace replication {

class duplication_view
{
public:
    // the maximum decree that's been persisted in meta server
    decree confirmed_decree;

    // the maximum decree that's been duplicated to remote.
    decree last_decree;

    duplication_status::type status;

    duplication_view() : confirmed_decree(0), last_decree(0), status(duplication_status::DS_INIT) {}

    duplication_view(const duplication_view &rhs)
        : confirmed_decree(rhs.confirmed_decree), last_decree(rhs.last_decree), status(rhs.status)
    {
    }

    duplication_view &set_last_decree(decree d)
    {
        last_decree = d;
        return *this;
    }

    duplication_view &set_confirmed_decree(decree d)
    {
        confirmed_decree = d;
        return *this;
    }
};

typedef std::unique_ptr<duplication_view> duplication_view_u_ptr;

// Each mutation_duplicator is responsible for one duplication.
class mutation_duplicator
{
    struct mutation_batch
    {
        static const int64_t prepare_list_num_entries = 200;

        explicit mutation_batch(int64_t init_decree, mutation_duplicator *duplicator)
            : _mutation_buffer(init_decree,
                               prepare_list_num_entries,
                               [](mutation_ptr &) {
                                   // do nothing when log commit
                               }),
              _last_decree(0),
              _duplicator(duplicator)
        {
        }

        error_s add(int log_length, mutation_ptr mu)
        {
            error_code ec = _mutation_buffer.prepare(mu, partition_status::PS_INACTIVE);
            if (ec != ERR_OK) {
                return FMT_ERR(
                    ERR_INVALID_DATA,
                    "failed to add mutation into prepare list [err: {}, mutation decree: {}, "
                    "ballot: {}]",
                    ec,
                    mu->get_decree(),
                    mu->get_ballot());
            }

            while (true) {
                mutation_ptr popped = _mutation_buffer.pop_min();
                if (popped == nullptr) {
                    break;
                }
                if (popped->get_decree() <= _mutation_buffer.last_committed_decree()) {
                    for (const mutation_update &update : mu->data.updates) {
                        dsn_message_t req = dsn_msg_create_received_request(
                            update.code,
                            (dsn_msg_serialize_format)update.serialization_type,
                            (void *)update.data.data(),
                            update.data.length());

                        _mutations.push_back(req);
                    }

                    // update last_decree
                    _last_decree = std::max(_last_decree, popped->get_decree());
                } else {
                    _mutation_buffer.prepare(popped, partition_status::PS_INACTIVE);
                    break;
                }
            }

            dassert_f(_mutation_buffer.count() < prepare_list_num_entries,
                      "impossible! prepare_list has reached the capacity [gpid: {}]",
                      _duplicator->_replica->get_gpid());
            return error_s::ok();
        }

        // After calling this function this `batch` is guaranteed to be empty.
        std::vector<dsn_message_t> move_to_vec_message() { return std::move(_mutations); }

        decree last_decree() const { return _last_decree; }

        bool empty() const { return _mutations.empty(); }

        friend class mutation_duplicator_test;

        prepare_list _mutation_buffer;
        std::vector<dsn_message_t> _mutations;
        decree _last_decree;
        mutation_duplicator *_duplicator;
    };

    using mutation_batch_u_ptr = std::unique_ptr<mutation_batch>;

public:
    mutation_duplicator(const duplication_entry &ent, replica *r)
        : _id(ent.dupid),
          _remote_cluster_address(ent.remote_address),
          _replica(r),
          _private_log(r->private_log()),
          _paused(true),
          _mutation_batch(make_unique<mutation_batch>(0, this)),
          _current_end_offset(0),
          _read_from_start(true),
          _view(make_unique<duplication_view>())
    {
        if (_replica->last_durable_decree() > ent.confirmed_decree) {
            dfatal_f("the logs haven't yet duplicated were accidentally truncated [replica: "
                     "(gpid: {}), last_durable_decree: {}, confirmed_decree: {}]",
                     _replica->get_gpid(),
                     _replica->last_durable_decree(),
                     ent.confirmed_decree);
        }

        _view->confirmed_decree = ent.confirmed_decree;
        _view->status = ent.status;

        // start from the last confirmed decree
        _view->last_decree = ent.confirmed_decree;
    }

    ~mutation_duplicator()
    {
        pause();
        dsn_task_tracker_wait_all(tracker()->tracker());
    }

    // Thread-safe
    void start()
    {
        ddebug_f("starting duplication [dupid: {}, gpid: {}, remote: {}]",
                 id(),
                 _replica->get_gpid(),
                 remote_cluster_address());

        _paused = false;
        enqueue_start_duplication();
    }

    // Thread-safe
    void pause()
    {
        ddebug_f("pausing duplication [dupid: {}, gpid: {}, remote: {}]",
                 id(),
                 _replica->get_gpid(),
                 remote_cluster_address());

        _paused = true;
    }

    dupid_t id() const { return _id; }

    const std::string &remote_cluster_address() const { return _remote_cluster_address; }

    // Thread-safe
    duplication_view view() const
    {
        ::dsn::service::zauto_read_lock l(_lock);
        return *_view;
    }

    // Thread-safe
    void update_state(const duplication_view &new_state)
    {
        ::dsn::service::zauto_write_lock l(_lock);
        _view->confirmed_decree = std::max(_view->confirmed_decree, new_state.confirmed_decree);
        _view->last_decree = std::max(_view->last_decree, new_state.last_decree);
        _view->status = new_state.status;
    }

    // Returns: the task tracker.
    clientlet *tracker() { return &_tracker; }

    // Await for all running tasks to complete.
    void wait_all() { dsn_task_tracker_wait_all(tracker()->tracker()); }

    /// ================================= Implementation =================================== ///

    void
    enqueue_start_duplication(std::chrono::milliseconds delay_ms = std::chrono::milliseconds(0))
    {
        tasking::enqueue(LPC_DUPLICATE_MUTATIONS,
                         tracker(),
                         std::bind(&mutation_duplicator::start_duplication, this),
                         gpid_to_thread_hash(_replica->get_gpid()),
                         delay_ms);
    }

    void start_duplication()
    {
        if (_paused) {
            return;
        }

        std::vector<std::string> log_files = log_utils::list_all_files_or_die(_private_log->dir());

        // start duplication from the first log file.
        _current_log_file = find_log_file_with_min_index(log_files);
        if (_current_log_file == nullptr) {
            // wait 10 seconds if there's no private log.
            enqueue_start_duplication(std::chrono::milliseconds(1000 * 10));
            return;
        }

        enqueue_do_duplication();
    }

    // RETURNS: null if there's no valid log file.
    static log_file_ptr find_log_file_with_min_index(const std::vector<std::string> &log_files)
    {
        std::map<int, log_file_ptr> log_file_map = log_utils::open_log_file_map(log_files);
        if (log_file_map.empty()) {
            return nullptr;
        }
        return log_file_map.begin()->second;
    }

    void enqueue_do_duplication(std::chrono::milliseconds delay_ms = std::chrono::milliseconds(0))
    {
        tasking::enqueue(LPC_DUPLICATE_MUTATIONS,
                         tracker(),
                         std::bind(&mutation_duplicator::do_duplicate, this),
                         gpid_to_thread_hash(_replica->get_gpid()),
                         delay_ms);
    }

    void do_duplicate()
    {
        if (_paused) {
            return;
        }

        ddebug_f("duplicating mutations [last_decree: {}, file: {}]",
                 view().last_decree,
                 _current_log_file->path());

        if (_mutation_batch->empty()) {
            if (!have_more()) {
                // wait 10 seconds for next try if no mutation was added.
                enqueue_do_duplication(std::chrono::milliseconds(1000 * 10));
                return;
            }

            if (!load_mutations_from_log_file(_current_log_file)) {
                if (try_switch_to_next_log_file()) {
                    // read another log file.
                    enqueue_do_duplication();
                } else {
                    // wait 10 sec if there're mutations written but unreadable.
                    enqueue_do_duplication(std::chrono::milliseconds(1000 * 10));
                }
                return;
            }

            if (_paused) { // check again
                return;
            }
        }

        if (_mutation_batch->empty()) {
            // retry if the loaded block contains no mutation
            enqueue_do_duplication(std::chrono::milliseconds(1000));
        } else {
            start_shipping_mutation_batch();
        }
    }

    void start_shipping_mutation_batch()
    {
        std::vector<dsn_message_t> mutations = _mutation_batch->move_to_vec_message();
        enqueue_ship_mutations(mutations);
    }

    // Switches to the log file with index = current_log_index + 1.
    bool try_switch_to_next_log_file()
    {
        std::string new_path = fmt::format("{}/log.{}.{}",
                                           _private_log->dir(),
                                           _current_log_file->index() + 1,
                                           _current_end_offset);
        ddebug_f("try switching log file to: {}", new_path);

        bool result = false;
        if (utils::filesystem::file_exists(new_path)) {
            result = true;
        } else {
            // TODO(wutao1): edge case handling
        }

        if (result) {
            _current_log_file = log_utils::open_read_or_die(new_path);
            _read_from_start = true;

            ddebug_f("switched log file to: {}", new_path);
        }
        return result;
    }

    bool have_more() const { return _private_log->max_commit_on_disk() > _view->last_decree; }

    // RETURNS: false if the operation encountered error.
    bool load_mutations_from_log_file(log_file_ptr &log_file)
    {
        error_s err = mutation_log::replay_block(
            log_file,
            [this](int log_length, mutation_ptr &mu) -> bool {
                auto es = _mutation_batch->add(log_length, std::move(mu));
                if (!es.is_ok()) {
                    dfatal_f("invalid mutation was found. err: {}, gpid: {}",
                             es.description(),
                             _replica->get_gpid());
                }
                return true;
            },
            _read_from_start,
            _current_end_offset);

        if (!err.is_ok()) {
            if (err.code() != ERR_HANDLE_EOF) {
                dwarn_f("error occurred while loading mutation logs: [err: {}, file: {}]",
                        err,
                        log_file->path());
            }
            return false;
        } else {
            _read_from_start = false;
        }
        return true;
    }

    task_ptr
    enqueue_ship_mutations(std::vector<dsn_message_t> &mutations,
                           std::chrono::milliseconds delay_ms = std::chrono::milliseconds(0))
    {
        return tasking::enqueue(
            LPC_DUPLICATE_MUTATIONS,
            tracker(),
            std::bind(&mutation_duplicator::ship_mutations, this, std::move(mutations)),
            gpid_to_thread_hash(_replica->get_gpid()),
            delay_ms);
    }

    void ship_mutations(std::vector<dsn_message_t> &mutations)
    {
        if (_paused) {
            return;
        }

        ddebug_f("start shipping mutations [size: {}]", mutations.size());

        auto backlog_handler = _replica->get_app()->get_duplication_backlog_handler();
        error_s err = backlog_handler->duplicate(&mutations);
        if (!err.is_ok()) {
            derror_f("failed to ship mutations [gpid: {}]: {}", _replica->get_gpid(), err);

            // retries forever until all mutations are duplicated.
            enqueue_ship_mutations(mutations, std::chrono::milliseconds(1000));
        } else {
            duplication_view new_state = view().set_last_decree(_mutation_batch->last_decree());
            update_state(new_state);

            // delay 1 second to start next duplication job
            enqueue_do_duplication(std::chrono::milliseconds(1000));
        }
    }

private:
    friend class mutation_duplicator_test;

    const dupid_t _id;
    const std::string _remote_cluster_address;

    replica *_replica;
    mutation_log_ptr _private_log;

    std::atomic<bool> _paused;

    std::unique_ptr<mutation_batch> _mutation_batch;

    int64_t _current_end_offset;
    log_file_ptr _current_log_file;
    bool _read_from_start;

    // protect the access of _view.
    mutable ::dsn::service::zrwlock_nr _lock;
    duplication_view_u_ptr _view;

    clientlet _tracker;
};

typedef std::shared_ptr<mutation_duplicator> mutation_duplicator_s_ptr;

} // namespace replication
} // namespace dsn