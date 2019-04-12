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
 * longlink_connect_monitor.cc
 *
 *  Created on: 2014-2-26
 *      Author: yerungui
 */

#include "longlink_connect_monitor.h"

#include "boost/bind.hpp"

#include "mars/app/app.h"
#include "mars/baseevent/active_logic.h"
#include "mars/comm/thread/lock.h"
#include "mars/comm/xlogger/xlogger.h"
#include "mars/comm/time_utils.h"
#include "mars/comm/socket/unix_socket.h"
#include "mars/comm/platform_comm.h"
#include "mars/sdt/src/checkimpl/dnsquery.h"
#include "mars/stn/config.h"

#include "longlink_speed_test.h"
#include "net_source.h"

using namespace mars::stn;
using namespace mars::app;

static const unsigned int kTimeCheckPeriod = 10 * 1000;     // 10s
static const unsigned int kStartCheckPeriod = 3 * 1000;     // 3s

static const unsigned long kNoNetSaltRate = 3;
static const unsigned long kNoNetSaltRise = 600;
static const unsigned long kNoAccountInfoSaltRate = 2;
static const unsigned long kNoAccountInfoSaltRise = 300;

static const unsigned long kNoAccountInfoInactiveInterval = (7 * 24 * 60 * 60);  // s

enum {
    kTaskConnect,
    kLongLinkConnect,
    kNetworkChangeConnect,
};

enum {
    kForgroundOneMinute,
    kForgroundTenMinute,
    kForgroundActive,
    kBackgroundActive,
    kInactive,
};

static unsigned long const sg_interval[][5] = {
        {5,  10, 20,  30,  300},
        {15, 30, 240, 300, 600},
        {0,  0,  0,   0,   0},
};

static int __CurActiveState(const ActiveLogic &activeLogic) {
    if (!activeLogic.IsActive()) return kInactive;

    if (!activeLogic.IsForeground()) return kBackgroundActive;

    if (10 * 60 * 1000 <= ::gettickcount() - activeLogic.LastForegroundChangeTime()) return kForgroundActive;

    if (60 * 1000 <= ::gettickcount() - activeLogic.LastForegroundChangeTime()) return kForgroundTenMinute;

    return kForgroundOneMinute;
}

static unsigned long __Interval(int type, const ActiveLogic &activeLogic) {
    unsigned long interval = sg_interval[type][__CurActiveState(activeLogic)];

    if (kLongLinkConnect != type) return interval;

    if (__CurActiveState(activeLogic) == kInactive ||
        __CurActiveState(activeLogic) == kForgroundActive) {  // now - LastForegroundChangeTime>10min
        if (!activeLogic.IsActive() && GetAccountInfo().username.empty()) {
            interval = kNoAccountInfoInactiveInterval;
            xwarn2(TSF"no account info and inactive, interval:%_", interval);

        } else if (kNoNet == getNetInfo()) {
            interval = interval * kNoNetSaltRate + kNoNetSaltRise;
            xinfo2(TSF"no net, interval:%0", interval);

        } else if (GetAccountInfo().username.empty()) {
            interval = interval * kNoAccountInfoSaltRate + kNoAccountInfoSaltRise;
            xinfo2(TSF"no account info, interval:%0", interval);

        } else {
            // default value
            interval += rand() % (20);
        }
    }

    return interval;
}

#define AYNC_HANDLER mAsyncReg.Get()

LongLinkConnectMonitor::LongLinkConnectMonitor(ActiveLogic &activeLogic, LongLink &longLink,
                                               MessageQueue::MessageQueue_t id)
        : mAsyncReg(MessageQueue::InstallAsyncHandler(id)), mActiveLogic(activeLogic), mLongLink(longLink),
          mAlarm(boost::bind(&LongLinkConnectMonitor::__OnAlarm, this), id), mStatus(LongLink::kDisConnected),
          mLastConnectTime(0), mLastConnectNetType(kNoNet),
          mThread(boost::bind(&LongLinkConnectMonitor::__Run, this), XLOGGER_TAG"::con_mon"), mContiSuccCount(0),
          mIsStart(false) {
    xinfo2(TSF"handler:(%_,%_)", mAsyncReg.Get().queue, mAsyncReg.Get().seq);
    mActiveLogic.SignalActive.connect(boost::bind(&LongLinkConnectMonitor::__OnSignalActive, this, _1));
    mActiveLogic.SignalForeground.connect(boost::bind(&LongLinkConnectMonitor::__OnSignalForeground, this, _1));
    mLongLink.SignalConnection.connect(boost::bind(&LongLinkConnectMonitor::__OnLongLinkStatuChanged, this, _1));
}

LongLinkConnectMonitor::~LongLinkConnectMonitor() {
#ifdef __APPLE__
    __StopTimer();
#endif
    mLongLink.SignalConnection.disconnect(boost::bind(&LongLinkConnectMonitor::__OnLongLinkStatuChanged, this, _1));
    mActiveLogic.SignalForeground.disconnect(boost::bind(&LongLinkConnectMonitor::__OnSignalForeground, this, _1));
    mActiveLogic.SignalActive.disconnect(boost::bind(&LongLinkConnectMonitor::__OnSignalActive, this, _1));
    mAsyncReg.CancelAndWait();
}

