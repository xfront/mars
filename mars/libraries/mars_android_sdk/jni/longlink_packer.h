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
 * longlink_packer.h
 *
 *  Created on: 2012-7-18
 *      Author: yerungui
 */

#ifndef STN_SRC_LONGLINK_PACKER_H_
#define STN_SRC_LONGLINK_PACKER_H_

#include <stdlib.h>
#include <stdint.h>

#define LONGLINK_UNPACK_CONTINUE (-2)
#define LONGLINK_UNPACK_FALSE (-1)
#define LONGLINK_UNPACK_OK (0)

//for HTTP2
#define LONGLINK_UNPACK_STREAM_END LONGLINK_UNPACK_OK
#define LONGLINK_UNPACK_STREAM_PACKAGE (1)

#ifndef __cplusplus
#error "support cpp only"
#endif

class AutoBuffer;

namespace mars {
namespace stn {

class longlink_tracker {
public:
    static longlink_tracker *(*Create)();

public:
    virtual ~longlink_tracker() {};
};

/**
 * package the request data
 * cmdId: business identifier
 * seq: task id
 * _raw: business send buffer
 * bufOut: business send buffer + request header
 */
extern void (*longlink_pack)(uint32_t cmdId, uint32_t seq, const AutoBuffer &body, const AutoBuffer &bufExt,
                             AutoBuffer &bufOut, longlink_tracker *tracker);

/**
 * unpackage the response data
 * bufOut: data received from server
 * cmdId: business identifier
 * seq: task id
 * packageLen:
 * body: business receive buffer
 * return: 0 if unpackage succ
 */
extern int
(*longlink_unpack)(const AutoBuffer &bufOut, uint32_t &cmdId, uint32_t &seq, size_t &packageLen, AutoBuffer &body,
                   AutoBuffer &bufExt, longlink_tracker *tracker);

//heartbeat signal to keep longlink network alive
extern uint32_t (*longlink_noop_cmdid)();

extern bool (*longlink_noop_isresp)(uint32_t taskId, uint32_t cmdId, uint32_t recvSeq, const AutoBuffer &body,
                                    const AutoBuffer &bufExt);

extern uint32_t (*signal_keep_cmdid)();

extern void (*longlink_noop_req_body)(AutoBuffer &body, AutoBuffer &bufExt);

extern void (*longlink_noop_resp_body)(const AutoBuffer &body, const AutoBuffer &bufExt);

extern uint32_t (*longlink_noop_interval)();

extern bool (*longlink_complexconnect_need_verify)();

/**
 * return: whether the received data is pushing from server or not
 */
extern bool (*longlink_ispush)(uint32_t cmdId, uint32_t taskId, const AutoBuffer &body, const AutoBuffer &bufExt);

extern bool
(*longlink_identify_isresp)(uint32_t sentSeq, uint32_t cmdId, uint32_t recvSeq, const AutoBuffer &body,
                            const AutoBuffer &bufExt);

}
}
#endif // STN_SRC_LONGLINKPACKER_H_
