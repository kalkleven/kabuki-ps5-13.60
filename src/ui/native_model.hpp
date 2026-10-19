// SPDX-License-Identifier: GPL-3.0-or-later
// The panel's state and navigation for the native ShellUI widgets. No rendering dependencies.
//
// Tiles come from the daemon's files (tiles.rec, tile-<ID>.rec): providers' tiles with their
// template data, and Kabuki's own System and Settings, which are always there. This decides
// what is shown and where the cursor is; native_layout.hpp draws it.
#pragma once
#include "content.hpp"
#include "player.hpp"
#include "tiles.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>

namespace overlay {
using kabuki::Card;
using kabuki::TileInfo;
constexpr int kSlots = kabuki::kSlots;

// The System card's readings; a negative or empty value is shown as unavailable.
struct SystemStats {
    float cpu=-1;                       // percent busy, all cores
    float cpu_temp=-1,soc_temp=-1;      // degrees C
    float mem_used_gb=-1,mem_total_gb=-1;
    float fps=-1,fps_min=-1;            // frames a second: average and lowest over the last few seconds
    int uptime_s=-1;
    std::string firmware,address,librespot;
};
constexpr int kHoldChoices[]={250,500,750,1000,1500,2000};
inline std::string chord_name(std::uint32_t chord) {
    struct Name { std::uint32_t bit; const char* name; };
    static constexpr Name names[]={{0x400,"L1"},{0x100,"L2"},{0x2,"L3"},{0x800,"R1"},{0x200,"R2"},{0x4,"R3"},
        {0x10,"Up"},{0x40,"Down"},{0x80,"Left"},{0x20,"Right"},{0x1000,"Triangle"},{0x2000,"Circle"},
        {0x4000,"Cross"},{0x8000,"Square"},{0x8,"Options"},{0x100000,"Touchpad"}};
    std::string text;
    for(const auto& n:names) if(chord&n.bit) { if(!text.empty()) text+=" + "; text+=n.name; }
    return text.empty()?"None":text;
}

// The card a tile opens, as the layout draws it.
enum Kind : int { kMedia=0, kList=1, kStats=3, kSettings=4, kCustom=5 };
inline int kind_of(Card c) {
    switch(c) { case Card::media: return kMedia; case Card::list: return kList; case Card::stats: return kStats;
                case Card::settings: return kSettings; case Card::custom: return kCustom; default: return kStats; }
}

struct TileView { std::string id,title,icon; int kind=kStats; bool online=false; };
struct RowView { std::string title,subtitle,art; };
struct ListView {
    std::string title,art,heading;  // the header's title and art for this level; a heading over the rows
    bool covers=true,loading=false;
    std::string empty,empty_detail;
    std::vector<RowView> rows;
    int selected=0,busy=-1,level=0; // busy: a row whose action is under way (the buffering light)
    std::string key;                // which list this is: a new key starts in place and fades in
};

struct NativeView {
    std::uintptr_t plugin_address=0;
    std::string plugin_data="{}",plugin_error;
    std::vector<std::string> plugin_live;
    std::uint32_t plugin_held=0,plugin_pressed=0;
    std::uint64_t plugin_input_seq=0;
    PlayerState state;        // the shown media card
    ListView list;            // the shown list card
    int tab=kMedia;           // the shown card's kind
    std::string tile;         // the shown card's tile
    std::vector<TileView> rail; int rail_at=0; // the rail's tiles in order, and the cursor's place
    int control=2;
    bool focused=false, seek=false;
    int seek_ms=-1, popup=0, option=0;
    int presses=0; // Cross presses on the focused item, for the press effect
    int seek_sent=-1; // a seek sent and not yet heard there: the bar and clock hold it
    SystemStats system;
    std::uint64_t opening=0; // the opening this view belongs to
    kabuki::Settings settings;
    int setting=0;            // the settings row with the cursor
    bool recording=false;     // waiting for the new shortcut
    std::uint32_t recorded=0; // buttons held so far while recording
    int page=0;               // settings: 0 the main list, 1 the sidebar's tiles
    int slot=0;               // the sidebar slot with the cursor
    bool moving=false;        // that slot's tile is being moved
    bool picking=false;       // the tile list for that slot is open
    int pick=0;               // its highlighted entry (the last is "None")
    TileView slots[kSlots];   // the Sidebar page: each slot's tile (offline or empty: not online)
    std::vector<TileView> picks; // the tile list's entries, without "None"
    bool nested=false;        // in a menu inside the card: Circle steps back instead of closing
    bool choosing=false;      // the hold-time list is open
    int choice=0;             // its highlighted entry
    std::string header, header_icon, header_art; // the card's title, icon, and art in place of the icon
};

// The console's own UI sounds (PUI SystemSoundType), as the control centre plays them.
namespace sound {
enum : int { focus_move=0, back=1, decide=2, list_decide=8, list_move=9, menu_open=10, popup_open=12,
             option_close=19, slider=21, switch_on=22, switch_off=23, tab_enter=28, cursor_end=30,
             menu_close=100 /* the control centre's close */, play_pause=101, skip=102 };
}

class ListRepeat {
    int direction=0;
    std::uint64_t next=0;
public:
    void reset() { direction=0; next=0; }
    int step(std::uint32_t pressed,std::uint32_t held,std::uint64_t now) {
        const int edge=(pressed&0x10)?-1:(pressed&0x40)?1:0;
        const int down=(held&0x50)==0x10?-1:(held&0x50)==0x40?1:0;
        if(edge) { direction=down==edge?edge:0; next=now+400; return edge; }
        if(!down||down!=direction) { reset(); return 0; }
        if(now<next) return 0;
        next=now+110;
        return direction;
    }
};

class NativeModel {
    std::function<void(int,int,const char*)> send;
    std::string dir="/user/data/custom-overlays"; // where the daemon writes (as ShellUI sees it)
    std::vector<TileInfo> online;                 // providers' tiles
    std::map<std::string,kabuki::Content> content;
    std::map<std::string,std::string> content_text;
    std::string tiles_text,settings_text,show_text,system_text;
    long long show_seq=-1;
    std::uint64_t last_meta=0,last_shown_content=0,last_other_content=0,last_stats=0,opening=0;
    ListRepeat repeat;
    std::uint32_t suppress=0;
    // Navigation: the shown card's tile and the rail cursor's.
    std::string tile,rail,last_shown;
    int control=2,popup=0,option=0,seek_ms=-1,presses=0,sent_ms=-1;
    std::uint32_t plugin_held=0,plugin_pressed=0; std::uint64_t plugin_input_seq=0;
    std::uint64_t sent_at=0,seek_deadline=0,seek_held_since=0;
    bool focused=false,seek=false,enter_when_loaded=false;
    int volume_asked=-1; std::uint64_t volume_at=0; // shown until the player reports it, or 3 s
    PlayerState media;                           // the shown media card, at this moment
    // Each list's cursor: the selected row by key (so it stays on the same row as the list
    // changes) and the opened row's list.
    struct ListState { std::string top_key,sub_key,open; int top=0,sub=0; };
    std::map<std::string,ListState> lists;
    std::vector<int> sounds; // played by the console's UI thread, in order
    SystemStats stats;
    kabuki::Settings settings;
    int setting=0,choice=0,page=0,slot=0,pick=0;
    bool recording=false,choosing=false,moving=false,picking=false;
    kabuki::Settings before_move; // put back if a move isn't placed
    std::uint32_t recorded=0;

