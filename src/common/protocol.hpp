/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <cstdint>
#include <cstddef>
#if defined(PROBE_BOREALIS) || defined(PROBE_NATIVE)
#define PROBE_PLAYER 1
#endif
namespace home_bridge {
#ifdef PROBE_INPUT
constexpr bool input_mode=true;
#ifdef PROBE_TOGGLE
#ifdef PROBE_PLAYER
#ifdef PROBE_NATIVE
constexpr const char* variant="shellui-probe";
constexpr const char* ready_path="/system_tmp/custom_overlay_shellui.ready";
constexpr const char* temp_path="/system_tmp/custom_overlay_shellui.tmp";
constexpr const char* attempt_path="/system_tmp/custom_overlay_shellui.attempt";
constexpr const char* shell_log="/system_tmp/custom_overlay_shellui.log";
#else
constexpr const char* variant="borealis-probe";
constexpr const char* ready_path="/system_tmp/custom_overlay_borealis.ready";
constexpr const char* temp_path="/system_tmp/custom_overlay_borealis.tmp";
constexpr const char* attempt_path="/system_tmp/custom_overlay_borealis.attempt";
constexpr const char* shell_log="/system_tmp/custom_overlay_borealis.log";
#endif
// ShellUI's sandbox has no /preinst/common/font, and /system_tmp is too small
// for the faces, so the parent stages them beside the probe log.
constexpr const char* font_stage_dir="/data/custom-overlays/fonts";
// ShellUI has no /data; /user/data is the same directory outside the sandbox.
constexpr const char* font_dirs[]={"/user/data/custom-overlays/fonts","/data/custom-overlays/fonts"};
// The parent's stop marker as ShellUI sees it (confirmed readable for fonts).
constexpr const char* worker_stop_path="/user/data/custom-overlays/stop.txt";
constexpr std::uint64_t magic=0x31534c52424f5448ULL;
constexpr std::uint32_t protocol_version=12;
#else
constexpr const char* variant="toggle-probe";
constexpr const char* ready_path="/system_tmp/custom_overlay_toggle.ready";
constexpr const char* temp_path="/system_tmp/custom_overlay_toggle.tmp";
constexpr const char* attempt_path="/system_tmp/custom_overlay_toggle.attempt";
constexpr const char* shell_log="/system_tmp/custom_overlay_toggle.log";
constexpr std::uint64_t magic=0x31454c47474f5448ULL;
constexpr std::uint32_t protocol_version=6;
#endif
#ifdef PROBE_PLAYER
// No practical time limits (one year): an opening lasts until Circle or L3+R3,
// and the session until `console.py HOST stop`. The guardian still releases
// input when ShellUI's UI heartbeat goes stale or the pad disconnects.
constexpr std::uint32_t session_seconds=365U*24*3600;
#else
constexpr std::uint32_t session_seconds=120;
#endif
#ifdef PROBE_PLAYER
constexpr std::uint64_t chord_hold_us=500000; // L3+R3 held this long opens or closes
#else
constexpr std::uint64_t chord_hold_us=2000000;
#endif
#else
constexpr const char* variant="controller-probe";
constexpr const char* ready_path="/system_tmp/custom_overlay_input.ready";
constexpr const char* temp_path="/system_tmp/custom_overlay_input.tmp";
constexpr const char* attempt_path="/system_tmp/custom_overlay_input.attempt";
constexpr const char* shell_log="/system_tmp/custom_overlay_input.log";
constexpr std::uint64_t magic=0x315455504e494248ULL;
constexpr std::uint32_t protocol_version=5;
#endif
#ifdef PROBE_PLAYER
constexpr std::uint64_t input_delay_us=5000000, input_limit_us=365ULL*24*3600*1000000;
#else
constexpr std::uint64_t input_delay_us=5000000, input_limit_us=10000000;
#endif
enum ReleaseReason : std::uint32_t { NoRelease=0, Deadline=1, BackButton=2, StopRequest=3, SetupError=4, GuardianLost=5, ToggleClose=6, PadLost=7, SystemMenu=8 };
#else
constexpr bool input_mode=false;
constexpr const char* variant="home-bridge";
constexpr const char* ready_path="/system_tmp/custom_overlay_bridge.ready";
constexpr const char* temp_path="/system_tmp/custom_overlay_bridge.tmp";
constexpr const char* attempt_path="/system_tmp/custom_overlay_bridge.attempt";
constexpr const char* shell_log="/system_tmp/custom_overlay_bridge.log";
constexpr std::uint64_t magic=0x3145474449524248ULL;
constexpr std::uint32_t protocol_version=4;
#endif
#ifndef PROBE_PLAYER
constexpr const char* worker_stop_path="/data/custom-overlays/stop.txt";
#endif
enum Phase : std::uint32_t { Prepared=1, Armed, Running, Removing, Done, Failed };
#ifndef PROBE_TOGGLE
constexpr std::uint32_t session_seconds=20;
#endif
struct State {
    std::uint64_t signature=magic;
    std::uint32_t version=protocol_version;
    std::int32_t pid=0;
    std::uint64_t update=0, hook=0, trampoline=0, original=0;
    std::uint32_t phase=Prepared, stop=0, seconds=20, removed=0;
    std::uint64_t started=0, frames=0;
#ifdef PROBE_INPUT
    std::uint64_t key_method=0,key_hook=0,key_trampoline=0,key_original=0;
    std::uint64_t input_started=0,input_deadline=0,input_released=0,guardian_heartbeat=0;
    std::uint64_t events_seen=0,events_consumed=0;
    std::uint32_t input_active=0,release_reason=NoRelease,last_key=0,last_type=0;
    std::uint32_t last_device=0,selection=0,enter_count=0,reserved=0;
#ifdef PROBE_TOGGLE
    std::uint64_t pad_samples=0,toggles=0,ui_heartbeat=0;
    std::uint32_t pad_buttons=0,pad_port=0,pad_connected=0;
    std::int32_t pad_rc=0;
#ifdef PROBE_PLAYER
    std::uint32_t player_ready=0,player_failed=0;
    std::uint64_t surface_frames=0,surface_uploads=0,render_max_us=0,upload_max_us=0;
    // Panel commands for librespot-ps5, written by the worker and read (never written) by
    // the parent: entry n is at n % 8, and command_seq counts every entry written.
    std::uint32_t command_seq=0,command_reserved=0;
    std::int32_t command_kind[8]{},command_value[8]{};
    // Text with the command (Spotify URIs), NUL-terminated.
    char command_text[8][128]{};
#endif
#endif
#endif
};
struct Ready { std::uint64_t address; State state; };
#ifdef PROBE_INPUT
#ifdef PROBE_TOGGLE
#ifdef PROBE_PLAYER
static_assert(sizeof(State)==1368);
#else
static_assert(sizeof(State)==232);
#endif
#else
static_assert(sizeof(State)==192);
#endif
static_assert(offsetof(State,input_active)==160);
#else
static_assert(sizeof(State)==80);
#endif
static_assert(offsetof(State,phase)==48);
static_assert(offsetof(State,original)==40);
inline bool user_address(std::uint64_t p) { return p>=0x10000 && p<0x800000000000ULL; }
}
