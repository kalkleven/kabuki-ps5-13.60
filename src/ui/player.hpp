// SPDX-License-Identifier: GPL-3.0-or-later
// What a `media` card shows: a player, as the panel sees it at this moment.
#pragma once

#include <string>

namespace overlay
{

enum class Repeat
{
    off,
    all,
    one,
};

struct PlayerState
{
    std::string title;
    std::string artist;
    std::string album;
    std::string cover; // a JPEG or PNG file; empty for the console's fallback art
    int duration_ms = 0;
    int position_ms = 0; // what is heard
    bool playing = false; // what the player is doing (the play/pause button)
    // Something asked for (a play, a seek, a new track) is not heard yet: the bar shows a
    // buffering light, and holds where it is heard until then.
    bool pending = false;
    bool can_previous = true;
    bool can_next = true;
    bool shuffle = false;
    Repeat repeat = Repeat::off;
    int volume = 100; // 0 to 100
};

} // namespace overlay
