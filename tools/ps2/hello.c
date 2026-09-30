/* Phase-0 hardware probe. Does not contain or change SRB2 game logic. */
#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libpad.h>
#include <libpwroff.h>
#include <audsrv.h>
/* Direct RPC descriptors are never passed to newlib, and vice versa. */
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <screenshot.h>

static char padbuf[256] __attribute__((aligned(64)));
static u32 count(void) { u32 v; __asm__ volatile("mfc0 %0,$9" : "=r"(v)); return v; }
static int module(const char *path) {
    int r = SifLoadModule(path,0,NULL);
    printf("P0 module=%s result=%d\n",path,r);
    return r;
}
static u32 rgb(unsigned i) { return i | ((i ^ 0x55u)<<8) | ((255u-i)<<16); }
static unsigned swap34(unsigned i) { return (i & ~24u) | ((i & 8u)<<1) | ((i & 16u)>>1); }
int main(int argc, char **argv) {
    u32 start = count(), t;
    int fd, r, n, failures=0, state=0;
    unsigned i, x, y, bad;
    GSGLOBAL *gs;
    GSTEXTURE tex;
    u8 *pixels=memalign(64,64000), *io=memalign(64,2048);
    u32 *clut=memalign(64,1024), *readback=memalign(64,320*224*4);
    short *pcm=memalign(64,4096);
    struct padButtonStatus buttons;
    struct audsrv_fmt_t fmt={44100,16,2};
    printf("P0 hello argc=%d argv0=%s memory=%ld stack=524288\n",argc,argv[0],(long)GetMemorySize());
    if (!pixels || !io || !clut || !readback || !pcm) { printf("P0 FAIL allocation\n"); return 1; }
    printf("P0 aligned64=%d buffers=%u\n", !(((u32)pixels|(u32)io|(u32)clut|(u32)readback|(u32)pcm|(u32)padbuf)&63),64000+2048+1024+320*224*4+4096+256);
    sceSifInitRpc(0);
    /* Host filesystem is also exercised through newlib, solely as a probe. */
    { FILE *f=fopen("host:sentinel.bin","rb"); n=f ? (int)fread(io,1,2048,f) : -1;
      if(f) fclose(f);
      printf("P0 stdio_host bytes=%d match=%d\n",n,n==2048 && io[0]==0x53 && io[2047]==0x42); }
    t=count();
    if(module("host:iomanX.irx")<0 || module("host:fileXio.irx")<0) return 2;
    fileXioInit();
    fd=fileXioOpen("host:sentinel.bin",O_RDONLY,0);
    n=fd>=0 ? fileXioRead(fd,io,2048) : fd;
    if(fd>=0) fileXioClose(fd);
    r=n==2048 && io[0]==0x53 && io[2047]==0x42;
    failures+=!r; printf("P0 fileXio_host bytes=%d match=%d count=%lu\n",n,r,(unsigned long)(count()-t));
    fd=fileXioOpen("host:host-write.bin",O_WRONLY|O_CREAT|O_TRUNC,0666);
    SyncDCache(io,io+2048);
    n=fd>=0 ? fileXioWrite(fd,io,2048) : fd;
    if(fd>=0) fileXioClose(fd);
    failures+=n!=2048; printf("P0 fileXio_write bytes=%d\n",n);
    t=count();
    dmaKit_init(D_CTRL_RELE_OFF,D_CTRL_MFD_OFF,D_CTRL_STS_UNSPEC,D_CTRL_STD_OFF,D_CTRL_RCYC_8,1<<DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gs=gsKit_init_global();
    gs->Mode=GS_MODE_NTSC; gs->Width=320; gs->Height=224;
    gs->Interlace=GS_NONINTERLACED; gs->Field=GS_FRAME;
    gs->PSM=GS_PSM_CT32; gs->ZBuffering=GS_SETTING_OFF; gs->Dithering=GS_SETTING_OFF;
    gsKit_init_screen(gs); gsKit_mode_switch(gs,GS_ONESHOT);
    memset(&tex,0,sizeof(tex)); tex.Width=320; tex.Height=200;
    tex.PSM=GS_PSM_T8; tex.ClutPSM=GS_PSM_CT32; tex.Filter=GS_FILTER_NEAREST;
    tex.Mem=(u32*)pixels; tex.Clut=clut; tex.ClutStorageMode=GS_CLUT_STORAGE_CSM1;
    tex.Vram=gsKit_vram_alloc(gs,gsKit_texture_size(320,200,GS_PSM_T8),GSKIT_ALLOC_USERBUFFER);
    tex.VramClut=gsKit_vram_alloc(gs,1024,GSKIT_ALLOC_USERBUFFER);
    for(y=0;y<200;y++) for(x=0;x<320;x++) pixels[y*320+x]=(x/20)+(y/12%16)*16;
    for(i=0;i<256;i++) clut[swap34(i)]=rgb(i)|0x80000000u;
    SyncDCache(pixels,pixels+64000); SyncDCache(clut,clut+256);
    gsKit_texture_upload(gs,&tex);
    for(i=0;i<3;i++) {
        gsKit_clear(gs,GS_SETREG_RGBAQ(0,0,0,0x80,0));
        gsKit_prim_sprite_texture(gs,&tex,0.0f,12.0f,0.0f,0.0f,320.0f,212.0f,320.0f,200.0f,1,GS_SETREG_RGBAQ(0x80,0x80,0x80,0x80,0));
        gsKit_queue_exec(gs); gsKit_sync_flip(gs);
    }
    printf("P0 gs_submit count=%lu tex_vram=%lu clut_vram=%lu tbw=%lu\n",(unsigned long)(count()-t),(unsigned long)tex.Vram,(unsigned long)tex.VramClut,(unsigned long)tex.TBW);
    t=count();
    r=ps2_screenshot(readback,gs->ScreenBuffer[gs->ActiveBuffer],0,0,320,224,GS_PSM_CT32);
    bad=0;
    for(y=1;y<199;y++) for(x=1;x<319;x++) if((readback[(y+12)*320+x]&0xffffff)!=rgb(pixels[y*320+x])) bad++;
    failures+=bad!=0 || r!=0;
    printf("P0 gs_readback result=%d mismatch=%u pixels=%u first=%08lx expected=%08lx count=%lu\n",r,bad,198*318,(unsigned long)readback[13*320+1],(unsigned long)rgb(pixels[321]),(unsigned long)(count()-t));
    fd=fileXioOpen("host:gs-rgba.bin",O_WRONLY|O_CREAT|O_TRUNC,0666);
    SyncDCache(readback,readback+320*224); if(fd>=0) { fileXioWrite(fd,readback,320*224*4); fileXioClose(fd); }
    t=count();
    if(module("rom0:LIBSD")<0 || module("host:audsrv.irx")<0) return 3;
    r=audsrv_init(); failures+=r!=0; printf("P0 audsrv_init result=%d\n",r);
    r=audsrv_set_format(&fmt); failures+=r!=0; printf("P0 audio_format hz=44100 bits=16 channels=2 result=%d\n",r);
    audsrv_set_volume(25);
    n=0;
    for(i=0;i<44;i++) {
        for(x=0;x<1024;x++) { unsigned s=i*1024+x; pcm[2*x]=(s%100<50)?4000:-4000; pcm[2*x+1]=(s%200<100)?4000:-4000; }
        SyncDCache(pcm,pcm+2048); r=audsrv_wait_audio(4096);
        if(r) { failures++; break; }
        r=audsrv_play_audio((char*)pcm,4096); if(r!=4096) { failures++; break; } n+=r;
    }
    printf("P0 audio bytes=%d expected=180224 last=%d count=%lu\n",n,r,(unsigned long)(count()-t));
    /* libpad (new PADMAN), not libpadx (old ROM PADMAN). */
    t=count();
    if(module("host:sio2man.irx")<0 || module("host:padman.irx")<0) return 4;
    r=padInit(0); printf("P0 pad_init result=%d\n",r);
    r=padPortOpen(0,0,padbuf); printf("P0 pad_open result=%d\n",r); failures+=r!=1;
    for(i=0;i<180;i++) { state=padGetState(0,0); if(state==PAD_STATE_STABLE || state==PAD_STATE_FINDCTP1) break; gsKit_vsync_wait(); }
    r=padRead(0,0,&buttons); failures+=r<=0;
    printf("P0 pad state=%d read=%d buttons=%04x mode=%02x sticks=%u,%u,%u,%u count=%lu\n",state,r,buttons.btns,buttons.mode,buttons.ljoy_h,buttons.ljoy_v,buttons.rjoy_h,buttons.rjoy_v,(unsigned long)(count()-t));
    padPortClose(0,0); audsrv_quit();
    printf("P0 COMPLETE failures=%d total_count=%lu\n",failures,(unsigned long)(count()-start));
    if(module("host:poweroff.irx")<0) return 5;
    r=poweroffInit(); printf("P0 poweroff_init result=%d\n",r);
    printf("P0 poweroff_request\n"); poweroffShutdown();
    printf("P0 poweroff_returned\n"); SleepThread(); return failures;
}
