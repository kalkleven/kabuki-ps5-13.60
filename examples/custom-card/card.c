/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "kabuki_plugin.h"
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
int _fltused;
#else
#define EXPORT __attribute__((visibility("default")))
#endif
struct counter { kabuki_widget shape,number; unsigned count; };
static struct counter state;
static int create(const kabuki_api* a,kabuki_widget card,void** instance) {
    state.count=0;
    state.shape=a->skin(a->context,card,(kabuki_box){24,80,72,72},2,36);
    kabuki_widget button=a->skin(a->context,card,(kabuki_box){24,300,472,80},1,12);
    a->tint(a->context,button,(kabuki_colour){1,1,1,1});
    state.number=a->label(a->context,card,(kabuki_box){40,314,440,52},26,1,(kabuki_colour){.08f,.08f,.08f,1},1,3,"Press Cross to count");
    *instance=&state;return state.shape&&button&&state.number?0:-1;
}
static int frame(const kabuki_api* a,void* instance,uint64_t now,float dt,kabuki_input input) {
    (void)dt;struct counter* s=instance;
    float p=(float)(now%4000000)/2000000.f;p=p>1?2-p:p;
    a->geometry(a->context,s->shape,(kabuki_box){24+p*376,80,72,72});
    a->tint(a->context,s->shape,(kabuki_colour){.2f+p*.7f,.7f,1-p*.5f,1});
    if(input.pressed&0x4000) {
        char text[32]="Count: ",digits[12];unsigned n=++s->count;int count=0,at=7;
        do{digits[count++]=(char)('0'+n%10);n/=10;}while(n);
        while(count)text[at++]=digits[--count];text[at]=0;
        a->words(a->context,s->number,text);a->action(a->context,"count",(int)s->count);
    }
    a->focus(a->context,s->number,(kabuki_box){24,300,472,80},12);
    return 0;
}
static void destroy(const kabuki_api* a,void* instance){(void)a;(void)instance;}
EXPORT const struct kabuki_plugin kabuki_plugin_v1={KABUKI_PLUGIN_ABI,sizeof(struct kabuki_plugin),create,frame,destroy};
EXPORT void kabuki_plugin_entry(void) {} /* mapped, never called */