    void sound(int type) { if(settings.sounds&&sounds.size()<16) sounds.push_back(type); }
    void action(const std::string& id,const char* name,int value=0,const std::string& key="") {
        std::string text=id+"\t"+name+"\t"+key;
        if(text.size()>=128) return; // the shared command ring's text capacity
        send(kabuki::kTileAction,value,text.c_str());
    }
    void save_settings() {
        char text[128];
        std::snprintf(text,sizeof text,"chord=%u hold=%d sounds=%d music=%d",settings.chord,settings.hold_ms,
                      settings.sounds?1:0,settings.librespot_tiles?1:0);
        send(kabuki::kSaveSettings,0,text);
    }
    void save_slots() {
        std::string text;
        for(int i=0;i<kSlots;++i) { if(i) text+=','; text+=settings.slots[i]; }
        settings.arranged=true;
        send(kabuki::kSaveSlots,0,text.c_str());
    }

    // Kabuki's own tiles are always there; providers' come and go.
    const TileInfo* info(const std::string& id) const {
        static const TileInfo sys=kabuki::system_tile(),set=kabuki::settings_tile();
        if(id==kabuki::kSystemTile) return &sys;
        if(id==kabuki::kSettingsTile) return &set;
        for(const auto& t:online) if(t.id==id) return &t;
        return nullptr;
    }
    int kind(const std::string& id) const { const auto* t=info(id); return t?kind_of(t->card):kStats; }
    std::vector<std::string> order() const { return kabuki::arrange(settings,online); }
    const kabuki::Content* data(const std::string& id) const { auto it=content.find(id); return it==content.end()?nullptr:&it->second; }
    // The list a tile shows now: its top level, or the opened row's list once it has arrived.
    const kabuki::ListLevel* level(const std::string& id,bool& sub) const {
        const auto* c=data(id); sub=false;
        if(!c||!c->list) return nullptr;
        auto it=lists.find(id);
        if(it!=lists.end()&&!it->second.open.empty()) { sub=true; return c->l.sub_key==it->second.open?&c->l.sub:nullptr; }
        return &c->l.top;
    }
    // A card the cursor can go into: a player and Settings always, a list with rows, System never.
    bool enterable(const std::string& id) const {
        const int k=kind(id);
        if(k==kMedia||k==kSettings||k==kCustom) return true;
        if(k!=kList) return false;
        bool sub; const auto* l=level(id,sub);
        if(sub) return !l||l->loading||!l->rows.empty(); // an opened list keeps the cursor while it loads
        return l&&!l->loading&&!l->rows.empty();
    }
    // Still loading when it was chosen: the cursor goes in once there are rows, if it is still there.
    bool loading_list(const std::string& id) const {
        bool sub; const auto* l=level(id,sub);
        return kind(id)==kList&&!sub&&(!l||l->loading);
    }
    int selected_index(const std::string& id) {
        auto& st=lists[id];
        bool sub; const auto* l=level(id,sub);
        if(!l) return 0;
        auto& key=sub?st.sub_key:st.top_key; auto& index=sub?st.sub:st.top;
        for(std::size_t i=0;i<l->rows.size();++i) if(l->rows[i].key==key) { index=int(i); break; }
        index=std::clamp(index,0,std::max(0,int(l->rows.size())-1));
        key=index<int(l->rows.size())?l->rows[std::size_t(index)].key:"";
        return index;
    }
    void shown(const std::string& id) {
        if(id==last_shown) return;
        last_shown=id;
        if(!kabuki::kabuki_tile(id)) send(kabuki::kTileShown,0,id.c_str());
    }
    void show_tile(const std::string& id,std::uint32_t held,bool focus) {
        tile=rail=id; focused=focus&&enterable(id); control=2; seek=false; popup=0; seek_ms=-1;
        suppress=held; repeat.reset();
        enter_when_loaded=!focused&&loading_list(id);
    }
    // The player's position now, from when it was heard and whether it was moving.
    void update_media(std::uint64_t now) {
        media=PlayerState{};
        const auto* c=kind(tile)==kMedia?data(tile):nullptr;
        if(!c||!c->media) { media.title=" "; return; }
        const auto& m=c->m;
        media.title=m.title; media.artist=m.subtitle; media.album=m.note; media.cover=m.art;
        media.duration_ms=m.duration; media.playing=m.playing; media.shuffle=m.shuffle;
        media.repeat=m.repeat==2?Repeat::one:m.repeat==1?Repeat::all:Repeat::off;
        media.volume=m.volume<0?100:m.volume;
        const kabuki::Point* heard=m.points.empty()?nullptr:&m.points.front();
        for(const auto& p:m.points) if(p.at<=now) heard=&p;
        if(heard) {
            const int moved=heard->moving&&now>heard->at?int(std::min<std::uint64_t>(now-heard->at,86400000)):0;
            media.position_ms=m.duration>0?std::clamp(heard->position+moved,0,m.duration):std::max(0,heard->position+moved);
        }
        media.pending=m.loading||now<m.pending_until;
    }
    void commit_seek(std::uint64_t now) {
        if(seek_ms>=0) { action(tile,"seek",seek_ms); sent_ms=seek_ms; sent_at=now; }
        seek_ms=-1;
    }
    void unmove() { if(moving) settings=before_move; moving=false; }
    // The tile list's entries: every tile there is, in the default order.
    std::vector<std::string> candidates() const { return kabuki::default_order(online); }

public:
    explicit NativeModel(std::function<void(int,int,const char*)> command) : send(std::move(command)) {
        load_settings();
    }
    void data_dir(std::string path) { dir=std::move(path); settings_text.clear(); load_settings(); } // the PC preview's copy
    void load_settings() {
        auto text=kabuki::read_file(dir+"/"+kabuki::kSettingsFile);
        if(text==settings_text) return;
        settings_text=text;
        auto next=kabuki::parse_settings(text);
        if(moving||picking) { settings.chord=next.chord; settings.hold_ms=next.hold_ms; settings.sounds=next.sounds; return; }
        settings=std::move(next);
    }
    const kabuki::Settings& current_settings() const { return settings; }
    void system(const SystemStats& s) { stats=s; }
    std::vector<int> take_sounds() { std::vector<int> out; out.swap(sounds); return out; }

