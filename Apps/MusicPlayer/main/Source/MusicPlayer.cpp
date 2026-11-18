#include "MusicPlayer.h"

#include <app/event.h>

#include <lvgl/fonts.h>
#include <lvgl/theme.h>
#include <lvgl/widgets/card.h>
#include <lvgl/widgets/text_button.h>
#include <lvgl/widgets/toolbar.h>

#include <tactility/device.h>
#include <tactility/drivers/audio_codec.h>
#include <tactility/drivers/audio_stream.h>
#include <tactility/log.h>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace {

constexpr uint32_t UI_REFRESH_PERIOD_MS = 250;

struct OutputScan {
    std::vector<std::string> names;
};

void onClosePressed(lv_event_t* event) {
    auto* context = static_cast<MusicPlayerContext*>(lv_event_get_user_data(event));
    app_event_emit_close(context->appInstanceId);
}

void showView(MusicPlayerContext* context, MusicPlayerView view);
void renderTrackList(MusicPlayerContext* context);
void updateTrackInfo(MusicPlayerContext* context);
void scheduleFolderScan(MusicPlayerContext* context, const std::string& path);

void onSettingsPressed(lv_event_t* event) {
    auto* context = static_cast<MusicPlayerContext*>(lv_event_get_user_data(event));
    context->returnView = context->view;
    showView(context, MusicPlayerView::Settings);
}

void onBackPressed(lv_event_t* event) {
    auto* context = static_cast<MusicPlayerContext*>(lv_event_get_user_data(event));
    showView(context, context->view == MusicPlayerView::Settings ? context->returnView : MusicPlayerView::Library);
}

void onParentFolderPressed(lv_event_t* event) {
    auto* context = static_cast<MusicPlayerContext*>(lv_event_get_user_data(event));
    if (context->folderPath == "/sdcard") {
        return;
    }
    const size_t separator = context->folderPath.find_last_of('/');
    const std::string parent = separator <= 7 ? "/sdcard" : context->folderPath.substr(0, separator);
    scheduleFolderScan(context, parent);
}

void setStatus(MusicPlayerContext* context, const std::string& status) {
    std::lock_guard lock(context->statusMutex);
    context->statusText = status;
}

bool scanOutputDevice(Device* device, void* userData) {
    auto* outputs = static_cast<OutputScan*>(userData);
    if (!device_is_ready(device) || device->name == nullptr) {
        return true;
    }

    bool outputSupported = false;
    if (audio_stream_is_supported(device, AUDIO_CODEC_DIR_OUTPUT, &outputSupported) == ERROR_NONE &&
        outputSupported) {
        outputs->names.emplace_back(device->name);
    }
    return true;
}

void refreshOutputDevices(MusicPlayerContext* context) {
    OutputScan scan;
    device_for_each_of_type(&AUDIO_STREAM_TYPE, &scan, scanOutputDevice);
    std::sort(scan.names.begin(), scan.names.end());

    std::lock_guard lock(context->outputMutex);
    context->outputNames = std::move(scan.names);
    if (std::find(context->outputNames.begin(), context->outputNames.end(), context->selectedOutputName) ==
        context->outputNames.end()) {
        context->selectedOutputName = context->outputNames.empty() ? "" : context->outputNames.front();
    }
}

void setVolumeSliderFromDevice(MusicPlayerContext* context) {
    if (context->volumeSlider == nullptr) {
        return;
    }

    std::string outputName;
    {
        std::lock_guard lock(context->outputMutex);
        outputName = context->selectedOutputName;
    }
    if (outputName.empty()) {
        return;
    }

    Device* device = nullptr;
    if (device_get_by_name(outputName.c_str(), &device) != ERROR_NONE || device == nullptr) {
        setStatus(context, "Audio output is unavailable");
        return;
    }
    float volume = 0.0f;
    const error_t error = audio_stream_get_volume(device, AUDIO_CODEC_DIR_OUTPUT, &volume);
    device_put(device);
    if (error != ERROR_NONE) {
        setStatus(context, "Volume control unavailable");
        return;
    }

    context->volumePercent = static_cast<uint8_t>(std::max(0.0f, std::min(volume, 100.0f)));
    context->updatingVolumeSlider = true;
    lv_slider_set_value(context->volumeSlider, context->volumePercent, LV_ANIM_OFF);
    context->updatingVolumeSlider = false;
    if (context->volumeLabel != nullptr) {
        lv_label_set_text_fmt(context->volumeLabel, "%u%%", context->volumePercent);
    }
}

