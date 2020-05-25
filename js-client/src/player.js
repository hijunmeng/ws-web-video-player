function Player() {
    this.wsUrl = null; //websocket服务地址
    this.playUrl = null;//实时流播放地址
    this.videoElement = null;//video元素


    this.codecMIME = 'video/mp4; codecs="avc1.4d4033, mp4a.40.2"';//编解码mime，用于判断MediaSource是否支持
    this.sourceBuffer = null;//存放fmp4数据
    this.mediaSource = null;
    this.websocket = null; //websocket句柄

    this.cacheQueue = [];//缓存fmp4数据队列
    this.canAppend = false;//是否可以追加fmp4数据的标识


}
////////////////////////////////////////////////api//////////////////////////////////////////////
Player.prototype.play = function (wsUrl, playUrl, videoElement) {
    if (!playUrl) {
        console.log("[ER] playVideo error, playUrl empty.");
        return -1;
    }
    this.playUrl = playUrl;

    if (!wsUrl) {
        console.log("[ER] playVideo error, wsUrl empty.");
        return -1;
    }
    this.wsUrl = wsUrl;

    if (!videoElement) {
        console.log("[ER] playVideo error, videoElement empty.");
        return -1;
    }
    this.videoElement = videoElement;

    this.resetParams();

    this.initWebsocket();
}

Player.prototype.stop = function () {
    //停止送流后，就去关闭解码器
    if (this.websocket) {
        this.websocket.send("{\"request\":\"close_url\",\"serial\":111}");

    }
    this.sourceBuffer.abort();
    this.mediaSource.endOfStream();

    this.resetParams();

}


///////////////////////////////////on-event///////////////////////////////////////////////////////////////
Player.prototype.onCanPlay = function () {
    console.log("[on_event]:onCanPlay");
    this.videoElement.play();
}

Player.prototype.onMediaSourceOpen = function (e) {
    console.log("[on_event]:onMediaSourceOpen");
    let self = this;
    // let mediaSource = e.target;
    console.log("codec mime is " + this.codecMIME);
    this.canAppend = false;
    this.sourceBuffer = this.mediaSource.addSourceBuffer(this.codecMIME);

    this.sourceBuffer.addEventListener('updateend', this.onUpdateend.bind(this));//必须监听到updateend才可append新的数据
    this.sourceBuffer.onerror = function (e) {
        console.error("error:" + e);
        //产生错误后则sourceBuffer，mediaSource都会不可用了
    };
    this.sourceBuffer.onabort = function (e) {
        console.error("abort:" + e);
    };
    //首次追加数据
    this.appendBuffer();

}

Player.prototype.onMediaSourceClosed = function (e) {
    console.log("[on_event]:onMediaSourceClosed");
}
Player.prototype.onMediaSourceEnded = function (e) {
    console.log("[on_event]:onMediaSourceEnded");
    this.videoElement.pause();
}

Player.prototype.onUpdateend = function (_) {
    console.log("[on_event]:onUpdateend:updating=" + this.sourceBuffer.updating);
    this.appendBuffer();
}
//////////////////////////////////////private///////////////////////////////////////////
Player.prototype.resetParams=function(){
    this.sourceBuffer = null;
    this.mediaSource = null;
    this.cacheQueue=[];
    this.canAppend=false;
}

Player.prototype.initMse = function () {
    console.log("init mse");
    if ('MediaSource' in window && MediaSource.isTypeSupported(this.codecMIME)) {

        this.mediaSource = new MediaSource();
        console.log(this.mediaSource.readyState); // closed
        this.mediaSource.addEventListener('sourceopen', this.onMediaSourceOpen.bind(this));//绑定到媒体元素后开始触发
        this.mediaSource.addEventListener('sourceclosed', this.onMediaSourceClosed.bind(this));//sourceclosed 未绑定到媒体元素后开始触发
        this.mediaSource.addEventListener('sourceended', this.onMediaSourceEnded.bind(this)); //sourceended 所有数据接收完成后触发

        this.videoElement.src = URL.createObjectURL(this.mediaSource);
        this.videoElement.addEventListener("canplay", this.onCanPlay.bind(this));

        console.log("开始请求流");
        let json = { request: "start_take_stream", serial: 111 };
        this.websocket.send(JSON.stringify(json));
    } else {
        console.error('Unsupported MIME type or codec: ', this.codecMIME);
    }

}


//根据缓存队列情况决定是否appendBuffer
Player.prototype.appendBuffer = function () {
    console.log("enter appendBuffer cachequeue len=" + this.cacheQueue.length);
    this.canAppend = false;
    if (this.cacheQueue.length) {
        if (this.sourceBuffer.updating) {
            this.canAppend = true;
        } else {
            this.canAppend = false;
            let buf = this.cacheQueue.shift();
            console.log("shift " + buf.byteLength);
            this.sourceBuffer.appendBuffer(buf);
            buf = null;
        }
    } else {
        this.canAppend = true;
    }
}

Player.prototype.handleFmp4Stream = function (arrayBuffer) {

    // console.log("push " + arrayBuffer.byteLength);
    this.cacheQueue.push(new Uint8Array(arrayBuffer));
    if (this.canAppend) {
        this.appendBuffer();
    }
}

Player.prototype.initWebsocket = function () {
    let self = this;
    this.websocket = new WebSocket(this.wsUrl);
    this.websocket.binaryType = "arraybuffer";//设置接收二进制类型，默认是blob

    this.websocket.onerror = function (event) {
        console.error("WebSocket error observed:", event);
        self.websocket = null;
    };
    // 打开websocket
    this.websocket.onopen = function (event) {
        console.log('websocket open');
        //打开之后则发送请求，结果在响应里处理
        let json = { request: "open_url", content: self.playUrl, serial: 111 };
        self.websocket.send(JSON.stringify(json));

    }
    // 结束websocket
    this.websocket.onclose = function (event) {
        console.log('websocket close');
        if (self.websocket) {
            self.websocket = null;
        }

    }

    // 接受到信息
    this.websocket.onmessage = function (e) {

        if (e.data instanceof ArrayBuffer) {//接收二进制流数据
            //console.log("ArrayBuffer:byteLength=" + e.data.byteLength);
            self.handleFmp4Stream(e.data);

        } else {
            console.log("String:data=" + e.data);
            let res = JSON.parse(e.data);
            if (res.response == "open_url") {
                if (res.code == 0) {
                    //成功则开始打开解码器,打开解码器成功后则可以请求流
                    self.codecMIME = res.mime;
                    self.initMse();
                } else {
                    console.error("open_url failed:" + res.msg);
                }

            } else if (res.response == "close_url") {
                if (res.code == 0) {
                    console.log("close_url success");
                }
                self.websocket.close();


            } else if (res.response == "start_take_stream") {
                if (res.code == 0) {
                    console.log("启动取流成功");//流数据会以二进制形式发送过来
                } else {
                    console.log("启动取流失败:" + res.msg);
                }
            }
        }

    }

}