#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <windows.h>

#include "libavformat/avformat.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersrc.h"
#include "libavfilter/buffersink.h"
#include "libavcodec/avcodec.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
#include "libavdevice/avdevice.h"
#include "libavutil/bprint.h"
#include "libavutil/audio_fifo.h"
#include "libavutil/fifo.h"
#include "libavutil/avutil.h"
#include "libavutil/frame.h"
#include "libavutil/imgutils.h"

#define INPUT_NUMBER 2 

pthread_mutex_t mutex;

typedef struct InputStream {
    AVFormatContext* fmt_ctx;
    AVCodecContext* codec_ctx;
    AVFilterContext* buffersrc_ctx;
    AVFifoBuffer* fifo;
} InputStream;

typedef struct OutputStream {
    AVFormatContext* fmt_ctx;
    AVCodecContext* codec_ctx;
    AVStream* st;
} OutputStream;

AVFilterGraph* filter_graph;
AVFilterContext* buffersink_ctx;
AVFilterInOut* inputs;
AVFilterInOut* outputs;





const char* filter_desc = "nullsrc=size=1920x1080[base]; [0:v]setpts=PTS-STARTPTS,scale=960x540[upperleft]; [1:v]setpts=PTS-STARTPTS,scale=960x540[upperright]; [base][upperleft]overlay=shortest=1[tmp1]; [tmp1][upperright]overlay=shortest=1:x=960";


static int open_intput_file(InputStream* inputStream, const char* filename) {
    int i = 0, ret = 0, err;
    inputStream->fmt_ctx = avformat_alloc_context();
    if (!inputStream->fmt_ctx) {
        printf("error code %d \n", AVERROR(ENOMEM));
        return ENOMEM;
    }
    if ((err = avformat_open_input(&inputStream->fmt_ctx, filename, NULL, NULL)) < 0) {
        printf("can not open file %d \n", err);
        return err;
    }

    //打开解码器
    inputStream->codec_ctx = avcodec_alloc_context3(NULL);
    // 参数赋值
    ret = avcodec_parameters_to_context(inputStream->codec_ctx, inputStream->fmt_ctx->streams[0]->codecpar);
    if (ret < 0) {
        printf("error code %d \n", ret);
        return ret;
    }
    // 查询解码器
    AVCodec* codec = avcodec_find_decoder(inputStream->codec_ctx->codec_id);
    if ((ret = avcodec_open2(inputStream->codec_ctx, codec, NULL)) < 0) {
        printf("open codec faile %d \n", ret);
        return ret;
    }
}

static int open_output_file(const char* filename) {
    int ret=0, err;
    err = avformat_alloc_output_context2(&output_fmt_ctx, NULL, NULL, filename);
    if (!output_fmt_ctx) {
        printf("error code %d \n", AVERROR(ENOMEM));
        return ENOMEM;
    }

    //打开编码器，并且设置 编码信息。
    AVCodec* output_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    output_codec_ctx = avcodec_alloc_context3(output_codec);
    AVStream *output_video_st = avformat_new_stream(output_codec_ctx, output_codec);

    output_codec_ctx->codec_id = AV_CODEC_ID_H264;
    output_codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;

    /* put sample parameters */
    output_codec_ctx->bit_rate = 400000;
    /* resolution must be a multiple of two */
    output_codec_ctx->width = 1920;
    output_codec_ctx->height = 1080;
    /* frames per second */
    output_codec_ctx->time_base = (AVRational){ 1, 25 };
    output_codec_ctx->framerate = (AVRational){ 25, 1 };

    output_codec_ctx->gop_size = 10;
    output_codec_ctx->max_b_frames = 1;
    output_codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    
    output_codec_ctx->gop_size = 10;
    output_codec_ctx->max_b_frames = 1;
    output_codec_ctx->profile = FF_PROFILE_H264_MAIN;

    if (output_codec->id == AV_CODEC_ID_H264)
        av_opt_set(output_codec_ctx->priv_data, "preset", "slow", 0);


    // 打开编码器
    if ((ret = avcodec_open2(output_codec_ctx, output_codec, NULL)) < 0) {
        printf("open codec faile %d \n", ret);
        return ret;
    }
    // 编码器参数复制给流
    ret = avcodec_parameters_from_context(output_video_st->codecpar, output_codec_ctx);
    if (ret < 0) {
        printf("error code %d \n", ret);
        return ret;
    }
    
    // 正式打开输出文件
    if ((ret = avio_open2(&output_fmt_ctx->pb, filename, AVIO_FLAG_WRITE, &output_fmt_ctx->interrupt_callback, NULL)) < 0) {
        printf("avio_open2 fail %d \n", ret);
        return ret;
    }

    // 写入文件头部
    ret = avformat_write_header(output_fmt_ctx, NULL);
    if (ret < 0) {
        printf("avformat_write_header fail %d \n", ret);
        return ret;
    }
}

