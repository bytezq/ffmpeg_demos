#include <iostream>
#include <thread>
#include <vector>
#include <mutex>
#include <chrono>
#include <stdexcept>
using namespace std;

extern "C" {
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
}


#if defined(_MSC_VER)
static char av_error[AV_ERROR_MAX_STRING_SIZE] = { 0 };
#define av_err2str(errnum) \
    av_make_error_string(av_error, AV_ERROR_MAX_STRING_SIZE, errnum)
#elif
#define av_err2str(errnum) \
    av_make_error_string((char[AV_ERROR_MAX_STRING_SIZE]){0}, AV_ERROR_MAX_STRING_SIZE, errnum)
#endif


static int counter = 0;
static std::mutex mtx;
static AVFifoBuffer* video_fifo[2];


void increase(int time) {
    for (int i = 0; i < time; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lock_guard<mutex> lock(mtx);
        ++counter;
    }
}

static int decode_packet(AVCodecContext* dec, const AVPacket* pkt, AVFrame* frame, AVFifoBuffer* video_fifo)
{
    int ret = 0;

    // submit the packet to the decoder
    ret = avcodec_send_packet(dec, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error submitting a packet for decoding (%s)\n", av_err2str(ret));
        return ret;
    }

    // get all the available frames from the decoder
    while (ret >= 0) {
        ret = avcodec_receive_frame(dec, frame);
        if (ret < 0) {
            // those two return values are special and mean there is no output
            // frame available, but there were no errors during decoding
            if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
                return 0;

            fprintf(stderr, "Error during decoding (%s)\n", av_err2str(ret));
            return ret;
        }
        // 处理frame，写入管道
        AVFrame* tmp = av_frame_alloc();
        av_frame_move_ref(tmp, frame);
        if (av_fifo_space(video_fifo) >= sizeof(tmp)) {
            lock_guard<mutex> lock(mtx);
            av_fifo_generic_write(video_fifo, &tmp, sizeof(tmp), NULL);
        }
        av_frame_unref(frame);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static void open_input(const string& filename, AVFifoBuffer* video_fifo) {
    int ret = 0;

    AVFormatContext* input_fmt_ctx = avformat_alloc_context();
    ret = avformat_open_input(&input_fmt_ctx, filename.c_str(), NULL, NULL);
    if (ret < 0) {
        printf("can not open file %d \n", ret);
        return;
    }
    
    //
    ret = avformat_find_stream_info(input_fmt_ctx, NULL);
    if (ret < 0) {
        printf("find_stream_info err %d\n", ret);
        return;
    }

    //
    int vidx = -1;
    vidx = av_find_best_stream(input_fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vidx < 0) {
        printf("find_best_stream audio err \n");
        return;
    }

    AVStream* input_vst = input_fmt_ctx->streams[vidx];
    AVCodec* input_codec = avcodec_find_decoder(input_vst->codecpar->codec_id);
    AVCodecContext* input_codec_ctx = avcodec_alloc_context3(input_codec);
    // 将编码参数复制到编码器上下文
    ret = avcodec_parameters_to_context(input_codec_ctx, input_vst->codecpar);
    if (ret < 0) {
        printf("error code %d \n", ret);
        return;
    }
    // 打开编码器上下文
    ret = avcodec_open2(input_codec_ctx, input_codec, NULL);
    if (ret < 0) {
        printf("open codec faile %d \n", ret);
        return;
    }

    // 打印媒体信息
    av_dump_format(input_fmt_ctx, 0, filename.c_str(), 0);


    AVPacket* input_pkt = av_packet_alloc();
    if (!input_pkt) {
        fprintf(stderr, "Could not allocate packet\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }

    // 开始编解码
    /* read frames from the file */
    AVFrame* frame = av_frame_alloc();
    while (av_read_frame(input_fmt_ctx, input_pkt) >= 0) {
        // check if the packet belongs to a stream we are interested in, otherwise
        // skip it
        if (input_pkt->stream_index == vidx)
            ret = decode_packet(input_codec_ctx, input_pkt, frame, video_fifo);
        av_packet_unref(input_pkt);
        if (ret < 0)
            break;
    }

end:
    avcodec_free_context(&input_codec_ctx);
    avformat_close_input(&input_fmt_ctx);
    av_packet_free(&input_pkt);
    av_frame_free(&frame);
}


static int output_video_frame(AVFifoBuffer* fifo)
{
    AVFrame* frame;
    while (true) {
        if (av_fifo_size(fifo) >= sizeof(frame)) {
            lock_guard<mutex> lock(mtx);
            av_fifo_generic_read(fifo, &frame, sizeof(frame), NULL);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 0;
}

static void encode(AVCodecContext* enc_ctx, AVFrame* frame, AVPacket* pkt,
    FILE* outfile)
{
    int ret;

    /* send the frame to the encoder */
    if (frame)
        printf("Send frame %3lld\n", frame->pts);

    ret = avcodec_send_frame(enc_ctx, frame);
    if (ret < 0) {
        fprintf(stderr, "Error sending a frame for encoding\n");
        exit(1);
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(enc_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            fprintf(stderr, "Error during encoding\n");
            exit(1);
        }

        printf("Write packet %3lld (size=%5d)\n", pkt->pts, pkt->size);
        fwrite(pkt->data, 1, pkt->size, outfile);
        av_packet_unref(pkt);
    }
}

static void open_output(const string& filename, AVFifoBuffer* fifo) {
    int ret = 0;
    AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        fprintf(stderr, "Codec libx264 not found\n");
        exit(1);
    }
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        fprintf(stderr, "Could not allocate video codec context\n");
        exit(1);
    }
    AVPacket* pkt = av_packet_alloc();
    if (!pkt)
        exit(1);

    /* put sample parameters */
    ctx->bit_rate = 400000;
    /* resolution must be a multiple of two */
    ctx->width = 1920;
    ctx->height = 1080;
    /* frames per second */
    ctx->time_base = AVRational{1, 25};
    ctx->framerate = AVRational{25, 1};

    ctx->gop_size = 30;
    ctx->max_b_frames = 10;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    if (codec->id == AV_CODEC_ID_H264)
        av_opt_set(ctx->priv_data, "preset", "slow", 0);

    /* open it */
    ret = avcodec_open2(ctx, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open codec: %s\n", av_err2str(ret));
        exit(1);
    }

    FILE* f = fopen(filename.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "Could not open %s\n", filename);
        exit(1);
    }

    // 读取缓存视频帧
    AVFrame* frame;
    auto start_time = std::chrono::steady_clock::now();
    const std::chrono::seconds timeout(5);
    while (true) {
        auto current_time = std::chrono::steady_clock::now();
        if (current_time - start_time > timeout)
            break;

        if (av_fifo_size(fifo) >= sizeof(frame)) {
            lock_guard<mutex> lock(mtx);
            av_fifo_generic_read(fifo, &frame, sizeof(frame), NULL);
            start_time = current_time; // 更新计时器
        }
        else {
            continue;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        encode(ctx, frame, pkt, f);
    }

    /* flush the encoder */
    encode(ctx, NULL, pkt, f);

    /* add sequence end code to have a real MPEG file */
    uint8_t endcode[] = { 0, 0, 1, 0xb7 };
    if (codec->id == AV_CODEC_ID_MPEG1VIDEO || codec->id == AV_CODEC_ID_MPEG2VIDEO)
        fwrite(endcode, 1, sizeof(endcode), f);
    fclose(f);

    avcodec_free_context(&ctx);
    av_frame_free(&frame);
    av_packet_free(&pkt);
}

static void merge() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        lock_guard<mutex> lock(mtx);
        cout << counter << endl;
    }
}


int main(int argc, char** argv) {
    vector<string> input_files = {"D:\\1.mp4"};

    std::vector<std::thread> input_threads;
    int i = 0; vector<string>::iterator it;
    for (i, it = input_files.begin(); it != input_files.end(); ++it, ++i) {
        video_fifo[i] = av_fifo_alloc(30 * 8);
        input_threads.emplace_back(std::thread(open_input, *it, video_fifo[i]));
    }

    std::thread output_thread(open_output, "D:\\2.mp4", video_fifo[0]);
    for (auto&& thread : input_threads) {
        thread.join();
    }
    output_thread.join();
    
    return 0;
}