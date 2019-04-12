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
//  longlink_identify_checker.cc
//  PublicComponent
//
//  Created by liucan on 13-12-6.
//  Copyright (c) 2013 Tencent. All rights reserved.
//

#include "longlink_identify_checker.h"

#include "mars/comm/xlogger/xlogger.h"
#include "mars/stn/stn.h"

#include "stn/proto/longlink_packer.h"

using namespace mars::stn;

LongLinkIdentifyChecker::LongLinkIdentifyChecker()
        : mHasChecked(false)
        , mCmdId(0)
        , mTaskId(0) {

}

LongLinkIdentifyChecker::~LongLinkIdentifyChecker() {}

bool LongLinkIdentifyChecker::GetIdentifyBuffer(AutoBuffer &buffer, uint32_t &cmdId) {
    if (mHasChecked) return false;

    mHashCodeBuffer.Reset();
    buffer.Reset();

    IdentifyMode mode = (IdentifyMode) GetLonglinkIdentifyCheckBuffer(buffer, mHashCodeBuffer, (int &) cmdId);

    switch (mode) {
        case kCheckNever: {
            mHasChecked = true;
        }
            break;
        case kCheckNext: {
            mHasChecked = false;
        }
            break;
        case kCheckNow: {
            mCmdId = cmdId;
            return true;
        }
            break;
        default:
            xassert2(false);
    }

    return false;
}

void LongLinkIdentifyChecker::SetID(uint32_t taskId) { mTaskId = taskId; }

bool LongLinkIdentifyChecker::IsIdentifyResp(uint32_t cmdId, uint32_t taskId, const AutoBuffer &buffer,
                                             const AutoBuffer &bufExt) const {
    return longlink_identify_isresp(mTaskId, cmdId, taskId, buffer, bufExt);
}

bool LongLinkIdentifyChecker::OnIdentifyResp(AutoBuffer &buffer) {
    xinfo2(TSF"identifycheck(synccheck) resp");
    bool ret = ::OnLonglinkIdentifyResponse(buffer, mHashCodeBuffer);
    mTaskId = 0;
    if (ret) {
        mHasChecked = true;
        return true;
    }
    return false;
}


void LongLinkIdentifyChecker::Reset() {
    mHasChecked = false;
    mTaskId = 0;
    mCmdId = 0;
    mHashCodeBuffer.Reset();
}
