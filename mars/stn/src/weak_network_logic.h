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
 * weak_network_logic.h
 *
 *  Created on: 2017-10-20
 *      Author: zhouzhijie
 */

#ifndef weak_network_logic_h
#define weak_network_logic_h

#include "mars/comm/singleton.h"
#include "comm/tickcount.h"
#include "mars/stn/task_profile.h"
#include "mars/baseevent/active_logic.h"

namespace mars {
namespace stn {

class WeakNetworkLogic {
public:
    SINGLETON_INTRUSIVE(WeakNetworkLogic, new WeakNetworkLogic, delete);

    boost::function<void(int key, int value, bool isImportant)> reportWeakHook;

    bool IsCurrentNetworkWeak();

    void OnConnectEvent(bool isSucc, int rtt, int index);

    void OnPkgEvent(bool isFirstPkg, int span);

    void OnTaskEvent(const TaskProfile &taskProfile);

    bool IsLastValidConnectFail(int64_t &span);

private:
    WeakNetworkLogic();

    virtual ~WeakNetworkLogic();

    void __SignalForeground(bool isForeground);

    void __ReportWeakLogic(int key, int value, bool isImportant);

private:
    tickcount_t mFirstMarkTick;
    tickcount_t mLastMarkTick;
    bool mIsCurrWeak;
    unsigned int mConnAfterWeak;
    tickcount_t mLastConnFailTick;
    tickcount_t mLastConnSuccTick;
};

}
}

#endif /* weak_network_logic_h */
