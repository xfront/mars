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
 *   simple_ipport_sort.cc
 *   network
 *
 *   Created by liucan on 14-6-16.
 *   Copyright (c) 2014年 Tencent. All rights reserved.
*/

#include "simple_ipport_sort.h"

#include <unistd.h>
#include <math.h>
#include <deque>
#include <algorithm>

#include "boost/filesystem.hpp"
#include "boost/bind.hpp"
#include "boost/accumulators/numeric/functional.hpp"


#include "mars/comm/time_utils.h"
#include "mars/comm/xlogger/xlogger.h"
#include "mars/comm/platform_comm.h"

#include "mars/app/app.h"

#define IPPORT_RECORDS_FILENAME "/ipportrecords2.xml"

static const time_t kRecordTimeout = 60 * 60 * 24;
static const char *const kFolderName = "host";
// const static char* const RECORDS = "records";
static const char *const kRecord = "record";
static const char *const kItem = "item";
static const char *const kTime = "time";
static const char *const kNetInfo = "netinfo";
static const char *const kIP = "ip";
static const char *const kPort = "port";
//static const char* const kSuccess = "succ";
//static const char* const kTotal = "total";
static const char *const kHistoryResult = "historyresult";

static const unsigned int kBanTime = 6 * 60 * 1000;  // 6 min
static const unsigned int kMaxBanTime = 30 * 60 * 1000; // 30 min
static const unsigned int kServerBanTime = 30 * 60 * 1000; // 30 min
static const int kBanFailCount = 3;
static const int kSuccessUpdateInterval = 10 * 1000;
static const int kFailUpdateInterval = 10 * 1000;

#define SET_BIT(SET, RECORDS)  RECORDS = (((RECORDS)<<1) | (bool(SET)))

static inline
uint32_t CAL_BIT_COUNT(uint64_t RECORDS) {
    uint32_t COUNT = 0;
    while (RECORDS) {
        RECORDS = RECORDS & (RECORDS - 1);
        COUNT++;
    }
    return COUNT;
}


static inline
uint32_t CAL_LAST_CONTINUOUS_BIT_COUNT(uint64_t RECORDS) {
    uint32_t COUNT = 0;
    uint64_t TMP = RECORDS & 0x1;
    while (TMP) {
        COUNT++;
        RECORDS = RECORDS >> 1;
        TMP = RECORDS & 0x1;
    }
    return COUNT;
}

///////////////////////////////////////////////////////////////////////////////////////////
namespace mars {
namespace stn {
struct BanItem {
    std::string ip;
    uint16_t port;
    uint8_t records;
    tickcount_t last_fail_time;
    tickcount_t last_suc_time;

    BanItem() : port(0), records(0) {}
};
}
}

using namespace mars::stn;

SimpleIPPortSort::SimpleIPPortSort()
        : mHostPath(mars::app::GetAppFilePath() + "/" + kFolderName) {

    if (!boost::filesystem::exists(mHostPath)) {
        boost::filesystem::create_directory(mHostPath);
    }

    ScopedLock lock(mMutex);
    __LoadXml();
    lock.unlock();
    InitHistory2BannedList(false);
}

SimpleIPPortSort::~SimpleIPPortSort() {
    ScopedLock lock(mMutex);
    __SaveXml();
}

void SimpleIPPortSort::__SaveXml() {
    __RemoveTimeoutXml();
    mRecordsXml.SaveFile((mHostPath + IPPORT_RECORDS_FILENAME).c_str());
}

void SimpleIPPortSort::__LoadXml() {
    tinyxml2::XMLError error = mRecordsXml.LoadFile((mHostPath + IPPORT_RECORDS_FILENAME).c_str());
    if (tinyxml2::XML_SUCCESS != error) return;
    __RemoveTimeoutXml();
}

