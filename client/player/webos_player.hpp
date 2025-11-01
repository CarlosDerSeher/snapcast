/***
    This file is part of snapcast
    Copyright (C) 2014-2025  Johannes Pohl

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
***/

#pragma once

// local headers
#include "player.hpp"

// 3rd party headers
#include <SDL2/SDL.h>

// standard headers
#include <atomic>
#include <memory>
#include <thread>

namespace player
{

static constexpr auto WEBOS = "webos";

/// WebOS Audio Player
/**
 * Audio player implementation for LG webOS using SDL2 audio subsystem
 * Based on moonlight-tv's approach to webOS audio streaming
 */
class WebOSPlayer : public Player
{
public:
    /// c'tor
    WebOSPlayer(boost::asio::io_context& io_context, const ClientSettings::Player& settings, std::shared_ptr<Stream> stream);
    /// d'tor
    virtual ~WebOSPlayer();

    void start() override;
    void stop() override;

protected:
    void worker() override;
    bool needsThread() const override
    {
        return true;
    }

private:
    /// SDL audio callback function
    static void audioCallback(void* userdata, Uint8* stream, int len);
    
    /// Initialize SDL audio subsystem
    bool initializeAudio();
    
    /// Cleanup SDL audio resources
    void cleanupAudio();
    
    /// Process audio data from stream
    void processAudio();

    SDL_AudioDeviceID audio_device_;
    SDL_AudioSpec audio_spec_;
    std::atomic<bool> initialized_;
    std::atomic<bool> audio_active_;
    std::thread worker_thread_;
    
    // Audio buffer management
    static constexpr size_t BUFFER_SIZE = 4096;
    std::unique_ptr<char[]> audio_buffer_;
    std::atomic<size_t> buffer_fill_;
    mutable std::mutex buffer_mutex_;
};

} // namespace player