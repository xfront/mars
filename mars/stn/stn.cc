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
//  stn.cpp
//  stn
//
//  Created by yanguoyue on 16/3/3.
//  Copyright © 2016年 Tencent. All rights reserved.
//

#include "mars/stn/stn.h"

#include "mars/comm/thread/atomic_oper.h"


namespace mars{
    namespace stn{
        
static uint32_t gs_taskid = 1;
Task::Task():Task(atomic_inc32(&gs_taskid)) {}
        
Task::Task(uint32_t taskId0) {
    
    taskId = taskId0;
    cmdId = 0;
    channelId = 0;
    channelSelect = 0;
    
    sendOnly = false;
    needAuthed = false;
    limitFlow = true;
    limitFrequency = true;
    
    channelStrategy = kChannelNormalStrategy;
    networkStatusSensitive = false;
    priority = kTaskPriorityNormal;
    
    retryCount = -1;
    serverProcessCost = -1;
    totalTimetout = -1;
    userContext = NULL;

}
        
    }
}
