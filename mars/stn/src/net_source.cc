// Tencent is pleased to support the open source community by making Mars available.
// Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.

// Licensed under the MIT License (the "License"); you may not use this file except in 
// compliance with the License. You may obtain a copy of the License at
// http://opensource.org/licenses/MIT

// Unless required by applicable law or agreed to in writing, software distributed under the License is
// distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
// either express or implied. See the License for the specific language governing permissions and
// limitations under the License.


/* netsource.cc
 *
 *  Created on: 2012-7-17
 *      Author: yerungui
 */

#include "net_source.h"

#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <set>

#include "boost/bind.hpp"

#include "mars/comm/marcotoolkit.h"
#include "mars/comm/socket/unix_socket.h"
#include "mars/comm/xlogger/xlogger.h"
#include "mars/comm/time_utils.h"
#include "mars/comm/strutil.h"
#include "mars/comm/thread/lock.h"
#include "mars/comm/thread/thread.h"
#include "mars/comm/platform_comm.h"
#include "mars/stn/stn.h"
#include "mars/stn/dns_profile.h"
#include "mars/stn/config.h"

using namespace mars::stn;

static const char *const kItemDelimiter = ":";

static const int kNumMakeCount = 5;

//mmnet ipport settings
static std::vector<std::string> sg_longlink_hosts;
static std::vector<uint16_t> sg_longlink_ports;
static std::string sg_longlink_debugip;

static int sg_shortlink_port;
static std::string sg_shortlink_debugip;
static std::map<std::string, std::vector<std::string> > sg_host_backupips_mapping;
static std::vector<uint16_t> sg_lowpriority_longlink_ports;

static std::map<std::string, std::string> sg_host_debugip_mapping;

static Mutex sg_ip_mutex;

NetSource::DnsUtil::DnsUtil() :
        mNewDns(OnNewDns) {
}

NetSource::DnsUtil::~DnsUtil() {}

void NetSource::DnsUtil::Cancel(const std::string &host) {
    if (host.empty()) {
        mNewDns.Cancel();
        mDns.Cancel();
    } else {
        mNewDns.Cancel(host);
        mDns.Cancel(host);
    }
}

NetSource::NetSource(ActiveLogic &activeLogic)
        : mActiveLogic(activeLogic) {
    xinfo_function();
}

NetSource::~NetSource() {
    xinfo_function();
}

/**
 *	host ip port setting from java
 */
void NetSource::SetLongLink(const std::vector<std::string> &hosts, const std::vector<uint16_t> &ports,
                            const std::string &debugIp) {
    ScopedLock lock(sg_ip_mutex);

    xgroup2_define(addr_print);
    xinfo2(TSF"task set longlink server addr, ") >> addr_print;
    for (std::vector<std::string>::const_iterator host_iter = hosts.begin(); host_iter != hosts.end(); ++host_iter) {
        xinfo2(TSF"host:%_ ", *host_iter) >> addr_print;
    }
    for (std::vector<uint16_t>::const_iterator port_iter = ports.begin(); port_iter != ports.end(); ++port_iter) {
        xinfo2(TSF"port:%_ ", *port_iter) >> addr_print;
    }
    xinfo2(TSF"debugip:%_", debugIp) >> addr_print;

    sg_longlink_debugip = debugIp;
    if (!hosts.empty()) {
        sg_longlink_hosts = hosts;
    } else {
        xerror2(TSF"host list should not be empty");
    }
    sg_longlink_ports = ports;
}

void NetSource::SetShortlink(const uint16_t port, const std::string &debugIp) {
    ScopedLock lock(sg_ip_mutex);

    xinfo2(TSF"task set shortlink server addr, port:%_, debugip:%_", port, debugIp);

    sg_shortlink_port = port;
    sg_shortlink_debugip = debugIp;
}

void NetSource::SetBackupIPs(const std::string &host, const std::vector<std::string> &ipList) {
    ScopedLock lock(sg_ip_mutex);

    xgroup2_define(addr_print);
    xinfo2(TSF"task set backup server addr, host:%_", host) >> addr_print;
    for (std::vector<std::string>::const_iterator ip_iter = ipList.begin(); ip_iter != ipList.end(); ++ip_iter) {
        xinfo2(TSF"ip:%_ ", *ip_iter) >> addr_print;
    }

    sg_host_backupips_mapping[host] = ipList;
}