    // Called from the worker's data thread (and the preview), often: re-reads what changed.
    void poll_files(std::uint64_t now) {
        if(now-last_meta>=200) {
            last_meta=now;
            auto text=kabuki::read_file(dir+"/"+kabuki::kTilesFile);
            if(text!=tiles_text) { tiles_text=text; online=kabuki::parse_tiles(text); }
            load_settings();
            auto show=kabuki::read_file(dir+"/"+kabuki::kShowFile);
            if(show!=show_text) {
                show_text=show;
                const auto s=kabuki::parse_show(show);
                if(show_seq<0) show_seq=s.seq; // requests from before this worker started don't count
                else if(s.seq>show_seq) { show_seq=s.seq; pending_show=s.tile; }
            }
        }
        // The shown card's data quickly; the others' less often (Square plays from the rail).
        const bool all=now-last_other_content>=600;
        if(all||now-last_shown_content>=100) {
            last_shown_content=now;
            if(all) last_other_content=now;
            for(auto it=content.begin();it!=content.end();) {
                const bool known=std::any_of(online.begin(),online.end(),[&](const TileInfo& t){ return t.id==it->first; });
                if(!known) { content_text.erase(it->first); it=content.erase(it); } else ++it;
            }
            for(const auto& t:online) {
                if(!all&&t.id!=tile) continue;
                auto text=kabuki::read_file(dir+"/"+kabuki::content_file(t.id),262144);
                auto& last=content_text[t.id];
                if(text!=last) {
                    auto next=kabuki::parse_content(text);
                    const auto old=content.find(t.id);
                    if(next.list&&!next.l.session.empty()&&
                       (old==content.end()||old->second.l.session!=next.l.session)) {
                        lists.erase(t.id); // includes an open list whose reply will never arrive
                        if(t.id==tile)enter_when_loaded=!focused&&next.l.top.loading;
                    }
                    last=text;content[t.id]=std::move(next);
                }
            }
        }
        if(now-last_stats>=500) {
            last_stats=now;
            auto text=kabuki::read_file(dir+"/system.rec");
            if(text!=system_text) {
                system_text=text;
                SystemStats s;
                for(const auto& r:kabuki::parse_records(text)) {
                    const auto& name=kabuki::field(r,1);
                    const float value=float(std::atof(kabuki::field(r,2).c_str()));
                    if(kabuki::field(r,0)=="stat") {
                        if(name=="cpu") s.cpu=value; else if(name=="cpu_temp") s.cpu_temp=value; else if(name=="soc_temp") s.soc_temp=value;
                        else if(name=="mem_used_gb") s.mem_used_gb=value; else if(name=="mem_total_gb") s.mem_total_gb=value;
                        else if(name=="fps") s.fps=value; else if(name=="fps_min") s.fps_min=value; else if(name=="uptime_s") s.uptime_s=int(value);
                    } else if(kabuki::field(r,0)=="info") {
                        if(name=="firmware") s.firmware=kabuki::field(r,2); else if(name=="address") s.address=kabuki::field(r,2);
                        else if(name=="librespot") s.librespot=kabuki::field(r,2);
                    }
                }
                stats=s;
            }
        }
    }
    std::string pending_show;