void SimpleIPPortSort::__RemoveTimeoutXml() {
    std::vector<tinyxml2::XMLElement *> remove_ele_ptr_list;

    for (tinyxml2::XMLElement *record = mRecordsXml.FirstChildElement(kRecord);
         NULL != record; record = record->NextSiblingElement(kRecord)) {
        const char *lasttime_chr = record->Attribute(kTime);

        if (lasttime_chr) {
            struct timeval now = {0};
            gettimeofday(&now, NULL);
            time_t lasttime = (time_t) strtoul(lasttime_chr, NULL, 10);

            if (now.tv_sec < lasttime || now.tv_sec - lasttime >= kRecordTimeout) {
                remove_ele_ptr_list.push_back(record);
            }
        } else {
            remove_ele_ptr_list.push_back(record);
        }
    }

    for (std::vector<tinyxml2::XMLElement *>::iterator iter = remove_ele_ptr_list.begin();
         iter != remove_ele_ptr_list.end(); ++iter) {
        mRecordsXml.DeleteChild(*iter);
    }
}

void SimpleIPPortSort::InitHistory2BannedList(bool saveXml) {
    ScopedLock lock(mMutex);
    if (saveXml) __SaveXml();

    mBanFailList.clear();

    std::string curr_netinfo;
    if (kNoNet == getCurrNetLabel(curr_netinfo)) return;

    const tinyxml2::XMLElement *record = NULL;

    for (record = mRecordsXml.FirstChildElement(kRecord);
         NULL != record; record = record->NextSiblingElement(kRecord)) {
        const char *netinfoChr = record->Attribute(kNetInfo);

        if (netinfoChr && (0 == strcmp(netinfoChr, curr_netinfo.c_str()))) {
            xwarn2(TSF"netinfoChr:%_, curr_netinfo.c_str():%_", netinfoChr, curr_netinfo.c_str());
            break;
        }
    }

    if (NULL == record) { return; }

    for (const tinyxml2::XMLElement *item = record->FirstChildElement(kItem);
         NULL != item; item = item->NextSiblingElement(kItem)) {
        const char *ip = item->Attribute(kIP);
        uint32_t port = item->UnsignedAttribute(kPort);
        uint64_t historyresult = (uint64_t) item->Int64Attribute(kHistoryResult);

        struct BanItem banitem;
        if (ip) banitem.ip = ip;
        banitem.port = port;
        banitem.records = 0;
        //8 in 1
        for (int i = 0; i < 8; ++i) {
            SET_BIT(historyresult & 0xFF, banitem.records);
            historyresult >>= 8;
        }
        mBanFailList.push_back(banitem);
    }
}

void SimpleIPPortSort::RemoveBannedList(const std::string &ip) {
    ScopedLock lock(mMutex);

    for (std::vector<BanItem>::iterator iter = mBanFailList.begin(); iter != mBanFailList.end();) {
        if (iter->ip == ip)
            iter = mBanFailList.erase(iter);
        else
            ++iter;
    }
}

void SimpleIPPortSort::Update(const std::string &ip, uint16_t port, bool isSuccess) {
    std::string curr_net_info;
    if (kNoNet == getCurrNetLabel(curr_net_info)) return;

    ScopedLock lock(mMutex);

    if (!__CanUpdate(ip, port, isSuccess)) return;

    __UpdateBanList(isSuccess, ip, port);

    tinyxml2::XMLElement *record = NULL;

    for (record = mRecordsXml.FirstChildElement(kRecord);
         NULL != record; record = record->NextSiblingElement(kRecord)) {
        const char *netinfo_chr = record->Attribute(kNetInfo);
        if (netinfo_chr && (0 == strcmp(netinfo_chr, curr_net_info.c_str()))) break;
    }

    if (NULL == record) {
        struct timeval timeval = {0};
        gettimeofday(&timeval, NULL);
        char timebuf[128] = {0};
        snprintf(timebuf, sizeof(timebuf), "%ld", timeval.tv_sec);

        record = mRecordsXml.NewElement(kRecord);
        record->SetAttribute(kNetInfo, curr_net_info.c_str());
        record->SetAttribute(kTime, timebuf);
        mRecordsXml.InsertEndChild(record);
    }

    tinyxml2::XMLElement *item = NULL;

    for (item = record->FirstChildElement(kItem);
         NULL != item; item = item->NextSiblingElement(kItem)) {
        const char *lip = item->Attribute(kIP);
        uint32_t lport = item->UnsignedAttribute(kPort);

        if (lip && (0 == strcmp(lip, ip.c_str())) && port == lport) break;
    }

    if (NULL == item) {
        item = mRecordsXml.NewElement(kItem);
        item->SetAttribute(kIP, ip.c_str());
        item->SetAttribute(kPort, port);
        record->InsertEndChild(item);
    }

    uint64_t history_result = item->Int64Attribute(kHistoryResult);
    SET_BIT(!isSuccess, history_result);
    item->SetAttribute(kHistoryResult, (int64_t) history_result);
}

