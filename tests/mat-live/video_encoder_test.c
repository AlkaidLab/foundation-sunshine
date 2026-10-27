/* Generated video -> anonymous pipe only. Not used by RTSP/native acceptance. */
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <io.h>
int main(void){
 _setmode(_fileno(stdout),_O_BINARY);const AVCodec* codec=avcodec_find_encoder_by_name("libx264");AVCodecContext* enc=avcodec_alloc_context3(codec);AVFrame* frame=av_frame_alloc();AVPacket* packet=av_packet_alloc();if(!codec||!enc||!frame||!packet)return 1;
 enc->width=640;enc->height=360;enc->pix_fmt=AV_PIX_FMT_YUV420P;enc->time_base=(AVRational){1,30};enc->framerate=(AVRational){30,1};enc->gop_size=30;enc->max_b_frames=0;enc->thread_count=1;
 av_opt_set(enc->priv_data,"preset","ultrafast",0);av_opt_set(enc->priv_data,"tune","zerolatency",0);if(avcodec_open2(enc,codec,NULL)<0)return 1;
 frame->width=640;frame->height=360;frame->format=enc->pix_fmt;if(av_frame_get_buffer(frame,32)<0)return 1;
 for(int n=0;n<120;++n){if(av_frame_make_writable(frame)<0)return 1;
  for(int y=0;y<360;++y)for(int x=0;x<640;++x)frame->data[0][y*frame->linesize[0]+x]=(uint8_t)((x+y+n)%220+16);
  for(int y=0;y<180;++y){memset(frame->data[1]+y*frame->linesize[1],110,320);memset(frame->data[2]+y*frame->linesize[2],140,320);}frame->pts=n;
  if(avcodec_send_frame(enc,frame)<0)return 1;while(avcodec_receive_packet(enc,packet)>=0){uint32_t length=packet->size;uint64_t pts=(uint64_t)packet->pts*1000000/30;if(fwrite(&length,4,1,stdout)!=1||fwrite(&pts,8,1,stdout)!=1||fwrite(packet->data,1,length,stdout)!=length)return 1;av_packet_unref(packet);}
 }
 av_packet_free(&packet);av_frame_free(&frame);avcodec_free_context(&enc);return 0;
}