    void tick(std::uint64_t now,std::uint64_t session,std::uint32_t held,std::uint32_t pressed,bool back) {
        const auto shown_order=order();
        const auto present=[&](const std::string& id){ return std::find(shown_order.begin(),shown_order.end(),id)!=shown_order.end(); };
        if(session!=opening) {
            // Each opening starts on the rail's first tile (a player: on play/pause).
            opening=session;
            unmove(); recording=choosing=picking=false; recorded=0; page=0; last_shown.clear();
            show_tile(shown_order.front(),held,true);
            pressed=0; back=false;
        }
        // A tile whose provider has gone: the panel moves to the rail's first tile.
        if(!present(tile)) { show_tile(shown_order.front(),held,false); }
        if(!present(rail)) rail=tile;
        update_media(now);
        auto& s=media;
        // A seek is held until it is heard from there (or the player never gets there).
        if(sent_ms>=0&&((now-sent_at>300&&!s.pending&&std::abs(s.position_ms-sent_ms)<2500)||now-sent_at>12000)) sent_ms=-1;
        suppress&=held; held&=~suppress; pressed&=~suppress;
        plugin_held=held&0x40f0;plugin_pressed=pressed&0x40f0;++plugin_input_seq;
        if(pressed&0x4000) ++presses;
        const int k=kind(tile);
        if(back) {
            repeat.reset(); suppress=held; sound(sound::back);
            if(recording) { recording=false; recorded=0; }
            else if(choosing) choosing=false;
            else if(picking) picking=false;
            else if(moving) unmove();
            else if(k==kSettings&&page>0) { if(page==2) setting=4; page=0; }
            else if(popup) popup=0;
            else if(seek&&seek_ms>=0) seek_ms=-1;
            else if(k==kList&&!lists[tile].open.empty()) { lists[tile].open.clear(); action(tile,"close"); }
            else { focused=false; seek=false; }
        } else if(!focused) {
            // With a player up, Square, L1 and R1 still play/pause and skip from the rail.
            if(k==kMedia) {
                if(pressed&0x8000) { action(tile,"play_pause"); sound(sound::play_pause); }
                if(pressed&0x400) { action(tile,"previous"); sound(sound::skip); }
                if(pressed&0x800) { action(tile,"next"); sound(sound::skip); }
            }
            const int n=int(shown_order.size());
            const int at=std::max(0,int(std::find(shown_order.begin(),shown_order.end(),rail)-shown_order.begin()));
            if(pressed&0x10) { sound(n>1?sound::focus_move:sound::cursor_end); rail=shown_order[std::size_t((at+n-1)%n)]; }
            else if(pressed&0x40) { sound(n>1?sound::focus_move:sound::cursor_end); rail=shown_order[std::size_t((at+1)%n)]; }
            else if(pressed&0x4000) {
                sound(sound::tab_enter);
                tile=rail; focused=enterable(tile); suppress=held; repeat.reset(); // nothing to select: stay on the rail
                enter_when_loaded=!focused&&loading_list(tile);
                if(kind(tile)==kMedia) { control=2; seek=false; popup=0; } // always onto play/pause
            } else if((pressed&0x80)&&enterable(tile)) {
                focused=true; suppress=held; repeat.reset(); sound(sound::focus_move);
                if(k==kMedia) { control=2; seek=false; popup=0; }
            }
        } else if(k==kSettings) {
            settings_tick(held,pressed,now);
        } else if(k==kList) {
            list_tick(held,pressed,now);
        } else if(k==kMedia) {
            media_tick(held,pressed,now);
        } else if(k==kCustom) { /* Plugin owns D-pad and Cross; Circle returns to rail above. */
        } else if(pressed&0x20) { focused=false; rail=tile; suppress=held; repeat.reset(); sound(sound::focus_move); }
        // A provider asking for one of its tiles (a chosen song starting: Now Playing), if one
        // of its tiles is the one shown.
        if(!pending_show.empty()) {
            const auto* want=info(pending_show); const auto* now_on=info(tile);
            if(want&&now_on&&want->provider==now_on->provider&&present(pending_show)) show_tile(pending_show,held,true);
            pending_show.clear();
        }
        if(focused&&kind(tile)!=kMedia&&!enterable(tile)) { focused=false; rail=tile; suppress=held; repeat.reset(); }
        if(enter_when_loaded&&(focused||rail!=tile||!loading_list(tile))) {
            if(!focused&&rail==tile&&enterable(tile)) { focused=true; repeat.reset(); }
            enter_when_loaded=false;
        }
        if(volume_asked>=0&&(volume_asked==s.volume||(now-volume_at>3000&&popup!=1))) volume_asked=-1;
        shown(tile);
    }

