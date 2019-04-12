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
//  signalling_keeper.h
//  network
//
//  Created by liucan on 13-12-24.
//  Copyright (c) 2013 Tencent. All rights reserved.
//

#ifndef STN_SRC_SIGNALLING_KEEPER_H_
#define STN_SRC_SIGNALLING_KEEPER_H_

#include "boost/function.hpp"

#include "mars/comm/messagequeue/message_queue.h"
#include "mars/comm/socket/udpclient.h"

#include "longlink.h"

namespace mars {
namespace stn {

class SignallingKeeper : IAsyncUdpClientEvent {
public:
    static void SetStrategy(unsigned int period, unsigned int keepTime);  // ms
public:
    SignallingKeeper(const LongLink &longLink, MessageQueue::MessageQueue_t msgQueueId, bool useUdp = true);

    ~SignallingKeeper();

    void OnNetWorkDataChanged(const char *, ssize_t, ssize_t);

    void Keep();

    void Stop();

    virtual void OnError(UdpClient *self, int errNO);

    virtual void OnDataGramRead(UdpClient *self, void *bufferReq, size_t len);

    virtual void OnDataSent(UdpClient *self);

public:
    boost::function<unsigned int(const AutoBuffer &, const AutoBuffer &, int)> SendBufferHook;

private:
    void __SendSignallingBuffer();

    void __OnTimeOut();

private:
    MessageQueue::ScopeRegister mMsgReg;
    uint64_t mLastTouchTime;
    bool mKeeping;
    MessageQueue::MessagePost_t mPostId;
    const LongLink &mLongLink;
    std::string mIP;
    unsigned int mPort;
    UdpClient mUdpClient;
    bool mUseUdp;
};

}
}


#endif // STN_SRC_SIGNALLING_KEEPER_H_
