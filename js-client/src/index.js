
let video=document.getElementById("video");
let playBtn=document.getElementById("playBtn");
let stopBtn=document.getElementById("stopBtn");

let playUrl="rtsp://192.168.1.102:8554/aaa";
playUrl="rtsp://127.0.0.1/videos/test.264";
playUrl="rtsp://127.0.0.1/videos/tc10.264";
let wsUrl="ws://127.0.0.1:9002";

let player=new Player();

playBtn.addEventListener("click",function(){
    player.play(wsUrl,playUrl,video);
});
stopBtn.addEventListener("click",function(){
    player.stop();
});
