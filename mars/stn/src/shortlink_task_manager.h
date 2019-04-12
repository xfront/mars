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
 * shortlink_task_manager.h
 *
 *  Created on: 2012-8-24
 *      Author: zhouzhijie
 */

#ifndef STN_SRC_SHORTLINK_TASK_MANAGER_H_
#define STN_SRC_SHORTLINK_TASK_MANAGER_H_

#include <list>
#include <stdint.h>

#include "boost/function.hpp"

#include "mars/comm/messagequeue/message_queue.h"
#include "mars/comm/alarm.h"
#include "mars/stn/stn.h"
#include "mars/stn/task_profile.h"

#include "shortlink.h"

class AutoBuffer;

#ifdef ANDROID

class WakeUpLock;

#endif

namespace mars {
namespace stn {

class DynamicTimeout;

class ShortLinkTaskManager {
public:
    boost::function<int(ErrCmdType errType, int errCode, int failHandle, const Task &task, unsigned int taskCostTime)> CallResultHook;

    boost::function<void(int _line, ErrCmdType errType, int errCode, const std::string &ip, const std::string &host, uint16_t port)> NotifyNetworkErrorHook;

    boost::function<bool(const Task &task, const void *buffer, int len)> AntiAvalancheCheckHook;

    boost::function<void(int _status_code)> ShortLinkRspHook;

    boost::function<void(ErrCmdType errType, int errCode, int failHandle, uint32_t srcTaskId)> NotifyRetryAllTasksHook;

public:
    ShortLinkTaskManager(mars::stn::NetSource &netSource, DynamicTimeout &_dynamictimeout,
                         MessageQueue::MessageQueue_t msgQueueId);

    virtual ~ShortLinkTaskManager();

    bool StartTask(const Task &task);

    bool StopTask(uint32_t taskId);

    bool HasTask(uint32_t taskId) const;

    void ClearTasks();

    void RedoTasks();

    void RetryTasks(ErrCmdType errType, int errCode, int failHandle, uint32_t srcTaskId);

    unsigned int GetTasksContinuousFailCount();

    ConnectProfile GetConnectProfile(uint32_t taskId) const;

private:
    void __RunLoop();

    void __RunOnTimeout();

    void __RunOnStartTask();

    void __OnResponse(ShortLinkInterface *worker, ErrCmdType errType, int status, AutoBuffer &body,
                      AutoBuffer &extension, bool cancelRetry, ConnectProfile &connProfile);

    void __OnSend(ShortLinkInterface *worker);

    void __OnRecv(ShortLinkInterface *worker, unsigned int cachedSize, unsigned int totalSize);

    void __BatchErrorRespHandle(ErrCmdType errType, int errCode, int failHandle, uint32_t srcTaskId,
                                bool callbackRunningTaskOnly = true);

    bool __SingleRespHandle(std::list<TaskProfile>::iterator it, ErrCmdType errType, int errCode, int failHandle,
                            size_t _resp_length, const ConnectProfile &connectProfile);

    std::list<TaskProfile>::iterator __LocateBySeq(intptr_t runningId);

    void __DeleteShortLink(intptr_t &runningId);

private:
    MessageQueue::ScopeRegister mAsyncReg;
    NetSource &mNetSource;

    std::list<TaskProfile> mTaskList;

    bool mDefaultUseProxy;
    unsigned int mTasksContinuousFailCount;
    DynamicTimeout &mDynamicTimeout;
#ifdef ANDROID
    WakeUpLock *mWakeupLock;
#endif
};

}
}


#endif // STN_SRC_SHORTLINK_TASK_MANAGER_H_