bool LongLinkConnectMonitor::MakeSureConnected() {
    __IntervalConnect(kTaskConnect);
    return LongLink::kConnected == mLongLink.ConnectStatus();
}

bool LongLinkConnectMonitor::NetworkChange() {
    xinfo_function();
#ifdef __APPLE__
    __StopTimer();

    do {
        if (LongLink::kConnected != mStatus || (::gettickcount() - mLastConnectTime) <= 10 * 1000) break;

        if (kMobile != mLastConnectNetType) break;

        int netifo = getNetInfo();

        if (kNoNet == netifo) break;

        if (__StartTimer()) return false;
    } while (false);

#endif
    mLongLink.Disconnect(LongLink::kNetworkChange);
    return 0 == __IntervalConnect(kNetworkChangeConnect);
}

uint64_t LongLinkConnectMonitor::__IntervalConnect(int type) {
    if (LongLink::kConnecting == mLongLink.ConnectStatus() || LongLink::kConnected == mLongLink.ConnectStatus())
        return 0;

    uint64_t interval = __Interval(type, mActiveLogic) * 1000ULL;
    uint64_t posttime = gettickcount() - mLongLink.Profile().dnsTime;

    if (posttime >= interval) {
        bool newone = false;
        bool ret = mLongLink.MakeSureConnected(&newone);
        xinfo2(TSF"made interval connect interval:%0, posttime:%_, newone:%_, connectstatus:%_, ret:%_", interval,
               posttime, newone, mLongLink.ConnectStatus(), ret);
        return 0;

    } else {
        return interval - posttime;
    }
}

uint64_t LongLinkConnectMonitor::__AutoIntervalConnect() {
    mAlarm.Cancel();
    uint64_t remain = __IntervalConnect(kLongLinkConnect);

    if (0 == remain) return remain;

    xinfo2(TSF"start auto connect after:%0", remain);
    mAlarm.Start((int) remain);
    return remain;
}

void LongLinkConnectMonitor::__OnSignalForeground(bool _isForeground) {
    ASYNC_BLOCK_START
#ifdef __APPLE__
                xinfo2(TSF"forground:%_ time:%_ tick:%_", _isForeground, timeMs(), gettickcount());

                if (_isForeground) {
                    xinfo2(TSF"longlink:%_ time:%_ %_ %_", mLongLink.ConnectStatus(), tickcount_t().gettickcount().get(), mLongLink.GetLastRecvTime().get(), int64_t(tickcount_t().gettickcount() - mLongLink.GetLastRecvTime()));

                    if ((mLongLink.ConnectStatus() == LongLink::kConnected) &&
                            (tickcount_t().gettickcount() - mLongLink.GetLastRecvTime() > tickcountdiff_t(4.5 * 60 * 1000))) {
                        xwarn2(TSF"sock long time no send data, close it");
                        __ReConnect();
                    }
                }

#endif
                __AutoIntervalConnect();
    ASYNC_BLOCK_END
}

void LongLinkConnectMonitor::__OnSignalActive(bool isActive) {
    ASYNC_BLOCK_START
                        __AutoIntervalConnect();
    ASYNC_BLOCK_END
}

void LongLinkConnectMonitor::__OnLongLinkStatuChanged(LongLink::TLongLinkStatus status) {
    mAlarm.Cancel();

    if (LongLink::kConnectFailed == status || LongLink::kDisConnected == status) {
        mAlarm.Start(500);
    } else if (LongLink::kConnected == status) {
        xinfo2(TSF"cancel auto connect");
    }

    mStatus = status;
    mLastConnectTime = ::gettickcount();
    mLastConnectNetType = ::getNetInfo();
}

void LongLinkConnectMonitor::__OnAlarm() {
    __AutoIntervalConnect();
}

#ifdef __APPLE__
bool LongLinkConnectMonitor::__StartTimer() {
    xdebug_function();

    mContiSuccCount = 0;

    ScopedLock lock(mTestMutex);
    mIsStart = true;

    if (mThread.isRunning()) {
        return true;
    }

    int ret = mThread.start_periodic(kStartCheckPeriod, kTimeCheckPeriod);
    return 0 == ret;
}


bool LongLinkConnectMonitor::__StopTimer() {
    xdebug_function();

    ScopedLock lock(mTestMutex);

    if (!mIsStart) return true;

    mIsStart = false;

    if (!mThread.isRunning()) {
        return true;
    }

    mThread.cancel_periodic();


    mThread.join();
    return true;
}
#endif


void LongLinkConnectMonitor::__Run() {
    int netifo = getNetInfo();

    if (LongLink::kConnected != mStatus || (::gettickcount() - mLastConnectTime) <= 12 * 1000
        || kMobile != mLastConnectNetType || kMobile == netifo) {
        mThread.cancelPeriodic();
        return;
    }

    struct socket_ipinfo_t dummyIpInfo;
    int ret = socket_gethostbyname(NetSource::GetLongLinkHosts().front().c_str(), &dummyIpInfo, 0, NULL);

    if (ret == 0) {
        ++mContiSuccCount;
    } else {
        mContiSuccCount = 0;
    }

    if (mContiSuccCount >= 3) {
        __ReConnect();
        mThread.cancelPeriodic();
    }
}

void LongLinkConnectMonitor::__ReConnect() {
    xinfo_function();
    xassert2(LongLinkResetHook);
    LongLinkResetHook();
}


