/*
    Copyright (C) 2004 Michael Niedermayer <michaelni@gmx.at>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

// #include <stdio.h>
#include <stdlib.h>
// #include <unistd.h>

#include "config.h"
#include "mp_msg.h"
// #include "help_mp.h"

#include "stream.h"
#include "demuxer.h"
#include "stheader.h"

#ifdef USE_LIBAVFORMAT_SO
#include <ffmpeg/avformat.h>
#else
#include "avformat.h"
#include "avi.h"
#endif

#define PROBE_BUF_SIZE 2048

typedef struct lavf_priv_t{
    AVInputFormat *avif;
    AVFormatContext *avfc;
    ByteIOContext pb;
    int audio_streams;
    int video_streams;
    int64_t last_pts;
}lavf_priv_t;

extern void print_wave_header(WAVEFORMATEX *h);
extern void print_video_header(BITMAPINFOHEADER *h);

int64_t ff_gcd(int64_t a, int64_t b);

static int mp_open(URLContext *h, const char *filename, int flags){
    return 0;
}

static int mp_read(URLContext *h, unsigned char *buf, int size){
    stream_t *stream = (stream_t*)h->priv_data;
    int ret;

    if(stream_eof(stream)) //needed?
        return -1;
    ret=stream_read(stream, buf, size);

    mp_msg(MSGT_HEADER,MSGL_DBG2,"%d=mp_read(%p, %p, %d), eof:%d\n", ret, h, buf, size, stream->eof);
    return ret;
}

static int mp_write(URLContext *h, unsigned char *buf, int size){
    return -1;
}

static offset_t mp_seek(URLContext *h, offset_t pos, int whence){
    stream_t *stream = (stream_t*)h->priv_data;
    
    mp_msg(MSGT_HEADER,MSGL_DBG2,"mp_seek(%p, %d, %d)\n", h, (int)pos, whence);
    if(whence == SEEK_CUR)
        pos +=stream_tell(stream);
    else if(whence == SEEK_END)
        pos += stream->end_pos;
    else if(whence != SEEK_SET)
        return -1;

    if(pos<stream->end_pos && stream->eof)
        stream_reset(stream);
    if(stream_seek(stream, pos)==0)
        return -1;

    return pos;
}

static int mp_close(URLContext *h){
    return 0;
}

static URLProtocol mp_protocol = {
    "mp",
    mp_open,
    mp_read,
    mp_write,
    mp_seek,
    mp_close,
};

static int lavf_check_file(demuxer_t *demuxer){
    AVProbeData avpd;
    uint8_t buf[PROBE_BUF_SIZE];
    lavf_priv_t *priv;
    
    if(!demuxer->priv) 
        demuxer->priv=calloc(sizeof(lavf_priv_t),1);
    priv= demuxer->priv;

    av_register_all();

    if(stream_read(demuxer->stream, buf, PROBE_BUF_SIZE)!=PROBE_BUF_SIZE)
        return 0;
    avpd.filename= demuxer->stream->url;
    avpd.buf= buf;
    avpd.buf_size= PROBE_BUF_SIZE;

    priv->avif= av_probe_input_format(&avpd, 1);
    if(!priv->avif){
        mp_msg(MSGT_HEADER,MSGL_V,"LAVF_check: no clue about this gibberish!\n");
        return 0;
    }else
        mp_msg(MSGT_HEADER,MSGL_V,"LAVF_check: %s\n", priv->avif->long_name);

    return DEMUXER_TYPE_LAVF;
}
    
static demuxer_t* demux_open_lavf(demuxer_t *demuxer){
    AVFormatContext *avfc;
    AVFormatParameters ap;
    lavf_priv_t *priv= demuxer->priv;
    int i,g;
    char mp_filename[256]="mp:";

    memset(&ap, 0, sizeof(AVFormatParameters));

    stream_seek(demuxer->stream, 0);

    register_protocol(&mp_protocol);

    if(demuxer->stream->url)
        strncpy(mp_filename + 3, demuxer->stream->url, sizeof(mp_filename)-3);
    else
        strncpy(mp_filename + 3, "foobar.dummy", sizeof(mp_filename)-3);
    
    url_fopen(&priv->pb, mp_filename, URL_RDONLY);
    
    ((URLContext*)(priv->pb.opaque))->priv_data= demuxer->stream;
        
    if(av_open_input_stream(&avfc, &priv->pb, mp_filename, priv->avif, &ap)<0){
        mp_msg(MSGT_HEADER,MSGL_ERR,"LAVF_header: av_open_input_stream() failed\n");
        return NULL;
    }

    priv->avfc= avfc;

    if(av_find_stream_info(avfc) < 0){
        mp_msg(MSGT_HEADER,MSGL_ERR,"LAVF_header: av_find_stream_info() failed\n");
        return NULL;
    }

    if(avfc->title    [0]) demux_info_add(demuxer, "name"     , avfc->title    );
    if(avfc->author   [0]) demux_info_add(demuxer, "author"   , avfc->author   );
    if(avfc->copyright[0]) demux_info_add(demuxer, "copyright", avfc->copyright);
    if(avfc->comment  [0]) demux_info_add(demuxer, "comments" , avfc->comment  );
    if(avfc->album    [0]) demux_info_add(demuxer, "album"    , avfc->album    );
//    if(avfc->year        ) demux_info_add(demuxer, "year"     , avfc->year     );
//    if(avfc->track       ) demux_info_add(demuxer, "track"    , avfc->track    );
    if(avfc->genre    [0]) demux_info_add(demuxer, "genre"    , avfc->genre    );

    for(i=0; i<avfc->nb_streams; i++){
        AVStream *st= avfc->streams[i];
#if LIBAVFORMAT_BUILD >= 4629
        AVCodecContext *codec= st->codec;
#else
        AVCodecContext *codec= &st->codec;
#endif
        
        switch(codec->codec_type){
        case CODEC_TYPE_AUDIO:{
            WAVEFORMATEX *wf= calloc(sizeof(WAVEFORMATEX) + codec->extradata_size, 1);
            sh_audio_t* sh_audio=new_sh_audio(demuxer, i);
            priv->audio_streams++;
            if(!codec->codec_tag)
                codec->codec_tag= codec_get_wav_tag(codec->codec_id);
            wf->wFormatTag= codec->codec_tag;
            wf->nChannels= codec->channels;
            wf->nSamplesPerSec= codec->sample_rate;
            wf->nAvgBytesPerSec= codec->bit_rate/8;
            wf->nBlockAlign= codec->block_align;
            wf->wBitsPerSample= codec->bits_per_sample;
            wf->cbSize= codec->extradata_size;
            if(codec->extradata_size){
                memcpy(
                    wf + 1, 
                    codec->extradata,
                    codec->extradata_size);
            }
            sh_audio->wf= wf;
            sh_audio->audio.dwSampleSize= codec->block_align;
            if(codec->frame_size && codec->sample_rate){
                sh_audio->audio.dwScale=codec->frame_size;
                sh_audio->audio.dwRate= codec->sample_rate;
            }else{
                sh_audio->audio.dwScale= codec->block_align ? codec->block_align*8 : 8;
                sh_audio->audio.dwRate = codec->bit_rate;
            }
            g= ff_gcd(sh_audio->audio.dwScale, sh_audio->audio.dwRate);
            sh_audio->audio.dwScale /= g;
            sh_audio->audio.dwRate  /= g;
//            printf("sca:%d rat:%d fs:%d sr:%d ba:%d\n", sh_audio->audio.dwScale, sh_audio->audio.dwRate, codec->frame_size, codec->sample_rate, codec->block_align);
            sh_audio->ds= demuxer->audio;
            sh_audio->format= codec->codec_tag;
            sh_audio->channels= codec->channels;
            sh_audio->samplerate= codec->sample_rate;
            sh_audio->i_bps= codec->bit_rate/8;
            switch (codec->codec_id) {
              case CODEC_ID_PCM_S8:
              case CODEC_ID_PCM_U8:
                sh_audio->samplesize = 1;
                break;
              case CODEC_ID_PCM_S16LE:
              case CODEC_ID_PCM_S16BE:
              case CODEC_ID_PCM_U16LE:
              case CODEC_ID_PCM_U16BE:
                sh_audio->samplesize = 2;
                break;
              case CODEC_ID_PCM_ALAW:
                sh_audio->format = 0x6;
                break;
              case CODEC_ID_PCM_MULAW:
                sh_audio->format = 0x7;
                break;
            }
            if(verbose>=1) print_wave_header(sh_audio->wf);
            if(demuxer->audio->id != i && demuxer->audio->id != -1)
                st->discard= AVDISCARD_ALL;
            else{
                demuxer->audio->id = i;
                demuxer->audio->sh= demuxer->a_streams[i];
            }
            break;}
        case CODEC_TYPE_VIDEO:{
            BITMAPINFOHEADER *bih=calloc(sizeof(BITMAPINFOHEADER) + codec->extradata_size,1);
            sh_video_t* sh_video=new_sh_video(demuxer, i);

	    priv->video_streams++;
            if(!codec->codec_tag)
                codec->codec_tag= codec_get_bmp_tag(codec->codec_id);
            bih->biSize= sizeof(BITMAPINFOHEADER) + codec->extradata_size;
            bih->biWidth= codec->width;
            bih->biHeight= codec->height;
            bih->biBitCount= codec->bits_per_sample;
            bih->biSizeImage = bih->biWidth * bih->biHeight * bih->biBitCount/8;
            bih->biCompression= codec->codec_tag;
            sh_video->bih= bih;
            sh_video->disp_w= codec->width;
            sh_video->disp_h= codec->height;
            if (st->time_base.den) { /* if container has time_base, use that */
                sh_video->video.dwRate= st->time_base.den;
                sh_video->video.dwScale= st->time_base.num;
            } else {
#if LIBAVFORMAT_BUILD >= 4624
            sh_video->video.dwRate= codec->time_base.den;
            sh_video->video.dwScale= codec->time_base.num;
#else
            sh_video->video.dwRate= codec->frame_rate;
            sh_video->video.dwScale= codec->frame_rate_base;
#endif
            }
            sh_video->fps=(float)sh_video->video.dwRate/(float)sh_video->video.dwScale;
            sh_video->frametime=(float)sh_video->video.dwScale/(float)sh_video->video.dwRate;
            sh_video->format = bih->biCompression;
            sh_video->aspect=   codec->width * codec->sample_aspect_ratio.num 
                              / (float)(codec->height * codec->sample_aspect_ratio.den);
            sh_video->i_bps= codec->bit_rate/8;
            mp_msg(MSGT_DEMUX,MSGL_DBG2,"aspect= %d*%d/(%d*%d)\n", 
                codec->width, codec->sample_aspect_ratio.num,
                codec->height, codec->sample_aspect_ratio.den);

            sh_video->ds= demuxer->video;
            if(codec->extradata_size)
                memcpy(sh_video->bih + 1, codec->extradata, codec->extradata_size);
            if(verbose>=1) print_video_header(sh_video->bih);
