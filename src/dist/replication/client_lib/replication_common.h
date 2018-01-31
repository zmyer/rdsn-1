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

/*
 * Description:
 *     What is this file about?
 *
 * Revision history:
 *     xxxx-xx-xx, author, first version
 *     xxxx-xx-xx, author, fix bug about xxx
 */

#pragma once

#include <string>
#include <dsn/tool-api/task.h>
#include <dsn/dist/replication.h>
#include "dist/replication/client_lib/replication_ds.h"

namespace dsn {
namespace replication {

typedef std::unordered_map<::dsn::rpc_address, partition_status::type> node_statuses;
typedef std::unordered_map<::dsn::rpc_address, dsn::task_ptr> node_tasks;

class replication_options
{
public:
    std::vector<::dsn::rpc_address> meta_servers;

    std::string app_name;
    std::string app_dir;
    std::string slog_dir;
    std::vector<std::string> data_dirs;
    std::vector<std::string> data_dir_tags;

    bool deny_client_on_start;
    bool verbose_client_log_on_start;
    bool verbose_commit_log_on_start;
    bool delay_for_fd_timeout_on_start;
    bool empty_write_disabled;

    int32_t prepare_timeout_ms_for_secondaries;
    int32_t prepare_timeout_ms_for_potential_secondaries;
    int32_t prepare_decree_gap_for_debug_logging;

    bool batch_write_disabled;
    int32_t staleness_for_commit;
    int32_t max_mutation_count_in_prepare_list;
    int32_t mutation_2pc_min_replica_count;

    bool group_check_disabled;
    int32_t group_check_interval_ms;

    bool checkpoint_disabled;
    int32_t checkpoint_interval_seconds;
    int64_t checkpoint_min_decree_gap;
    int32_t checkpoint_max_interval_hours;

    bool gc_disabled;
    int32_t gc_interval_ms;
    int32_t gc_memory_replica_interval_ms;
    int32_t gc_disk_error_replica_interval_seconds;
    int32_t gc_disk_garbage_replica_interval_seconds;

    bool disk_stat_disabled;
    int32_t disk_stat_interval_seconds;

    bool fd_disabled;
    int32_t fd_check_interval_seconds;
    int32_t fd_beacon_interval_seconds;
    int32_t fd_lease_seconds;
    int32_t fd_grace_seconds;

    int32_t log_private_file_size_mb;
    int32_t log_private_batch_buffer_kb;
    int32_t log_private_batch_buffer_count;
    int32_t log_private_batch_buffer_flush_interval_ms;
    int32_t log_private_reserve_max_size_mb;
    int32_t log_private_reserve_max_time_seconds;

    int32_t log_shared_file_size_mb;
    int32_t log_shared_file_count_limit;
    int32_t log_shared_batch_buffer_kb;
    bool log_shared_force_flush;

    bool config_sync_disabled;
    int32_t config_sync_interval_ms;

    int32_t lb_interval_ms;

    int32_t learn_app_max_concurrent_count;

public:
    replication_options();
    void initialize();
    ~replication_options();

private:
    void sanity_check();
};
}
} // namespace