void NetSource::SetDebugIP(const std::string &host, const std::string &ip) {
    ScopedLock lock(sg_ip_mutex);

    xinfo2(TSF"task set debugip:%_ for host:%_", ip, host);

    if (ip.empty() && sg_host_debugip_mapping.find(host) != sg_host_debugip_mapping.end()) {
        sg_host_debugip_mapping.erase(host);
    } else {
        sg_host_debugip_mapping[host] = ip;
    }
}

const std::string &NetSource::GetLongLinkDebugIP() {
    ScopedLock lock(sg_ip_mutex);

    return sg_longlink_debugip;
}

const std::string &NetSource::GetShortLinkDebugIP() {
    ScopedLock lock(sg_ip_mutex);
    return sg_shortlink_debugip;
}

void NetSource::SetLowPriorityLonglinkPorts(const std::vector<uint16_t> &lowPriorityLonglinkPorts) {
    sg_lowpriority_longlink_ports = lowPriorityLonglinkPorts;
}

/**
 *
 * longlink functions
 *
 */
const std::vector<std::string> &NetSource::GetLongLinkHosts() {
    ScopedLock lock(sg_ip_mutex);
    return sg_longlink_hosts;
}

void NetSource::GetLonglinkPorts(std::vector<uint16_t> &ports) {
    ScopedLock lock(sg_ip_mutex);
    ports = sg_longlink_ports;
}

bool NetSource::GetLongLinkItems(std::vector<IPPortItem> &ipPortList, DnsUtil &dnsUtil) {
    xinfo_function();
    ScopedLock lock(sg_ip_mutex);

    if (__GetLonglinkDebugIPPort(ipPortList)) {
        return true;
    }

    lock.unlock();

    std::vector<std::string> longlink_hosts = NetSource::GetLongLinkHosts();
    if (longlink_hosts.empty()) {
        xerror2("longlink host empty.");
        return false;
    }

    __GetIPPortItems(ipPortList, longlink_hosts, dnsUtil, true);

    return !ipPortList.empty();
}

bool NetSource::__GetLonglinkDebugIPPort(std::vector<IPPortItem> &ipPortList) {

    for (std::vector<std::string>::iterator ip_iter = sg_longlink_hosts.begin();
         ip_iter != sg_longlink_hosts.end(); ++ip_iter) {
        if (sg_host_debugip_mapping.find(*ip_iter) != sg_host_debugip_mapping.end()) {
            for (std::vector<uint16_t>::iterator iter = sg_longlink_ports.begin();
                 iter != sg_longlink_ports.end(); ++iter) {
                IPPortItem item;
                item.ip = (*sg_host_debugip_mapping.find(*ip_iter)).second;
                item.host = *ip_iter;
                item.port = *iter;
                item.sourceType = kIPSourceDebug;
                ipPortList.push_back(item);
            }
            return true;
        }
    }

    if (!sg_longlink_debugip.empty()) {
        for (std::vector<uint16_t>::iterator iter = sg_longlink_ports.begin();
             iter != sg_longlink_ports.end(); ++iter) {
            IPPortItem item;
            item.ip = sg_longlink_debugip;
            item.host = sg_longlink_hosts.front();
            item.port = *iter;
            item.sourceType = kIPSourceDebug;
            ipPortList.push_back(item);
        }
        return true;
    }

    return false;
}

void NetSource::GetBackupIPs(std::string const& host, std::vector<std::string> &ipList) {
    ScopedLock lock(sg_ip_mutex);
    if (sg_host_backupips_mapping.find(host) != sg_host_backupips_mapping.end()) {
        ipList = (*sg_host_backupips_mapping.find(host)).second;
    }
}

void NetSource::ReportLongIP(bool isSuccess, const std::string &ip, uint16_t port) {
    xinfo2_if(!isSuccess, TSF"isSuccess=%0, ip=%1, port=%2", isSuccess, ip, port);

    if (ip.empty() || 0 == port) return;

    if (kNoNet == getNetInfo()) return;

    mIpPortStrategy.Update(ip, port, isSuccess);
}

void NetSource::RemoveLongBanIP(const std::string &ip) {
    mIpPortStrategy.RemoveBannedList(ip);
}

/**
 *
 * shortlink functions
 *
 */
uint16_t NetSource::GetShortLinkPort() {
    ScopedLock lock(sg_ip_mutex);
    return sg_shortlink_port;
}

bool NetSource::__HasShortLinkDebugIP(const std::vector<std::string> &hostList) {
    if (!sg_shortlink_debugip.empty()) {
        return true;
    }

    for (std::vector<std::string>::const_iterator host = hostList.begin(); host != hostList.end(); ++host) {
        if (sg_host_debugip_mapping.find(*host) != sg_host_debugip_mapping.end()) {
            return true;
        }
    }

    return false;
}

