#include "TakeStream.h"

//ffmpeg内部日志的回调，这个对于开发阶段非常有用
static void ffmpegLogCallback(void* ptr, int level, const char* fmt, va_list vl) {
	static int printPrefix = 1;
	static int count = 0;
	static char prev[1024] = { 0 };
	char line[1024] = { 0 };
	static int is_atty;
	AVClass* avc = ptr ? *(AVClass**)ptr : NULL;
	if (level > AV_LOG_DEBUG) {
		return;
	}

	line[0] = 0;

	if (printPrefix && avc) {
		if (avc->parent_log_context_offset) {
			AVClass** parent = *(AVClass***)(((uint8_t*)ptr) + avc->parent_log_context_offset);
			if (parent && *parent) {
				snprintf(line, sizeof(line), "[%s @ %p] ", (*parent)->item_name(parent), parent);
			}
		}
		snprintf(line + strlen(line), sizeof(line) - strlen(line), "[%s @ %p] ", avc->item_name(ptr), ptr);
	}

	vsnprintf(line + strlen(line), sizeof(line) - strlen(line), fmt, vl);
	line[strlen(line) + 1] = 0;

	printf("%s\n", line);
}

static int interruptCallback(void* opaque) {
	Runner* r = (Runner*)opaque;
	if (r->lastTime > 0) {
		if (time(NULL) - r->lastTime > OPEN_TIMEOUT_S) {
			// 等待超过8s则中断
			return 1;//返回1表示中断
		}
	}

	return 0;
}

//从extradata里获取视频的mime
static std::string GetMIME(uint8_t* data, int len)
{
	int n = 0;
	if (data[0] == 0)
	{
		while (n + 3 < len)
		{
			if ((data[n] == 0) & (data[n + 1] == 0) & (data[n + 2] == 1))
			{
				n += 3;
				break;
			}
			else
				n++;
		}
	}
	n += 1;
	if (n + 3 > len) {
		return "";
	}
	char mime[10] = { 0 };
	//sprintf_s(mime, "%.2x%.2x%.2x", data[n], data[n + 1], data[n + 2]);
	sprintf_s(mime, "%02x%02x%02x", data[n], data[n + 1], data[n + 2]);//输出3个16进制数，共6位
	return std::string(mime);
}
static void writeFile(unsigned char* buff, int size) {
	FILE* fp = NULL;
	fopen_s(&fp, "./test.hevc", "ab+");
	if (fp != nullptr && buff != nullptr && size != 0) {
		fwrite(buff, size, 1, fp);
	}
	fclose(fp);
}

static void writeFile(char* filename, unsigned char* buff, int size) {
	FILE* fp = NULL;
	fopen_s(&fp, filename, "ab+");
	if (fp != nullptr && buff != nullptr && size != 0) {
		fwrite(buff, size, 1, fp);
	}
	fclose(fp);
}
static int write_packet(void* opaque, uint8_t* buf, int buf_size) {


	printf("write_packet=%d", buf_size);

	TakeStream* takeStream = (TakeStream*)opaque;
	takeStream->handleFmp4Data(buf, buf_size);
	//writeFile((char*)"./test.fmp4", buf, buf_size);
	return buf_size;
}

///////////////////////////////////////////////////////////////////

TakeStream::TakeStream()
{

	avformat_network_init();
	packet = av_packet_alloc();
	running = false;
	isOpen = false;
	hasAudio = false;
	videoStreamIdx = -1;
	audioStreamIdx = -1;
	outVideoStreamIdx = -1;
	outAudioStreamIdx = -1;
	this->packetCallback = NULL;
	this->packetCallbackFunction = NULL;
	this->fmp4Callback = NULL;
	this->fmp4CallbackFunction = NULL;

	takeSleepTimeMs = TAKE_SLEEP_TIME_MS;

	pbBuffer = (uint8_t*)av_malloc(PB_BUFFER_SIZE);

}
TakeStream::~TakeStream()
{
	close();
	std::lock_guard<std::mutex> lock(m_mutex);//这把锁主要是为了阻塞直到取流线程退出
	if (pbBuffer != NULL) {
		av_free(pbBuffer);
		pbBuffer = NULL;
	}
	if (packet != NULL) {
		av_packet_free(&packet);
		//packet = NULL;
		log(AV_LOG_INFO, "packet released.");
	}
	if (pFormatContext != NULL) {
		avformat_close_input(&pFormatContext);//内部会调用avformat_free_context
		pFormatContext = NULL;
		log(AV_LOG_INFO, "format context released.");
	}
	if (pOutFormatContext != NULL) {
		avformat_free_context(pOutFormatContext);
		pOutFormatContext = NULL;
	}
	log(AV_LOG_INFO, "TakeStream released.");


}
int TakeStream::handleFmp4Data(uint8_t* buf, int buf_size) {

	if (fmp4Callback) {
		fmp4Callback(packetCallbackUserData,buf, buf_size);
	}
	if (fmp4CallbackFunction) {
		fmp4CallbackFunction(packetCallbackUserData, buf, buf_size);
	}

	return buf_size;
}

