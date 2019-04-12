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
 * longlink_packer.cc
 *
 *  Created on: 2012-7-18
 *      Author: yerungui, caoshaokun
 */

#include "longlink_packer.h"

#ifndef WIN32

#include <arpa/inet.h>

#endif // !WIN32

#ifdef __APPLE__
#include "mars/xlog/xlogger.h"
#else

#include "mars/comm/xlogger/xlogger.h"

#endif

#include "mars/comm/autobuffer.h"
#include "mars/stn/stn.h"

static uint32_t sg_client_version = 0;

#pragma pack(push, 1)
struct __STNetMsgXpHeader {
    uint32_t head_length;
    uint32_t client_version;
    uint32_t cmdid;
    uint32_t seq;
    uint32_t body_length;
};
#pragma pack(pop)

namespace mars {
namespace stn {
longlink_tracker *(*longlink_tracker::Create)()
= []() {
    return new longlink_tracker;
};

void SetClientVersion(uint32_t clientVersion) {
    sg_client_version = clientVersion;
}


static int
__unpack_test(const void *bufOut, size_t packedLen, uint32_t &cmdId, uint32_t &seq, size_t &packageLen,
              size_t &_body_len) {
    __STNetMsgXpHeader st = {0};
    if (packedLen < sizeof(__STNetMsgXpHeader)) {
        packageLen = 0;
        _body_len = 0;
        return LONGLINK_UNPACK_CONTINUE;
    }

    memcpy(&st, bufOut, sizeof(__STNetMsgXpHeader));

    uint32_t head_len = ntohl(st.head_length);
    uint32_t client_version = ntohl(st.client_version);
    if (client_version != sg_client_version) {
        packageLen = 0;
        _body_len = 0;
        return LONGLINK_UNPACK_FALSE;
    }
    cmdId = ntohl(st.cmdid);
    seq = ntohl(st.seq);
    _body_len = ntohl(st.body_length);
    packageLen = head_len + _body_len;

    if (packageLen > 1024 * 1024) { return LONGLINK_UNPACK_FALSE; }
    if (packageLen > packedLen) { return LONGLINK_UNPACK_CONTINUE; }

    return LONGLINK_UNPACK_OK;
}

void (*longlink_pack)(uint32_t cmdId, uint32_t seq, const AutoBuffer &body, const AutoBuffer &bufExt,
                      AutoBuffer &bufOut, longlink_tracker *tracker)
= [](uint32_t cmdId, uint32_t seq, const AutoBuffer &body, const AutoBuffer &bufExt, AutoBuffer &bufOut,
     longlink_tracker *tracker) {
    __STNetMsgXpHeader st = {0};
    st.head_length = htonl(sizeof(__STNetMsgXpHeader));
    st.client_version = htonl(sg_client_version);
    st.cmdid = htonl(cmdId);
    st.seq = htonl(seq);
    st.body_length = htonl(body.Length());

    bufOut.AllocWrite(sizeof(__STNetMsgXpHeader) + body.Length());
    bufOut.Write(&st, sizeof(st));

    if (NULL != body.Ptr()) bufOut.Write(body.Ptr(), body.Length());

    bufOut.Seek(0, AutoBuffer::ESeekStart);
};


int
(*longlink_unpack)(const AutoBuffer &bufOut, uint32_t &cmdId, uint32_t &seq, size_t &packageLen, AutoBuffer &body,
                   AutoBuffer &bufExt, longlink_tracker *tracker)
= [](const AutoBuffer &bufOut, uint32_t &cmdId, uint32_t &seq, size_t &packageLen, AutoBuffer &body,
     AutoBuffer &bufExt, longlink_tracker *tracker) {
    size_t body_len = 0;
    int ret = __unpack_test(bufOut.Ptr(), bufOut.Length(), cmdId, seq, packageLen, body_len);

    if (LONGLINK_UNPACK_OK != ret) return ret;

    body.Write(AutoBuffer::ESeekCur, bufOut.Ptr(packageLen - body_len), body_len);

    return ret;
};


#define NOOP_CMDID 6
#define SIGNALKEEP_CMDID 243
#define PUSH_DATA_TASKID 0

uint32_t (*longlink_noop_cmdid)()
= []() -> uint32_t {
    return NOOP_CMDID;
};

bool (*longlink_noop_isresp)(uint32_t taskId, uint32_t cmdId, uint32_t recvSeq, const AutoBuffer &body,
                             const AutoBuffer &bufExt)
= [](uint32_t taskId, uint32_t cmdId, uint32_t recvSeq, const AutoBuffer &body, const AutoBuffer &bufExt) {
    return Task::kNoopTaskID == taskId && NOOP_CMDID == cmdId;
};

uint32_t (*signal_keep_cmdid)()
= []() -> uint32_t {
    return SIGNALKEEP_CMDID;
};

void (*longlink_noop_req_body)(AutoBuffer &body, AutoBuffer &bufExt)
= [](AutoBuffer &body, AutoBuffer &bufExt) {

};

void (*longlink_noop_resp_body)(const AutoBuffer &body, const AutoBuffer &bufExt)
= [](const AutoBuffer &body, const AutoBuffer &bufExt) {

};

uint32_t (*longlink_noop_interval)()
= []() -> uint32_t {
    return 0;
};

bool (*longlink_complexconnect_need_verify)()
= []() {
    return false;
};

bool (*longlink_ispush)(uint32_t cmdId, uint32_t taskId, const AutoBuffer &body, const AutoBuffer &bufExt)
= [](uint32_t cmdId, uint32_t taskId, const AutoBuffer &body, const AutoBuffer &bufExt) {
    return PUSH_DATA_TASKID == taskId;
};

bool (*longlink_identify_isresp)(uint32_t sentSeq, uint32_t cmdId, uint32_t recvSeq, const AutoBuffer &body,
                                 const AutoBuffer &bufExt)
= [](uint32_t sentSeq, uint32_t cmdId, uint32_t recvSeq, const AutoBuffer &body, const AutoBuffer &bufExt) {
    return sentSeq == recvSeq && 0 != sentSeq;
};

}
}
