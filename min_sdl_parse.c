/*
 * http://ffmpeg.org/doxygen/trunk/index.html
 *
 * Main components
 *
 * Format (Container) - a wrapper, providing sync, metadata and muxing for the streams.
 * Stream - a continuous stream (audio or video) of data over time.
 * Codec - defines how data are enCOded (from Frame to Packet)
 *        and DECoded (from Packet to Frame).
 * Packet - are the data (kind of slices of the stream data) to be decoded as raw frames.
 * Frame - a decoded raw frame (to be encoded or filtered).
 */

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <SDL2/SDL.h>

// Refresh Event
#define SFM_REFRESH_EVENT     (SDL_USEREVENT + 1)
#define SFM_BREAK_EVENT       (SDL_USEREVENT + 2)

static volatile int thread_exit = 0;
static volatile int thread_pause = 0;

static int parse_packet(AVCodecContext *pCodecCtx, 
        AVCodecParserContext *pCodecParserCtx,
        FILE *fp_in,
        AVFrame *pFrame, AVPacket *pPacket);

static int sfp_refresh_thread(void *opaque) {
    while (!thread_exit) {
        if (!thread_pause) {
            SDL_Event event;
            event.type = SFM_REFRESH_EVENT;
            SDL_PushEvent(&event);
        }
        SDL_Delay(40);
    }
    thread_exit = 0;
    thread_pause = 0;
    //Break
    SDL_Event event;
    event.type = SFM_BREAK_EVENT;
    SDL_PushEvent(&event);

    return 0;
}

// print out the steps and errors
static void logging(const char *fmt, ...);
// decode packets into frames
static int decode_packet(AVPacket *pPacket, AVCodecContext *pCodecContext, AVFrame *pFrame);

