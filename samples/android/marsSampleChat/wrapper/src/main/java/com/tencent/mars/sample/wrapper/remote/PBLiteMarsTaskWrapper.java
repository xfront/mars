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

package com.tencent.mars.sample.wrapper.remote;

import com.google.protobuf.CodedOutputStream;
import com.google.protobuf.MessageLite;
import com.tencent.mars.stn.StnLogic;
import com.tencent.mars.xlog.Log;
import com.tencent.mars.sample.utils.print.MemoryDump;

/**
 * MarsTaskWrapper using nano protocol buffer encoding
 * <p></p>
 * Created by zhaoyuan on 16/2/29.
 */
public abstract class PBLiteMarsTaskWrapper<T extends MessageLite.Builder, R extends MessageLite.Builder> extends AbstractTaskWrapper {

    private static final String TAG = "Mars.Sample.NanoMarsTaskWrapper";

    protected T mRequest;
    protected R mResponse;

    public PBLiteMarsTaskWrapper(T req, R resp) {
        super();

        this.mRequest = req;
        this.mResponse = resp;
    }

    @Override
    public byte[] req2buf() {
        try {
            onPreEncode(mRequest);
            MessageLite msg = mRequest.build();
            final byte[] flatArray = new byte[msg.getSerializedSize()];
            final CodedOutputStream output = CodedOutputStream.newInstance(flatArray);
            msg.writeTo(output);

            Log.d(TAG, "encoded request to buffer, [%s]", MemoryDump.dumpHex(flatArray));

            return flatArray;

        } catch (Exception e) {
            e.printStackTrace();
        }

        return new byte[0];
    }

    @Override
    public int buf2resp(byte[] buf) {
        try {
            Log.d(TAG, "decode response buffer, [%s]", MemoryDump.dumpHex(buf));

            mResponse.mergeFrom(buf);
            onPostDecode(mResponse);
            return StnLogic.RESP_FAIL_HANDLE_NORMAL;

        } catch (Exception e) {
            Log.e(TAG, "%s", e);
        }

        return StnLogic.RESP_FAIL_HANDLE_TASK_END;
    }

    public abstract void onPreEncode(T request);

    public abstract void onPostDecode(R response);
}