/*    short 	biPlanes;
    int  	biXPelsPerMeter;
    int  	biYPelsPerMeter;
    int 	biClrUsed;
    int 	biClrImportant;*/
            if(demuxer->video->id != i && demuxer->video->id != -1)
                st->discard= AVDISCARD_ALL;
            else{
                demuxer->video->id = i;
                demuxer->video->sh= demuxer->v_streams[i];
            }
            break;}
        default:
            st->discard= AVDISCARD_ALL;
        }
    }
    
    mp_msg(MSGT_HEADER,MSGL_V,"LAVF: %d audio and %d video streams found\n",priv->audio_streams,priv->video_streams);
    mp_msg(MSGT_HEADER,MSGL_V,"LAVF: build %d\n", LIBAVFORMAT_BUILD);
    if(!priv->audio_streams) demuxer->audio->id=-2;  // nosound
//    else if(best_audio > 0 && demuxer->audio->id == -1) demuxer->audio->id=best_audio;
    if(!priv->video_streams){
        if(!priv->audio_streams){
	    mp_msg(MSGT_HEADER,MSGL_ERR,"LAVF: no audio or video headers found - broken file?\n");
            return NULL; 
        }
        demuxer->video->id=-2; // audio-only
    } //else if (best_video > 0 && demuxer->video->id == -1) demuxer->video->id = best_video;

    return demuxer;
}

