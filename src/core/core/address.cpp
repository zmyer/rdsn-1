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

#ifdef _WIN32

#define _WINSOCK_DEPRECATED_NO_WARNINGS 1

#include <Winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#pragma comment(lib, "ws2_32.lib")

#else
#include <sys/socket.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#if defined(__FreeBSD__)
#include <netinet/in.h>
#endif

#endif

#include <dsn/utility/ports.h>
#include <dsn/utility/fixed_size_buffer_pool.h>
#include <dsn/service_api_c.h>
#include <dsn/cpp/address.h>
#include <dsn/tool-api/task.h>
#include "group_address.h"
#include "uri_address.h"

namespace dsn {
const rpc_address rpc_group_address::_invalid;
}

#ifdef _WIN32
static void net_init()
{
    static std::once_flag flag;
    static bool flag_inited = false;
    if (!flag_inited) {
        std::call_once(flag, [&]() {
            WSADATA wsaData;
            WSAStartup(MAKEWORD(2, 2), &wsaData);
            flag_inited = true;
        });
    }
}
#endif

// name to ip etc.
DSN_API uint32_t dsn_ipv4_from_host(const char *name)
{
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    if ((addr.sin_addr.s_addr = inet_addr(name)) == (unsigned int)(-1)) {
        hostent *hp = ::gethostbyname(name);
        int err =
#ifdef _WIN32
            (int)::WSAGetLastError()
#else
            h_errno
#endif
            ;

        if (hp == nullptr) {
            derror("gethostbyname failed, name = %s, err = %d.", name, err);
            return 0;
        } else {
            memcpy((void *)&(addr.sin_addr.s_addr), (const void *)hp->h_addr, (size_t)hp->h_length);
        }
    }

    // converts from network byte order to host byte order
    return (uint32_t)ntohl(addr.sin_addr.s_addr);
}

// if network_interface is "", then return the first
// site-local ipv4 address: 10.*.*.*, 172.16.*.*, 192.168.*.*
DSN_API uint32_t dsn_ipv4_local(const char *network_interface)
{
    uint32_t ret = 0;

    static auto is_site_local = [&](uint32_t ip_net) {
        const uint8_t *addr = reinterpret_cast<const uint8_t*>(&ip_net);
        return addr[0] == 10 || (addr[0] == 172 && addr[1] == 16) ||
               (addr[0] == 192 && addr[1] == 168);
    };

    struct ifaddrs *ifa = nullptr;
    if (getifaddrs(&ifa) == 0) {
        struct ifaddrs *i = ifa;
        while (i != nullptr) {
            if (i->ifa_name != nullptr && i->ifa_addr != nullptr &&
                i->ifa_addr->sa_family == AF_INET) {
                uint32_t ip_val = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;

                if (strcmp(i->ifa_name, network_interface) == 0 ||
                    (network_interface[0] == '\0' && is_site_local(ip_val))) {
                    ret = (uint32_t)ntohl(ip_val);
                    break;
                }
            }
            i = i->ifa_next;
        }

        if (i == nullptr) {
            derror("get local ip from network interfaces failed, network_interface = %s",
                   network_interface);
        }

        if (ifa != nullptr) {
            // remember to free it
            freeifaddrs(ifa);
        }
    }

    return ret;
}

static __thread fixed_size_buffer_pool<8, 256> bf;
DSN_API const char *dsn_address_to_string(dsn_address_t addr)
{
    char *p = bf.next();
    auto sz = bf.get_chunk_size();
    struct in_addr net_addr;
#ifdef _WIN32
    char *ip_str;
#else
    int ip_len;
#endif

    switch (addr.u.v4.type) {
    case HOST_TYPE_IPV4:
        net_addr.s_addr = htonl((uint32_t)addr.u.v4.ip);
#ifdef _WIN32
        ip_str = inet_ntoa(net_addr);
        snprintf_p(p, sz, "%s:%hu", ip_str, (uint16_t)addr.u.v4.port);
#else
        inet_ntop(AF_INET, &net_addr, p, sz);
        ip_len = strlen(p);
        snprintf_p(p + ip_len, sz - ip_len, ":%hu", (uint16_t)addr.u.v4.port);
#endif
        break;
    case HOST_TYPE_URI:
        p = (char *)(uintptr_t)addr.u.uri.uri;
        break;
    case HOST_TYPE_GROUP:
        p = (char *)(((dsn::rpc_group_address *)(uintptr_t)(addr.u.group.group))->name());
        break;
    default:
        p = (char *)"invalid address";
        break;
    }

    return (const char *)p;
}

