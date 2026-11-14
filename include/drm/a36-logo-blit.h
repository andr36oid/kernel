/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef A36_LOGO_BLIT_H
#define A36_LOGO_BLIT_H
/* Bounded raw DRM little-endian RGB565/RGB888/XRGB8888 conversion. No BMP
 * decoder, allocation, device I/O or permanent extra framebuffer.
 */
struct a36_logo_pixels {
 unsigned char *data;
 unsigned long long bytes;
 unsigned int width,height,pitch,offset,bpp;
};
static inline int a36_logo_pixels_valid(const struct a36_logo_pixels *p)
{
 unsigned long long n;
 if(!p || !p->data || !p->width || !p->height || p->width>4096 ||
    p->height>4096 || (p->bpp!=16 && p->bpp!=24 && p->bpp!=32) ||
    p->pitch<(unsigned long long)p->width*(p->bpp/8) ||
    p->offset>=p->bytes)return -1;
 n=(unsigned long long)p->pitch*p->height;
 return n>p->bytes-p->offset ? -1:0;
}
static inline unsigned int a36_logo_pixel(const unsigned char *p,unsigned int bpp)
{
 unsigned int v;
 if(bpp!=16)return p[0]|((unsigned int)p[1]<<8)|((unsigned int)p[2]<<16);
 v=p[0]|((unsigned int)p[1]<<8);
 return (((v>>11)*255/31)<<16)|((((v>>5)&63)*255/63)<<8)|((v&31)*255/31);
}
static inline int a36_logo_blit(struct a36_logo_pixels *dst,
 const struct a36_logo_pixels *src,unsigned int dx,unsigned int dy,
 unsigned int dw,unsigned int dh,unsigned int sx,unsigned int sy,
 unsigned int sw,unsigned int sh,int reflect_y)
{
 unsigned int x,y,px,py,v;unsigned char *d;const unsigned char *s;
 if(a36_logo_pixels_valid(dst)||a36_logo_pixels_valid(src)||!dw||!dh||!sw||!sh||
    dx>=dst->width || dy>=dst->height || dw>dst->width-dx || dh>dst->height-dy||
    sx>=src->width || sy>=src->height || sw>src->width-sx || sh>src->height-sy)
  return -1;
 for(y=0;y<dh;y++){
  py=sy+(unsigned long long)y*sh/dh;
  if(reflect_y)py=src->height-1-py;
  s=src->data+src->offset+(unsigned long long)py*src->pitch;
  d=dst->data+dst->offset+(unsigned long long)(y+dy)*dst->pitch+dx*(dst->bpp/8);
  for(x=0;x<dw;x++){
   px=sx+(unsigned long long)x*sw/dw;v=a36_logo_pixel(s+px*(src->bpp/8),src->bpp);
   if(dst->bpp==16){v=(((v>>19)&31)<<11)|(((v>>10)&63)<<5)|((v>>3)&31);d[0]=v;d[1]=v>>8;}
   else{d[0]=v;d[1]=v>>8;d[2]=v>>16;if(dst->bpp==32)d[3]=255;}
   d+=dst->bpp/8;
  }
 }
 return 0;
}
#endif
