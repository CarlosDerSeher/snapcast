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

// prototype/interface header file
#include "webos_player.hpp"

// local headers
#include "common/aixlog.hpp"
#include "common/snap_exception.hpp"
#include "common/utils/string_utils.hpp"

// standard headers
#include <chrono>
#include <cstring>
#include <iostream>

using namespace std;

namespace player
{

static constexpr auto LOG_TAG = "SDL2Player";

WebOSPlayer::WebOSPlayer(boost::asio::io_context& io_context, const ClientSettings::Player& settings, std::shared_ptr<Stream> stream)
    : Player(io_context, settings, stream)
    , audio_device_(0)
    , initialized_(false)
    , audio_active_(false)
    , audio_buffer_(std::make_unique<char[]>(BUFFER_SIZE))
    , buffer_fill_(0)
{
    LOG(INFO, LOG_TAG) << "WebOSPlayer created\n";
}

WebOSPlayer::~WebOSPlayer()
{
    stop();
    cleanupAudio();
    LOG(INFO, LOG_TAG) << "WebOSPlayer destroyed\n";
}

void WebOSPlayer::start()
{
    LOG(INFO, LOG_TAG) << "Starting WebOS player\n";
    
    if (!initializeAudio())
    {
        throw SnapException("Failed to initialize WebOS audio");
    }
    
    Player::start();
    audio_active_ = true;
    
    // Start SDL audio playback
    SDL_PauseAudioDevice(audio_device_, 0);
    
    LOG(INFO, LOG_TAG) << "WebOS player started successfully\n";
}

void WebOSPlayer::stop()
{
    LOG(INFO, LOG_TAG) << "Stopping WebOS player\n";
    
    audio_active_ = false;
    
    if (audio_device_ != 0)
    {
        SDL_PauseAudioDevice(audio_device_, 1);
    }
    
    Player::stop();
    
    LOG(INFO, LOG_TAG) << "WebOS player stopped\n";
}

bool WebOSPlayer::initializeAudio()
{
    LOG(INFO, LOG_TAG) << "Initializing SDL audio subsystem\n";
    
    if (SDL_Init(SDL_INIT_AUDIO) < 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to initialize SDL audio: " << SDL_GetError() << "\n";
        return false;
    }
    
    // Get the stream format
    const auto& format = stream_->getFormat();
    
    // Configure SDL audio specification
    SDL_zero(audio_spec_);
    audio_spec_.freq = format.rate();
    audio_spec_.format = AUDIO_S16LSB; // 16-bit signed little-endian
    audio_spec_.channels = format.channels();
    audio_spec_.samples = 1024; // Buffer size in samples
    audio_spec_.callback = audioCallback;
    audio_spec_.userdata = this;
    
    LOG(INFO, LOG_TAG) << "Audio format: " << format.rate() << "Hz, " 
                       << format.channels() << " channels, " 
                       << format.bits() << " bits\n";
    
    // Open audio device
    SDL_AudioSpec obtained_spec;
    audio_device_ = SDL_OpenAudioDevice(nullptr, 0, &audio_spec_, &obtained_spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    
    if (audio_device_ == 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to open audio device: " << SDL_GetError() << "\n";
        SDL_Quit();
        return false;
    }
    
    LOG(INFO, LOG_TAG) << "Obtained audio format: " << obtained_spec.freq << "Hz, " 
                       << (int)obtained_spec.channels << " channels\n";
    
    // Update our spec with what we actually got
    audio_spec_ = obtained_spec;
    initialized_ = true;
    
    return true;
}

void WebOSPlayer::cleanupAudio()
{
    if (audio_device_ != 0)
    {
        SDL_CloseAudioDevice(audio_device_);
        audio_device_ = 0;
    }
    
    if (initialized_)
    {
        SDL_Quit();
        initialized_ = false;
    }
    
    LOG(INFO, LOG_TAG) << "SDL audio cleaned up\n";
}

void WebOSPlayer::audioCallback(void* userdata, Uint8* stream, int len)
{
    WebOSPlayer* player = static_cast<WebOSPlayer*>(userdata);
    
    if (!player || !player->audio_active_)
    {
        // Fill with silence if not active
        SDL_memset(stream, 0, len);
        return;
    }
    
    std::lock_guard<std::mutex> lock(player->buffer_mutex_);
    
    // Get audio data from snapcast stream
    size_t available = player->buffer_fill_.load();
    size_t bytes_to_copy = std::min(static_cast<size_t>(len), available);
    
    if (bytes_to_copy > 0)
    {
        std::memcpy(stream, player->audio_buffer_.get(), bytes_to_copy);
        
        // Move remaining data to front of buffer
        if (bytes_to_copy < available)
        {
            std::memmove(player->audio_buffer_.get(),
                        player->audio_buffer_.get() + bytes_to_copy,
                        available - bytes_to_copy);
            player->buffer_fill_.store(available - bytes_to_copy);
        }
        else
        {
            player->buffer_fill_.store(0);
        }
    }
    
    // Fill remaining with silence if needed
    if (bytes_to_copy < static_cast<size_t>(len))
    {
        SDL_memset(stream + bytes_to_copy, 0, len - bytes_to_copy);
    }
}

void WebOSPlayer::worker()
{
    LOG(INFO, LOG_TAG) << "WebOS player worker thread started\n";
    
    while (active_)
    {
        processAudio();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    LOG(INFO, LOG_TAG) << "WebOS player worker thread stopped\n";
}

void WebOSPlayer::processAudio()
{
    if (!audio_active_ || !initialized_)
        return;

    std::lock_guard<std::mutex> lock(buffer_mutex_);

    // Current fill in bytes
    size_t fill = buffer_fill_.load();
    if (fill >= BUFFER_SIZE)
    {
        LOG(DEBUG, LOG_TAG) << "Audio buffer full, skipping\n";
        return;
    }

    // Determine how many frames we can ask for (frame = bytes per sample frame)
    const auto& fmt = stream_->getFormat();
    const size_t frame_size = fmt.frameSize();
    if (frame_size == 0)
        return;

    size_t space = BUFFER_SIZE - fill;
    uint32_t frames = static_cast<uint32_t>(space / frame_size);
    if (frames == 0)
        return;

    // Fill buffer at current write position. getPlayerChunkOrSilence will write silence
    // if no real data is available, so we always advance the fill by frames*frame_size.
    stream_->getPlayerChunkOrSilence(audio_buffer_.get() + fill, chronos::usec{0}, frames);
    buffer_fill_.fetch_add(static_cast<size_t>(frames) * frame_size);

    // Apply the software mixer to the newly buffered PCM data.
    // The server sends volume updates to the client; those updates end up in
    // Player::volume_. We scale the samples here so the audio callback only
    // needs to copy the already-adjusted data to SDL.
    //
    // A few notes for future readers:
    // - adjustVolume expects a frame count (not bytes), so we pass `frames`.
    // - adjustVolume takes the player mutex internally, so this call is
    //   thread-safe with respect to setVolume() and other volume changes.
    try
    {
        // frames = number of sample frames written into the buffer
        adjustVolume(audio_buffer_.get() + fill, frames);
    }
    catch (const std::exception& e)
    {
        LOG(ERROR, LOG_TAG) << "adjustVolume failed: " << e.what() << "\n";
    }
}

} // namespace player