void TakeStream::log(int level, const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	av_vlog(NULL, level, fmt, ap);
	printf("\n");
	va_end(ap);
}


int TakeStream::setPacketCallback(PacketCallback  packetCallback, void* userData)
{
	//av_log_set_callback(ffmpegLogCallback);
	this->packetCallback = packetCallback;
	this->packetCallbackUserData = userData;
	return 0;
}

int TakeStream::setPacketCallback(PacketCallbackFunction packetCallback, void* userData)
{
	//av_log_set_callback(ffmpegLogCallback);
	this->packetCallbackFunction = packetCallback;
	this->packetCallbackUserData = userData;
	return 0;
}
int TakeStream::setFmp4Callback(Fmp4Callback  fmp4Callback, void* userData)
{
	//av_log_set_callback(ffmpegLogCallback);
	this->fmp4Callback= fmp4Callback;
	this->packetCallbackUserData = userData;
	return 0;
}

int TakeStream::setFmp4Callback(Fmp4CallbackFunction fmp4Callback, void* userData)
{
	//av_log_set_callback(ffmpegLogCallback);
	this->fmp4CallbackFunction = fmp4Callback;
	this->packetCallbackUserData = userData;
	return 0;
}

int TakeStream::getVideoInfo(VideoInfo& videoInfo) {
	if (!isOpen) {
		log(AV_LOG_INFO, "has not open");
		return -1;
	}
	videoInfo.width = pVideoCodecParameters->width;
	videoInfo.height = pVideoCodecParameters->height;
	videoInfo.codeID = pVideoCodecParameters->codec_id;
	AVStream* st = pFormatContext->streams[videoStreamIdx];
	videoInfo.timebase = av_q2d(st->time_base);
	return 0;
}

int TakeStream::getAudioInfo(AudioInfo& audioInfo) {
	if (!hasAudio) {
		log(AV_LOG_INFO, "no audio");
		return -1;
	}
	audioInfo.channelLayout = pAudioCodecParameters->channel_layout;
	audioInfo.channels = pAudioCodecParameters->channels;
	audioInfo.codeID = pAudioCodecParameters->codec_id;
	audioInfo.sampleRate = pAudioCodecParameters->sample_rate;
	return 0;
}
char* TakeStream::getCodecMIME() {
	return codecMIME;
}

