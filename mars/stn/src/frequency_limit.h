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
 * frequency_limit.h
 *
 *  Created on: 2012-9-3
 *      Author: yerungui
 */

#ifndef STN_SRC_FREQUENCY_LIMIT_H_
#define STN_SRC_FREQUENCY_LIMIT_H_

#include <vector>

namespace mars {
namespace stn {

struct Task;
struct STAvalancheRecord;

struct STAvalancheRecord {
    unsigned long hash;
    int count;
    unsigned long lastUpdateTime;
};

class FrequencyLimit {
public:
    FrequencyLimit();

    virtual ~FrequencyLimit();

    bool Check(const mars::stn::Task &task, const void *buffer, int len, unsigned int &span);

private:
    void __ClearRecord();

    void __InsertRecord(unsigned long hash);

    bool __CheckRecord(int index) const;

    void __UpdateRecord(int index);

    unsigned int __GetLastUpdateTillNow(int index);

    int __LocateIndex(unsigned long hash) const;

private:
    std::vector<STAvalancheRecord> mRecordList;
    unsigned long mTimeRecordClear;
};

}
}

#endif // STN_SRC_FREQUENCY_LIMIT_H_
