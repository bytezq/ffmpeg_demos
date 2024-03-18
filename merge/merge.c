#include <stdio.h>
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


AVFormatContext* input_fmt_ctx;
AVCodecContext* input_codec_ctx;

AVFormatContext* output_fmt_ctx;
AVCodecContext* output_codec_ctx;

AVFilterGraph* filter_graph;

AVFilterInOut* inputs;
AVFilterInOut* outputs;
AVFilterInOut* cur;

AVFilterContext* buffersrc_ctx;
AVFilterContext* buffersink_ctx;



static int open_intput_file(const char* filename) {
    int ret = 0, err;

    input_fmt_ctx = avformat_alloc_context();
    if (!input_fmt_ctx) {
        printf("error code %d \n", AVERROR(ENOMEM));
        return ENOMEM;
    }
    if ((err = avformat_open_input(&input_fmt_ctx, filename, NULL, NULL)) < 0) {
        printf("can not open file %d \n", err);
        return err;
    }

    //打开解码器
    input_codec_ctx = avcodec_alloc_context3(NULL);
    // 参数赋值
    ret = avcodec_parameters_to_context(input_codec_ctx, input_fmt_ctx->streams[0]->codecpar);
    if (ret < 0) {
        printf("error code %d \n", ret);
        return ret;
    }
    // 查询解码器
    AVCodec* codec = avcodec_find_decoder(input_codec_ctx->codec_id);
    if ((ret = avcodec_open2(input_codec_ctx, codec, NULL)) < 0) {
        printf("open codec faile %d \n", ret);
        return ret;
    }
}