std::vector<BanItem>::iterator SimpleIPPortSort::__FindBannedIter(const std::string &ip, unsigned short port) const {
    std::vector<BanItem>::iterator iter;

    for (iter = mBanFailList.begin(); iter != mBanFailList.end(); ++iter) {
        if (iter->ip == ip && iter->port == port) {
            return iter;
        }
    }

    return iter;
}

bool SimpleIPPortSort::__IsBanned(const std::string &ip, unsigned short port) const {
    return __IsBanned(__FindBannedIter(ip, port));
}

bool SimpleIPPortSort::__IsBanned(std::vector<BanItem>::iterator it) const {
    if (it == mBanFailList.end()) return false;

    bool baned = CAL_BIT_COUNT(it->records) >= kBanFailCount;
    if (!baned) return false;

    uint32_t last_continuous_cnt = CAL_LAST_CONTINUOUS_BIT_COUNT(it->records);
    int64_t ban_time = kBanTime;
    if (last_continuous_cnt > kBanFailCount) {
        ban_time += (last_continuous_cnt - kBanFailCount) * kBanTime;
        if (ban_time > kMaxBanTime) {
            ban_time = kMaxBanTime;
        }
        xinfo2(TSF"%_:%_ ban time:%_", it->ip, it->port, ban_time);
    }

    if (it->last_fail_time.gettickspan() < ban_time) {
        return true;
    }

    return false;
}

void SimpleIPPortSort::__UpdateBanList(bool isSuccess, const std::string &ip, unsigned short port) {
    for (std::vector<BanItem>::iterator iter = mBanFailList.begin(); iter != mBanFailList.end(); ++iter) {
        if (iter->ip == ip && iter->port == port) {
            SET_BIT(!isSuccess, iter->records);
            if (isSuccess)
                iter->last_suc_time.gettickcount();
            else
                iter->last_fail_time.gettickcount();
            return;
        }
    }

    BanItem item;
    item.ip = ip;
    item.port = port;
    SET_BIT(!isSuccess, item.records);

    if (isSuccess)
        item.last_suc_time.gettickcount();
    else
        item.last_fail_time.gettickcount();

    mBanFailList.push_back(item);
}

bool SimpleIPPortSort::__CanUpdate(const std::string &ip, uint16_t port, bool isSuccess) const {
    for (std::vector<BanItem>::iterator iter = mBanFailList.begin(); iter != mBanFailList.end(); ++iter) {
        if (iter->ip == ip && iter->port == port) {
            if (isSuccess) {
                return kSuccessUpdateInterval < iter->last_suc_time.gettickspan();
            } else {
                return kFailUpdateInterval < iter->last_fail_time.gettickspan();
            }
        }
    }

    return true;
}

void SimpleIPPortSort::__FilterbyBanned(std::vector<IPPortItem> &items) const {
    for (std::vector<IPPortItem>::iterator it = items.begin(); it != items.end();) {
        if (__IsBanned(it->ip, it->port) || __IsServerBan(it->ip)) {
            xwarn2(TSF"ip:%0, port:%1, is ban!!", it->ip, it->port);
            it = items.erase(it);
        } else {
            ++it;
        }
    }
}

bool SimpleIPPortSort::__IsServerBan(const std::string &ip) const {
    std::map<std::string, uint64_t>::iterator iter = mServerBans.find(ip);

    if (iter == mServerBans.end()) return false;

    uint64_t now = ::gettickcount();
    xassert2(now >= iter->second, TSF"%_:%_", now, iter->second);
    if (now - iter->second < kServerBanTime) {
        xwarn2(TSF"ip %0 is ban by server, haha!", ip.c_str());
        return true;
    }

    mServerBans.erase(iter);
    return false;
}

