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
 *   simple_ipport_sort.h
 *   network
 *
 *   Created by liucan on 14-6-16.
 *   Copyright (c) 2014 Tencent. All rights reserved.
*/

#ifndef STN_SRC_SIMPLE_IPPORT_SORT_H_
#define STN_SRC_SIMPLE_IPPORT_SORT_H_

#include <string>
#include <vector>
#include <map>

#include "mars/comm/thread/lock.h"
#include "mars/comm/tinyxml2.h"
#include "mars/comm/tickcount.h"
#include "mars/stn/stn.h"

namespace mars {
namespace stn {

struct BanItem;

class SimpleIPPortSort {
public:
    SimpleIPPortSort();

    ~SimpleIPPortSort();

    void InitHistory2BannedList(bool saveXml);

    void RemoveBannedList(const std::string &ip);

    void Update(const std::string &ip, uint16_t port, bool isSuccess);

    void SortandFilter(std::vector<IPPortItem> &items, int needCount) const;

    void AddServerBan(const std::string &ip);

private:
    void __LoadXml();

    void __SaveXml();

    void __RemoveTimeoutXml();

    std::vector<BanItem>::iterator __FindBannedIter(const std::string &ip, uint16_t port) const;

    bool __IsBanned(std::vector<BanItem>::iterator it) const;

    bool __IsBanned(const std::string &ip, uint16_t port) const;

    void __UpdateBanList(bool isSuccess, const std::string &ip, uint16_t port);

    bool __CanUpdate(const std::string &ip, uint16_t port, bool isSuccess) const;

    void __FilterbyBanned(std::vector<IPPortItem> &items) const;

    void __SortbyBanned(std::vector<IPPortItem> &items) const;

    bool __IsServerBan(const std::string &ip) const;

private:
    SimpleIPPortSort(const SimpleIPPortSort &);

    SimpleIPPortSort &operator=(const SimpleIPPortSort &);

private:
    std::string mHostPath;
    tinyxml2::XMLDocument mRecordsXml;

    mutable Mutex mMutex;
    mutable std::vector<BanItem> mBanFailList;
    mutable std::map<std::string, uint64_t> mServerBans;
};

}
}
#endif // STN_SRC_SIMPLE_IPPORT_SORT_H_
