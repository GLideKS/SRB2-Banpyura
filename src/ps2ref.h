/* Host-only observational reference hooks. GPL-2.0, like the engine. */
#ifndef SRB2_PS2REF_H
#define SRB2_PS2REF_H
#include "doomtype.h"
#ifdef PS2REF
void PS2Ref_Init(void);
void PS2Ref_Tic(void);
void PS2Ref_Frame(void);
void PS2Ref_Number(const char *word, INT32 value);
void PS2Ref_Sfx(const char *name, const void *data, size_t size);
void PS2Ref_End(void);
boolean PS2Ref_Clock(void);
#else
#define PS2Ref_Init() ((void)0)
#define PS2Ref_Tic() ((void)0)
#define PS2Ref_Frame() ((void)0)
#define PS2Ref_Number(w,v) ((void)0)
#define PS2Ref_Sfx(n,d,s) ((void)0)
#define PS2Ref_End() ((void)0)
#define PS2Ref_Clock() false
#endif
#endif
