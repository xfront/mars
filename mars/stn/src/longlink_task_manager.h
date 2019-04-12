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
 * longlink_task_manager.h
 *
 *  Created on: 2012-7-17
 *      Author: yerungui
 */

#ifndef STN_SRC_LONGLINK_TASK_MANAGER_H_
#define STN_SRC_LONGLINK_TASK_MANAGER_H_

#include <list>
#include <stdint.h>

#include "boost/function.hpp"

#include "mars/comm/messagequeue/message_queue.h"
#include "mars/comm/alarm.h"
#include "mars/stn/stn.h"

#include "longlink.h"
#include "longlink_connect_monitor.h"

class AutoBuffer;

class ActiveLogic;

struct STChannelResp;

#ifdef ANDROID

class WakeUpLock;

#endif

namespace mars {
namespace stn {

struct TaskProfile;

class DynamicTimeout;

class LongLinkConnectMonitor;

class LongLinkTaskManager {
public:
    boost::function<int(ErrCmdType errType, int errCode, int failHandle, const Task &task,
                        unsigned int taskCostTime)> CallResultHook;

    boost::function<void(ErrCmdType errType, int errCode, int failHandle,
                         uint32_t srcTaskId)> NotifyRetryAllTasksHook;
    boost::function<void(int _line, ErrCmdType errType, int errCode, const std::string &ip,
                         uint16_t port)> NotifyNetworkErrorHook;
    boost::function<bool(const Task &task, const void *buffer, int len)> AntiAvalancheCheckHook;

    boost::function<void(uint64_t channelId, uint32_t cmdId, uint32_t taskId, const AutoBuffer &body,
                         const AutoBuffer &bufExt)> OnPushHook;

public:
    LongLinkTaskManager(mars::stn::NetSource &netSource, ActiveLogic &activeLogic, DynamicTimeout &_dynamictimeout,
                        MessageQueue::MessageQueue_t msgQueueId);

    virtual ~LongLinkTaskManager();

    bool StartTask(const Task &task);

    bool StopTask(uint32_t taskId);

    bool HasTask(uint32_t taskId) const;

    void ClearTasks();

    void RedoTasks();

    void RetryTasks(ErrCmdType errType, int errCode, int failHandle, uint32_t srcTaskId);

    LongLink &LongLinkChannel() { return *mLongLink; }

    LongLinkConnectMonitor &getLongLinkConnectMonitor() { return *mLongLinkConnectMon; }

    unsigned int GetTaskCount();

    unsigned int GetTasksContinuousFailCount();

private:
    // from ILongLinkObserver
    void __OnResponse(ErrCmdType errorType, int errorCode, uint32_t cmdId, uint32_t taskId, AutoBuffer &body,
                      AutoBuffer &extension, const ConnectProfile &connectProfile);

    void __OnSend(uint32_t taskId);

    void __OnRecv(uint32_t taskId, size_t _cachedsize, size_t _totalsize);

    void __SignalConnection(LongLink::TLongLinkStatus _connect_status);

    void __RunLoop();

    void __RunOnTimeout();

    void __RunOnStartTask();

    void __BatchErrorRespHandle(ErrCmdType errType, int errCode, int failHandle, uint32_t srcTaskId,
                                const ConnectProfile &connectProfile, bool callbackRunningTaskOnly = true);

    bool __SingleRespHandle(std::list<TaskProfile>::iterator it, ErrCmdType errType, int errCode, int failHandle,
                            const ConnectProfile &connectProfile);

    std::list<TaskProfile>::iterator __Locate(uint32_t taskId);

private:
    MessageQueue::ScopeRegister mAsyncReg;
    std::list<TaskProfile> mTaskList;
    uint64_t mLastBatchErrorTime;   // ms
    unsigned long mRetryInterval;    //ms
    unsigned int mTasksContinuousFailCount;

    LongLink *mLongLink;
    LongLinkConnectMonitor *mLongLinkConnectMon;
    DynamicTimeout &mDynamicTimeout;

#ifdef ANDROID
    WakeUpLock *mWakeupLock;
#endif
};
}
}

#endif // STN_SRC_LONGLINK_TASK_MANAGER_H_
