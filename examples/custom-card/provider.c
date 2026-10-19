/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "kabuki.h"
#include <unistd.h>
int main(void) {
    char plugin[96]="",marker[96];
    for(int n=0;n<8;++n){snprintf(marker,sizeof marker,"/mnt/usb%d/librespot-ps5.elf",n);
        if(!access(marker,F_OK)){snprintf(plugin,sizeof plugin,"/mnt/usb%d/kabuki-custom-card.elf",n);break;}}
    if(!*plugin)return 1;
    while(access("/data/kabuki-custom-card.stop",F_OK)) {
        kabuki_client k;if(kabuki_connect(&k)){sleep(2);continue;}
        char hello[512];snprintf(hello,sizeof hello,"{\"v\":1,\"hello\":{\"provider\":\"custom-card\",\"tiles\":[{\"id\":\"DEMOCC\",\"title\":\"Custom card\",\"card\":\"plugin\",\"plugin\":\"%s\"}]}}",plugin);
        if(kabuki_send(&k,hello)){kabuki_close(&k);sleep(2);continue;}
        kabuki_event e;
        while(access("/data/kabuki-custom-card.stop",F_OK)&&kabuki_poll(&k,&e,1000)>=0){}
        kabuki_close(&k);
    }
    return 0;
}
