#include "doomdef.h"
#include "ps2ref.h"
#include "m_argv.h"
#include "m_random.h"
#include "g_game.h"
#include "g_demo.h"
#include "d_main.h"
#include "p_local.h"
#include "info.h"
#include "screen.h"
#include "v_video.h"
#include "z_zone.h"
#include "i_system.h"
#include "i_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *tics, *frames, *numbers, *sfxlog;
static char directory[1024];
static UINT32 seq, lastframe;
static UINT32 hash_bytes(UINT32 h, const void *p, size_t n)
{
    const UINT8 *b=p;
    while (n--) { h^=*b++; h*=16777619u; }
    return h;
}
/* Integer serialization is independent of pointer size, padding and endianness. */
static UINT32 hash_u32(UINT32 h, UINT32 v)
{
    UINT8 b[4]={(UINT8)v,(UINT8)(v>>8),(UINT8)(v>>16),(UINT8)(v>>24)};
    return hash_bytes(h,b,4);
}
static FILE *output(const char *name, const char *mode)
{
    char path[1200]; FILE *f;
    snprintf(path,sizeof(path),"%s/%s",directory,name);
    f=fopen(path,mode);
    if (!f) I_Error("PS2Ref: cannot write %s",path);
    return f;
}
static void memory(const char *name)
{
    INT32 tag; FILE *f=output(name,"wb");
    fprintf(f,"tag,bytes\n");
    for(tag=0;tag<256;tag++) { size_t n=Z_TagUsage(tag); if(n) fprintf(f,"%d,%lu\n",tag,(unsigned long)n); }
    fclose(f);
}
void PS2Ref_Init(void)
{
    INT32 p=M_CheckParm("-ps2ref");
    if(!p) return;
    if(p+1>=myargc) I_Error("-ps2ref requires an existing output directory");
    snprintf(directory,sizeof(directory),"%s",myargv[p+1]);
    tics=output("tics.csv","wb"); frames=output("frames.csv","wb");
    numbers=output("soc.tsv","wb"); sfxlog=output("sfx.csv","wb");
    fprintf(tics,"seq,leveltic,map,rng,x,y,z,momx,momy,momz,health,rings,state,mobjs,state_hash\n");
    fprintf(frames,"seq,leveltic,width,height,fnv1a32\n");
    fprintf(numbers,"expression_hex\tvalue\n");
    fprintf(sfxlog,"name,bytes,fnv1a32\n");
}
void PS2Ref_Number(const char *word, INT32 value)
{
    const unsigned char *p=(const unsigned char*)word;
    if(!numbers) return;
    while(*p) fprintf(numbers,"%02x",*p++);
    fprintf(numbers,"\t%d\n",value);
}
/* A controlled platform clock: loading/title wipes cannot skip ticks due to
 * host scheduling. Timedemo already advances gameplay one tick per frame. */
boolean PS2Ref_Clock(void)
{
    if(!tics) return false;
    g_time.time++;
    g_time.timefrac=0;
    return true;
}
void PS2Ref_Tic(void)
{
    thinker_t *th; UINT32 h=2166136261u, n=0; INT32 i;
    mobj_t *mo; player_t *p;
    if(!tics || !demoplayback || gamestate!=GS_LEVEL) return;
    p=&players[consoleplayer]; mo=p->mo;
    if(!mo) I_Error("PS2Ref: demo has no player mobj");
    h=hash_u32(h,P_GetRandSeed());
    for(i=0;i<MAXPLAYERS;i++) if(playeringame[i]) {
        player_t *q=&players[i]; mobj_t *m=q->mo;
        h=hash_u32(h,i); h=hash_u32(h,q->rings); h=hash_u32(h,q->score);
        h=hash_u32(h,q->pflags); h=hash_u32(h,q->playerstate); h=hash_u32(h,q->lives);
        if(m) { h=hash_u32(h,m->x); h=hash_u32(h,m->y); h=hash_u32(h,m->z); h=hash_u32(h,m->angle); }
    }
    for(th=thlist[THINK_MOBJ].next;th!=&thlist[THINK_MOBJ];th=th->next) {
        mobj_t *m=(mobj_t*)th; n++;
        h=hash_u32(h,th->removing); h=hash_u32(h,m->type);
        h=hash_u32(h,m->x); h=hash_u32(h,m->y); h=hash_u32(h,m->z);
        h=hash_u32(h,m->momx); h=hash_u32(h,m->momy); h=hash_u32(h,m->momz);
        h=hash_u32(h,m->angle); h=hash_u32(h,m->health); h=hash_u32(h,m->tics);
        h=hash_u32(h,m->state ? (UINT32)(m->state-states) : UINT32_MAX);
        h=hash_u32(h,m->flags); h=hash_u32(h,m->flags2); h=hash_u32(h,m->eflags);
    }
    seq++;
    fprintf(tics,"%u,%u,%d,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%08x\n",
        seq,leveltime,gamemap,P_GetRandSeed(),mo->x,mo->y,mo->z,mo->momx,mo->momy,mo->momz,
        mo->health,p->rings,mo->state?(INT32)(mo->state-states):-1,n,h);
    if(seq==1) memory("memory-level.csv");
}
void PS2Ref_Frame(void)
{
    UINT32 h; char name[64]; FILE *f; size_t size;
    if(!frames || !seq || seq==lastframe || seq%35 || !demoplayback || gamestate!=GS_LEVEL) return;
    if(vid.width!=320 || vid.height!=200 || vid.bpp!=1) I_Error("PS2Ref: expected 320x200x8");
    size=(size_t)vid.width*vid.height; h=hash_bytes(2166136261u,screens[0],size);
    fprintf(frames,"%u,%u,%d,%d,%08x\n",seq,leveltime,vid.width,vid.height,h);
    snprintf(name,sizeof(name),"frame-%06u.idx",seq); f=output(name,"wb");
    if(fwrite(screens[0],1,size,f)!=size) I_Error("PS2Ref: short frame write");
    fclose(f); lastframe=seq;
}
void PS2Ref_Sfx(const char *name,const void *data,size_t size)
{
    char path[96]; FILE *f;
    if(!sfxlog || !name || !data) return;
    snprintf(path,sizeof(path),"sfx-%s.pcm",name); f=output(path,"wb");
    if(fwrite(data,1,size,f)!=size) I_Error("PS2Ref: short PCM write");
    fclose(f);
    fprintf(sfxlog,"%s,%lu,%08x\n",name,(unsigned long)size,hash_bytes(2166136261u,data,size));
}
void PS2Ref_End(void)
{
    FILE *done;
    if(!tics) return;
    memory("memory-end.csv");
    fclose(tics); fclose(frames); fclose(numbers); fclose(sfxlog);
    tics=frames=numbers=sfxlog=NULL;
    done=output("complete.txt","wb"); fprintf(done,"complete tics=%u\n",seq); fclose(done);
    I_Quit();
}
