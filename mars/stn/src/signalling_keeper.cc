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
//  signalling_keeper.cc
//  network
//
//  Created by liucan on 13-12-24.
//  Copyright (c) 2013 Tencent. All rights reserved.
//

#include "signalling_keeper.h"

#include <vector>
#include <string>

#include "boost/bind.hpp"

#include "mars/comm/socket/udpclient.h"
#include "mars/comm/time_utils.h"
#include "mars/comm/xlogger/xlogger.h"
#include "mars/stn/proto/longlink_packer.h"

using namespace mars::stn;

static unsigned int g_period = 5 * 1000;  // ms
static unsigned int g_keepTime = 20 * 1000;  // ms


SignallingKeeper::SignallingKeeper(const LongLink &longLink, MessageQueue::MessageQueue_t msgQueueId,
                                   bool useUdp)
        : mMsgReg(MessageQueue::InstallAsyncHandler(msgQueueId))
        , mLastTouchTime(0)
        , mKeeping(false)
        , mLongLink(longLink)
        , mPort(0)
        , mUdpClient(mIP, mPort, this)
        , mUseUdp(useUdp) {
    xinfo2(TSF"SignallingKeeper messagequeue_id=%_, handler:(%_,%_)", MessageQueue::Handler2Queue(mMsgReg.Get()),
           mMsgReg.Get().queue, mMsgReg.Get().seq);
}

SignallingKeeper::~SignallingKeeper() {
    Stop();
}


void SignallingKeeper::SetStrategy(unsigned int period, unsigned int keepTime) {
    xinfo2(TSF"signal keeper period:%0, keepTime:%1", period, keepTime);
    xassert2(period > 0);
    xassert2(keepTime > 0);
    if (period == 0 || keepTime == 0) {
        xerror2(TSF"wrong strategy");
        return;
    }

    g_period = period;
    g_keepTime = keepTime;
}

void SignallingKeeper::OnNetWorkDataChanged(const char *, ssize_t, ssize_t) {
    if (!mKeeping) return;
    uint64_t now = ::gettickcount();
    xassert2(now >= mLastTouchTime);

    if (now < mLastTouchTime || now - mLastTouchTime > g_keepTime) {
        mKeeping = false;
        return;
    }

    if (mPostId != MessageQueue::KNullPost) {
        MessageQueue::CancelMessage(mPostId);
    }

    mPostId = MessageQueue::AsyncInvokeAfter(g_period, boost::bind(&SignallingKeeper::__OnTimeOut, this), mMsgReg.Get(),
                                             "SignallingKeeper::__OnTimeOut");
}


void SignallingKeeper::Keep() {
    xinfo2(TSF"start signalling, period:%0, keepTime:%1, use udp:%2, mKeeping:%3", g_period, g_keepTime, mUseUdp,
           mKeeping);
    mLastTouchTime = ::gettickcount();

    if (!mKeeping) {
        __SendSignallingBuffer();
        mKeeping = true;
    }
}

void SignallingKeeper::Stop() {
    xinfo2(TSF"stop signalling");

    if (mKeeping && mPostId != MessageQueue::KNullPost) {
        mKeeping = false;
        MessageQueue::CancelMessage(mPostId);
    }
}

void SignallingKeeper::__SendSignallingBuffer() {
    if (mUseUdp) {
        ConnectProfile link_info = mLongLink.Profile();
        if (mUdpClient.HasBuuferToSend()) return;

        if (link_info.ip != "" && link_info.port != 0
            && link_info.ip != mIP && link_info.port != mPort) {
            mIP = link_info.ip;
            mPort = link_info.port;
        }

        if (mIP != "" && mPort != 0) {
            mUdpClient.SetIpPort(mIP, mPort);
            AutoBuffer buffer;
            longlink_pack(signal_keep_cmdid(), 0, KNullAtuoBuffer, KNullAtuoBuffer, buffer, NULL);
            mUdpClient.SendAsync(buffer.Ptr(), buffer.Length());
        }
    } else {
        if (SendBufferHook) {
            SendBufferHook(KNullAtuoBuffer, KNullAtuoBuffer, signal_keep_cmdid());
        }
    }
}

void SignallingKeeper::__OnTimeOut() {
    xdebug2(TSF"sent signalling, period:%0", g_period);
    __SendSignallingBuffer();
}

void SignallingKeeper::OnError(UdpClient *self, int errNO) {
}

void SignallingKeeper::OnDataGramRead(UdpClient *self, void *bufferReq, size_t len) {
}

void SignallingKeeper::OnDataSent(UdpClient *self) {
    OnNetWorkDataChanged("", 0, 0);
}
