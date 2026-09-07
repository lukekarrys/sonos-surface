#pragma once
#include "WaveshareUi.h"
#include <Arduino_GFX_Library.h>

namespace surface::device {
// Rendering stays local to the AMOLED adapter and shares hit rectangles with UI.
class WaveshareDrawing {
  Arduino_GFX& gfx;
  static constexpr uint16_t white = 0xFFFF, muted = 0x8C71, accent = 0x6F9B, tile = 0x18E3, amber = 0xFD88;
  // The pinned built-in font is ASCII. Consume complete UTF-8 sequences so a
  // clipped title never emits fragments or lets metadata wrap outside its area.
  static std::string displayText(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size();) {
      const unsigned char c = text[i++];
      if (c >= 32 && c < 127) out += char(c);
      else if (c < 128) out += ' ';
      else {
        while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) ++i;
        out += '?';
      }
    }
    return out;
  }
  void text(int x, int y, const std::string& value, int columns, int size = 2, uint16_t color = white) {
    auto s = displayText(value);
    if (int(s.size()) > columns) s = s.substr(0, columns-3) + "...";
    gfx.setTextSize(size); gfx.setTextColor(color); gfx.setCursor(x,y); gfx.print(s.c_str());
  }
  void lines(int x, int y, const std::string& value, int columns, unsigned count) {
    auto s = displayText(value);
    for (unsigned line = 0; line < count && !s.empty(); ++line) {
      if (line+1 == count || int(s.size()) <= columns) { text(x,y+int(line)*21,s,columns); break; }
      size_t split = s.rfind(' ',columns);
      if (split == std::string::npos || split < size_t(columns/2)) split = columns;
      text(x,y+int(line)*21,s.substr(0,split),columns);
      s.erase(0,split); if (!s.empty() && s.front() == ' ') s.erase(0,1);
    }
  }
  void button(const UiRect& r, const std::string& title, bool enabled = true, bool selected = false, int size = 2) {
    gfx.fillRoundRect(r.x,r.y,r.w,r.h,10,selected ? accent : tile);
    const int width = std::min<int>(title.size(),r.w/(6*size)-1)*6*size;
    text(r.x+(r.w-width)/2,r.y+(r.h-8*size)/2,title,r.w/(6*size)-1,size,selected ? 0 : enabled ? white : muted);
  }
  static std::string time(std::optional<uint32_t> ms) {
    if (!ms) return "--:--";
    const auto sec = *ms/1000;
    return std::to_string(sec/60) + ":" + (sec%60 < 10 ? "0" : "") + std::to_string(sec%60);
  }
  void slider(int y, uint32_t value, uint32_t maximum, bool enabled, bool preview) {
    using namespace waveshareLayout;
    const int width = sliderRight-sliderLeft;
    const int fill = maximum ? uint64_t(std::min(value,maximum))*width/maximum : 0;
    gfx.fillRoundRect(sliderLeft,y,width,6,3,tile);
    if (fill) gfx.fillRoundRect(sliderLeft,y,fill,6,3,enabled ? accent : muted);
    if (enabled) gfx.fillCircle(sliderLeft+fill,y+3,preview ? 8 : 5,preview ? amber : white);
  }
  void nowPlaying(const WaveshareUi& ui, const uint16_t* artwork) {
    using namespace waveshareLayout;
    const auto& o = ui.state.observed;
    text(32,40,o.room.empty() ? "Choose room" : o.room,23);
    text(316,42,"v",1,2,accent);
    text(32,76,ui.context.readOnly ? "READ ONLY" : "SONOS",16,1,ui.context.readOnly ? amber : muted);
    text(206,76,!ui.context.online ? "Wi-Fi offline" : o.stale ? "State out of date" : "",22,1,amber);
    if (artwork) gfx.draw16bitRGBBitmap(32,96,artwork,64,64);
    else {
      gfx.fillRoundRect(32,96,64,64,10,tile);
      gfx.drawCircle(64,128,20,muted); gfx.fillCircle(64,128,5,accent);
    }
    const char* sourceTitle = !o.known ? "Loading room..." : o.source == PlaybackSource::Live ? "Live / TV" :
      o.source == PlaybackSource::AppleMusicStation ? "Radio station" : o.source == PlaybackSource::Queue ? "Queue playback" :
      o.source == PlaybackSource::Other ? "Audio source" : "Nothing selected";
    lines(108,97,o.title.empty() ? sourceTitle : o.title,19,2);
    text(108,145,o.artist.empty() ? "Artist unavailable" : o.artist,19,2,muted);
    text(32,169,o.album,50,1,muted);
    std::string status = !ui.toast.empty() ? ui.toast : ui.context.busy ? "Updating..." :
      ui.state.recoveryRequired ? "Check room; recovery needed" : !ui.state.refreshError.empty() ? "Room unavailable - retry" :
      !ui.context.online ? "Wi-Fi offline" : playbackLabel(o.transport);
    text(32,190,status,50,1,!ui.toast.empty() || !ui.state.refreshError.empty() ? amber : muted);
    slider(216,ui.seekPreview.value_or(o.positionMs.value_or(0)),o.durationMs.value_or(0),ui.canSeek(),ui.seekPreview.has_value());
    text(52,236,time(ui.seekPreview ? ui.seekPreview : o.positionMs),12,1,ui.seekPreview ? amber : muted);
    text(240,236,o.source == PlaybackSource::Live || o.source == PlaybackSource::AppleMusicStation ? "LIVE" : time(o.durationMs),12,1,muted);
    button(previous,"|<",ui.fresh());
    button(play,o.transport == PlaybackStatus::Playing ? "Pause" : "Play",ui.canPlay(),ui.canPlay());
    button(next,">|",ui.fresh());
    text(52,324,"Volume " + (ui.volumePreview ? std::to_string(*ui.volumePreview) : o.volume ? std::to_string(*o.volume) : "--") +
      (o.mute == true ? " (muted)" : ""),22,2,ui.volumePreview ? amber : muted);
    slider(351,ui.volumePreview.value_or(o.volume.value_or(0)),100,ui.fresh() && o.volume.has_value(),ui.volumePreview.has_value());
    button(shuffle,"Shuffle " + std::string(o.shuffle ? (*o.shuffle ? "On" : "Off") : "--"),ui.activeQueue() && o.shuffle.has_value(),false,1);
    button(repeat,"Repeat " + std::string(o.repeat ? repeatLabel(*o.repeat) : "--"),ui.activeQueue() && o.repeat.has_value(),false,1);
    button(waveshareLayout::queue,"Queue",true,false,2);
    text(52,426,ui.seekPreview || ui.volumePreview ? "Preview - lift to submit" :
      o.queueIndex && o.queueTotal ? "Track " + std::to_string(*o.queueIndex+1) + " of " + std::to_string(*o.queueTotal) :
      "Tap room name to switch",44,1,muted);
  }
  void navigation(const WaveshareUi& ui, const char* title) {
    using namespace waveshareLayout;
    button(back,"Back",true,false,2); button(reload,"Refresh",true,false,1);
    text(132,44,title,14,1,accent);
    text(32,83,ui.state.observed.room.empty() ? "Choose a room" : ui.state.observed.room,25,2);
  }
  void rooms(const WaveshareUi& ui) {
    using namespace waveshareLayout;
    navigation(ui,"ROOMS");
    if (ui.context.rooms.empty()) text(40,160,ui.context.busy ? "Finding rooms..." : "No selectable rooms",24);
    for (unsigned i = 0; i < 4 && ui.roomStart+i < ui.context.rooms.size(); ++i) {
      const auto& room = ui.context.rooms[ui.roomStart+i];
      button(row(i),room.name,true,room.id == ui.state.observed.targetId);
    }
    text(52,349,std::to_string(ui.context.rooms.size()) + " configured rooms available",44,1,muted);
    button(pageBack,"Earlier",ui.roomStart > 0);
    button(pageNext,"More",uint64_t(ui.roomStart)+4 < ui.context.rooms.size());
  }
  void queue(const WaveshareUi& ui) {
    using namespace waveshareLayout;
    navigation(ui,"QUEUE");
    if (ui.matchingPage()) {
      const auto& page = *ui.state.queue;
      if (page.items.empty()) text(52,174,page.total ? "No items on this page" : "Queue is empty",24);
      for (unsigned i = 0; i < page.items.size() && i < 4; ++i) {
        const auto& item = page.items[i]; const auto r = row(i);
        const bool current = ui.canSelectQueue() && ui.state.observed.queueIndex == item.index;
        gfx.fillRoundRect(r.x,r.y,r.w,r.h,10,tile);
        if (current) gfx.fillRoundRect(r.x,r.y,4,r.h,2,accent);
        text(44,r.y+8,std::to_string(item.index+1) + ". " + (item.title.empty() ? "Untitled" : item.title),23,2,current ? accent : white);
        text(44,r.y+35,item.artist.empty() ? "Artist unavailable" : item.artist,46,1,muted);
      }
      text(52,349,"Page " + std::to_string(page.start/4+1) + " / " + std::to_string(std::max<uint64_t>(1,(uint64_t(page.total)+3)/4)) +
        (ui.activeQueue() ? "" : " - stored queue"),44,1,muted);
    } else {
      text(52,158,!ui.state.queueError.empty() ? "Queue could not load" : !ui.context.online ? "Wi-Fi offline" : "Loading queue...",24);
      text(52,188,!ui.state.queueError.empty() ? "Tap Refresh to try again" : "Four items per page",40,1,muted);
    }
    button(pageBack,"Earlier",ui.queueStart > 0);
    button(pageNext,"More",ui.matchingPage() && uint64_t(ui.queueStart)+4 < ui.state.queue->total);
  }
public:
  explicit WaveshareDrawing(Arduino_GFX& output) : gfx(output) {}
  void draw(const WaveshareUi& ui, const uint16_t* artwork = nullptr) {
    gfx.fillScreen(0); gfx.setTextWrap(false);
    switch (ui.screen) {
      case WaveshareScreen::NowPlaying: nowPlaying(ui, artwork); break;
      case WaveshareScreen::Rooms: rooms(ui); break;
      case WaveshareScreen::Queue: queue(ui); break;
    }
    if (ui.screen != WaveshareScreen::NowPlaying)
      text(40,426,!ui.toast.empty() ? ui.toast : ui.context.readOnly ? "READ ONLY" : ui.context.busy ? "Updating..." : "SONOS",48,1,amber);
  }
};
} // namespace surface::device
