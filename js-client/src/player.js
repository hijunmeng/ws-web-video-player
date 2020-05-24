

function Player() {
    this.wsUrl = null;
    this.playUrl = null;
    this.video=null;
    this.codecMIME='video/mp4; codecs="avc1.4d4033, mp4a.40.2"';
    this.sourceBuffer=null;
    this.mediaSource=null;
    this.websocket=null;


}

Player.prototype.play = function (wsUrl, playUrl,video) {
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

    if (!video) {
        console.log("[ER] playVideo error, video empty.");
        return -1;
    }
    this.video = video;

    this.initWebsocket();
}

Player.prototype.stop = function () {
 //停止送流后，就去关闭解码器
 if(this.websocket){
    this.websocket.send("{\"request\":\"close_url\",\"serial\":111}");
    
}
}


Player.prototype.initMse=function(){
    console.log("init mse");
    if ('MediaSource' in window && MediaSource.isTypeSupported(this.codecMIME)) {
        this.mediaSource = new MediaSource();

        //console.log(mediaSource.readyState); // closed
        this.video.src = URL.createObjectURL(this.mediaSource);
        this.mediaSource.addEventListener('sourceopen',this.onMediaSourceOpen.bind(this));
      } else {
        console.error('Unsupported MIME type or codec: ', this.codecMIME);
      }
   
}

Player.prototype.onMediaSourceOpen=function(e){
    console.log("enter sourceOpen");
    let self=this;

    this.sourceBuffer = this.mediaSource.addSourceBuffer(this.codecMIME);

    this.sourceBuffer.addEventListener('updateend', function (_) {
        self.mediaSource.endOfStream();
        self.video.play();
        //console.log(mediaSource.readyState); // ended
      });
}

Player.prototype.handleFmp4Stream=function(arrayBuffer){
    if( this.sourceBuffer){
        this.sourceBuffer.appendBuffer(new Uint8Array(arrayBuffer));
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
        if(self.websocket){
            self.websocket = null;
        }
        
    }

    // 接受到信息
    this.websocket.onmessage = function (e) {

        if (e.data instanceof ArrayBuffer) {//接收二进制流数据
            console.log("ArrayBuffer:byteLength=" + e.data.byteLength);
            self.handleFmp4Stream(e.data);

        } else {
            console.log("String:data=" + e.data);
            let res = JSON.parse(e.data);
            if (res.response == "open_url") {
                if (res.code == 0) {
                    //成功则开始打开解码器,打开解码器成功后则可以请求流
                    self.codecMIME=res.mime;
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

