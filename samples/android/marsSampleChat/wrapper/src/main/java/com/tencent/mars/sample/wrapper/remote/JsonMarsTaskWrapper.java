package com.tencent.mars.sample.wrapper.remote;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import com.tencent.mars.stn.StnLogic;
import com.tencent.mars.xlog.Log;
import com.tencent.mars.sample.utils.print.MemoryDump;

/**
 * MarsTaskWrapper using json encoding
 * <p></p>
 * Created by zhaoyuan on 16/2/29.
 */
public abstract class JsonMarsTaskWrapper extends AbstractTaskWrapper {

    private static final String TAG = "Mars.Sample.JsonMarsTaskWrapper";

    protected JsonObject mRequest;
    protected JsonObject mResponse;

    public JsonMarsTaskWrapper(JsonObject req, JsonObject resp) {
        super();

        this.mRequest = req;
        this.mResponse = resp;
    }

    @Override
    public byte[] req2buf() {
        try {
            onPreEncode(mRequest);
            final byte[] flatArray = mRequest.toString().getBytes("utf-8");
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
            mResponse = new JsonParser().parse(new String(buf, "utf-8")).getAsJsonObject();
            onPostDecode(mResponse);
            return StnLogic.RESP_FAIL_HANDLE_NORMAL;

        } catch (Exception e) {
            Log.e(TAG, "%s", e);
        }

        return StnLogic.RESP_FAIL_HANDLE_TASK_END;
    }

    public abstract void onPreEncode(JsonObject request);

    public abstract void onPostDecode(JsonObject response);
}