void onOutputChanged(lv_event_t* event) {
    auto* context = static_cast<MusicPlayerContext*>(lv_event_get_user_data(event));
    const uint32_t selected = lv_dropdown_get_selected(context->outputDropdown);
    {
        std::lock_guard lock(context->outputMutex);
        if (selected < context->outputNames.size()) {
            context->selectedOutputName = context->outputNames[selected];
        } else {
            context->selectedOutputName.clear();
        }
    }
    setVolumeSliderFromDevice(context);
}

void onVolumeChanged(lv_event_t* event) {
    auto* context = static_cast<MusicPlayerContext*>(lv_event_get_user_data(event));
    if (context->updatingVolumeSlider) {
        return;
    }
    context->volumePercent = static_cast<uint8_t>(lv_slider_get_value(context->volumeSlider));
    if (context->volumeLabel != nullptr) {
        lv_label_set_text_fmt(context->volumeLabel, "%u%%", context->volumePercent);
    }

    std::string outputName;
    {
        std::lock_guard lock(context->outputMutex);
        outputName = context->selectedOutputName;
    }
    if (outputName.empty()) {
        setStatus(context, "Select an audio output");
        return;
    }

    Device* device = nullptr;
    if (device_get_by_name(outputName.c_str(), &device) != ERROR_NONE || device == nullptr) {
        setStatus(context, "Audio output is unavailable");
        return;
    }
    const error_t error = audio_stream_set_volume(device, AUDIO_CODEC_DIR_OUTPUT, context->volumePercent);
    device_put(device);
    if (error != ERROR_NONE) {
        LOG_W(MUSIC_PLAYER_TAG, "Could not set output volume: %d", error);
        setStatus(context, "Volume control unavailable");
    }
}

bool copyTrack(MusicPlayerContext* context, int32_t index, MusicTrack* track) {
    std::lock_guard lock(context->tracksMutex);
    if (index < 0 || static_cast<size_t>(index) >= context->tracks.size()) {
        return false;
    }
    *track = context->tracks[static_cast<size_t>(index)];
    return true;
}

int32_t getSelectedTrackIndex(MusicPlayerContext* context) {
    std::lock_guard lock(context->tracksMutex);
    return context->selectedTrackIndex;
}

size_t getTrackCount(MusicPlayerContext* context) {
    std::lock_guard lock(context->tracksMutex);
    return context->tracks.size();
}

size_t getPlaybackQueueCount(MusicPlayerContext* context) {
    std::lock_guard lock(context->tracksMutex);
    return context->playbackQueue.size();
}

void formatTime(uint32_t seconds, char* output, size_t outputSize) {
    snprintf(output, outputSize, "%u:%02u", static_cast<unsigned>(seconds / 60),
             static_cast<unsigned>(seconds % 60));
}

void updateTrackInfo(MusicPlayerContext* context) {
    if (context->trackTitleLabel == nullptr && context->miniTitleLabel == nullptr) {
        return;
    }
    MusicTrack track;
    const int32_t playbackIndex = context->currentTrackIndex.load();
    bool hasTrack = false;
    if (playbackIndex >= 0) {
        std::lock_guard lock(context->tracksMutex);
        if (static_cast<size_t>(playbackIndex) < context->playbackQueue.size()) {
            track = context->playbackQueue[static_cast<size_t>(playbackIndex)];
            hasTrack = true;
        }
    } else {
        hasTrack = copyTrack(context, getSelectedTrackIndex(context), &track);
    }
    if (!hasTrack) {
        if (context->trackTitleLabel != nullptr) {
            lv_label_set_text(context->trackTitleLabel, "No track selected");
            lv_label_set_text(context->trackArtistLabel, "");
            lv_label_set_text(context->trackAlbumLabel, "");
        }
        if (context->miniTitleLabel != nullptr) {
            lv_label_set_text(context->miniTitleLabel, "Nothing playing");
            lv_label_set_text(context->miniArtistLabel, "");
        }
        return;
    }
    if (context->trackTitleLabel != nullptr) {
        lv_label_set_text(context->trackTitleLabel, track.title.c_str());
        lv_label_set_text(context->trackArtistLabel, track.artist.empty() ? "Unknown artist" : track.artist.c_str());
        lv_label_set_text(context->trackAlbumLabel, track.album.empty() ? " " : track.album.c_str());
    }
    if (context->miniTitleLabel != nullptr) {
        lv_label_set_text(context->miniTitleLabel, track.title.c_str());
        lv_label_set_text(context->miniArtistLabel, track.artist.empty() ? " " : track.artist.c_str());
    }
}