static int demux_lavf_fill_buffer(demuxer_t *demux, demux_stream_t *dsds){
    lavf_priv_t *priv= demux->priv;
    AVPacket pkt;
    demux_packet_t *dp;
    demux_stream_t *ds;
    int id;
    mp_msg(MSGT_DEMUX,MSGL_DBG2,"demux_lavf_fill_buffer()\n");

    demux->filepos=stream_tell(demux->stream);

    if(stream_eof(demux->stream)){
//        demuxre->stream->eof=1;
        return 0;
    }

    if(av_read_frame(priv->avfc, &pkt) < 0)
        return 0;
        
    id= pkt.stream_index;

    if(id==demux->audio->id){
        // audio
        ds=demux->audio;
        if(!ds->sh){
            ds->sh=demux->a_streams[id];
            mp_msg(MSGT_DEMUX,MSGL_V,"Auto-selected LAVF audio ID = %d\n",ds->id);
        }
    } else if(id==demux->video->id){
        // video
        ds=demux->video;
        if(!ds->sh){
            ds->sh=demux->v_streams[id];
            mp_msg(MSGT_DEMUX,MSGL_V,"Auto-selected LAVF video ID = %d\n",ds->id);
        }
    } else {
        av_free_packet(&pkt);
        return 1;
    }
        
    if(0/*pkt.destruct == av_destruct_packet*/){
        //ok kids, dont try this at home :)
        dp=(demux_packet_t*)malloc(sizeof(demux_packet_t));
        dp->len=pkt.size;
        dp->next=NULL;
        dp->refcount=1;
        dp->master=NULL;
        dp->buffer=pkt.data;
        pkt.destruct= NULL;
    }else{
        dp=new_demux_packet(pkt.size);
        memcpy(dp->buffer, pkt.data, pkt.size);
        av_free_packet(&pkt);
    }

    if(pkt.pts != AV_NOPTS_VALUE){
#if LIBAVFORMAT_BUILD >= 4624
        dp->pts=pkt.pts * av_q2d(priv->avfc->streams[id]->time_base);
        priv->last_pts= dp->pts * AV_TIME_BASE;
#else
        priv->last_pts= pkt.pts;
        dp->pts=pkt.pts / (float)AV_TIME_BASE;
#endif
    }
    dp->pos=demux->filepos;
    dp->flags= !!(pkt.flags&PKT_FLAG_KEY);
    // append packet to DS stream:
    ds_add_packet(ds,dp);
    return 1;
}

