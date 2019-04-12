// Tencent is pleased to support the open source community by making Mars available.
// Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.

// Licensed under the MIT License (the "License"); you may not use this file except in
// compliance with the License. You may obtain a copy of the License at
// http://opensource.org/licenses/MIT

// Unless required by applicable law or agreed to in writing, software distributed under the License is
// distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
// either express or implied. See the License for the specific language governing permissions and
// limitations under the License.

//
//  weak_network_logic.cc
//
//  Created by zhouzhijie on 2017-10-20.
//  Copyright © 2016年 Tencent. All rights reserved.
//

#include "weak_network_logic.h"
#include "mars/comm/xlogger/xlogger.h"

#define MARK_TIMEOUT (60*1000)
#define WEAK_CONNECT_RTT (2 * 1000)
#define WEAK_PKG_SPAN (2*1000)
#define GOOD_TASK_SPAN (600)
#define SURE_WEAK_SPAN (5*1000)
#define WEAK_TASK_SPAN (5*1000)
//#define LAST_CONNECTINFO_VALID_SPAN (10*1000)

namespace mars {
namespace stn {

//do not delete or insert
enum TKey {
    kEnterWeak = 0,
    kExitWeak,
    kWeakTime,
    kCGICount,
    kCGICost,
    kCGISucc,
    kSceneRtt,
    kSceneIndex,
    kSceneFirstPkg,
    kScenePkgPkg,
    kSceneTask,
    kExitSceneTask,
    kExitSceneTimeout,
    kExitSceneConnect,
    kExitSceneBackground,
    kExitQuickConnectNoNet,
    kSceneTaskBad,
    kFailStepDns = 31,
    kFailStepConnect,
    kFailStepFirstPkg,
    kFailStepPkgPkg,
    kFailStepDecode,
    kFailStepOther,
    kFailStepTimeout,
};

WeakNetworkLogic::WeakNetworkLogic()
    : mIsCurrWeak(false)
    , mConnAfterWeak(0)
    , mLastConnFailTick(false)
    , mLastConnSuccTick(false) {
    ActiveLogic::Singleton::Instance()->SignalForeground.connect(
            boost::bind(&WeakNetworkLogic::__SignalForeground, this, _1));
}

WeakNetworkLogic::~WeakNetworkLogic() {
    ActiveLogic::Singleton::Instance()->SignalForeground.disconnect(
            boost::bind(&WeakNetworkLogic::__SignalForeground, this, _1));
}

void WeakNetworkLogic::__SignalForeground(bool isForeground) {
    if (!isForeground && mIsCurrWeak) {
        mIsCurrWeak = false;
        __ReportWeakLogic(kExitWeak, 1, false);
        __ReportWeakLogic(kExitSceneBackground, 1, false);
        __ReportWeakLogic(kWeakTime, (int) mFirstMarkTick.gettickspan(), false);
        xinfo2(TSF"weak network end");
    }
}

void WeakNetworkLogic::__ReportWeakLogic(int key, int value, bool isImportant) {
    if (reportWeakHook) {
        reportWeakHook(key, value, isImportant);
    }
}

bool WeakNetworkLogic::IsCurrentNetworkWeak() {
    if (mIsCurrWeak) {
        if (mLastMarkTick.gettickspan() < MARK_TIMEOUT) return true;
        else {
            mIsCurrWeak = false;
            __ReportWeakLogic(kExitWeak, 1, false);
            __ReportWeakLogic(kExitSceneTimeout, 1, false);
            __ReportWeakLogic(kWeakTime, (int) mFirstMarkTick.gettickspan(), false);
            xinfo2(TSF"weak network end");
            return false;
        }
    }
    return false;
}

bool WeakNetworkLogic::IsLastValidConnectFail(int64_t &span) {
    if (mLastConnFailTick.isValid()) {
        span = mLastConnFailTick.gettickspan();
        return true;
    } else if (mLastConnSuccTick.isValid()) {
        span = mLastConnSuccTick.gettickspan();
        return false;
    }
    return false;
}

void WeakNetworkLogic::OnConnectEvent(bool isSucc, int rtt, int index) {
    if (isSucc) {
        mLastConnFailTick.setInvalid();
        mLastConnSuccTick.gettickcount();
    } else {
        mLastConnFailTick.gettickcount();
        mLastConnSuccTick.setInvalid();
    }

    if (!ActiveLogic::Singleton::Instance()->IsForeground())
        return;

    if (mIsCurrWeak) ++mConnAfterWeak;
    if (!isSucc) {
        if (mIsCurrWeak) {
            mIsCurrWeak = false;
            __ReportWeakLogic(kExitWeak, 1, false);
            __ReportWeakLogic(kExitSceneConnect, 1, false);
            __ReportWeakLogic(kWeakTime, (int) mFirstMarkTick.gettickspan(), false);
            if (mConnAfterWeak <= 1 && mFirstMarkTick.gettickspan() < SURE_WEAK_SPAN)
                __ReportWeakLogic(kExitQuickConnectNoNet, 1, false);
            xinfo2(TSF"weak network end");
        }

        return;
    }

    bool is_weak = false;
    if (index > 0) {
        is_weak = true;
        if (!mIsCurrWeak) __ReportWeakLogic(kSceneIndex, 1, false);
    } else if (rtt > WEAK_CONNECT_RTT) {
        is_weak = true;
        if (!mIsCurrWeak) __ReportWeakLogic(kSceneRtt, 1, false);
    }

    if (is_weak) {
        if (!mIsCurrWeak) {
            mIsCurrWeak = true;
            mConnAfterWeak = 0;
            mFirstMarkTick.gettickcount();
            mLastMarkTick.gettickcount();
            __ReportWeakLogic(kEnterWeak, 1, false);
            xinfo2(TSF"weak network rtt:%_, index:%_", rtt, index);
        }

        mLastMarkTick.gettickcount();
    }
}

void WeakNetworkLogic::OnPkgEvent(bool isFirstPkg, int span) {
    if (!ActiveLogic::Singleton::Instance()->IsForeground())
        return;

    bool is_weak = (span > WEAK_PKG_SPAN);
    if (is_weak) {
        if (!mIsCurrWeak) {
            mFirstMarkTick.gettickcount();
            __ReportWeakLogic(kEnterWeak, 1, false);
            __ReportWeakLogic(isFirstPkg ? kSceneFirstPkg : kScenePkgPkg, 1, false);
            mIsCurrWeak = true;
            mConnAfterWeak = 0;
            mLastMarkTick.gettickcount();
            xinfo2(TSF"weak network span:%_", span);
        }
        mLastMarkTick.gettickcount();
    }
}

void WeakNetworkLogic::OnTaskEvent(const TaskProfile &taskProfile) {
    if (!ActiveLogic::Singleton::Instance()->IsForeground())
        return;

    bool old_weak = mIsCurrWeak;
    bool is_weak = false;
    if (taskProfile.transferProfile.connectProfile.ipIndex > 0 && taskProfile.errType != kEctOK &&
        taskProfile.errType != kEctEnDecode) {
        is_weak = true;
        if (!mIsCurrWeak) __ReportWeakLogic(kSceneTask, 1, false);
    } else if (taskProfile.errType == kEctOK &&
               (taskProfile.endTaskTime - taskProfile.startTaskTime) >= WEAK_TASK_SPAN) {
        is_weak = true;
        if (!mIsCurrWeak) __ReportWeakLogic(kSceneTaskBad, 1, false);
    }
    if (is_weak) {
        if (!mIsCurrWeak) {
            mFirstMarkTick.gettickcount();
            __ReportWeakLogic(kEnterWeak, 1, false);
            mIsCurrWeak = true;
            mConnAfterWeak = 0;
            mLastMarkTick.gettickcount();
            xinfo2(TSF"weak network errtype:%_", taskProfile.errType);
        }
        mLastMarkTick.gettickcount();
    } else {
        if (taskProfile.errType == kEctOK &&
            (taskProfile.endTaskTime - taskProfile.startTaskTime) < GOOD_TASK_SPAN) {
            if (mIsCurrWeak) {
                mIsCurrWeak = false;
                __ReportWeakLogic(kExitWeak, 1, false);
                __ReportWeakLogic(kExitSceneTask, 1, false);
                __ReportWeakLogic(kWeakTime, (int) mFirstMarkTick.gettickspan(), false);
                xinfo2(TSF"weak network end");
            }
        }
    }

    if (mIsCurrWeak || old_weak) {
        __ReportWeakLogic(kCGICount, 1, false);
        if (taskProfile.errType == kEctOK) {
            __ReportWeakLogic(kCGISucc, 1, false);
            __ReportWeakLogic(kCGICost, (int) (taskProfile.endTaskTime - taskProfile.startTaskTime), false);
        } else {
            __ReportWeakLogic(kFailStepDns + taskProfile.GetFailStep() - 1, 1, false);
        }
    }
}

}
}