static int init_filter_graph() {
    //初始化滤镜
    filter_graph = avfilter_graph_alloc();
    if (!filter_graph) {
        printf("Error: allocate filter graph failed\n");
        return -1;
    }
    return 0;
}

static int init_input_filters() {
    int ret = 0;

    //AVBPrint args;
    //av_bprint_init(&args, 0, AV_BPRINT_SIZE_AUTOMATIC);
    //av_bprintf(&args, "[0:v]scale=%d:%d", input_codec_ctx->width / 2, input_codec_ctx->height / 2);
    // ret = avfilter_graph_parse2(filter_graph, args.str, &inputs, &outputs);
    ret = avfilter_graph_parse2(filter_graph, filter_desc, &inputs, &outputs);
    if (ret < 0) {
        printf("Cannot configure graph\n");
        return ret;
    }
    return ret;
}

static int video_config_input_filter(AVFormatContext* input_fmt_ctx,
                                     AVCodecContext* input_codec_ctx) {
    int ret = 0;
    int i = 0;
    for (cur = inputs; cur; cur = cur->next, ++i) {
        // 初始化 AVBPrint 结构体，用于字符串缓冲区管理
        AVBPrint args;
        av_bprint_init(&args, 0, AV_BPRINT_SIZE_AUTOMATIC);
        AVRational tb = input_fmt_ctx->streams[0]->time_base; // 对准时间基
        AVRational fr = av_guess_frame_rate(input_fmt_ctx, input_fmt_ctx->streams[0], NULL); // 存储视频帧率信息
        AVRational sar = input_codec_ctx->sample_aspect_ratio; // 视频帧样本像素的长宽比例
        av_bprintf(&args, "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:frame_rate=%d/%d",
            input_codec_ctx->width, input_codec_ctx->height, input_codec_ctx->pix_fmt,
            tb.num, tb.den, sar.num, sar.den, fr.num, fr.den);
        //创建 buffer 滤镜 ctx
        const char* filter_name;
        sprintf(filter_name, "src_in_%d", i);
        ret = avfilter_graph_create_filter(&buffersrc_ctx_arr[0], avfilter_get_by_name("buffer"), filter_name, args.str, NULL, filter_graph);
        if (ret < 0) {
            printf("video config input filter fail.\n");
            return ret;
        }

        printf("cur->name : %s, cur->filter_ctx->name : %s \n", cur->name, cur->filter_ctx->name);
        //连接 buffer滤镜 跟 scale 滤镜
        if ((ret = avfilter_link(buffersrc_ctx_arr[i], 0, cur->filter_ctx, 0)) < 0) {
            printf("link ctx fail\n");
            return ret;
        }
    }

    return ret;
}

static int video_config_output_filter()
{
    int ret = 0;
    for (cur = outputs; cur; cur = cur->next) {
        ret = avfilter_graph_create_filter(&buffersink_ctx, avfilter_get_by_name("buffersink"), "video_out", NULL, NULL, filter_graph);
        if (ret < 0) {
            printf("buffersink ctx fail\n");
            return ret;
        } 

        printf("cur->name : %s, cur->filter_ctx->name : %s \n", cur->name, cur->filter_ctx->name);
        //连接 scale滤镜 跟 buffersink滤镜
        if ((ret = avfilter_link(cur->filter_ctx, 0, buffersink_ctx, 0)) < 0) {
            printf("link ctx fail\n");
            return ret;
        }
    }

    return ret;
}

