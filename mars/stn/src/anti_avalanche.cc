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
 * anti_avalanche.cc
 *
 *  Created on: 2012-9-3
 *      Author: yerungui
 */

#include "anti_avalanche.h"

#include "mars/baseevent/active_logic.h"
#include "mars/comm/platform_comm.h"
#include "mars/comm/singleton.h"
#include "mars/comm/xlogger/xlogger.h"
#include "mars/stn/stn.h"

#include "flow_limit.h"
#include "frequency_limit.h"

using namespace mars::stn;

AntiAvalanche::AntiAvalanche(bool isActive)
        : mFrequencyLimit(new FrequencyLimit())
        , mFlowLimit(new FlowLimit((isActive))) {

}

AntiAvalanche::~AntiAvalanche() {
    delete mFlowLimit;
    delete mFrequencyLimit;
}

bool AntiAvalanche::Check(const Task &task, const void *buffer, int len) {
    xverbose_function();

    unsigned int span = 0;
    if (!mFrequencyLimit->Check(task, buffer, len, span)) {
        ReportTaskLimited(kFrequencyLimit, task, span);
        return false;
    }

    if (kMobile == getNetInfo() && !mFlowLimit->Check(task, buffer, len)) {
        ReportTaskLimited(kFlowLimit, task, (unsigned int &) len);
        return false;
    }

    return true;
}

void AntiAvalanche::OnSignalActive(bool isActive) {
    mFlowLimit->Active(isActive);
}
