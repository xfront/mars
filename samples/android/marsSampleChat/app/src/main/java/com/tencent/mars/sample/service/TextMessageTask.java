/*
 * Tencent is pleased to support the open source community by making Mars available.
 * Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.
 *
 * Licensed under the MIT License (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://opensource.org/licenses/MIT
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License is
 * distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.tencent.mars.sample.service;

import com.tencent.mars.sample.SampleApplicaton;
import com.tencent.mars.sample.chat.proto.Chat;
import com.tencent.mars.sample.proto.Main;
import com.tencent.mars.sample.wrapper.TaskProperty;
import com.tencent.mars.sample.wrapper.remote.PBLiteMarsTaskWrapper;

import io.reactivex.Observable;
import io.reactivex.subjects.PublishSubject;

/**
 * Text messaging task
 * <p/>
 * Created by zhaoyuan on 16/2/29.
 */
@TaskProperty(
        host = "marsopen.cn",
        path = "/mars/sendmessage",
        cmdID = Main.CmdID.CMD_ID_SEND_MESSAGE_VALUE,
        longChannelSupport = true,
        shortChannelSupport = false
)
public class TextMessageTask extends PBLiteMarsTaskWrapper<Chat.SendMessageRequest.Builder, Chat.SendMessageResponse.Builder> {

    private PublishSubject<Chat.SendMessageResponse> mSubject = PublishSubject.create();

    public Observable<Chat.SendMessageResponse> observe() {
        return mSubject;
    }

    public TextMessageTask(String topicName, String text) {
        super(Chat.SendMessageRequest.newBuilder(), Chat.SendMessageResponse.newBuilder());

        mRequest.setAccessToken("test_token");
        mRequest.setFrom(SampleApplicaton.accountInfo.userName);
        mRequest.setTo("all");
        mRequest.setText(text);
        mRequest.setTopic(topicName);
    }

    @Override
    public void onPreEncode(Chat.SendMessageRequest.Builder request) {
        // TODO: Not thread-safe here
    }

    @Override
    public void onPostDecode(Chat.SendMessageResponse.Builder response) {

    }

    @Override
    public void onTaskEnd(int errType, int errCode) {
        if (errCode == 0) {
            mSubject.onNext(mResponse.build());
            mSubject.onComplete();
        } else {
            mSubject.onError(new Exception(String.format("errType=%d, errCode=%d", errType, errCode)));
        }
    }

}
