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

#include <dsn/service_api_cpp.h>
#include <dsn/dist/replication/replication_types.h>
#include <dsn/dist/replication/meta_service_app.h>

#include "dist/replication/meta_server/server_state.h"
#include "dist/replication/meta_server/meta_service.h"

#include <gtest/gtest.h>

class spin_counter
{
private:
    std::atomic_int _counter;

public:
    spin_counter() { _counter.store(0); }
    void wait()
    {
        while (_counter.load() != 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    void block() { ++_counter; }
    void notify() { --_counter; }
};

struct reply_context
{
    dsn_message_t response;
    spin_counter e;
};

inline dsn_message_t create_corresponding_receive(dsn_message_t request_msg)
{
    return dsn_msg_copy(request_msg, true, true);
}

// fake_receiver_meta_service overrides `reply_message` of meta_service
//
class fake_receiver_meta_service : public dsn::replication::meta_service
{
public:
    fake_receiver_meta_service() : meta_service() {}
    virtual ~fake_receiver_meta_service() {}
    virtual void reply_message(dsn_message_t request, dsn_message_t response) override
    {
        uint64_t ptr;
        dsn::unmarshall(request, ptr);
        reply_context *ctx = reinterpret_cast<reply_context *>(ptr);
        ctx->response = create_corresponding_receive(response);
        dsn_msg_add_ref(ctx->response);

        // release the response
        dsn_msg_add_ref(response);
        dsn_msg_release_ref(response);

        ctx->e.notify();
    }
};

// release the dsn_message who's reference is 0
inline void destroy_message(dsn_message_t msg)
{
    dsn_msg_add_ref(msg);
    dsn_msg_release_ref(msg);
}

class meta_service_test_app : public dsn::service_app
{
public:
    meta_service_test_app(dsn_gpid pid) : service_app(pid) {}

public:
    virtual dsn::error_code start(int, char **argv) override;
    virtual dsn::error_code stop(bool /*cleanup*/) { return dsn::ERR_OK; }
    void state_sync_test();
    void data_definition_op_test();
    void update_configuration_test();
    void balancer_validator();
    void balance_config_file();
    void apply_balancer_test();
    void cannot_run_balancer_test();
    void construct_apps_test();

    void simple_lb_cure_test();
    void simple_lb_balanced_cure();
    void simple_lb_from_proposal_test();
    void simple_lb_collect_replica();
    void simple_lb_construct_replica();
    void json_compacity();

    void policy_context_test();
    void backup_service_test();

    // test for bug found
    void adjust_dropped_size();

    void call_update_configuration(
        dsn::replication::meta_service *svc,
        std::shared_ptr<dsn::replication::configuration_update_request> &request);
    void call_config_sync(
        dsn::replication::meta_service *svc,
        std::shared_ptr<dsn::replication::configuration_query_by_node_request> &request);

private:
    typedef std::function<bool(const dsn::replication::app_mapper &)> state_validator;
    bool
    wait_state(dsn::replication::server_state *ss, const state_validator &validator, int time = -1);
};

template <typename TRequest, typename RequestHandler>
std::shared_ptr<reply_context>
fake_rpc_call(dsn_task_code_t rpc_code,
              dsn_task_code_t lpc_code,
              RequestHandler *handle_class,
              void (RequestHandler::*handle)(dsn_message_t request),
              const TRequest &data,
              int hash = 0,
              std::chrono::milliseconds delay = std::chrono::milliseconds(0))
{
    dsn_message_t msg = dsn_msg_create_request(rpc_code);
    dsn::marshall(msg, data);

    std::shared_ptr<reply_context> result = std::make_shared<reply_context>();
    result->e.block();
    uint64_t ptr = reinterpret_cast<uint64_t>(result.get());
    dsn::marshall(msg, ptr);

    dsn_message_t received = create_corresponding_receive(msg);
    dsn_msg_add_ref(received);
    dsn::tasking::enqueue(
        lpc_code, nullptr, std::bind(handle, handle_class, received), hash, delay);

    // release the sending message
    destroy_message(msg);

    return result;
}

#define fake_create_app(state, request_data)                                                       \
    fake_rpc_call(                                                                                 \
        RPC_CM_CREATE_APP, LPC_META_STATE_NORMAL, state, &server_state::create_app, request_data)

#define fake_drop_app(state, request_data)                                                         \
    fake_rpc_call(                                                                                 \
        RPC_CM_DROP_APP, LPC_META_STATE_NORMAL, state, &server_state::drop_app, request_data)

#define fake_recall_app(state, request_data)                                                       \
    fake_rpc_call(                                                                                 \
        RPC_CM_RECALL_APP, LPC_META_STATE_NORMAL, state, &server_state::recall_app, request_data)

#define fake_wait_rpc(context, response_data)                                                      \
    do {                                                                                           \
        context->e.wait();                                                                         \
        ::dsn::unmarshall(context->response, response_data);                                       \
        dsn_msg_release_ref(context->response);                                                    \
    } while (0)
