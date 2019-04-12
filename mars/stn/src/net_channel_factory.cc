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
 * net_channel_factory.cc
 *
 *  Created on: Jul 19, 2016
 *      Author: wutianqiang
 */

#include "net_channel_factory.h"

#include "mars/comm/messagequeue/message_queue.h"
#include "mars/comm/xlogger/xlogger.h"

#include "longlink.h"
#include "shortlink.h"

namespace mars {
namespace stn {

namespace ShortLinkChannelFactory {


ShortLinkInterface *
(*Create)(const mq::MessageQueue_t &msgQueueId, NetSource &netSource, const Task &task, bool useProxy)
= [](const mq::MessageQueue_t &msgQueueId, NetSource &netSource, const Task &task,
     bool useProxy) -> ShortLinkInterface * {
    xdebug2(TSF"use weak func Create");
    return new ShortLink(msgQueueId, netSource, task, useProxy);
};

void (*Destory)(ShortLinkInterface *_short_link_channel)
= [](ShortLinkInterface *_short_link_channel) {
    delete _short_link_channel;
    _short_link_channel = NULL;
};

}

namespace LongLinkChannelFactory {

LongLink *(*Create)(const mq::MessageQueue_t &msgQueueId, NetSource &netSource)
= [](const mq::MessageQueue_t &msgQueueId, NetSource &netSource) {
    return new LongLink(msgQueueId, netSource);
};

void (*Destory)(LongLink *_long_link_channel)
= [](LongLink *_long_link_channel) {
    delete _long_link_channel;
    _long_link_channel = NULL;
};

}
}
}
