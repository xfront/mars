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
 * net_core.h
 *
 *  Created on: 2012-7-18
 *      Author: yerungui
 */

#ifndef STN_SRC_NET_CORE_H_
#define STN_SRC_NET_CORE_H_

#include "mars/comm/singleton.h"
#include "mars/comm/messagequeue/message_queue.h"

#include "mars/stn/stn.h"
#include "mars/stn/config.h"

#ifdef USE_LONG_LINK

#include "mars/stn/src/longlink.h"

#endif

namespace mars {

namespace stn {

class NetSource;


class ShortLinkTaskManager;

#ifdef USE_LONG_LINK

class LongLinkTaskManager;

class TimingSync;

class ZombieTaskManager;

class NetSourceTimerCheck;

#endif

class SignallingKeeper;

class NetCheckLogic;

class DynamicTimeout;

class AntiAvalanche;

enum {
    kCallFromLong,
    kCallFromShort,
    kCallFromZombie,
};

class NetCore {
public:
    SINGLETON_INTRUSIVE(NetCore, new NetCore, __Release);

public:
    boost::function<void(Task &task)> TaskProcessHook;
    boost::function<int(int _from, ErrCmdType errType, int errCode, int failHandle,
                        const Task &task)> CallResultHook;
    boost::signals2::signal<void(uint32_t cmdId, const AutoBuffer &buffer)> OnPushHook;

public:
    MessageQueue::MessageQueue_t GetMessageQueueId() { return mMsgQueueCreator.GetMessageQueue(); }

    NetSource &GetNetSourceRef() { return *mNetSource; }

    void CancelAndWait() { mMsgQueueCreator.CancelAndWait(); }

    void StartTask(const Task &task);

    void StopTask(uint32_t taskId);

    bool HasTask(uint32_t taskId) const;

    void ClearTasks();

    void RedoTasks();

    void RetryTasks(ErrCmdType errType, int errCode, int failHandle, uint32_t srcTaskId);

    void MakeSureLongLinkConnect();

    bool LongLinkIsConnected();

    void OnNetworkChange();

    void KeepSignal();

    void StopSignal();

    ConnectProfile GetConnectProfile(uint32_t taskId, int channelSelect);

    void AddServerBan(const std::string &ip);

#ifdef USE_LONG_LINK

    LongLink &Longlink();

#endif

private:
    NetCore();

    virtual ~NetCore();

    static void __Release(NetCore *_instance);

private:
    int __CallBack(int _from, ErrCmdType errType, int errCode, int failHandle, const Task &task,
                   unsigned int taskCostTime);

    void __OnShortLinkNetworkError(int _line, ErrCmdType errType, int errCode, const std::string &ip,
                                   const std::string &host, uint16_t port);

    void __OnShortLinkResponse(int _status_code);

#ifdef USE_LONG_LINK

    void
    __OnLongLinkNetworkError(int _line, ErrCmdType errType, int errCode, const std::string &ip, uint16_t port);

    void __OnLongLinkConnStatusChange(LongLink::TLongLinkStatus status);

    void __ResetLongLink();

#endif

    void __ConnStatusCallBack();

    void __OnTimerCheckSuc();

    void __OnSignalActive(bool isActive);

    void __OnPush(uint64_t channelId, uint32_t cmdId, uint32_t taskId, const AutoBuffer &body,
                  const AutoBuffer &bufExt);

private:
    NetCore(const NetCore &);

    NetCore &operator=(const NetCore &);

private:
    MessageQueue::MessageQueueCreater mMsgQueueCreator;
    MessageQueue::ScopeRegister mAsyncReg;
    NetSource *mNetSource;
    NetCheckLogic *mNetcheckLogic;
    AntiAvalanche *mAntiAvalanche;

    DynamicTimeout *mDynamicTimeout;
    ShortLinkTaskManager *mShortLinkTaskManager;
    int mShortLinkErrorCount;

#ifdef USE_LONG_LINK
    ZombieTaskManager *mZombieTaskManager;
    LongLinkTaskManager *mLongLinkTaskManager;
    SignallingKeeper *mSignalKeeper;
    NetSourceTimerCheck *mNetsourceTimercheck;
    TimingSync *mTimingSync;
#endif

    bool mShortLinkTryFlag;

};

}
}

#endif // STN_SRC_NET_CORE_H_
