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
 * zombie_task_manager.cc
 *
 *  Created on: 2014-6-19
 *      Author: yerungui
 *  Copyright (c) 2013-2015 Tencent. All rights reserved.
 *
 */

#include "zombie_task_manager.h"

#include "boost/bind.hpp"

#include "mars/comm/time_utils.h"
#include "mars/comm/messagequeue/message_queue.h"
#include "mars/comm/xlogger/xlogger.h"

using namespace mars::stn;

static uint64_t RETRY_INTERVAL = 60 * 1000;

struct ZombieTask {
    Task task;
    uint64_t save_time;
};

static bool __compare_task(const ZombieTask &first, const ZombieTask &second) {
    return first.task.priority < second.task.priority;
}

ZombieTaskManager::ZombieTaskManager(MessageQueue::MessageQueue_t msgQueueId)
        : mAsyncReg(MessageQueue::InstallAsyncHandler(msgQueueId)),
          mLastTaskTime(gettickcount()) {
    xinfo2(TSF"handler:(%_,%_)", mAsyncReg.Get().queue, mAsyncReg.Get().seq);
}

ZombieTaskManager::~ZombieTaskManager() {
    mAsyncReg.CancelAndWait();
}

bool ZombieTaskManager::SaveTask(const Task &task, unsigned int taskCostTime) {
    if (task.networkStatusSensitive) return false;
    ZombieTask zombie_task = {task, ::gettickcount()};

    zombie_task.task.retryCount = 0;
    zombie_task.task.totalTimetout -= taskCostTime;

    if (0 >= zombie_task.task.totalTimetout) return false;

    mTaskList.push_back(zombie_task);

    xinfo2(TSF"task end callback zombie savetask cgi:%_, cmdid:%_, taskId:%_", task.cgi, task.cmdId,
           task.taskId);

    MessageQueue::SingletonMessage(false, mAsyncReg.Get(),
                                   MessageQueue::Message((MessageQueue::MessageTitle_t) this,
                                                         boost::bind(&ZombieTaskManager::__TimerChecker, this),
                                                         "ZombieTaskManager::__TimerChecker"),
                                   MessageQueue::MessageTiming(3000, 3000));
    return true;
}

bool ZombieTaskManager::StopTask(uint32_t taskId) {
    std::list<ZombieTask>::iterator first = mTaskList.begin();
    std::list<ZombieTask>::iterator last = mTaskList.end();
    while (first != last) {
        if (taskId == first->task.taskId) {
            xinfo2(TSF"find the task taskId:%0", taskId);
            mTaskList.erase(first);
            return true;
        }
        ++first;
    }
    return false;
}

bool ZombieTaskManager::HasTask(uint32_t taskId) const {
    xverbose_function();

    std::list<ZombieTask>::const_iterator first = mTaskList.begin();
    std::list<ZombieTask>::const_iterator last = mTaskList.end();

    while (first != last) {
        if (taskId == first->task.taskId) {
            return true;
        }
        ++first;
    }

    return false;
}

void ZombieTaskManager::ClearTasks() {
    mTaskList.clear();
}

void ZombieTaskManager::RedoTasks() {
    xinfo_function();
    __StartTask();
}

void ZombieTaskManager::OnNetCoreStartTask() {
    mLastTaskTime = gettickcount();
}

void ZombieTaskManager::__StartTask() {
    xassert2(StartTaskHook);

    if (mTaskList.empty()) return;

    std::list<ZombieTask> taskList = mTaskList;
    mTaskList.clear();
    taskList.sort(__compare_task);

    for (std::list<ZombieTask>::iterator it = taskList.begin(); it != taskList.end(); ++it) {
        uint64_t cur_time = ::gettickcount();

        if ((cur_time - it->save_time) >= (uint64_t) it->task.totalTimetout) {
            xinfo2(TSF"task end callback zombie start timeout cgi:%_, cmdid:%_, taskId:%_, err(%_, %_), cost:%_",
                   it->task.cgi, it->task.cmdId, it->task.taskId, kEctLocal, kEctLocalTaskTimeout,
                   cur_time - it->save_time);
            CallResultHook(kEctLocal, kEctLocalTaskTimeout, kTaskFailHandleTaskEnd, it->task,
                          (unsigned int) (cur_time - it->save_time));
        } else {
            xinfo2(TSF"task start zombie cgi:%_, cmdid:%_, taskId:%_,", it->task.cgi, it->task.cmdId,
                   it->task.taskId);
            it->task.totalTimetout -= (cur_time - it->save_time);
            StartTaskHook(it->task);
        }
    }
}

void ZombieTaskManager::__TimerChecker() {
    xassert2(CallResultHook);

    std::list<ZombieTask> &lsttask = mTaskList;
    uint64_t cur_time = ::gettickcount();
    uint64_t netCoreLastStartTaskTime = mLastTaskTime;

    for (std::list<ZombieTask>::iterator it = lsttask.begin(); it != lsttask.end();) {
        if ((cur_time - it->save_time) >= (uint64_t) it->task.totalTimetout) {
            xinfo2(TSF"task end callback zombie timeout cgi:%_, cmdid:%_, taskId:%_, err(%_, %_), cost:%_",
                   it->task.cgi, it->task.cmdId, it->task.taskId, kEctLocal, kEctLocalTaskTimeout,
                   cur_time - it->save_time);
            CallResultHook(kEctLocal, kEctLocalTaskTimeout, kTaskFailHandleTaskEnd, it->task,
                          (unsigned int) (cur_time - it->save_time));
            it = lsttask.erase(it);
        } else if ((cur_time - it->save_time) >= RETRY_INTERVAL &&
                   (cur_time - netCoreLastStartTaskTime) >= RETRY_INTERVAL) {
            xinfo2(TSF"task start zombie cgi:%_, cmdid:%_, taskId:%_,", it->task.cgi, it->task.cmdId,
                   it->task.taskId);
            it->task.totalTimetout -= (cur_time - it->save_time);
            StartTaskHook(it->task);
            it = lsttask.erase(it);
        } else {
            ++it;
        }
    }

    if (lsttask.empty())
        MessageQueue::CancelMessage(mAsyncReg.Get(), (MessageQueue::MessageTitle_t) this);
}