static int* read_frame(InputStream* inputStream) {
    int ret = 0;
    int frame_num = 0;
    int read_end = 0;

    AVPacket* input_pkt = av_packet_alloc();
    AVFrame* input_frame = av_frame_alloc();
    
    for (;;) {
        if (1 == read_end) {
            break;
        }
        ret = av_read_frame(inputStream->fmt_ctx, input_pkt);

        //跳过不处理音频包
        if (1 == input_pkt->stream_index) {
            av_packet_unref(input_pkt);
            continue;
        }
        if (AVERROR(EAGAIN) == ret) {
            continue;
        }
        if (AVERROR_EOF == ret) { //读取完毕，这时候 input_pkt 的 data 跟 size 应该是 null
            avcodec_send_packet(inputStream->codec_ctx, NULL);
        } else {
            if (0 != ret) {
                printf("read error code %d \n", ret);
                return ENOMEM;
            }
            else {
            retry:
                ret = avcodec_send_packet(inputStream->codec_ctx, input_pkt);
                if (AVERROR(EAGAIN) == ret) {
                    printf("Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                    //这里可以考虑休眠 0.1 秒，返回 EAGAIN 通常是 ffmpeg 的内部 api 有bug，发送失败就重新发送
                    goto retry;
                }
                //释放 input_pkt 里面的编码数据
                av_packet_unref(input_pkt);
            }
        }

        for (;;) {
            ret = avcodec_receive_frame(inputStream->codec_ctx, input_frame);
            if (AVERROR(EAGAIN) == ret) {
                //提示 EAGAIN 代表解码器需要更多的 AVPacket，跳到第一层循环，让解码器拿到更多的 AVPacket
                break;
            }
            else if (AVERROR_EOF == ret) {
                /* 提示 AVERROR_EOF 代表之前已经往 解码器发送了一个 data 跟 size 都是 NULL 的 AVPacket
                 * 发送 NULL 的 AVPacket 是提示解码器把所有的缓存帧全都刷出来。
                 * 通常只有在 读完输入文件才会发送 NULL 的 AVPacket，或者需要用现有的解码器解码另一个的视频流才会这么干。
                 * 往编码器发送 null 的 AVFrame，让编码器把剩下的数据刷出来。
                 */
                read_end = 1;   // 跳出所有循环
                break;
            }
            else if (ret >= 0) {
                // 写入管道，浅拷贝帧数据
                AVFrame* tmp_frame = av_frame_alloc();
                av_frame_move_ref(tmp_frame, input_frame);
                if (av_fifo_space(inputStream->fifo) >= sizeof(tmp_frame)) {
                    // 需要加锁
                    av_fifo_generic_write(inputStream->fifo, &tmp_frame, sizeof(tmp_frame), NULL);
                }
            }
            else {
                printf("other fail \n");
                return ret;
            }
        }
    }
}

static void merge() {
    int ret = 0;
    AVFrame* tmp_frame;
    AVFrame* result_frame;
    AVPacket* output_pkt = av_packet_alloc();

    for (;;) {
        for (int i = 0; i < INPUT_NUMBER; ++i) {
            if (av_fifo_size(fifo[i]) >= sizeof(tmp_frame)) {
                av_fifo_generic_read(fifo, &tmp_frame, sizeof(tmp_frame), NULL);
            }
            else {
                continue;
            }
            ret = av_buffersrc_add_frame_flags(buffersrc_ctx_arr[0], tmp_frame, AV_BUFFERSRC_FLAG_PUSH);
            if (ret < 0) {
                printf("Error: av_buffersrc_add_frame failed\n");
                return ret;
            }
        }

        ret = av_buffersink_get_frame_flags(buffersink_ctx, result_frame, AV_BUFFERSRC_FLAG_PUSH);

        ret = avcodec_send_frame(output_codec_ctx, result_frame);
        if (ret < 0) {
            printf("avcodec_send_frame fail %d \n", ret);
            return ret;
        }
        for (;;) {
            ret = avcodec_receive_packet(output_codec_ctx, output_pkt);
            if (ret == AVERROR(EAGAIN)) {
                break;
            }
            if (ret < 0) {
                printf("avcodec_receive_packet fail %d \n", ret);
                return ret;
            }
            //编码出 AVPacket ，先打印一些信息，然后把它写入文件。
            printf("pkt_out size : %d \n", output_pkt->size);

            /* rescale output packet timestamp values from codec to stream timebase */
            av_packet_rescale_ts(output_pkt, output_codec_ctx->time_base, output_st->time_base);
            output_pkt->stream_index = output_st->index;

            //转换 AVPacket 的时间基为 输出流的时间基。
            //output_pkt->pts = av_rescale_q_rnd(output_pkt->pts, input_fmt_ctx->streams[0]->time_base, st->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
            //output_pkt->dts = av_rescale_q_rnd(output_pkt->dts, input_fmt_ctx->streams[0]->time_base, st->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
            //output_pkt->duration = av_rescale_q_rnd(output_pkt->duration, input_fmt_ctx->streams[0]->time_base, st->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);

            ret = av_interleaved_write_frame(output_fmt_ctx, output_pkt);
            if (ret < 0) {
                printf("av_interleaved_write_frame faile %d \n", ret);
                return ret;
            }
            av_packet_unref(output_pkt);
        }
    }
    av_frame_free(&tmp_frame);
}


int main()
{
    int ret = 0; int err;
    pthread_t t1, t2, t3;
    pthread_mutex_init(&mutex, NULL); // 初始化mutex

    const char* input_filename[] = { "D:\\1.mp4", "D:\\1.mp4" };
    const char* output_filename = "juren-30s-5.mp4";

    open_intput_file(input_filename);
    open_output_file(output_filename);


    if (output_fmt_ctx->oformat->video_codec != AV_CODEC_ID_NONE) {
        output_st = avformat_new_stream(output_fmt_ctx, NULL);
        output_st->id = output_fmt_ctx->nb_streams - 1;
    }


    AVPacket* input_pkt = av_packet_alloc();
    AVFrame* input_frame = av_frame_alloc();
  
    // 申请管道用于缓冲帧数据
    for (int i = 0; i < INPUT_NUMBER; ++i) {
        fifo[i] = av_fifo_alloc(30 * sizeof(input_frame));  //申请30帧缓存
    }

    int frame_num = 0;
    
    // 初始化滤镜操作
    init_filter_graph();
    init_input_filters();
    for (int i = 0; i < INPUT_NUMBER; ++i) {
        video_config_input_filter(input_fmt_ctx_arr[i], input_codec_ctx_arr[i]);
    }
    video_config_output_filter();
    ret = avfilter_graph_config(filter_graph, NULL);
    if (ret < 0) {
        printf("Cannot configure graph\n");
        return ret;
    }

    pthread_create(&t1, NULL, read_frame, NULL);
    pthread_create(&t2, NULL, read_frame, NULL);
    pthread_create(&t3, NULL, merge, NULL);

    av_frame_free(&input_frame);
    av_packet_free(&input_pkt);


    //关闭编码器，解码器
    for (int i = 0; i < INPUT_NUMBER; ++i) {
        avcodec_close(input_codec_ctx_arr[i]);
        avformat_free_context(input_fmt_ctx_arr[i]);
    }
    
    avcodec_close(output_codec_ctx);

    //必须调 avio_closep ，要不可能会没把数据写进去
    avio_closep(&output_fmt_ctx->pb);
    avformat_free_context(output_fmt_ctx);

    //释放滤镜。
    avfilter_graph_free(&filter_graph);
    pthread_mutex_destroy(&mutex); // 回收 mutex
    printf("done \n");

    return 0;
}