void SimpleIPPortSort::__SortbyBanned(std::vector<IPPortItem> &items) const {
    srand((unsigned int) gettickcount());
    //random_shuffle new and history
    std::random_shuffle(items.begin(), items.end());

    int cnt = items.size();
    for (int i = 1; i < cnt - 1; ++i) {
        if (items[i].ip == items[i - 1].ip) {
            bool find = false;
            for (int j = i + 1; j < cnt; ++j) {
                if (items[i - 1].ip != items[j].ip) {
                    IPPortItem tmp_item = items[i];
                    items[i] = items[j];
                    items[j] = tmp_item;
                    find = true;
                    break;
                }
            }
            if (!find)break;
        }
    }

    //separate new and history
    std::deque<IPPortItem> items_history(items.size());
    std::deque<IPPortItem> items_new(items.size());
    auto find_lambda = [&](const IPPortItem &_v) {
        for (std::vector<BanItem>::const_iterator it_banned = mBanFailList.begin();
             it_banned != mBanFailList.end(); ++it_banned) {
            if (it_banned->ip == _v.ip && it_banned->port == _v.port) {
                return true;
            }
        }
        return false;
    };

    items_history.erase(std::remove_copy_if(items.begin(), items.end(), items_history.begin(),
                                            !boost::bind<bool>(find_lambda, _1)), items_history.end());
    items_new.erase(std::remove_copy_if(items.begin(), items.end(), items_new.begin(), find_lambda), items_new.end());
    xassert2(items.size() == items_history.size() + items_new.size(), TSF"_item:%_, history:%_, new:%_", items.size(), items_history.size(), items_new.size());

    //sort history
    std::sort(items_history.begin(), items_history.end(),
              [&](const IPPortItem &_l, const IPPortItem &_r) {
                  auto find_lr_lambda = [](const BanItem &_v, const IPPortItem &_find) {
                      return _v.ip == _find.ip && _v.port == _find.port;
                  };

                  std::vector<BanItem>::const_iterator l = std::find_if(mBanFailList.begin(), mBanFailList.end(),
                                                                        boost::bind<bool>(find_lr_lambda, _1, _l));
                  std::vector<BanItem>::const_iterator r = std::find_if(mBanFailList.begin(), mBanFailList.end(),
                                                                        boost::bind<bool>(find_lr_lambda, _1, _r));

                  xassert2(l != mBanFailList.end());
                  xassert2(r != mBanFailList.end());

                  if (l == mBanFailList.end() || r == mBanFailList.end())
                      return false;

                  if (CAL_BIT_COUNT(l->records) != CAL_BIT_COUNT(r->records))
                      return CAL_BIT_COUNT(l->records) < CAL_BIT_COUNT(r->records);

                  if (l->last_fail_time != r->last_fail_time)
                      return l->last_fail_time < r->last_fail_time;

                  if (l->last_suc_time != r->last_suc_time)
                      return l->last_suc_time > r->last_suc_time;

                  //random by std::random_shuffle(items.begin(), items.end());
                  return false;
              });

    //merge
    items.clear();
    while (!items_history.empty() || !items_new.empty()) {
        int ran = rand() % (items_history.size() + items_new.size());
        if (0 <= ran && ran < (int) items_history.size()) {
            items.push_back(items_history.front());
            items_history.pop_front();
        } else if ((int) items_history.size() <= ran && ran < (int) (items_history.size() + items_new.size())) {
            items.push_back(items_new.front());
            items_new.pop_front();
        } else {
            xassert2(false, TSF"ran:%_, history:%_, new:%_", ran, items_history.size(), items_new.size());
        }
    }
}


void SimpleIPPortSort::SortandFilter(std::vector<IPPortItem> &items, int needCount) const {
    ScopedLock lock(mMutex);
    __FilterbyBanned(items);
    __SortbyBanned(items);

    if (needCount < (int) items.size()) items.resize(needCount);
}

void SimpleIPPortSort::AddServerBan(const std::string &ip) {
    if (ip.empty()) return;

    ScopedLock lock(mMutex);
    mServerBans[ip] = ::gettickcount();
}