bool NetSource::GetShortLinkItems(const std::vector<std::string> &hostList, std::vector<IPPortItem> &ipPortList,
                                  DnsUtil &dnsUtil) {

    ScopedLock lock(sg_ip_mutex);

    if (__GetShortlinkDebugIPPort(hostList, ipPortList)) {
        return true;
    }

    lock.unlock();

    if (hostList.empty()) return false;
    __GetIPPortItems(ipPortList, hostList, dnsUtil, false);

    return !ipPortList.empty();
}

bool NetSource::__GetShortlinkDebugIPPort(const std::vector<std::string> &hostList,
                                          std::vector<IPPortItem> &ipPortList) {

    for (std::vector<std::string>::const_iterator host = hostList.begin(); host != hostList.end(); ++host) {
        if (sg_host_debugip_mapping.find(*host) != sg_host_debugip_mapping.end()) {
            IPPortItem item;
            item.ip = (*sg_host_debugip_mapping.find(*host)).second;
            item.host = *host;
            item.port = sg_shortlink_port;
            item.sourceType = kIPSourceDebug;
            ipPortList.push_back(item);
            return true;
        }
    }

    if (!sg_shortlink_debugip.empty()) {
        IPPortItem item;
        item.ip = sg_shortlink_debugip;
        item.host = hostList.front();
        item.port = sg_shortlink_port;
        item.sourceType = kIPSourceDebug;
        ipPortList.push_back(item);
    }

    return !ipPortList.empty();
}

void NetSource::__GetIPPortItems(std::vector<IPPortItem> &ipPortList, const std::vector<std::string> &hostList,
                                 DnsUtil &dnsUtil, bool isLongLink) {
    if (mActiveLogic.IsActive()) {
        unsigned int merge_type_count = 0;
        unsigned int makelist_count = kNumMakeCount;

        for (std::vector<std::string>::const_iterator iter = hostList.begin(); iter != hostList.end(); ++iter) {
            if (merge_type_count == 1 && ipPortList.size() == kNumMakeCount) makelist_count = kNumMakeCount + 1;

            if (0 < __MakeIPPorts(ipPortList, *iter, makelist_count, dnsUtil, false, isLongLink))
                merge_type_count++;
        }

        for (std::vector<std::string>::const_iterator iter = hostList.begin(); iter != hostList.end(); ++iter) {
            if (merge_type_count == 1 && ipPortList.size() == kNumMakeCount) makelist_count = kNumMakeCount + 1;

            if (0 < __MakeIPPorts(ipPortList, *iter, makelist_count, dnsUtil, true, isLongLink))
                merge_type_count++;
        }
    } else {
        size_t host_count = hostList.size();
        size_t ret = (kNumMakeCount - 1) / host_count;
        size_t ret2 = (kNumMakeCount - 1) % host_count;
        size_t i = 0;
        size_t count = 0;

        for (std::vector<std::string>::const_iterator host_iter = hostList.begin();
             host_iter != hostList.end() && count < kNumMakeCount - 1; ++host_iter) {
            count += i < ret2 ? ret + 1 : ret;
            __MakeIPPorts(ipPortList, *host_iter, count, dnsUtil, false, isLongLink);
            i++;
        }

        for (std::vector<std::string>::const_iterator host_iter = hostList.begin();
             host_iter != hostList.end() && count < kNumMakeCount; ++host_iter) {
            __MakeIPPorts(ipPortList, *host_iter, kNumMakeCount, dnsUtil, true, isLongLink);
        }
    }
}

