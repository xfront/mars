// Tencent is pleased to support the open source community by making Mars available.
// Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.

// Licensed under the MIT License (the "License"); you may not use this file except in 
// compliance with the License. You may obtain a copy of the License at
// http://opensource.org/licenses/MIT

// Unless required by applicable law or agreed to in writing, software distributed under the License is
// distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
// either express or implied. See the License for the specific language governing permissions and
// limitations under the License.


/*
 * netsource.h
 *
 *  Created on: 2012-7-17
 *      Author: yerungui
 */

#ifndef STN_SRC_NETSOURCE_H_
#define STN_SRC_NETSOURCE_H_

#include <vector>
#include <string>
#include <map>

#include "boost/function.hpp"

#include "mars/baseevent/active_logic.h"
#include "mars/comm/thread/mutex.h"
#include "mars/comm/dns/dns.h"
#include "mars/stn/config.h"

#include "simple_ipport_sort.h"

class ActiveLogic;

namespace mars {
namespace stn {

struct IPPortItem;

class NetSource {
public:
    class DnsUtil {
    public:
        DnsUtil();

        ~DnsUtil();

    public:
        DNS &GetNewDNS() { return mNewDns; }

        DNS &GetDNS() { return mDns; }

        void Cancel(const std::string &host = "");

    private:
        DnsUtil(const DnsUtil &);

        DnsUtil &operator=(const DnsUtil &);

    private:
        DNS mNewDns;
        DNS mDns;
    };

public:
    //set longlink host and ports
    static void SetLongLink(const std::vector<std::string> &hosts, const std::vector<uint16_t> &ports,
                            const std::string &debugIp);

    //set shortlink port
    static void SetShortlink(const uint16_t port, const std::string &debugIp);

    //set backup ips for host, these ips would be used when host dns failed
    static void SetBackupIPs(const std::string &host, const std::vector<std::string> &ipList);

    //set debug ip
    static void SetDebugIP(const std::string &host, const std::string &ip);

    static const std::string &GetLongLinkDebugIP();

    static const std::string &GetShortLinkDebugIP();

    static void SetLowPriorityLonglinkPorts(const std::vector<uint16_t> &lowPriorityLonglinkPorts);

    static void GetLonglinkPorts(std::vector<uint16_t> &ports);

    static const std::vector<std::string> &GetLongLinkHosts();

    static uint16_t GetShortLinkPort();

    static void GetBackupIPs(std::string const& host, std::vector<std::string> &ipList);

    static std::string DumpTable(const std::vector<IPPortItem> &ipPortList);

public:
    NetSource(ActiveLogic &activeLogic);

    ~NetSource();

public:
    // for long link
    bool GetLongLinkItems(std::vector<IPPortItem> &ipPortList, DnsUtil &dnsUtil);

    // for short link
    bool GetShortLinkItems(const std::vector<std::string> &hostList, std::vector<IPPortItem> &ipPortList,
                           DnsUtil &dnsUtil);

    void AddServerBan(const std::string &ip);

    void ClearCache();

    void ReportLongIP(bool isSuccess, const std::string &ip, uint16_t port);

    void ReportShortIP(bool isSuccess, const std::string &ip, const std::string &host, uint16_t port);

    void RemoveLongBanIP(const std::string &ip);

    bool GetLongLinkSpeedTestIPs(std::vector<IPPortItem> &ipPortList);

    void ReportLongLinkSpeedTestResult(std::vector<IPPortItem> &ipPortList);

private:

    bool __HasShortLinkDebugIP(const std::vector<std::string> &hostList);

    bool __GetLonglinkDebugIPPort(std::vector<IPPortItem> &ipPortList);

    bool __GetShortlinkDebugIPPort(const std::vector<std::string> &hostList, std::vector<IPPortItem> &ipPortList);

    void __GetIPPortItems(std::vector<IPPortItem> &ipPortList, const std::vector<std::string> &hostList,
                          DnsUtil &dnsUtil, bool isLongLink);

    size_t
    __MakeIPPorts(std::vector<IPPortItem> &_ip_items, const std::string &host, size_t _count, DnsUtil &dnsUtil,
                  bool isBackup, bool isLongLink);

private:
    ActiveLogic &mActiveLogic;
    SimpleIPPortSort mIpPortStrategy;
};

}
}


#endif // STN_SRC_NETSOURCE_H_
