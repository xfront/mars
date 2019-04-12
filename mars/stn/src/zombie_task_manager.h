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
 * zombie_task_manager.h
 *
 *  Created on: 2014-6-19
 *      Author: yerungui
 *  Copyright (c) 2013-2015 Tencent. All rights reserved.
 *
 */

#ifndef STN_SRC_ZOMBIE_TASK_MANAGER_H_
#define STN_SRC_ZOMBIE_TASK_MANAGER_H_

#include <list>
#include <stdint.h>

#include "boost/function.hpp"

#include "mars/comm/messagequeue/message_queue.h"
#include "mars/stn/stn.h"

struct ZombieTask;

namespace mars {
namespace stn {

class ZombieTaskManager {
public:
    boost::function<void(const Task &task)> StartTaskHook;
    boost::function<int(ErrCmdType errType, int errCode, int failHandle, const Task &task,
                        unsigned int taskCostTime)> CallResultHook;

public:
    ZombieTaskManager(MessageQueue::MessageQueue_t msgQueueId);

    ~ZombieTaskManager();

    bool SaveTask(const Task &task, unsigned int taskCostTime /*ms*/);

    bool StopTask(uint32_t taskId);

    bool HasTask(uint32_t taskId) const;

    void ClearTasks();

    void RedoTasks();

    void OnNetCoreStartTask();

private:
    ZombieTaskManager(const ZombieTaskManager &);

    ZombieTaskManager &operator=(const ZombieTaskManager &);

private:
    void __StartTask();

    void __TimerChecker();

private:
    MessageQueue::ScopeRegister mAsyncReg;
    std::list<ZombieTask> mTaskList;
    uint64_t mLastTaskTime;
};

}
}

#endif // STN_SRC_ZOMBIE_TASK_MANAGER_H_