int main(int argc, const char *argv[])
{
    logging("initializing all the containers, codecs and protocols.");

    // AVFormatContext holds the header information from the format (Container)
    // Allocating memory for this component
    // http://ffmpeg.org/doxygen/trunk/structAVFormatContext.html
    AVFormatContext *pFormatContext = avformat_alloc_context();
    if (!pFormatContext) {
        logging("ERROR could not allocate memory for Format Context");
        return -1;
    }

    logging("opening the input file (%s) and loading format (container) header", argv[1]);
    // Open the file and read its header. The codecs are not opened.
    // The function arguments are:
    // AVFormatContext (the component we allocated memory for),
    // url (filename),
    // AVInputFormat (if you pass NULL it'll do the auto detect)
    // and AVDictionary (which are options to the demuxer)
    // http://ffmpeg.org/doxygen/trunk/group__lavf__decoding.html#ga31d601155e9035d5b0e7efedc894ee49
    if (avformat_open_input(&pFormatContext, argv[1], NULL, NULL) != 0) {
        logging("ERROR could not open the file");
        return -1;
    }

    // now we have access to some information about our file
    // since we read its header we can say what format (container) it's
    // and some other information related to the format itself.
    logging("format %s, duration %lld us, bit_rate %lld", pFormatContext->iformat->name, pFormatContext->duration, pFormatContext->bit_rate);

    logging("finding stream info from format");
    // read Packets from the Format to get stream information
    // this function populates pFormatContext->streams
    // (of size equals to pFormatContext->nb_streams)
    // the arguments are:
    // the AVFormatContext
    // and options contains options for codec corresponding to i-th stream.
    // On return each dictionary will be filled with options that were not found.
    // https://ffmpeg.org/doxygen/trunk/group__lavf__decoding.html#gad42172e27cddafb81096939783b157bb
    if (avformat_find_stream_info(pFormatContext,  NULL) < 0) {
        logging("ERROR could not get the stream info");
        return -1;
    }

    // the component that knows how to enCOde and DECode the stream
    // it's the codec (audio or video)
    // http://ffmpeg.org/doxygen/trunk/structAVCodec.html
    AVCodec *pCodec = NULL;
    // this component describes the properties of a codec used by the stream i
    // https://ffmpeg.org/doxygen/trunk/structAVCodecParameters.html
    AVCodecParameters *pCodecParameters =  NULL;
    int video_stream_index = -1;
	AVCodecParserContext *pCodecParserCtx=NULL;
	enum AVCodecID codec_id=AV_CODEC_ID_H264;

	pCodecParserCtx=av_parser_init(codec_id);
	if (!pCodecParserCtx){
		printf("Could not allocate video parser context\n");
		return -1;
	}

    // loop though all the streams and print its main information
    for (int i = 0; i < pFormatContext->nb_streams; i++)
    {
        AVCodecParameters *pLocalCodecParameters =  NULL;
        pLocalCodecParameters = pFormatContext->streams[i]->codecpar;
        logging("AVStream->time_base before open coded %d/%d", pFormatContext->streams[i]->time_base.num, pFormatContext->streams[i]->time_base.den);
        logging("AVStream->r_frame_rate before open coded %d/%d", pFormatContext->streams[i]->r_frame_rate.num, pFormatContext->streams[i]->r_frame_rate.den);
        logging("AVStream->start_time %" PRId64, pFormatContext->streams[i]->start_time);
        logging("AVStream->duration %" PRId64, pFormatContext->streams[i]->duration);

        logging("finding the proper decoder (CODEC)");

        AVCodec *pLocalCodec = NULL;

        // finds the registered decoder for a codec ID
        // https://ffmpeg.org/doxygen/trunk/group__lavc__decoding.html#ga19a0ca553277f019dd5b0fec6e1f9dca
        pLocalCodec = avcodec_find_decoder(pLocalCodecParameters->codec_id);

        if (pLocalCodec==NULL) {
            logging("ERROR unsupported codec!");
            return -1;
        }

        // when the stream is a video we store its index, codec parameters and codec
        if (pLocalCodecParameters->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            pCodec = pLocalCodec;
            pCodecParameters = pLocalCodecParameters;

            logging("Video Codec: resolution %d x %d", pLocalCodecParameters->width, pLocalCodecParameters->height);
        } else if (pLocalCodecParameters->codec_type == AVMEDIA_TYPE_AUDIO) {
            logging("Audio Codec: %d channels, sample rate %d", pLocalCodecParameters->channels, pLocalCodecParameters->sample_rate);
        }

        // print its name, id and bitrate
        logging("\tCodec %s ID %d bit_rate %lld", pLocalCodec->name, pLocalCodec->id, pCodecParameters->bit_rate);
    }
    // https://ffmpeg.org/doxygen/trunk/structAVCodecContext.html
    AVCodecContext *pCodecContext = avcodec_alloc_context3(pCodec);
    if (!pCodecContext)
    {
        logging("failed to allocated memory for AVCodecContext");
        return -1;
    }

    // Fill the codec context based on the values from the supplied codec parameters
    // https://ffmpeg.org/doxygen/trunk/group__lavc__core.html#gac7b282f51540ca7a99416a3ba6ee0d16
    if (avcodec_parameters_to_context(pCodecContext, pCodecParameters) < 0)
    {
        logging("failed to copy codec params to codec context");
        return -1;
    }

    // Initialize the AVCodecContext to use the given AVCodec.
    // https://ffmpeg.org/doxygen/trunk/group__lavc__core.html#ga11f785a188d7d9df71621001465b0f1d
    if (avcodec_open2(pCodecContext, pCodec, NULL) < 0)
    {
        logging("failed to open codec through avcodec_open2");
        return -1;
    }

    // https://ffmpeg.org/doxygen/trunk/structAVFrame.html
    AVFrame *pFrame = av_frame_alloc();
    if (!pFrame)
    {
        logging("failed to allocated memory for AVFrame");
        return -1;
    }
    // https://ffmpeg.org/doxygen/trunk/structAVPacket.html
    AVPacket *pPacket = av_packet_alloc();
    if (!pPacket)
    {
        logging("failed to allocated memory for AVPacket");
        return -1;
    }

    avformat_close_input(&pFormatContext);
    avformat_free_context(pFormatContext);

    struct SwsContext *pSwsCtx = NULL;
    //SDL
    int screen_w, screen_h;
    SDL_Window *screen=NULL;
    SDL_Renderer* sdlRenderer = NULL;
    SDL_Texture* sdlTexture = NULL;
    SDL_Rect sdlRect;
    SDL_Thread *video_tid = NULL;
    SDL_Event event;
    AVFrame *pFrameYUV = NULL;
    uint8_t *outBuffer = NULL;
    int response;
    FILE *fp_in;
         
    pFrameYUV = av_frame_alloc();
    outBuffer = (uint8_t *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, pCodecContext->width, pCodecContext->height, 1) * sizeof(uint8_t));
    av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize, outBuffer, AV_PIX_FMT_YUV420P, pCodecContext->width, pCodecContext->height, 1);

    pSwsCtx = sws_getContext(pCodecContext->width, pCodecContext->height, pCodecContext->pix_fmt,
            pCodecContext->width, pCodecContext->height, AV_PIX_FMT_YUV420P, 0, NULL, NULL, NULL);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        logging("Could not initialize SDL - ");
        logging(SDL_GetError());
        return -1;
    }
    screen_w = pCodecContext->width;
    screen_h = pCodecContext->height;
    screen = SDL_CreateWindow("WS ffmpeg player", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            pCodecContext->width/2, pCodecContext->height/2, SDL_WINDOW_OPENGL);
    if (!screen) {
        logging("SDL: could not create window - exiting");
        logging(SDL_GetError());
        return -1;
    }

    sdlRenderer = SDL_CreateRenderer(screen, -1, 0);
    sdlTexture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, pCodecContext->width, pCodecContext->height);

    sdlRect.x = 0;
    sdlRect.y = 0;
    sdlRect.w = screen_w;
    sdlRect.h = screen_h;

    video_tid = SDL_CreateThread(sfp_refresh_thread, NULL, NULL);

    fp_in = fopen(argv[1], "rb");
    if (!fp_in) {
        printf("Could not open input stream\n");
        return -1;
    }

    for (;;) {
        //Wait
        SDL_WaitEvent(&event);
        if (event.type == SFM_REFRESH_EVENT) {
            int ret = parse_packet(pCodecContext, pCodecParserCtx, fp_in, pFrame, pPacket);
            logging("parse_packet: %d", ret);
            if (ret == 0) {

                // if it's the video stream

                    logging("AVPacket->pts %" PRId64, pPacket->pts);
                    decode_packet(pPacket, pCodecContext, pFrame);

                    logging("scaling");
                    if (sws_scale(pSwsCtx, (const uint8_t* const*)pFrame->data, pFrame->linesize, 0, pCodecContext->height,
                                pFrameYUV->data, pFrameYUV->linesize) == 0) {
                        continue;
                    }
                    av_frame_unref(pFrame);

                    SDL_UpdateTexture(sdlTexture, NULL, pFrameYUV->data[0], pFrameYUV->linesize[0]);
                    SDL_RenderClear(sdlRenderer);
                    SDL_RenderCopy(sdlRenderer, sdlTexture, &sdlRect, NULL);
                    SDL_RenderPresent(sdlRenderer);

            } else {
                thread_exit = 1;
            }

            // https://ffmpeg.org/doxygen/trunk/group__lavc__packet.html#ga63d5a489b419bd5d45cfd09091cbcbc2
            av_packet_unref(pPacket);
        }
        else if (event.type == SDL_KEYDOWN) {
            logging("Pause");
            //Pause
            if (event.key.keysym.sym == SDLK_SPACE)
                thread_pause = !thread_pause;
        }
        else if (event.type == SDL_QUIT) {
            logging("quit");
            thread_exit = 1;
            break;
        }
        else if (event.type == SFM_BREAK_EVENT) {
            logging("break");
            break;
        }
    }
    SDL_Quit();

    logging("releasing all the resources");

    if (pSwsCtx != NULL) {
        sws_freeContext(pSwsCtx);
    }
    if (outBuffer != NULL) {
        av_free(outBuffer);
    }
    if (pFrameYUV != NULL) {
        av_frame_free(&pFrameYUV);
    }

    av_packet_free(&pPacket);
    av_frame_free(&pFrame);
    avcodec_free_context(&pCodecContext);
    return 0;
}