static int open_output_file(const char* filename) {
    int err;
    err = avformat_alloc_output_context2(&output_fmt_ctx, NULL, NULL, filename);
    if (!output_fmt_ctx) {
        printf("error code %d \n", AVERROR(ENOMEM));
        return ENOMEM;
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
    //用 avfilter_graph_parse2 创建 scale 滤镜。
    AVBPrint args;
    av_bprint_init(&args, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_bprintf(&args, "[0:v]scale=%d:%d", input_codec_ctx->width / 2, input_codec_ctx->height / 2);

    ret = avfilter_graph_parse2(filter_graph, args.str, &inputs, &outputs);
    if (ret < 0) {
        printf("Cannot configure graph\n");
        return ret;
    }
    return ret;
}

static int video_config_input_filter()
{
    int ret = 0;
    // 初始化 AVBPrint 结构体，用于字符串缓冲区管理
    AVBPrint args;
    av_bprint_init(&args, 0, AV_BPRINT_SIZE_AUTOMATIC);
    //
    AVRational tb = input_fmt_ctx->streams[0]->time_base; // 对准时间基
    AVRational fr = av_guess_frame_rate(input_fmt_ctx, input_fmt_ctx->streams[0], NULL); // 存储视频帧率信息
    AVRational sar = input_codec_ctx->sample_aspect_ratio; // 视频帧样本像素的长宽比例
    av_bprintf(&args,
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:frame_rate=%d/%d",
        input_codec_ctx->width, input_codec_ctx->height, input_codec_ctx->pix_fmt,
        tb.num, tb.den, sar.num, sar.den, fr.num, fr.den);
    //创建 buffer 滤镜 ctx
    ret = avfilter_graph_create_filter(&buffersrc_ctx, avfilter_get_by_name("buffer"),
        "src_in", args.str, NULL, filter_graph);
    if (ret < 0) {
        printf("video config input filter fail.\n");
        return ret;
    }


    for (cur = inputs; cur; cur = cur->next) {
        printf("cur->name : %s, cur->filter_ctx->name : %s \n", cur->name, cur->filter_ctx->name);
        //连接 buffer滤镜 跟 scale 滤镜
        if ((ret = avfilter_link(buffersrc_ctx, 0, cur->filter_ctx, 0)) < 0) {
            printf("link ctx fail\n");
            return ret;
        }
    }

    //AVFilterContext* first_filter = inputs->filter_ctx;
    //int pad_idx = inputs->pad_idx; // 过滤器输入或输出索引
    // 连接input_filter_ctx滤镜的第 0 个输出流，到 first_filter 滤镜的第 pad_idx 输入流
    //ret = avfilter_link(*input_filter_ctx, 0, first_filter, pad_idx);
    //assert(ret >= 0);

    //printf("video_config_input_filter avfilter_link ret = %d\n", ret);

    return ret;
}

static int video_config_output_filter()
{


    int ret = 0;
    AVFilter* buffersink = avfilter_get_by_name("buffersink");
    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "video_out", NULL, NULL, filter_graph);
    if (ret < 0) {
        printf("buffersink ctx fail\n");
        return ret;
    }

    for (cur = outputs; cur; cur = cur->next) {
        printf("cur->name : %s, cur->filter_ctx->name : %s \n", cur->name, cur->filter_ctx->name);
        //连接 scale滤镜 跟 buffersink滤镜
        if ((ret = avfilter_link(cur->filter_ctx, 0, buffersink_ctx, 0)) < 0) {
            printf("link ctx fail\n");
            return ret;
        }
    }
    // AVFilterContext* first_filter = inputs->filter_ctx;
    // int pad_idx = inputs->pad_idx; // 过滤器输入或输出索引
    // 连接input_filter_ctx滤镜的第 0 个输出流，到 first_filter 滤镜的第 pad_idx 输入流
    // ret = avfilter_link(*input_filter_ctx, 0, first_filter, pad_idx);
    // assert(ret >= 0);
    // printf("video_config_input_filter avfilter_link ret = %d\n", ret);

    return ret;
}

static int init_output_codec(AVFrame* frame) {

}

int main()
{
    int ret = 0; int err;

    //打开输入文件
    char filename[] = "D:\\1.mp4";
    //打开输出文件容器
    char filename_out[] = "juren-30s-5.mp4";

    open_intput_file(filename);
    open_output_file(filename_out);



    //添加一路流到容器上下文
    AVStream* st = avformat_new_stream(output_fmt_ctx, NULL);
    st->time_base = input_fmt_ctx->streams[0]->time_base;



    AVFrame* result_frame = av_frame_alloc();

    AVPacket* input_pkt = av_packet_alloc();
    AVFrame* input_frame = av_frame_alloc();
    AVPacket* pkt_out = av_packet_alloc();
    
    // 申请管道用于缓冲帧数据
    AVFifoBuffer *video_fifo = av_fifo_alloc(30 * sizeof(input_frame));  //申请30帧缓存

    int frame_num = 0;
    int read_end = 0;
    for (;;) {
        if (1 == read_end) {
            break;
        }
        // 读取编码数据
        ret = av_read_frame(input_fmt_ctx, input_pkt);
        //跳过不处理音频包
        if (1 == input_pkt->stream_index) {
            av_packet_unref(input_pkt);
            continue;
        }

        if (AVERROR_EOF == ret) {
            //读取完文件，这时候 input_pkt 的 data 跟 size 应该是 null
            avcodec_send_packet(input_codec_ctx, NULL);
        }
        else {
            if (0 != ret) {
                printf("read error code %d \n", ret);
                return ENOMEM;
            }
            else {
            retry:
                // 将编码数据发送到解码器
                if (avcodec_send_packet(input_codec_ctx, input_pkt) == AVERROR(EAGAIN)) {
                    printf("Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                    //这里可以考虑休眠 0.1 秒，返回 EAGAIN 通常是 ffmpeg 的内部 api 有bug
                    // 发送失败就重新发送
                    goto retry;
                }
                //释放 input_pkt 里面的编码数据
                av_packet_unref(input_pkt);
            }
        }

        //循环不断从解码器读数据，直到没有数据可读。
        for (;;) {
            //读取 AVFrame
            ret = avcodec_receive_frame(input_codec_ctx, input_frame);
            /* 释放 input_frame 里面的YUV数据，
             * 由于 avcodec_receive_frame 函数里面会调用 av_frame_unref，所以下面的代码可以注释。
             * 所以我们不需要 手动 unref 这个 AVFrame
             */
             //av_frame_unref(input_frame);

            if (AVERROR(EAGAIN) == ret) {
                //提示 EAGAIN 代表解码器需要更多的 AVPacket，跳出当前for，让解码器拿到更多的 AVPacket
                break;
            }
            else if (AVERROR_EOF == ret) {
                /* 提示 AVERROR_EOF 代表之前已经往 解码器发送了一个 data 跟 size 都是 NULL 的 AVPacket
                 * 发送 NULL 的 AVPacket 是提示解码器把所有的缓存帧全都刷出来。
                 * 通常只有在 读完输入文件才会发送 NULL 的 AVPacket，或者需要用现有的解码器解码另一个的视频流才会这么干。
                 *
                 */

                 /* 往编码器发送 null 的 AVFrame，让编码器把剩下的数据刷出来。
                  * */
                ret = avcodec_send_frame(output_codec_ctx, NULL);
                for (;;) {
                    ret = avcodec_receive_packet(output_codec_ctx, pkt_out);
                    //这里不可能返回 EAGAIN，如果有直接退出。
                    if (ret == AVERROR(EAGAIN)) {
                        printf("avcodec_receive_packet error code %d \n", ret);
                        return ret;
                    }
                    if (AVERROR_EOF == ret) {
                        break;
                    }
                    //编码出 AVPacket ，先打印一些信息，然后把它写入文件。
                    printf("pkt_out size : %d \n", pkt_out->size);
                    //设置 AVPacket 的 stream_index ，这样才知道是哪个流的。
                    pkt_out->stream_index = st->index;
                    //转换 AVPacket 的时间基为 输出流的时间基。
                    pkt_out->pts = av_rescale_q_rnd(pkt_out->pts, input_fmt_ctx->streams[0]->time_base, st->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
                    pkt_out->dts = av_rescale_q_rnd(pkt_out->dts, input_fmt_ctx->streams[0]->time_base, st->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
                    pkt_out->duration = av_rescale_q_rnd(pkt_out->duration, input_fmt_ctx->streams[0]->time_base, st->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);

                    // 将音视频数据写入文件
                    ret = av_interleaved_write_frame(output_fmt_ctx, pkt_out);
                    if (ret < 0) {
                        printf("av_interleaved_write_frame faile %d \n", ret);
                        return ret;
                    }
                    av_packet_unref(pkt_out);
                }
                av_write_trailer(output_fmt_ctx);
                //跳出 第二层 for，文件已经解码完毕。
                read_end = 1;
                break;
            }
            else if (ret >= 0) {
                // 写入管道，浅拷贝帧数据
                AVFrame* tmp1 = av_frame_alloc();
                av_frame_move_ref(tmp1, input_frame);
                if (av_fifo_space(video_fifo) >= sizeof(tmp1)) {
					av_fifo_generic_write(video_fifo, &tmp1, sizeof(tmp1), NULL);
				}

                if (NULL == filter_graph) {
                    init_filter_graph();
                    init_input_filters(); // 根据字符串初始化输入滤镜，初始化inputs 和 outputs
                    
                    // 初始化buffersrc滤镜
                    video_config_input_filter();

                    //创建 buffersink 滤镜 ctx
                    video_config_output_filter();

                    //正式打开滤镜
                    ret = avfilter_graph_config(filter_graph, NULL);
                    if (ret < 0) {
                        printf("Cannot configure graph\n");
                        return ret;
                    }
                }

                // 从管道读取数据到buffer
                AVFrame* tmp;
                if (av_fifo_size(video_fifo) >= sizeof(tmp)) {
                    av_fifo_generic_read(video_fifo, &tmp, sizeof(tmp), NULL);
                }else{
                    break;
                }
                ret = av_buffersrc_add_frame_flags(buffersrc_ctx, tmp, AV_BUFFERSRC_FLAG_PUSH);
                av_frame_free(&tmp);
                if (ret < 0) {
                    printf("Error: av_buffersrc_add_frame failed\n");
                    return ret;
                }

                ret = av_buffersink_get_frame_flags(buffersink_ctx, result_frame, AV_BUFFERSRC_FLAG_PUSH);


                //只有解码出来一个帧，才可以开始初始化编码器。
                if (NULL == output_codec_ctx) {
                    //打开编码器，并且设置 编码信息。
                    AVCodec* encode = avcodec_find_encoder(AV_CODEC_ID_H264);
                    output_codec_ctx = avcodec_alloc_context3(encode);
                    output_codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
                    output_codec_ctx->bit_rate = 400000;
                    output_codec_ctx->framerate = input_codec_ctx->framerate;
                    output_codec_ctx->gop_size = 30;
                    output_codec_ctx->max_b_frames = 10;
                    output_codec_ctx->profile = FF_PROFILE_H264_MAIN;
                    /*
                     * 其实下面这些信息在容器那里也有，也可以一开始直接在容器那里打开编码器
                     * 我从 AVFrame 里拿这些编码器参数是因为，容器的不一定就是最终的。
                     * 因为你解码出来的 AVFrame 可能会经过 filter 滤镜，经过滤镜之后信息就会变换，但是本文没有使用滤镜。
                     */
                     //编码器的时间基要取 AVFrame 的时间基，因为 AVFrame 是输入。AVFrame 的时间基就是 流的时间基。
                    output_codec_ctx->time_base = input_fmt_ctx->streams[0]->time_base;

                    // output_codec_ctx->width = fmt_ctx->streams[0]->codecpar->width;
                    // output_codec_ctx->height = fmt_ctx->streams[0]->codecpar->height;
                    output_codec_ctx->width = result_frame->width;
                    output_codec_ctx->height = result_frame->height;
                    output_codec_ctx->sample_aspect_ratio = st->sample_aspect_ratio = result_frame->sample_aspect_ratio;
                    output_codec_ctx->pix_fmt = result_frame->format;
                    output_codec_ctx->color_range = result_frame->color_range;
                    output_codec_ctx->color_primaries = result_frame->color_primaries;
                    output_codec_ctx->color_trc = result_frame->color_trc;
                    output_codec_ctx->colorspace = result_frame->colorspace;
                    output_codec_ctx->chroma_sample_location = result_frame->chroma_location;

                    /* 注意，这个 field_order 不同的视频的值是不一样的，这里我写死了。
                     * 因为 本文的视频就是 AV_FIELD_PROGRESSIVE
                     * 生产环境要对不同的视频做处理的
                     */
                    output_codec_ctx->field_order = AV_FIELD_PROGRESSIVE;

                    /* 现在我们需要把 编码器参数复制给流，解码的时候是 从流赋值参数给解码器。
                     * 现在要反着来。
                     * */
                    ret = avcodec_parameters_from_context(st->codecpar, output_codec_ctx);
                    if (ret < 0) {
                        printf("error code %d \n", ret);
                        return ret;
                    }
                    if ((ret = avcodec_open2(output_codec_ctx, encode, NULL)) < 0) {
                        printf("open codec faile %d \n", ret);
                        return ret;
                    }

                    //正式打开输出文件
                    if ((ret = avio_open2(&output_fmt_ctx->pb, filename_out, AVIO_FLAG_WRITE, &output_fmt_ctx->interrupt_callback, NULL)) < 0) {
                        printf("avio_open2 fail %d \n", ret);
                        return ret;
                    }

                    //要先写入文件头部。
                    ret = avformat_write_header(output_fmt_ctx, NULL);
                    if (ret < 0) {
                        printf("avformat_write_header fail %d \n", ret);
                        return ret;
                    }

                }

                //往编码器发送 AVFrame，然后不断读取 AVPacket
                ret = avcodec_send_frame(output_codec_ctx, result_frame);
                if (ret < 0) {
                    printf("avcodec_send_frame fail %d \n", ret);
                    return ret;
                }
                for (;;) {
                    ret = avcodec_receive_packet(output_codec_ctx, pkt_out);
                    if (ret == AVERROR(EAGAIN)) {
                        break;
                    }
                    if (ret < 0) {
                        printf("avcodec_receive_packet fail %d \n", ret);
                        return ret;
                    }
                    //编码出 AVPacket ，先打印一些信息，然后把它写入文件。
                    printf("pkt_out size : %d \n", pkt_out->size);

                    //设置 AVPacket 的 stream_index ，这样才知道是哪个流的。
                    pkt_out->stream_index = st->index;
                    //转换 AVPacket 的时间基为 输出流的时间基。
                    pkt_out->pts = av_rescale_q_rnd(pkt_out->pts, input_fmt_ctx->streams[0]->time_base, st->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
                    pkt_out->dts = av_rescale_q_rnd(pkt_out->dts, input_fmt_ctx->streams[0]->time_base, st->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
                    pkt_out->duration = av_rescale_q_rnd(pkt_out->duration, input_fmt_ctx->streams[0]->time_base, st->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);

                    ret = av_interleaved_write_frame(output_fmt_ctx, pkt_out);
                    if (ret < 0) {
                        printf("av_interleaved_write_frame faile %d \n", ret);
                        return ret;
                    }
                    av_packet_unref(pkt_out);
                }
            }
            else {
                printf("other fail \n");
                return ret;
            }
        }


    }

    av_frame_free(&input_frame);
    av_frame_free(&result_frame);
    av_packet_free(&input_pkt);
    av_packet_free(&pkt_out);



    //关闭编码器，解码器。
    avcodec_close(input_codec_ctx);
    avcodec_close(output_codec_ctx);

    //释放容器内存。
    avformat_free_context(input_fmt_ctx);

    //必须调 avio_closep ，要不可能会没把数据写进去，会是 0kb
    avio_closep(&output_fmt_ctx->pb);
    avformat_free_context(output_fmt_ctx);

    //释放滤镜。
    avfilter_graph_free(&filter_graph);

    printf("done \n");

    return 0;
}