int TakeStream::open(char* url)
{
	if (url == nullptr) {
		return -1;
	}
	if (isOpen) {
		log(AV_LOG_INFO, "has open ,do not open again");
		return -1;
	}

	int ret = 0;


	log(AV_LOG_INFO, "avformat_alloc_context.");
	if (pFormatContext != NULL) {
		avformat_close_input(&pFormatContext);//内部会调用avformat_free_context
		pFormatContext = NULL;
	}
	pFormatContext = avformat_alloc_context();
	//必须设置超时，否则会导致长时间不返回
	Runner input_runner = { 0 };
	pFormatContext->interrupt_callback.callback = interruptCallback;
	pFormatContext->interrupt_callback.opaque = &input_runner;
	//设置查找时间以避免avformat_find_stream_info耗时过长
	pFormatContext->probesize = 100 * 1024;//默认是5000000
	pFormatContext->max_analyze_duration = 2 * AV_TIME_BASE;//2秒,默认是5秒

	log(AV_LOG_INFO, "avformat_open_input...");
	AVDictionary* options = NULL;
	av_dict_set(&options, "buffer_size", "1048576", 0);//524288=512KB  1048576=1MB
													   //av_dict_set(&options, "pkt_size", "524288", 0);//524288=512KB  1048576=1MB

	av_dict_set(&options, "rtsp_transport", "tcp", 0);
	av_dict_set(&options, "stimeout", "5000000", 0); //设置超时断开连接时间，单位微秒,这个可以防止tcp模式下av_read_frame一直阻塞不退出
													 //av_dict_set(&options, "rtsp_flags", "prefer_tcp", 0);//优先尝试tcp
	av_dict_set(&options, "bbb", "1048576", 0);//测试用

	input_runner.lastTime = time(NULL);
	ret = avformat_open_input(&pFormatContext, url, NULL, &options);
	if (ret != 0) {
		char err_info[32] = { 0 };
		av_strerror(ret, err_info, 32);
		log(AV_LOG_ERROR, "avformat_open_input failed %d %s.", ret, err_info);
		return -1;
	}
	log(AV_LOG_INFO, "avformat_open_input success.");
	AVDictionaryEntry* entry = NULL;
	while (entry = av_dict_get(options, "", entry, AV_DICT_IGNORE_SUFFIX)) {
		log(AV_LOG_INFO, "Option %s not recognized by the demuxer,and the count is %d \n", entry->key, av_dict_count(options));
	}


	av_dict_free(&options);



	ret = avformat_find_stream_info(pFormatContext, NULL);
	if (ret < 0) {
		log(AV_LOG_ERROR, "av_find_stream_info failed %d.", ret);
		return -1;
	}
	log(AV_LOG_INFO, "avformat_find_stream_info success.");


	ret = av_find_best_stream(pFormatContext, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (ret < 0) {
		log(AV_LOG_ERROR, "Could not find %s stream.", av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
		return -1;
	}
	videoStreamIdx = ret;

	AVStream* in_video_stream = pFormatContext->streams[videoStreamIdx];
	pVideoCodecParameters = in_video_stream->codecpar;
	log(AV_LOG_INFO, "VideoCodecParameters:videoCodecID=%d,width=%d,height=%d.",
		pVideoCodecParameters->codec_id,
		pVideoCodecParameters->width,
		pVideoCodecParameters->height);


	ret = av_find_best_stream(pFormatContext, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (ret < 0) {
		log(AV_LOG_ERROR, "Could not find %s stream.", av_get_media_type_string(AVMEDIA_TYPE_AUDIO));
		hasAudio = false;
	}
	else {
		hasAudio = true;
		audioStreamIdx = ret;
		AVStream* st = pFormatContext->streams[audioStreamIdx];
		pAudioCodecParameters = st->codecpar;
		log(AV_LOG_INFO, "AudioCodecParameters:audioCodecID=%d,sample_rate=%d,channels=%d,channel_layout=%d.",
			pAudioCodecParameters->codec_id,
			pAudioCodecParameters->sample_rate,
			pAudioCodecParameters->channels,
			pAudioCodecParameters->channel_layout

		);
	}


	ret = avformat_alloc_output_context2(&pOutFormatContext, NULL, "mp4", NULL);
	if (ret < 0) {//avformat_free_context()
		log(AV_LOG_ERROR, "avformat_alloc_output_context2 failed :ret=%d", ret);
		return ret;
	}

	pIOContext = avio_alloc_context(pbBuffer, PB_BUFFER_SIZE, 1, this, NULL, write_packet, NULL);
	if (!pIOContext) {
		log(AV_LOG_ERROR, "avio_alloc_context failed ");
		return -1;
	}
	pOutFormatContext->pb = pIOContext;
	pOutFormatContext->pb->write_flag = 1;
	pOutFormatContext->pb->seekable = 1;
	pOutFormatContext->flags = AVFMT_FLAG_CUSTOM_IO;
	pOutFormatContext->flags |= AVFMT_FLAG_FLUSH_PACKETS;
	pOutFormatContext->flags |= AVFMT_NOFILE;
	pOutFormatContext->flags |= AVFMT_FLAG_AUTO_BSF;
	pOutFormatContext->flags |= AVFMT_FLAG_NOBUFFER;


	//av_format_inject_global_side_data
	//视频
	AVStream* out_video_stream = avformat_new_stream(pOutFormatContext, NULL);
	if (!out_video_stream)
	{
		log(AV_LOG_ERROR, "avformat_new_stream failed ");
		return -1;
	}
	outVideoStreamIdx = out_video_stream->index;
	AVCodec* in_codec = avcodec_find_encoder(in_video_stream->codecpar->codec_id);
	AVCodecContext* codec_ctx = avcodec_alloc_context3(in_codec);
	codec_ctx->framerate = { 0,1 };
	ret = avcodec_parameters_to_context(codec_ctx, in_video_stream->codecpar);
	if (ret < 0)
	{
		log(AV_LOG_ERROR, "avcodec_parameters_to_context failed ");
		return -1;
	}
	codec_ctx->codec_tag = 0;

	if (pOutFormatContext->oformat->flags & AVFMT_GLOBALHEADER) {
		codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}
	ret = avcodec_parameters_from_context(out_video_stream->codecpar, codec_ctx);
	if (ret < 0)
	{
		log(AV_LOG_ERROR, "avcodec_parameters_from_context failed ");
		return -1;
	}
	out_video_stream->time_base = in_video_stream->time_base;
	avcodec_free_context(&codec_ctx);


	//音频
	hasAudio = false;//音频暂未测试
	if (hasAudio) {
		AVStream* in_audio_stream = pFormatContext->streams[audioStreamIdx];
		AVStream* out_audio_stream = avformat_new_stream(pOutFormatContext, NULL);
		if (!out_audio_stream)
		{
			log(AV_LOG_ERROR, "avformat_new_stream failed ");
			return -1;
		}
		outAudioStreamIdx = out_audio_stream->index;
		//AVCodec* in_codec = avcodec_find_encoder(in_audio_stream->codecpar->codec_id);
		AVCodec* in_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
		in_audio_stream->codecpar->codec_id = AV_CODEC_ID_AAC;
		AVCodecContext* codec_ctx = avcodec_alloc_context3(in_codec);
		codec_ctx->framerate = { 0,1 };
		ret = avcodec_parameters_to_context(codec_ctx, in_audio_stream->codecpar);
		if (ret < 0)
		{
			log(AV_LOG_ERROR, "audio avcodec_parameters_to_context failed ");
			return -1;
		}
		codec_ctx->codec_tag = 0;

		if (pOutFormatContext->oformat->flags & AVFMT_GLOBALHEADER) {
			codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		}
		ret = avcodec_parameters_from_context(out_audio_stream->codecpar, codec_ctx);
		if (ret < 0)
		{
			log(AV_LOG_ERROR, "audio avcodec_parameters_from_context failed ");
			return -1;
		}
		out_audio_stream->time_base = in_audio_stream->time_base;
		avcodec_free_context(&codec_ctx);
	}

	AVDictionary* opts = NULL;
	av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov+omit_tfhd_offset+faststart+dash+frag_custom", 0);
	av_dict_set(&opts, "frag_duration", "0", 0);
	av_dict_set(&opts, "min_frag_duration", "0", 0);

	log(AV_LOG_INFO, "avformat_write_header");
	ret = avformat_write_header(pOutFormatContext, &opts);
	if (ret < 0)
	{
		log(AV_LOG_ERROR, "audio avformat_write_header failed ");
		return -1;
	}
	pOutFormatContext->oformat->flags |= AVFMT_TS_NONSTRICT;
	av_dict_free(&opts);
	AVCodecParameters* pCodecParam = pOutFormatContext->streams[outVideoStreamIdx]->codecpar;
	std::string codecMime = "video/mp4; codecs=\"avc1." + GetMIME(pCodecParam->extradata, pCodecParam->extradata_size);
	if (outAudioStreamIdx != -1)
	{
		int profile = pOutFormatContext->streams[outAudioStreamIdx]->codecpar->profile;
		log(AV_LOG_INFO, "audio profile is %d", profile);
		switch (profile)
		{
		case FF_PROFILE_AAC_MAIN:
			codecMime += ",mp4a.40.1";
			break;
		case FF_PROFILE_AAC_LOW:
			codecMime += ",mp4a.40.2";
			break;
		case FF_PROFILE_AAC_SSR:
			codecMime += ",mp4a.40.3";
			break;
		case FF_PROFILE_AAC_LTP:
			codecMime += ",mp4a.40.4";
			break;
		case FF_PROFILE_AAC_HE:
			codecMime += ",mp4a.40.5";
			break;
		case FF_PROFILE_AAC_HE_V2:
			codecMime += ",mp4a.40.1D";
			break;
		case FF_PROFILE_AAC_LD:
			codecMime += ",mp4a.40.17";
			break;
		case FF_PROFILE_AAC_ELD:
			codecMime += ",mp4a.40.27";
			break;
		default:
			log(AV_LOG_ERROR, "audio codec string failed ");
			break;
		}
	}
	codecMime += "\"";
	
	strcpy_s(codecMIME, codecMime.c_str());
	log(AV_LOG_INFO, "\ncodec string is %s", codecMIME);

	isOpen = true;
	running = false;

	return 0;
}



int TakeStream::close()
{
	running = false;
	isOpen = false;
	hasAudio = false;
	return 0;
}

int TakeStream::startTakeStreamThread(int takeSleepTimeMs) {
	if (!isOpen) {
		log(AV_LOG_INFO, "has not open,please open it first");
		return -1;
	}
	if (running) {
		log(AV_LOG_INFO, "has start take stream thread,do not try again");
		return -1;
	}
	if (takeSleepTimeMs >= 0) {
		this->takeSleepTimeMs = takeSleepTimeMs;
	}
	else {
		log(AV_LOG_WARNING, "takeSleepTimeMs must be greater than or equal to 0");
	}

	//开启取流线程
	TakeStreamLoop loop = std::bind(&TakeStream::takeLoop, this);
	std::thread take(loop);
	take.detach();
	running = true;
	return 0;
}

//需要放置在线程中
int TakeStream::takeLoop() {
	std::lock_guard<std::mutex> lock(m_mutex);//这把锁主要是为了保证析构前当前线程已经退出
	log(AV_LOG_INFO, "enter take Stream Loop");
	av_init_packet(packet);
	packet->data = NULL;
	packet->size = 0;

	while (running) {
		av_packet_unref(packet);//必须增加这一句释放之前占用的内存
		int ret = av_read_frame(pFormatContext, packet);
		if (ret == AVERROR_EOF) {
			log(AV_LOG_ERROR, "av_read_frame end of file.");
			running = false;
			break;
		}
		else if (ret < 0) {
			log(AV_LOG_ERROR, "av_read_frame faile or end of file.");
			continue;
		}
		bool isVideoStream = packet->stream_index == videoStreamIdx;
		bool isKeyFrame = packet->flags & AV_PKT_FLAG_KEY;//判断是否关键帧
		//log(AV_LOG_INFO, "av_read_frame packet size=%d,stream_index=%d,isKeyFrame=%d.\n", packet->size, packet->stream_index, isKeyFrame);


		/*if (isVideoStream) {//测试用
			log(AV_LOG_INFO, "av_read_frame packet size=%d,stream_index=%d", packet->size, packet->stream_index);
			writeFile(packet->data, packet->size);
		}*/
		//将裸流回调出去
		if (packetCallback) {
			packetCallback(packetCallbackUserData, isVideoStream, packet->data, packet->size, packet->pts, packet->dts, packet->duration);
		}
		if (packetCallbackFunction) {
			packetCallbackFunction(packetCallbackUserData, isVideoStream, packet->data, packet->size, packet->pts, packet->dts, packet->duration);
		}

		packet->flags |= AV_PKT_FLAG_KEY;
		packet->pos = -1;
		ret = av_interleaved_write_frame(pOutFormatContext, packet);//之后可以关注自定义io的write_packet 

		//最好休眠一段时间,控制读取速度
		std::chrono::milliseconds dura(takeSleepTimeMs);
		std::this_thread::sleep_for(dura);
	}
	log(AV_LOG_INFO, "take stream Loop has exit");
	//取流线程退出，关闭输入流
	if (pFormatContext != NULL) {
		avformat_close_input(&pFormatContext);
		pFormatContext = NULL;
	}
	if (pOutFormatContext != NULL) {
		av_write_trailer(pOutFormatContext);
		avformat_free_context(pOutFormatContext);
		pOutFormatContext = NULL;
	}
	close();
	return 0;
}

int TakeStream::readOneFrame() {
	if (!isOpen) {
		log(AV_LOG_INFO, "has not open,please open it first");
		return -1;
	}
	if (running) {
		log(AV_LOG_INFO, "has start take stream thread,can not call this function");
		return -1;
	}
	av_init_packet(packet);
	packet->data = NULL;
	packet->size = 0;
	av_packet_unref(packet);//必须增加这一句释放之前占用的内存
	int ret = av_read_frame(pFormatContext, packet);
	if (ret != 0) {
		log(AV_LOG_ERROR, "av_read_frame faile");
		return ret;
	}

	bool isVideoStream = packet->stream_index == videoStreamIdx;
	bool isKeyFrame = packet->flags & AV_PKT_FLAG_KEY;

	//将裸流回调出去
	if (packetCallback) {
		packetCallback(packetCallbackUserData, isVideoStream, packet->data, packet->size, packet->pts, packet->dts, packet->duration);
	}
	if (packetCallbackFunction) {
		packetCallbackFunction(packetCallbackUserData, isVideoStream, packet->data, packet->size, packet->pts, packet->dts, packet->duration);
	}



	return 0;
}