size_t NetSource::__MakeIPPorts(std::vector<IPPortItem> &_ip_items, const std::string &host, size_t _count,
                                DnsUtil &dnsUtil, bool isBackup, bool isLongLink) {

    IPSourceType ist = kIPSourceNULL;
    std::vector<std::string> iplist;
    std::vector<uint16_t> ports;

    if (!isBackup) {
        DnsProfile dns_profile;
        dns_profile.host = host;

        bool ret = dnsUtil.GetNewDNS().GetHostByName(host, iplist);

        dns_profile.endTime = gettickcount();
        if (!ret) dns_profile.OnFailed();
        ReportDnsProfile(dns_profile);

        xgroup2_define(dnsxlog);
        xdebug2(TSF"link host:%_, new dns ret:%_, size:%_ ", host, ret, iplist.size()) >> dnsxlog;

        if (iplist.empty()) {
            dns_profile.Reset();
            dns_profile.dnsType = kType_Dns;

            ist = kIPSourceDNS;
            ret = dnsUtil.GetDNS().GetHostByName(host, iplist);

            dns_profile.endTime = gettickcount();
            if (!ret) dns_profile.OnFailed();
            ReportDnsProfile(dns_profile);

            xdebug2(TSF"dns ret:%_, size:%_,", ret, iplist.size()) >> dnsxlog;
        } else {
            ist = kIPSourceNewDns;
        }

        if (isLongLink) {
            NetSource::GetLonglinkPorts(ports);
        } else {
            ports.push_back(NetSource::GetShortLinkPort());
        }
    } else {
        NetSource::GetBackupIPs(host, iplist);
        xdebug2(TSF"link host:%_, backup ips size:%_", host, iplist.size());

        if (iplist.empty() && dnsUtil.GetDNS().GetHostByName(host, iplist)) {
            ScopedLock lock(sg_ip_mutex);
            sg_host_backupips_mapping[host] = iplist;
        }

        if (isLongLink) {
            if (sg_lowpriority_longlink_ports.empty()) {
                NetSource::GetLonglinkPorts(ports);
            } else {
                ports = sg_lowpriority_longlink_ports;
            }
        } else {
            ports.push_back(NetSource::GetShortLinkPort());
        }
        ist = kIPSourceBackup;
        if (!iplist.empty() && !ports.empty()) {
            std::set<std::string> setIps;
            for (auto it = _ip_items.begin(); it != _ip_items.end(); ++it) {
                setIps.insert(it->ip);
            }
            size_t ports_cnt = ports.size();
            size_t require_cnt = _count - _ip_items.size();
            if (require_cnt < ports_cnt) require_cnt += ports_cnt;
            size_t cur_cnt = iplist.size() * ports_cnt;
            size_t i = 0;
            while (cur_cnt > require_cnt && i < iplist.size()) {
                if (setIps.find(iplist[i]) != setIps.end()) {
                    iplist.erase(iplist.begin() + i);
                    cur_cnt -= ports_cnt;
                } else {
                    i++;
                }
            }
        }
    }

    if (iplist.empty()) return 0;

    size_t len = _ip_items.size();

    std::vector<IPPortItem> temp_items;
    for (std::vector<std::string>::iterator ip_iter = iplist.begin(); ip_iter != iplist.end(); ++ip_iter) {
        for (std::vector<uint16_t>::iterator port_iter = ports.begin(); port_iter != ports.end(); ++port_iter) {
            IPPortItem item;
            item.ip = *ip_iter;
            item.sourceType = ist;
            item.host = host;
            item.port = *port_iter;
            temp_items.push_back(item);
        }
    }

    if (!isBackup) {
        mIpPortStrategy.SortandFilter(temp_items, (int) (_count - len));
        _ip_items.insert(_ip_items.end(), temp_items.begin(), temp_items.end());
    } else {
        _ip_items.insert(_ip_items.end(), temp_items.begin(), temp_items.end());
        srand((unsigned) gettickcount());
        std::random_shuffle(_ip_items.begin() + len, _ip_items.end());
        _ip_items.resize(std::min(_ip_items.size(), (size_t) _count));
    }

    return _ip_items.size();
}

void NetSource::ReportShortIP(bool isSuccess, const std::string &ip, const std::string &host, uint16_t port) {
    xinfo2_if(!isSuccess, TSF"isSuccess=%0, ip=%1, port=%2 host=%3", isSuccess, ip, port, host);

    if (ip.empty()) return;

    if (kNoNet == getNetInfo()) return;

    mIpPortStrategy.Update(ip, port, isSuccess);
}

void NetSource::ClearCache() {
    xinfo_function();
    mIpPortStrategy.InitHistory2BannedList(true);
}

std::string NetSource::DumpTable(const std::vector<IPPortItem> &ipPortList) {
    XMessage stream;

    for (unsigned int i = 0; i < ipPortList.size(); ++i) {
        stream << ipPortList[i].ip << kItemDelimiter << ipPortList[i].port << kItemDelimiter
               << ipPortList[i].host
               << kItemDelimiter << IPSourceTypeString[ipPortList[i].sourceType];

        if (i != ipPortList.size() - 1) {
            stream << "|";
        }
    }

    return stream.String();
}

bool NetSource::GetLongLinkSpeedTestIPs(std::vector<IPPortItem> &ipPortList) {
    xverbose_function();

    return true;
}

void NetSource::ReportLongLinkSpeedTestResult(std::vector<IPPortItem> &ipPortList) {
}

void NetSource::AddServerBan(const std::string &ip) {
    mIpPortStrategy.AddServerBan(ip);
}