static void logging(const char *fmt, ...)
{
    va_list args;
    fprintf( stderr, "LOG: " );
    va_start( args, fmt );
    vfprintf( stderr, fmt, args );
    va_end( args );
    fprintf( stderr, "\n" );
}

static int decode_packet(AVPacket *pPacket, AVCodecContext *pCodecContext, AVFrame *pFrame)
{
    // Supply raw packet data as input to a decoder
    // https://ffmpeg.org/doxygen/trunk/group__lavc__decoding.html#ga58bc4bf1e0ac59e27362597e467efff3
    int response = avcodec_send_packet(pCodecContext, pPacket);

    if (response < 0) {
        logging("Error while sending a packet to the decoder: %s", av_err2str(response));
        return response;
    }

        // Return decoded output data (into a frame) from a decoder
        // https://ffmpeg.org/doxygen/trunk/group__lavc__decoding.html#ga11e6542c4e66d3028668788a1a74217c
        response = avcodec_receive_frame(pCodecContext, pFrame);
        if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) {
            return response;
        } else if (response < 0) {
            logging("Error while receiving a frame from the decoder: %s", av_err2str(response));
            return response;
        }

        if (response >= 0) {
            logging(
                    "Frame %d (type=%c, size=%d bytes) pts %d key_frame %d [DTS %d]",
                    pCodecContext->frame_number,
                    av_get_picture_type_char(pFrame->pict_type),
                    pFrame->pkt_size,
                    pFrame->pts,
                    pFrame->key_frame,
                    pFrame->coded_picture_number
                   );

        }
    return response;
}

