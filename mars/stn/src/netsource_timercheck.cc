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
 * netsource_timercheck.cc
 *
 *  Created on: 2013-5-16
 *      Author: yanguoyue
 */

#include "netsource_timercheck.h"

#include <unistd.h>

#include "boost/bind.hpp"

#include "mars/comm/comm_frequency_limit.h"
#include "mars/comm/xlogger/xlogger.h"
#include "mars/stn/config.h"
#include "mars/stn/stn.h"

#include "longlink.h"
#include "longlink_speed_test.h"

using namespace mars::stn;

static const unsigned int kTimeCheckPeriod = 2.5 * 60 * 1000;     // 2.5min
// const static unsigned int TIME_CHECK_PERIOD = 30 * 1000;     //30min
static const int kTimeout = 10 * 1000;     // s
static const int kMaxSpeedTestCount = 30;
static const unsigned long kIntervalTime = 1 * 60 * 60 * 1000;    // ms

#define AYNC_HANDLER mAsyncReg.Get()
#define RETURN_NETCORE_SYNC2ASYNC_FUNC(func) RETURN_SYNC2ASYNC_FUNC(func, )

NetSourceTimerCheck::NetSourceTimerCheck(NetSource *netSource, ActiveLogic &activeLogic, LongLink &longLink,
                                         MessageQueue::MessageQueue_t msgQueueId)
        : mNetSource(netSource)
        , mSelector(mBreaker)
        , mLongLink(longLink)
        ,mAsyncReg(MessageQueue::InstallAsyncHandler(msgQueueId))
{
    xassert2(mBreaker.IsCreateSuc(), "create breaker fail");
    xinfo2(TSF"handler:(%_,%_)", mAsyncReg.Get().queue, mAsyncReg.Get().seq);
    mFrequencyLimit = new CommFrequencyLimit(kMaxSpeedTestCount, kIntervalTime);

    ActiveConn = activeLogic.SignalActive.connect(
            boost::bind(&NetSourceTimerCheck::__OnActiveChanged, this, _1));

    if (activeLogic.IsActive()) {
        __StartCheck();
    }
}

NetSourceTimerCheck::~NetSourceTimerCheck() {

    do {
        if (!mThread.isRunning()) {
            break;
        }

        if (!mBreaker.Break()) {
            xerror2(TSF"write into pipe error");
            break;
        }

        mThread.join();
    } while (false);

    delete mFrequencyLimit;
}

void NetSourceTimerCheck::CancelConnect() {
    RETURN_NETCORE_SYNC2ASYNC_FUNC(boost::bind(&NetSourceTimerCheck::CancelConnect, this));
    xinfo_function();

    if (!mThread.isRunning()) {
        return;
    }

    if (!mBreaker.Break()) {
        xerror2(TSF"write into pipe error");
    }

}

void NetSourceTimerCheck::__StartCheck() {

    RETURN_NETCORE_SYNC2ASYNC_FUNC(boost::bind(&NetSourceTimerCheck::__StartCheck, this));
    xdebug_function();

    if (mAsyncPost != MessageQueue::KNullPost) return;

    mAsyncPost = MessageQueue::AsyncInvokePeriod(kTimeCheckPeriod, kTimeCheckPeriod,
                                                 boost::bind(&NetSourceTimerCheck::__Check, this), mAsyncReg.Get(),
                                                 "NetSourceTimerCheck::__Check()");

}

void NetSourceTimerCheck::__Check() {

    IPSourceType pre_iptype = mLongLink.Profile().ipType;
    if (kIPSourceDebug == pre_iptype || kIPSourceNULL == pre_iptype
        || kIPSourceNewDns == pre_iptype || kIPSourceDNS == pre_iptype) {
        return;
    }

    if (mThread.isRunning()) {
        return;
    }

    // limit the frequency of speed test
    if (!mFrequencyLimit->Check()) {
        xwarn2(TSF"frequency limit");
        return;
    }

    if (!mBreaker.IsCreateSuc() && !mBreaker.ReCreate()) {
        xassert2(false, "break error!");
        return;
    }

    std::string linkedhost = mLongLink.Profile().host;
    xdebug2(TSF"current host:%0", linkedhost);

    mThread.start(boost::bind(&NetSourceTimerCheck::__Run, this, linkedhost));

}

void NetSourceTimerCheck::__StopCheck() {

    RETURN_NETCORE_SYNC2ASYNC_FUNC(boost::bind(&NetSourceTimerCheck::__StopCheck, this));

    xdebug_function();

    if (mAsyncPost == MessageQueue::KNullPost) return;

    if (!mThread.isRunning()) {
        return;
    }

    if (!mBreaker.Break()) {
        xerror2(TSF"write into pipe error");
        return;
    }

    mThread.join();

    mAsyncReg.Cancel();
    mAsyncPost = MessageQueue::KNullPost;
}

void NetSourceTimerCheck::__Run(const std::string &host) {
    //clear the pipe
    mBreaker.Clear();

    if (__TryConnnect(host)) {

        xassert2(TimeCheckSuccHook);

        if (TimeCheckSuccHook) {
            // reset the long link
            TimeCheckSuccHook();
        }

    }

}


bool NetSourceTimerCheck::__TryConnnect(const std::string &host) {
    std::vector<std::string> ip_vec;

    mDnsUtil.GetNewDNS().GetHostByName(host, ip_vec);

    if (ip_vec.empty()) mDnsUtil.GetDNS().GetHostByName(host, ip_vec);
    if (ip_vec.empty()) return false;

    for (std::vector<std::string>::iterator iter = ip_vec.begin(); iter != ip_vec.end(); ++iter) {
        if (*iter == mLongLink.Profile().ip) {
            return false;
        }
    }

    std::vector<uint16_t> port_vec;
    NetSource::GetLonglinkPorts(port_vec);

    if (port_vec.empty()) {
        xerror2(TSF"get ports empty!");
        return false;
    }

    // random get speed test ip and port
    srand((unsigned) gettickcount());
    size_t ip_index = rand() % ip_vec.size();
    size_t port_index = rand() % port_vec.size();

    LongLinkSpeedTestItem speed_item(ip_vec[ip_index], port_vec[port_index]);

    while (true) {
        mSelector.PreSelect();
        speed_item.HandleSetFD(mSelector);

        int select_ret = mSelector.Select(kTimeout);

        if (select_ret == 0) {
            xerror2(TSF"time out");
            break;
        }

        if (select_ret < 0) {
            xerror2(TSF"select errror, ret:%0, strerror(errno):%1", select_ret, strerror(errno));
        }

        if (mSelector.IsException()) {
            xerror2(TSF"pipe exception");
            break;
        }

        if (mSelector.IsBreak()) {
            xwarn2(TSF"FD_ISSET(pipe_[0], &readfd)");
            break;
        }

        speed_item.HandleFDISSet(mSelector);

        if (kLongLinkSpeedTestSuc == speed_item.GetState() || kLongLinkSpeedTestFail == speed_item.GetState()) {
            break;
        }
    }

    speed_item.CloseSocket();


    if (kLongLinkSpeedTestSuc == speed_item.GetState()) {
        mNetSource->RemoveLongBanIP(speed_item.GetIP());
        return true;
    }

    return false;
}

void NetSourceTimerCheck::__OnActiveChanged(bool isActive) {
    ASYNC_BLOCK_START

                xdebug2(TSF"isActive:%0", isActive);

                if (isActive) {
                    __StartCheck();
                } else {
                    __StopCheck();
                }

    ASYNC_BLOCK_END
}