    void list_tick(std::uint32_t held,std::uint32_t pressed,std::uint64_t now) {
        if(pressed&0x20) { focused=false; rail=tile; repeat.reset(); suppress=held; sound(sound::focus_move); return; }
        bool sub; const auto* l=level(tile,sub);
        auto& st=lists[tile];
        const int index=selected_index(tile);
        if(int delta=repeat.step(pressed,held,now)) {
            const int count=l?int(l->rows.size()):0;
            const int next=std::clamp(index+delta,0,std::max(0,count-1));
            sound(next!=index?sound::list_move:sound::cursor_end);
            if(l&&next<count) { (sub?st.sub:st.top)=next; (sub?st.sub_key:st.top_key)=l->rows[std::size_t(next)].key; }
        }
        if((pressed&0x4000)&&l&&index<int(l->rows.size())) {
            const auto& row=l->rows[std::size_t(index)];
            sound(sound::list_decide);
            if(!sub&&row.opens) { st.open=row.key; st.sub=0; st.sub_key.clear(); action(tile,"open",0,row.key); }
            else action(tile,"select",index,row.key);
        }
    }

    void media_tick(std::uint32_t held,std::uint32_t pressed,std::uint64_t now) {
        auto& s=media;
        if(pressed&0x8000) { action(tile,"play_pause"); sound(sound::play_pause); }
        if(pressed&0x400) { action(tile,"previous"); sound(sound::skip); }
        if(pressed&0x800) { action(tile,"next"); sound(sound::skip); }
        if(popup==1) {
            // Left/Right move the slider (Up/Down too).
            const auto as_steps=[](std::uint32_t b) { return ((b&0x80)?0x10U:0U)|((b&0x20)?0x40U:0U)|((b&0x10)?0x40U:0U)|((b&0x40)?0x10U:0U); };
            if(int delta=repeat.step(as_steps(pressed),as_steps(held),now)) {
                const int from=volume_asked>=0?volume_asked:s.volume;
                const int volume=std::clamp(from+delta*5,0,100);
                sound(volume!=from?sound::slider:sound::cursor_end);
                if(volume!=from) { action(tile,"volume",volume); volume_asked=volume; volume_at=now; }
            }
            if(pressed&0x4000) { popup=0; sound(sound::option_close); }
        } else if(popup==2) {
            if(pressed&0x10) { sound(option?sound::list_move:sound::cursor_end); option=0; }
            if(pressed&0x40) { sound(option?sound::cursor_end:sound::list_move); option=1; }
            if(pressed&0x4000) {
                if(option==0) { action(tile,"shuffle",!s.shuffle); sound(s.shuffle?sound::switch_off:sound::switch_on); }
                else {
                    action(tile,"repeat",s.repeat==Repeat::off?1:s.repeat==Repeat::all?2:0);
                    sound(sound::decide);
                }
            }
        } else if(seek) {
            const auto horizontal=[](std::uint32_t b) { return ((b&0x80)?0x10U:0U)|((b&0x20)?0x40U:0U); };
            if(horizontal(pressed)) seek_held_since=now;
            if(int delta=repeat.step(horizontal(pressed),horizontal(held),now)) {
                const auto duration=now-seek_held_since;
                const int step=duration>=4000?60000:duration>=1500?15000:5000;
                seek_ms=std::clamp((seek_ms<0?s.position_ms:seek_ms)+delta*step,0,std::max(0,s.duration_ms));
                seek_deadline=now+700; sound(sound::slider);
            }
            if(seek_ms>=0&&now>=seek_deadline) commit_seek(now);
            if(pressed&0x4000) { commit_seek(now); sound(sound::decide); }
            if(pressed&0x40) { commit_seek(now); seek=false; repeat.reset(); sound(sound::focus_move); }
        } else {
            if(pressed&0x10) { seek=true; repeat.reset(); sound(sound::focus_move); }
            else if(pressed&0x40) { focused=false; rail=tile; suppress=held; repeat.reset(); sound(sound::focus_move); }
            else if(pressed&0x80) { sound(control>0?sound::focus_move:sound::cursor_end); control=std::max(0,control-1); }
            else if(pressed&0x20) {
                sound(sound::focus_move);
                if(control<4) ++control;
                else { focused=false; rail=tile; suppress=held; }
            } else if(pressed&0x4000) {
                if(control==0) { popup=1; sound(sound::popup_open); }
                else if(control==1) { action(tile,"previous"); sound(sound::skip); }
                else if(control==2) { action(tile,"play_pause"); sound(sound::play_pause); }
                else if(control==3) { action(tile,"next"); sound(sound::skip); }
                else { popup=2; option=0; sound(sound::popup_open); }
            }
        }
    }