static void render_frame(struct SwsContext *pSwsCtx, AVCodecContext *pCodecCtx, 
        SDL_Renderer* sdlRenderer, SDL_Texture* sdlTexture, SDL_Rect sdlRect,
        AVFrame *pFrame, AVFrame *pFrameYUV)
{
    if (sws_scale(pSwsCtx, (const uint8_t* const*)pFrame->data, pFrame->linesize, 0, pCodecCtx->height,
                pFrameYUV->data, pFrameYUV->linesize) == 0) {
        return;
    }

    SDL_UpdateTexture(sdlTexture, NULL, pFrameYUV->data[0], pFrameYUV->linesize[0]);
    SDL_RenderClear(sdlRenderer);
    SDL_RenderCopy(sdlRenderer, sdlTexture, &sdlRect, NULL);
    SDL_RenderPresent(sdlRenderer);
}

static int parse_packet(AVCodecContext *pCodecCtx, 
        AVCodecParserContext *pCodecParserCtx,
        FILE *fp_in,
        AVFrame *pFrame, AVPacket *pPacket)
{
	static const int in_buffer_size=4096;
	static unsigned char in_buffer[in_buffer_size + AV_INPUT_BUFFER_PADDING_SIZE]={0};
	static unsigned char *cur_ptr;
	static int cur_size = 0;

    logging("parse_packet: %lld", fp_in);

    // consume remaining buffer
    while (cur_size>0){
        logging("consume remaining buffer");

        int len = av_parser_parse2(
                pCodecParserCtx, pCodecCtx,
                &(pPacket->data), &(pPacket->size),
                cur_ptr, cur_size ,
                AV_NOPTS_VALUE, AV_NOPTS_VALUE, AV_NOPTS_VALUE);

        cur_ptr += len;
        cur_size -= len;

        if (pPacket->size > 0)
            return 0;
    }

    while (1) {
        cur_size = fread(in_buffer, 1, in_buffer_size, fp_in);
        if (cur_size == 0) {
            logging("EOF");
            return EOF;
        }

        cur_ptr=in_buffer;

        while (cur_size>0) {

            int len = av_parser_parse2(
                    pCodecParserCtx, pCodecCtx,
                    &(pPacket->data), &(pPacket->size),
                    cur_ptr, cur_size ,
                    AV_NOPTS_VALUE, AV_NOPTS_VALUE, AV_NOPTS_VALUE);

            cur_ptr += len;
            cur_size -= len;

            if (pPacket->size > 0)
                return 0;
        }
    }
}
