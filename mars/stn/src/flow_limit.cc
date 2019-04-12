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
 * flow_limit.cc
 *
 *  Created on: 2012-9-3
 *      Author: yerungui
 */

#include "flow_limit.h"

#include <algorithm>

#include "mars/comm/xlogger/xlogger.h"
#include "mars/comm/time_utils.h"
#include "mars/stn/stn.h"

#if true
static const int kInactiveSpeed = (2 * 1024 * 1024 / 3600);
static const int kActiveSpeed = (8 * 1024 * 1024 / 3600);
static const int kInactiveMinvol = (6 * 1024 * 1024);
static const int kMaxVol = (8 * 1024 * 1024);
#else
static const int kInactiveSpeed = (1);
static const int kActiveSpeed = (3);
static const int kInactiveMinvol = (1500);
static const int kMaxVol = (2 * 1024);
#endif

using namespace mars::stn;

FlowLimit::FlowLimit(bool isActive)
        : mFunnelSpeed(isActive ? kActiveSpeed : kInactiveSpeed)
        , mCurFunnelVol(0)
        , mTimeLastFlowComputer(::gettickcount()) {

}

FlowLimit::~FlowLimit() {

}

bool FlowLimit::Check(const mars::stn::Task &task, const void *buffer, int len) {
    xverbose_function();

    if (!task.limitFlow) {
        return true;
    }

    __FlashCurVol();

    if (mCurFunnelVol + len > kMaxVol) {
        xerror2(TSF"Task Info: ptr=%_, cmdid=%_, need_authed=%_, cgi:%_, channelSelect=%_, limit_flow=%_, mCurFunnelVol(%_)+len(%_)=%_,MAX_VOL:%_ ",
                &task, task.cmdId, task.needAuthed, task.cgi, task.channelSelect, task.limitFlow,
                mCurFunnelVol + len, mCurFunnelVol, len, mCurFunnelVol + len, kMaxVol);

        return false;
    }

    mCurFunnelVol += len;
    return true;
}

void FlowLimit::Active(bool isActive) {
    __FlashCurVol();

    if (!isActive) {
        xdebug2(TSF"iCurFunnelVol=%0, INACTIVE_MIN_VOL=%1", mCurFunnelVol, kInactiveMinvol);

        if (mCurFunnelVol > kInactiveMinvol)
            mCurFunnelVol = kInactiveMinvol;
    }

    mFunnelSpeed = isActive ? kActiveSpeed : kInactiveSpeed;
    xdebug2(TSF"Active:%0, iFunnelSpeed=%1", isActive, mFunnelSpeed);
}

void FlowLimit::__FlashCurVol() {
    uint64_t timeCur = ::gettickcount();
    xassert2(timeCur >= mTimeLastFlowComputer, TSF"%_, %_", timeCur, mTimeLastFlowComputer);
    uint64_t interval = (timeCur - mTimeLastFlowComputer) / 1000;

    if (0 == interval) return;

    xdebug2(TSF"iCurFunnelVol=%0, iFunnelSpeed=%1, interval=%2", mCurFunnelVol, mFunnelSpeed, interval);
    mCurFunnelVol -= interval * mFunnelSpeed;
    mCurFunnelVol = std::max(0, mCurFunnelVol);
    xdebug2(TSF"iCurFunnelVol=%0", mCurFunnelVol);

    mTimeLastFlowComputer = timeCur;
}
