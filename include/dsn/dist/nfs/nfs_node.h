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
 *     network file system component base interface
 *
 * Revision history:
 *     Mar., 2015, @imzhenyu (Zhenyu Guo), first version
 *     xxxx-xx-xx, author, fix bug about xxx
 */

#pragma once

#include <dsn/service_api_c.h>
#include <string>
#include <dsn/utility/utils.h>
#include <dsn/tool-api/task.h>
#include <dsn/tool-api/async_calls.h>

namespace dsn {

/*!
@addtogroup tool-api-providers
@{
*/

struct remote_copy_request
{
    ::dsn::rpc_address source;
    std::string source_dir;
    std::vector<std::string> files;
    std::string dest_dir;
    bool overwrite;
    bool high_priority;
};

struct remote_copy_response
{
};

DSN_API extern void marshall(::dsn::binary_writer &writer, const remote_copy_request &val);

DSN_API extern void unmarshall(::dsn::binary_reader &reader, /*out*/ remote_copy_request &val);

class service_node;
class task_worker_pool;
class task_queue;

class nfs_node
{
public:
    static nfs_node *create_new();

public:
    nfs_node() {}

    virtual ~nfs_node() {}

    virtual ::dsn::error_code start() = 0;

    virtual error_code stop() = 0;

    virtual void call(std::shared_ptr<remote_copy_request> rci, const aio_task_ptr &callback) = 0;

    aio_task_ptr copy_remote_files(::dsn::rpc_address remote,
                                   const std::string &source_dir,
                                   const std::vector<std::string> &files, // empty for all
                                   const std::string &dest_dir,
                                   bool overwrite,
                                   bool high_priority,
                                   dsn::task_code callback_code,
                                   task_tracker *tracker,
                                   aio_handler &&callback,
                                   int hash = 0)
    {
        aio_task_ptr tsk = file::create_aio_task(callback_code, tracker, std::move(callback), hash);

        std::shared_ptr<::dsn::remote_copy_request> rci(new ::dsn::remote_copy_request());
        rci->source = remote;
        rci->source_dir = source_dir;
        rci->files.clear();
        rci->dest_dir = dest_dir;
        rci->overwrite = overwrite;
        rci->high_priority = high_priority;

        if (!files.empty()) {
            for (const std::string &file : files) {
                dinfo("copy remote file %s from %s", file.c_str(), rci->source.to_string());
                rci->files.push_back(file);
            }
        }
        call(rci, tsk);
        return tsk;
    }

    aio_task_ptr copy_remote_directory(::dsn::rpc_address remote,
                                       const std::string &source_dir,
                                       const std::string &dest_dir,
                                       bool overwrite,
                                       bool high_priority,
                                       dsn::task_code callback_code,
                                       task_tracker *tracker,
                                       aio_handler &&callback,
                                       int hash = 0)
    {
        return copy_remote_files(remote,
                                 source_dir,
                                 {},
                                 dest_dir,
                                 overwrite,
                                 high_priority,
                                 callback_code,
                                 tracker,
                                 std::move(callback),
                                 hash);
    }
};

/*@}*/
}