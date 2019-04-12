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
 * frequency_limit.cc
 *
 *  Created on: 2012-9-3
 *      Author: yerungui
 */

#include "frequency_limit.h"

#include "mars/comm/adler32.h"
#include "mars/comm/time_utils.h"
#include "mars/comm/xlogger/xlogger.h"
#include "mars/stn/stn.h"

#define MAX_RECORD_COUNT (30)
#define RECORD_INTERCEPT_COUNT (105)

#define NOT_CLEAR_INTERCEPT_COUNT_RETRY (99)
#define NOT_CLEAR_INTERCEPT_COUNT (75)
#define NOT_CLEAR_INTERCEPT_INTERVAL_MINUTE (10*60*1000)
#define RUN_CLEAR_RECORDS_INTERVAL_MINUTE  (60*60*1000)

using namespace mars::stn;

FrequencyLimit::FrequencyLimit()
        : mTimeRecordClear(::gettickcount()) {

}

FrequencyLimit::~FrequencyLimit() {}

bool FrequencyLimit::Check(const mars::stn::Task &task, const void *buffer, int len, unsigned int &span) {
    xverbose_function();

    if (!task.limitFrequency) return true;

    unsigned long time_cur = ::gettickcount();
    xassert2(time_cur >= mTimeRecordClear);
    unsigned long interval = time_cur - mTimeRecordClear;

    if (RUN_CLEAR_RECORDS_INTERVAL_MINUTE <= interval) {
        xdebug2(TSF"__ClearRecord interval=%0, timeCur=%1, itimeRecordClear=%2", interval, time_cur,
                mTimeRecordClear);
        mTimeRecordClear = time_cur;
        __ClearRecord();
    }

    unsigned long hash = ::adler32(0, (const unsigned char *) buffer, len);
    int find_index = __LocateIndex(hash);

    if (0 <= find_index) {
        span = __GetLastUpdateTillNow(find_index);
        __UpdateRecord(find_index);

        if (!__CheckRecord(find_index)) {
            xerror2(TSF"Anti-Avalanche had Catch Task, Task Info: ptr=%0, cmdid=%1, need_authed=%2, cgi:%3, channelSelect=%4, limit_flow=%5",
                    &task, task.cmdId, task.needAuthed, task.cgi, task.channelSelect, task.limitFlow);
            xerror2(TSF"apBuffer Len=%0, Hash=%1, Count=%2, timeLastUpdate=%3",
                    len, mRecordList[find_index].hash, mRecordList[find_index].count,
                    mRecordList[find_index].lastUpdateTime);
            xassert2(false);

            return false;
        }
    } else {
        xdebug2(TSF"InsertRecord Task Info: ptr=%0, cmdid=%1, need_authed=%2, cgi:%3, channelSelect=%4, limit_flow=%5",
                &task, task.cmdId, task.needAuthed, task.cgi, task.channelSelect, task.limitFlow);

        __InsertRecord(hash);
    }

    return true;
}

void FrequencyLimit::__ClearRecord() {
    xdebug2(TSF"iarrRecord size=%0", mRecordList.size());

    unsigned long time_cur = ::gettickcount();

    std::vector<STAvalancheRecord>::iterator first = mRecordList.begin();

    while (first != mRecordList.end()) {
        xassert2(time_cur >= first->lastUpdateTime);
        unsigned long interval = time_cur - first->lastUpdateTime;

        if (interval <= NOT_CLEAR_INTERCEPT_INTERVAL_MINUTE && NOT_CLEAR_INTERCEPT_COUNT <= first->count) {
            int oldcount = first->count;

            if (NOT_CLEAR_INTERCEPT_COUNT_RETRY < first->count) first->count = NOT_CLEAR_INTERCEPT_COUNT_RETRY;

            xwarn2(TSF"timeCur:%_,  first->timeLastUpdate:%_, interval:%_, Hash:%_, oldcount:%_, Count:%_",
                   time_cur, first->lastUpdateTime, interval, first->hash, oldcount, first->count);
            ++first;
        } else {
            first = mRecordList.erase(first);
        }
    }
}

int FrequencyLimit::__LocateIndex(unsigned long hash) const {
    for (int i = (int) mRecordList.size() - 1; i >= 0; --i) {
        if (mRecordList[i].hash == hash)
            return i;
    }

    return -1;
}

void FrequencyLimit::__InsertRecord(unsigned long hash) {
    if (MAX_RECORD_COUNT < mRecordList.size()) {
        xassert2(false);
        return;
    }

    STAvalancheRecord temp;
    temp.count = 1;
    temp.hash = hash;
    temp.lastUpdateTime = ::gettickcount();

    if (MAX_RECORD_COUNT == mRecordList.size()) {
        unsigned int del_index = 0;

        for (unsigned int i = 1; i < mRecordList.size(); i++) {
            if (mRecordList[del_index].lastUpdateTime > mRecordList[i].lastUpdateTime) {
                del_index = i;
            }
        }

        std::vector<STAvalancheRecord>::iterator it = mRecordList.begin();
        it += del_index;
        mRecordList.erase(it);
    }

    mRecordList.push_back(temp);
}

void FrequencyLimit::__UpdateRecord(int index) {
    xassert2(0 <= index && (unsigned int) index < mRecordList.size());

    mRecordList[index].count += 1;
    mRecordList[index].lastUpdateTime = ::gettickcount();
}

unsigned int FrequencyLimit::__GetLastUpdateTillNow(int index) {
    xassert2(0 <= index && (unsigned int) index < mRecordList.size());

    return (unsigned int) (::gettickcount() - mRecordList[index].lastUpdateTime);
}

bool FrequencyLimit::__CheckRecord(int index) const {
    xassert2(0 <= index && (unsigned int) index < mRecordList.size());
    return (mRecordList[index].count) <= RECORD_INTERCEPT_COUNT;
}