void onFolderTimer(lv_timer_t* timer) {
    auto* context = static_cast<MusicPlayerContext*>(lv_timer_get_user_data(timer));
    context->folderTimer = nullptr;
    musicPlayerScanFolder(context, context->pendingFolderPath);
    renderTrackList(context);
    updateTrackInfo(context);
}

void scheduleFolderScan(MusicPlayerContext* context, const std::string& path) {
    context->pendingFolderPath = path;
    if (context->folderTimer == nullptr) {
        context->folderTimer = lv_timer_create(onFolderTimer, 0, context);
        lv_timer_set_repeat_count(context->folderTimer, 1);
    }
}

void onBrowserEntryClicked(lv_event_t* event) {
    auto* context = static_cast<MusicPlayerContext*>(lv_event_get_user_data(event));
    auto* row = lv_event_get_target_obj(event);
    auto* selectedEntry = static_cast<MusicBrowserEntry*>(lv_obj_get_user_data(row));
    if (selectedEntry == nullptr) {
        return;
    }

    MusicBrowserEntry entry;
    int32_t trackIndex = -1;
    {
        std::lock_guard lock(context->tracksMutex);
        const auto found = std::find_if(context->browserEntries.begin(), context->browserEntries.end(),
                                        [selectedEntry](const MusicBrowserEntry& candidate) {
                                            return &candidate == selectedEntry;
                                        });
        if (found == context->browserEntries.end()) {
            return;
        }
        entry = *found;
        if (!entry.isDirectory) {
            for (size_t i = 0; i < context->tracks.size(); i++) {
                if (context->tracks[i].path == entry.path) {
                    trackIndex = static_cast<int32_t>(i);
                    context->selectedTrackIndex = trackIndex;
                    break;
                }
            }
        }
    }
    if (entry.isDirectory) {
        scheduleFolderScan(context, entry.path);
    } else if (trackIndex >= 0) {
        musicPlayerRequestStart(context, trackIndex);
        updateTrackInfo(context);
    }
}