    // Settings: Up/Down rows; Cross records the shortcut (hold the new buttons together, then let go:
    // two or more), opens the hold-time list, switches sounds, or opens the Sidebar page.
    void settings_tick(std::uint32_t held,std::uint32_t pressed,std::uint64_t now) {
        if(recording) {
            recorded|=held; // any buttons, Circle too: closing the panel abandons a recording
            if(!held&&recorded) {
                if(__builtin_popcount(recorded)>=2) { settings.chord=recorded; save_settings(); sound(sound::switch_on); }
                else sound(sound::cursor_end);
                recording=false; recorded=0;
            }
            return;
        }
        if(choosing) {
            constexpr int n=int(sizeof kHoldChoices/sizeof kHoldChoices[0]);
            if(int delta=repeat.step(pressed,held,now)) {
                const int next=std::clamp(choice+delta,0,n-1);
                sound(next!=choice?sound::list_move:sound::cursor_end); choice=next;
            }
            if(pressed&0x4000) { settings.hold_ms=kHoldChoices[choice]; choosing=false; save_settings(); sound(sound::option_close); }
            return;
        }
        if(page==1) { sidebar_tick(held,pressed,now); return; }
        if(page==2) { librespot_settings_tick(held,pressed,now); return; }
        if(pressed&0x20) { focused=false; rail=tile; repeat.reset(); suppress=held; sound(sound::focus_move); return; }
        if(int delta=repeat.step(pressed,held,now)) {
            const int next=std::clamp(setting+delta,0,4);
            sound(next!=setting?sound::list_move:sound::cursor_end); setting=next;
        }
        if(pressed&0x4000) {
            if(setting==0) { recording=true; recorded=0; suppress=held; sound(sound::popup_open); }
            else if(setting==1) {
                choosing=true; choice=0; sound(sound::popup_open);
                for(int i=0;i<int(sizeof kHoldChoices/sizeof kHoldChoices[0]);++i) if(kHoldChoices[i]==settings.hold_ms) choice=i;
            } else if(setting==2) {
                settings.sounds=!settings.sounds; save_settings();
                if(settings.sounds) sound(sound::switch_on);
            } else if(setting==3) { page=1; slot=0; moving=picking=false; sound(sound::tab_enter); }
            else { page=2; setting=0; sound(sound::tab_enter); }
        }
    }
    // Librespot's receiver remains a separate process; Kabuki owns whether its music tiles are
    // shown and can request that the exact librespot-ps5 process stop. Cross on Connection is read-only.
    void librespot_settings_tick(std::uint32_t held,std::uint32_t pressed,std::uint64_t now) {
        if(pressed&0x20) { focused=false; rail=tile; repeat.reset(); suppress=held; sound(sound::focus_move); return; }
        if(int delta=repeat.step(pressed,held,now)) {
            const int next=std::clamp(setting+delta,0,2);
            sound(next!=setting?sound::list_move:sound::cursor_end); setting=next;
        }
        if(pressed&0x4000) {
            if(setting==0) sound(sound::cursor_end); // Connection is read-only.
            else if(setting==1) {
                settings.librespot_tiles=!settings.librespot_tiles;
                save_settings();
                sound(settings.librespot_tiles?sound::switch_on:sound::switch_off);
            } else {
                send(kabuki::kTerminateLibrespot,0,nullptr);
                sound(sound::decide);
            }
        }
    }
    // The sidebar's tiles: Up/Down a slot; Cross chooses its tile (one already placed swaps with
    // it; Settings stays); Square picks the tile up, Up/Down carry it, Square or Cross puts it down.
    void sidebar_tick(std::uint32_t held,std::uint32_t pressed,std::uint64_t now) {
        // Editing starts from what the rail shows, and from then on is the user's arrangement.
        const auto arrange_from_rail=[&] {
            if(settings.arranged) return;
            kabuki::current_slots(settings,online,settings.slots);
            settings.arranged=true;
        };
        auto& t=settings.slots;
        if(picking) {
            const auto list=candidates();
            const int n=int(list.size());
            if(int delta=repeat.step(pressed,held,now)) {
                const int next=std::clamp(pick+delta,0,n);
                sound(next!=pick?sound::list_move:sound::cursor_end); pick=next;
            }
            if(pressed&0x4000) {
                const std::string chosen=pick>=n?"":list[std::size_t(pick)];
                kabuki::current_slots(settings,online,t);
                if(t[slot]==kabuki::kSettingsTile&&chosen!=kabuki::kSettingsTile) { sound(sound::cursor_end); return; }
                if(!chosen.empty()) for(int i=0;i<kSlots;++i) if(i!=slot&&t[i]==chosen) t[i]=t[slot]; // swap
                t[slot]=chosen; picking=false; save_slots(); sound(sound::option_close);
            }
            return;
        }
        if(int delta=repeat.step(pressed,held,now)) {
            const int next=std::clamp(slot+delta,0,kSlots-1);
            if(next!=slot&&moving) std::swap(t[slot],t[next]);
            sound(next!=slot?(moving?sound::slider:sound::list_move):sound::cursor_end); slot=next;
        }
        if(pressed&0x8000) {
            if(!moving) { before_move=settings; arrange_from_rail(); }
            moving=!moving; sound(moving?sound::popup_open:sound::option_close);
            if(!moving) save_slots();
        } else if(pressed&0x4000) {
            std::string shown_slots[kSlots]; kabuki::current_slots(settings,online,shown_slots);
            if(moving) { moving=false; save_slots(); sound(sound::option_close); }
            else if(shown_slots[slot]==kabuki::kSettingsTile) sound(sound::cursor_end); // Settings can't be swapped out
            else {
                picking=true;
                const auto list=candidates();
                pick=int(list.size());
                for(std::size_t i=0;i<list.size();++i) if(list[i]==shown_slots[slot]) pick=int(i);
                sound(sound::popup_open);
            }
        } else if(pressed&0x20) { focused=false; rail=tile; repeat.reset(); suppress=held; sound(sound::focus_move); }
    }