DSN_API dsn_address_t dsn_address_build(const char *host, uint16_t port)
{
    dsn::rpc_address addr(host, port);
    return addr.c_addr();
}

DSN_API dsn_address_t dsn_address_build_ipv4(uint32_t ipv4, uint16_t port)
{
    dsn::rpc_address addr(ipv4, port);
    return addr.c_addr();
}

DSN_API dsn_address_t dsn_address_build_group(dsn_group_t g)
{
    dsn::rpc_address addr;
    addr.assign_group(g);
    return addr.c_addr();
}

DSN_API dsn_address_t dsn_address_build_uri(dsn_uri_t uri)
{
    dsn::rpc_address addr;
    addr.assign_uri(uri);
    return addr.c_addr();
}

DSN_API dsn_group_t dsn_group_build(const char *name) // must be paired with release later
{
    return new ::dsn::rpc_group_address(name);
}

DSN_API dsn_group_t dsn_group_clone(dsn_group_t g) // must be paired with release later
{
    auto grp = (::dsn::rpc_group_address *)(g);
    return new ::dsn::rpc_group_address(*grp);
}

DSN_API int dsn_group_count(dsn_group_t g)
{
    auto grp = (::dsn::rpc_group_address *)(g);
    return grp->count();
}

DSN_API bool dsn_group_add(dsn_group_t g, dsn_address_t ep)
{
    auto grp = (::dsn::rpc_group_address *)(g);
    ::dsn::rpc_address addr(ep);
    return grp->add(addr);
}

DSN_API void dsn_group_set_leader(dsn_group_t g, dsn_address_t ep)
{
    auto grp = (::dsn::rpc_group_address *)(g);
    ::dsn::rpc_address addr(ep);
    grp->set_leader(addr);
}

DSN_API dsn_address_t dsn_group_get_leader(dsn_group_t g)
{
    auto grp = (::dsn::rpc_group_address *)(g);
    return grp->leader().c_addr();
}

DSN_API bool dsn_group_is_leader(dsn_group_t g, dsn_address_t ep)
{
    auto grp = (::dsn::rpc_group_address *)(g);
    return grp->leader() == ep;
}

DSN_API bool dsn_group_is_update_leader_automatically(dsn_group_t g)
{
    auto grp = (::dsn::rpc_group_address *)(g);
    return grp->is_update_leader_automatically();
}

DSN_API void dsn_group_set_update_leader_automatically(dsn_group_t g, bool v)
{
    auto grp = (::dsn::rpc_group_address *)(g);
    grp->set_update_leader_automatically(v);
}

DSN_API dsn_address_t dsn_group_next(dsn_group_t g, dsn_address_t ep)
{
    auto grp = (::dsn::rpc_group_address *)(g);
    ::dsn::rpc_address addr(ep);
    return grp->next(addr).c_addr();
}

DSN_API dsn_address_t dsn_group_forward_leader(dsn_group_t g)
{
    auto grp = (::dsn::rpc_group_address *)(g);
    grp->leader_forward();
    return grp->leader().c_addr();
}

DSN_API bool dsn_group_remove(dsn_group_t g, dsn_address_t ep)
{
    auto grp = (::dsn::rpc_group_address *)(g);
    ::dsn::rpc_address addr(ep);
    return grp->remove(addr);
}

DSN_API void dsn_group_destroy(dsn_group_t g)
{
    auto grp = (::dsn::rpc_group_address *)(g);
    delete grp;
}

DSN_API dsn_uri_t dsn_uri_build(const char *url) // must be paired with destroy later
{
    return (dsn_uri_t) new ::dsn::rpc_uri_address(url);
}

DSN_API dsn_uri_t dsn_uri_clone(dsn_uri_t uri) // must be paired with destroy later
{
    auto u = (::dsn::rpc_uri_address *)(uri);
    return (dsn_uri_t) new ::dsn::rpc_uri_address(*u);
}

DSN_API void dsn_uri_destroy(dsn_uri_t uri) { delete (::dsn::rpc_uri_address *)(uri); }