void renderTrackList(MusicPlayerContext* context) {
    if (context->browserList == nullptr) {
        return;
    }
    lv_obj_clean(context->browserList);

    std::lock_guard lock(context->tracksMutex);
    if (context->browserPathLabel != nullptr) {
        lv_label_set_text(context->browserPathLabel, context->folderPath.c_str());
    }
    if (context->browserEntries.empty()) {
        auto* emptyLabel = lv_label_create(context->browserList);
        lv_label_set_text(emptyLabel, "No folders or MP3 files");
        return;
    }

    for (MusicBrowserEntry& entry : context->browserEntries) {
        auto* row = lv_button_create(context->browserList);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(row, 0, LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(row, 0, LV_STATE_DEFAULT);
        lv_obj_set_user_data(row, &entry);
        lv_obj_add_event_cb(row, onBrowserEntryClicked, LV_EVENT_CLICKED, context);

        auto* label = lv_label_create(row);
        lv_obj_set_width(label, LV_PCT(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text_fmt(label, "%s  %s", entry.isDirectory ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE,
                              entry.name.c_str());
    }
}

void onScanPressed(lv_event_t* event) {
    auto* context = static_cast<MusicPlayerContext*>(lv_event_get_user_data(event));
    const char* text = lv_textarea_get_text(context->folderInput);
    musicPlayerRequestStop(context);
    musicPlayerScanFolder(context, text == nullptr ? "" : text);
    renderTrackList(context);
    updateTrackInfo(context);
}

void selectAndPlay(MusicPlayerContext* context, int32_t index) {
    const size_t count = getTrackCount(context);
    if (count == 0) {
        return;
    }
    index = (index + static_cast<int32_t>(count)) % static_cast<int32_t>(count);
    {
        std::lock_guard lock(context->tracksMutex);
        context->selectedTrackIndex = index;
    }
    updateTrackInfo(context);
    musicPlayerRequestStart(context, index);
}

void onPlayPressed(lv_event_t* event) {
    auto* context = static_cast<MusicPlayerContext*>(lv_event_get_user_data(event));
    const MusicPlaybackState state = context->playbackState.load();
    if (state == MusicPlaybackState::Playing || state == MusicPlaybackState::Opening) {
        context->pauseRequested.store(true);
    } else if (state == MusicPlaybackState::Paused) {
        context->pauseRequested.store(false);
    } else {
        const int32_t currentTrack = context->currentTrackIndex.load();
        if (currentTrack >= 0 && static_cast<size_t>(currentTrack) < getPlaybackQueueCount(context)) {
            musicPlayerRequestQueueStart(context, currentTrack);
        } else {
            musicPlayerRequestStart(context, getSelectedTrackIndex(context));
        }
    }
}

void onStopPressed(lv_event_t* event) {
    musicPlayerRequestStop(static_cast<MusicPlayerContext*>(lv_event_get_user_data(event)));
}

void onPreviousPressed(lv_event_t* event) {
    auto* context = static_cast<MusicPlayerContext*>(lv_event_get_user_data(event));
    const int32_t current = context->currentTrackIndex.load();
    const size_t queueCount = getPlaybackQueueCount(context);
    if (current >= 0 && queueCount > 0) {
        musicPlayerRequestQueueStart(context, (current + static_cast<int32_t>(queueCount) - 1) %
                                                 static_cast<int32_t>(queueCount));
        return;
    }
    const int32_t selected = getSelectedTrackIndex(context);
    if (selected >= 0) {
        selectAndPlay(context, selected - 1);
    }
}

void onNextPressed(lv_event_t* event) {
    auto* context = static_cast<MusicPlayerContext*>(lv_event_get_user_data(event));
    const int32_t current = context->currentTrackIndex.load();
    const size_t queueCount = getPlaybackQueueCount(context);
    if (current >= 0 && queueCount > 0) {
        musicPlayerRequestQueueStart(context, (current + 1) % static_cast<int32_t>(queueCount));
        return;
    }
    const int32_t selected = getSelectedTrackIndex(context);
    if (selected >= 0) {
        selectAndPlay(context, selected + 1);
    }
}

lv_obj_t* createCard(lv_obj_t* parent) {
    auto* card = lvgl_card_create(parent);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(card, false);
    return card;
}

lv_obj_t* createRow(lv_obj_t* parent) {
    auto* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, LV_DPX(8), LV_STATE_DEFAULT);
    lv_obj_set_scrollable(row, false);
    return row;
}

void setButtonText(lv_obj_t* button, const char* text) {
    auto* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
}

void onViewTimer(lv_timer_t* timer) {
    auto* context = static_cast<MusicPlayerContext*>(lv_timer_get_user_data(timer));
    context->viewTimer = nullptr;
    musicPlayerCreateWidgets(context->root, context);
}

void showView(MusicPlayerContext* context, MusicPlayerView view) {
    context->view = view;
    if (context->viewTimer == nullptr) {
        context->viewTimer = lv_timer_create(onViewTimer, 0, context);
        lv_timer_set_repeat_count(context->viewTimer, 1);
    }
}

void createSettingsWidgets(lv_obj_t* parent, MusicPlayerContext* context, lv_coord_t padding) {
    auto* toolbar = lvgl_toolbar_create(parent, "Music Player Settings");
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_LEFT, onBackPressed, context);

    auto* content = lv_obj_create(parent);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, padding, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_gap(content, padding, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(content, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);

    auto* folderCard = createCard(content);
    auto* folderTitle = lv_label_create(folderCard);
    lv_label_set_text(folderTitle, "SD card folder");
    auto* folderRow = createRow(folderCard);
    context->folderInput = lv_textarea_create(folderRow);
    lv_textarea_set_one_line(context->folderInput, true);
    lv_textarea_set_text(context->folderInput, context->folderPath.c_str());
    lv_obj_set_flex_grow(context->folderInput, 1);
    auto* scanButton = lvgl_text_button_create(folderRow);
    setButtonText(scanButton, "Scan");
    lv_obj_add_event_cb(scanButton, onScanPressed, LV_EVENT_CLICKED, context);

    auto* outputCard = createCard(content);
    auto* outputTitle = lv_label_create(outputCard);
    lv_label_set_text(outputTitle, "Audio output");
    context->outputDropdown = lv_dropdown_create(outputCard);
    lv_obj_set_width(context->outputDropdown, LV_PCT(100));

    std::string outputOptions;
    uint32_t selectedOutputIndex = 0;
    {
        std::lock_guard lock(context->outputMutex);
        for (size_t i = 0; i < context->outputNames.size(); i++) {
            if (i > 0) {
                outputOptions.push_back('\n');
            }
            outputOptions += context->outputNames[i];
            if (context->outputNames[i] == context->selectedOutputName) {
                selectedOutputIndex = static_cast<uint32_t>(i);
            }
        }
    }
    lv_dropdown_set_options(context->outputDropdown, outputOptions.empty() ? "No output device" : outputOptions.c_str());
    lv_dropdown_set_selected(context->outputDropdown, selectedOutputIndex);
    lv_obj_set_state(context->outputDropdown, LV_STATE_DISABLED, outputOptions.empty());
    lv_obj_add_event_cb(context->outputDropdown, onOutputChanged, LV_EVENT_VALUE_CHANGED, context);

    auto* volumeRow = createRow(outputCard);
    auto* volumeTitle = lv_label_create(volumeRow);
    lv_label_set_text(volumeTitle, "Volume");
    context->volumeSlider = lv_slider_create(volumeRow);
    lv_slider_set_range(context->volumeSlider, 0, 100);
    lv_slider_set_value(context->volumeSlider, context->volumePercent, LV_ANIM_OFF);
    lv_obj_set_flex_grow(context->volumeSlider, 1);
    lv_obj_add_event_cb(context->volumeSlider, onVolumeChanged, LV_EVENT_VALUE_CHANGED, context);
    context->volumeLabel = lv_label_create(volumeRow);
    lv_obj_set_width(context->volumeLabel, LV_SIZE_CONTENT);
    lv_label_set_text_fmt(context->volumeLabel, "%u%%", context->volumePercent);
    setVolumeSliderFromDevice(context);
}

void onMiniPlayerClicked(lv_event_t* event) {
    showView(static_cast<MusicPlayerContext*>(lv_event_get_user_data(event)), MusicPlayerView::Player);
}

void createMiniPlayer(lv_obj_t* parent, MusicPlayerContext* context) {
    auto* miniPlayer = lv_obj_create(parent);
    lv_obj_set_width(miniPlayer, LV_PCT(100));
    lv_obj_set_height(miniPlayer, LV_DPX(60));
    lv_obj_set_flex_flow(miniPlayer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(miniPlayer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(miniPlayer, LV_DPX(6), LV_STATE_DEFAULT);
    lv_obj_set_style_pad_gap(miniPlayer, LV_DPX(8), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(miniPlayer, lv_color_make(25, 100, 190), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(miniPlayer, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(miniPlayer, 0, LV_STATE_DEFAULT);
    lv_obj_add_flag(miniPlayer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(miniPlayer, onMiniPlayerClicked, LV_EVENT_CLICKED, context);

    auto* artwork = lv_obj_create(miniPlayer);
    lv_obj_set_size(artwork, LV_DPX(46), LV_DPX(46));
    lv_obj_set_style_bg_color(artwork, lv_color_make(105, 50, 165), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(artwork, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(artwork, 0, LV_STATE_DEFAULT);
    auto* artworkLabel = lv_label_create(artwork);
    lv_label_set_text(artworkLabel, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(artworkLabel, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_center(artworkLabel);

    auto* metadata = lv_obj_create(miniPlayer);
    lv_obj_remove_style_all(metadata);
    lv_obj_set_flex_grow(metadata, 1);
    lv_obj_set_height(metadata, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(metadata, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(metadata, false);
    context->miniTitleLabel = lv_label_create(metadata);
    lv_obj_set_width(context->miniTitleLabel, LV_PCT(100));
    lv_label_set_long_mode(context->miniTitleLabel, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_color(context->miniTitleLabel, lv_color_white(), LV_STATE_DEFAULT);
    context->miniArtistLabel = lv_label_create(metadata);
    lv_obj_set_width(context->miniArtistLabel, LV_PCT(100));
    lv_label_set_long_mode(context->miniArtistLabel, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_color(context->miniArtistLabel, lv_color_white(), LV_STATE_DEFAULT);

    auto* playButton = lv_button_create(miniPlayer);
    lv_obj_set_size(playButton, LV_DPX(38), LV_DPX(42));
    context->miniPlayButtonLabel = lv_label_create(playButton);
    lv_label_set_text(context->miniPlayButtonLabel, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(context->miniPlayButtonLabel, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_center(context->miniPlayButtonLabel);
    lv_obj_add_event_cb(playButton, onPlayPressed, LV_EVENT_CLICKED, context);

    auto* nextButton = lv_button_create(miniPlayer);
    lv_obj_set_size(nextButton, LV_DPX(38), LV_DPX(42));
    auto* nextLabel = lv_label_create(nextButton);
    lv_label_set_text(nextLabel, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_color(nextLabel, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_center(nextLabel);
    lv_obj_add_event_cb(nextButton, onNextPressed, LV_EVENT_CLICKED, context);
}

void createLibraryWidgets(lv_obj_t* parent, MusicPlayerContext* context, lv_coord_t padding) {
    auto* toolbar = lvgl_toolbar_create(parent, "Music Player");
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_CLOSE, onClosePressed, context);
    lvgl_toolbar_add_text_button_action(toolbar, LV_SYMBOL_SETTINGS, onSettingsPressed, context);

    auto* content = lv_obj_create(parent);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, padding, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_gap(content, padding, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(content, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);

    auto* pathRow = createRow(content);
    context->browserPathLabel = lv_label_create(pathRow);
    lv_obj_set_flex_grow(context->browserPathLabel, 1);
    lv_label_set_long_mode(context->browserPathLabel, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(context->browserPathLabel, context->folderPath.c_str());
    auto* parentButton = lv_button_create(pathRow);
    setButtonText(parentButton, LV_SYMBOL_LEFT);
    lv_obj_add_event_cb(parentButton, onParentFolderPressed, LV_EVENT_CLICKED, context);

    context->browserList = lv_obj_create(content);
    lv_obj_remove_style_all(context->browserList);
    lv_obj_set_width(context->browserList, LV_PCT(100));
    lv_obj_set_flex_grow(context->browserList, 1);
    lv_obj_set_flex_flow(context->browserList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(context->browserList, LV_DPX(2), LV_STATE_DEFAULT);
    lv_obj_set_scroll_dir(context->browserList, LV_DIR_VER);
    renderTrackList(context);

    createMiniPlayer(parent, context);
}

void createWidgetsImpl(lv_obj_t* parent, MusicPlayerContext* context) {
    const lv_coord_t padding = lvgl_get_ui_density() == LVGL_UI_DENSITY_COMPACT ? 6 : 10;

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(parent, false);
    lv_obj_clean(parent);

    if (context->view == MusicPlayerView::Settings) {
        createSettingsWidgets(parent, context, padding);
        return;
    }
    if (context->view == MusicPlayerView::Library) {
        createLibraryWidgets(parent, context, padding);
        return;
    }

    auto* toolbar = lvgl_toolbar_create(parent, "Now Playing");
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_LEFT, onBackPressed, context);
    lvgl_toolbar_add_text_button_action(toolbar, LV_SYMBOL_SETTINGS, onSettingsPressed, context);

    auto* content = lv_obj_create(parent);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, padding, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_gap(content, padding, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(content, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);

    auto* playerCard = createCard(content);
    lv_obj_set_flex_grow(playerCard, 1);
    lv_obj_set_flex_align(playerCard, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    auto* artwork = lv_obj_create(playerCard);
    lv_obj_set_size(artwork, LV_DPX(120), LV_DPX(120));
    lv_obj_set_style_bg_color(artwork, lv_color_make(105, 50, 165), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(artwork, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(artwork, 0, LV_STATE_DEFAULT);
    auto* artworkLabel = lv_label_create(artwork);
    lv_label_set_text(artworkLabel, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(artworkLabel, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(artworkLabel, lvgl_get_text_font(FONT_SIZE_LARGE), LV_STATE_DEFAULT);
    lv_obj_center(artworkLabel);

    context->trackTitleLabel = lv_label_create(playerCard);
    lv_obj_set_width(context->trackTitleLabel, LV_PCT(100));
    lv_obj_set_style_text_font(context->trackTitleLabel, lvgl_get_text_font(FONT_SIZE_LARGE), LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(context->trackTitleLabel, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_label_set_long_mode(context->trackTitleLabel, LV_LABEL_LONG_MODE_DOTS);
    context->trackArtistLabel = lv_label_create(playerCard);
    lv_obj_set_width(context->trackArtistLabel, LV_PCT(100));
    lv_obj_set_style_text_align(context->trackArtistLabel, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    context->trackAlbumLabel = lv_label_create(playerCard);
    lv_obj_set_width(context->trackAlbumLabel, LV_PCT(100));
    lv_obj_set_style_text_align(context->trackAlbumLabel, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);

    context->progressBar = lv_bar_create(playerCard);
    lv_obj_set_width(context->progressBar, LV_PCT(100));
    lv_bar_set_range(context->progressBar, 0, 100);
    lv_bar_set_value(context->progressBar, 0, LV_ANIM_OFF);
    context->progressLabel = lv_label_create(playerCard);
    lv_obj_set_width(context->progressLabel, LV_PCT(100));
    lv_label_set_text(context->progressLabel, "0:00 / --:--");
    context->playbackStatusLabel = lv_label_create(playerCard);
    lv_obj_set_width(context->playbackStatusLabel, LV_PCT(100));

    auto* controls = createRow(playerCard);
    auto* previousButton = lv_button_create(controls);
    setButtonText(previousButton, LV_SYMBOL_PREV);
    lv_obj_add_event_cb(previousButton, onPreviousPressed, LV_EVENT_CLICKED, context);
    auto* playButton = lv_button_create(controls);
    context->playButtonLabel = lv_label_create(playButton);
    lv_label_set_text(context->playButtonLabel, LV_SYMBOL_PLAY);
    lv_obj_center(context->playButtonLabel);
    lv_obj_add_event_cb(playButton, onPlayPressed, LV_EVENT_CLICKED, context);
    auto* stopButton = lv_button_create(controls);
    setButtonText(stopButton, LV_SYMBOL_STOP);
    lv_obj_add_event_cb(stopButton, onStopPressed, LV_EVENT_CLICKED, context);
    auto* nextButton = lv_button_create(controls);
    setButtonText(nextButton, LV_SYMBOL_NEXT);
    lv_obj_add_event_cb(nextButton, onNextPressed, LV_EVENT_CLICKED, context);

    updateTrackInfo(context);
    setVolumeSliderFromDevice(context);
}

void onRefreshTimer(lv_timer_t* timer) {
    auto* context = static_cast<MusicPlayerContext*>(lv_timer_get_user_data(timer));
    if (context == nullptr) {
        return;
    }

    const MusicPlaybackState state = context->playbackState.load();
    updateTrackInfo(context);

    if (context->playButtonLabel != nullptr) {
        if (state == MusicPlaybackState::Playing || state == MusicPlaybackState::Opening) {
            lv_label_set_text(context->playButtonLabel, LV_SYMBOL_PAUSE);
        } else {
            lv_label_set_text(context->playButtonLabel, LV_SYMBOL_PLAY);
        }
    }
    if (context->miniPlayButtonLabel != nullptr) {
        if (state == MusicPlaybackState::Playing || state == MusicPlaybackState::Opening) {
            lv_label_set_text(context->miniPlayButtonLabel, LV_SYMBOL_PAUSE);
        } else {
            lv_label_set_text(context->miniPlayButtonLabel, LV_SYMBOL_PLAY);
        }
    }

    const uint32_t current = context->currentTimeSeconds.load();
    const uint32_t total = context->totalTimeSeconds.load();
    if (context->progressLabel != nullptr) {
        char currentText[16];
        char totalText[16];
        char text[48];
        formatTime(current, currentText, sizeof(currentText));
        if (total == 0) {
            snprintf(totalText, sizeof(totalText), "--:--");
        } else {
            formatTime(total, totalText, sizeof(totalText));
        }
        snprintf(text, sizeof(text), "%s / %s", currentText, totalText);
        lv_label_set_text(context->progressLabel, text);
    }
    if (context->progressBar != nullptr) {
        const int32_t percent = total == 0 ? 0 : std::min<int32_t>(100, static_cast<int32_t>(current * 100 / total));
        lv_bar_set_value(context->progressBar, percent, LV_ANIM_OFF);
    }

    if (context->playbackStatusLabel != nullptr) {
        std::string status;
        {
            std::lock_guard lock(context->statusMutex);
            status = context->statusText;
        }
        if (state == MusicPlaybackState::Paused) {
            status = "Paused";
        } else if (state == MusicPlaybackState::Playing) {
            status = "Playing";
        } else if (state == MusicPlaybackState::Opening) {
            status = "Opening track";
        }
        lv_label_set_text(context->playbackStatusLabel, status.c_str());
    }
    if (context->outputDropdown != nullptr) {
        const bool active = state == MusicPlaybackState::Opening || state == MusicPlaybackState::Playing ||
                            state == MusicPlaybackState::Paused;
        bool noOutput = false;
        {
            std::lock_guard lock(context->outputMutex);
            noOutput = context->selectedOutputName.empty();
        }
        lv_obj_set_state(context->outputDropdown, LV_STATE_DISABLED, active || noOutput);
    }
}

void clearWidgetPointers(MusicPlayerContext* context) {
    context->folderInput = nullptr;
    context->browserPathLabel = nullptr;
    context->browserList = nullptr;
    context->miniTitleLabel = nullptr;
    context->miniArtistLabel = nullptr;
    context->miniPlayButtonLabel = nullptr;
    context->outputDropdown = nullptr;
    context->volumeSlider = nullptr;
    context->volumeLabel = nullptr;
    context->trackTitleLabel = nullptr;
    context->trackArtistLabel = nullptr;
    context->trackAlbumLabel = nullptr;
    context->playbackStatusLabel = nullptr;
    context->progressBar = nullptr;
    context->progressLabel = nullptr;
    context->playButtonLabel = nullptr;
}

} // namespace

void musicPlayerCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* context = static_cast<MusicPlayerContext*>(userData);
    context->root = parent;
    if (context->refreshTimer != nullptr) {
        lv_timer_delete(context->refreshTimer);
        context->refreshTimer = nullptr;
    }
    if (context->folderTimer != nullptr) {
        lv_timer_delete(context->folderTimer);
        context->folderTimer = nullptr;
    }
    clearWidgetPointers(context);
    refreshOutputDevices(context);
    createWidgetsImpl(parent, context);
    context->refreshTimer = lv_timer_create(onRefreshTimer, UI_REFRESH_PERIOD_MS, context);
    onRefreshTimer(context->refreshTimer);
}

void musicPlayerDestroyWidgets(void* userData) {
    auto* context = static_cast<MusicPlayerContext*>(userData);
    if (context->refreshTimer != nullptr) {
        lv_timer_delete(context->refreshTimer);
        context->refreshTimer = nullptr;
    }
    if (context->viewTimer != nullptr) {
        lv_timer_delete(context->viewTimer);
        context->viewTimer = nullptr;
    }
    if (context->folderTimer != nullptr) {
        lv_timer_delete(context->folderTimer);
        context->folderTimer = nullptr;
    }
    clearWidgetPointers(context);
}