    NativeView view() const {
        NativeView v;
        v.state=media; v.tile=tile; v.tab=kind(tile); v.control=control; v.focused=focused; v.seek=seek;
        v.seek_ms=seek_ms; v.popup=popup; v.option=option; v.presses=presses; v.seek_sent=sent_ms; v.system=stats;
        v.opening=opening; v.settings=settings; v.setting=setting; v.recording=recording; v.recorded=recorded;
        v.page=page; v.slot=slot; v.moving=moving; v.picking=picking; v.pick=pick; v.choosing=choosing; v.choice=choice;
        if(volume_asked>=0) v.state.volume=volume_asked;
        const auto view_of=[&](const std::string& id) {
            TileView t; t.id=id;
            if(const auto* i=info(id)) { t.title=i->title; t.icon=i->icon; t.kind=kind_of(i->card); t.online=true; }
            return t;
        };
        const auto shown_order=order();
        for(std::size_t i=0;i<shown_order.size();++i) {
            v.rail.push_back(view_of(shown_order[i]));
            if(shown_order[i]==rail) v.rail_at=int(i);
        }
        std::string shown_slots[kSlots]; kabuki::current_slots(settings,online,shown_slots);
        for(int i=0;i<kSlots;++i) v.slots[i]=shown_slots[i].empty()?TileView{}:view_of(shown_slots[i]);
        for(const auto& id:candidates()) v.picks.push_back(view_of(id));
        const auto here=view_of(tile);
        v.header=here.title; v.header_icon=here.icon;
        for(const auto& t:online)if(t.card==Card::custom)v.plugin_live.push_back(t.id);
        if(v.tab==kCustom)if(const auto* c=data(tile)) {v.plugin_address=c->plugin_address;v.plugin_data=c->plugin_data;v.plugin_error=c->plugin_error;}
        v.plugin_held=plugin_held;v.plugin_pressed=plugin_pressed;v.plugin_input_seq=plugin_input_seq;
        if(v.tab==kSettings) v.header=page==1?"Sidebar":page==2?"Librespot":"Kabuki Settings";
        if(v.tab==kList) {
            auto self=const_cast<NativeModel*>(this);
            bool sub; const auto* l=level(tile,sub);
            const auto it=lists.find(tile);
            auto& out=v.list;
            out.level=sub?1:0;
            out.key=tile+(sub?"|"+(it!=lists.end()?it->second.open:std::string()):"");
            if(l) {
                out.title=l->title; out.art=l->art; out.heading=l->heading; out.covers=l->covers; out.loading=l->loading;
                out.empty=l->empty; out.empty_detail=l->empty_detail;
                for(const auto& r:l->rows) out.rows.push_back({r.title,r.subtitle,r.art});
                out.selected=self->selected_index(tile);
                const auto* c=data(tile);
                for(std::size_t i=0;c&&i<l->rows.size();++i) if(!c->l.busy.empty()&&l->rows[i].key==c->l.busy) out.busy=int(i);
            } else {
                out.loading=true; out.covers=false; // the opened row's list hasn't arrived: placeholders
                const auto* c=data(tile);
                if(c&&it!=lists.end()) for(const auto& r:c->l.top.rows) if(r.key==it->second.open) { out.title=r.title; out.art=r.art; }
            }
            if(sub) { if(!out.title.empty()) v.header=out.title; v.header_art=out.art; }
        }
        v.nested=focused&&(popup||(v.tab==kList&&it_open(tile))||recording||choosing||(v.tab==kSettings&&page>0));
        return v;
    }
    bool it_open(const std::string& id) const { auto it=lists.find(id); return it!=lists.end()&&!it->second.open.empty(); }
};
} // namespace overlay