static void demux_seek_lavf(demuxer_t *demuxer, float rel_seek_secs, int flags){
    lavf_priv_t *priv = demuxer->priv;
    mp_msg(MSGT_DEMUX,MSGL_DBG2,"demux_seek_lavf(%p, %f, %d)\n", demuxer, rel_seek_secs, flags);
    
#if LIBAVFORMAT_BUILD < 4619
    av_seek_frame(priv->avfc, -1, priv->last_pts + rel_seek_secs*AV_TIME_BASE);
#else
    av_seek_frame(priv->avfc, -1, priv->last_pts + rel_seek_secs*AV_TIME_BASE, rel_seek_secs < 0 ? AVSEEK_FLAG_BACKWARD : 0);
#endif
}

static int demux_lavf_control(demuxer_t *demuxer, int cmd, void *arg)
{
    lavf_priv_t *priv = demuxer->priv;
    
    switch (cmd) {
        case DEMUXER_CTRL_GET_TIME_LENGTH:
	    if (priv->avfc->duration == 0)
	        return DEMUXER_CTRL_DONTKNOW;
	    
	    *((double *)arg) = (double)priv->avfc->duration / AV_TIME_BASE;
	    return DEMUXER_CTRL_OK;

	case DEMUXER_CTRL_GET_PERCENT_POS:
	    if (priv->avfc->duration == 0)
	        return DEMUXER_CTRL_DONTKNOW;
	    
	    *((int *)arg) = (int)(priv->last_pts*100 / priv->avfc->duration);
	    return DEMUXER_CTRL_OK;
	
	default:
	    return DEMUXER_CTRL_NOTIMPL;
    }
}

static void demux_close_lavf(demuxer_t *demuxer)
{
    lavf_priv_t* priv = demuxer->priv;
    if (priv){
        if(priv->avfc)
       {
         av_close_input_file(priv->avfc); priv->avfc= NULL;
        }
        free(priv); demuxer->priv= NULL;
    }
}


demuxer_desc_t demuxer_desc_lavf = {
  "libavformat demuxer",
  "lavf",
  "libavformat",
  "Michael Niedermayer",
  "supports many formats, requires libavformat",
  DEMUXER_TYPE_LAVF,
  0, // Check after other demuxer
  lavf_check_file,
  demux_lavf_fill_buffer,
  demux_open_lavf,
  demux_close_lavf,
  demux_seek_lavf,
  demux_lavf_control
};

