#pragma once

#include "text_glyph.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class EmojiCollection;
class LvglFont;
class LvglImage {
public:
    virtual ~LvglImage() = default;
};

class Theme {
public:
    explicit Theme(const std::string& name) : name_(name) {}
    virtual ~Theme() = default;
    std::string name() const { return name_; }
    virtual std::shared_ptr<LvglFont> GetTextFont() const { return nullptr; }
private:
    std::string name_;
};

class Display {
public:
    Display() = default;
    virtual ~Display() = default;
    virtual void SetStatus(const char*) {}
    virtual void ShowNotification(const char*, int = 3000) {}
    virtual void ShowNotification(const std::string&, int = 3000) {}
    virtual bool SetEmotion(const char*) { return false; }
    virtual void SetChatMessage(const char*, const char*) {}
    virtual void RemoveChatMessage(const char*, const char*) {}
    virtual void ClearChatMessages() {}
    virtual void SetTheme(Theme* theme) { current_theme_ = theme; }
    virtual Theme* GetTheme() { return current_theme_; }
    virtual void UpdateStatusBar(bool = false) {}
    virtual void SetPowerSaveMode(bool) {}
    virtual bool AddTextGlyphs(const std::vector<TextGlyph>&, uint8_t) { return false; }
    virtual void ClearTextGlyphs() {}
    virtual void SetEmojiCollection(std::shared_ptr<EmojiCollection>) {}
    virtual void SetupUI() { setup_ui_called_ = true; }
    virtual bool IsMonochrome() const { return true; }
    virtual bool SupportsGuiOperations() const { return false; }
    virtual void SetHideSubtitle(bool) {}
    virtual bool InsertAnimDialog(const char*, uint32_t) { return false; }
    virtual bool MountAssets(const char*) { return false; }
    virtual void UnmountAssets() {}
    virtual bool GetAssetData(const std::string&, const uint8_t*&, size_t&) { return false; }
    virtual void LoadAssets() {}
    virtual void SetPreviewImage(std::unique_ptr<LvglImage>) {}
    virtual bool SetTextFont(std::shared_ptr<LvglFont>) { return false; }
    virtual bool SnapshotToJpeg(std::string&, int = 80) { return false; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool IsSetupUICalled() const { return setup_ui_called_; }
protected:
    int width_ = 0;
    int height_ = 0;
    bool setup_ui_called_ = false;
    Theme* current_theme_ = nullptr;
    friend class DisplayLockGuard;
    virtual bool Lock(int = 0) = 0;
    virtual void Unlock() = 0;
};

class NoDisplay : public Display {
private:
    bool Lock(int = 0) override { return true; }
    void Unlock() override {}
};

class DisplayLockGuard {
public:
    explicit DisplayLockGuard(Display* display)
        : display_(display), locked_(display_ != nullptr && display_->Lock(30000)) {}
    ~DisplayLockGuard() { if (locked_) display_->Unlock(); }
    DisplayLockGuard(const DisplayLockGuard&) = delete;
    DisplayLockGuard& operator=(const DisplayLockGuard&) = delete;
    explicit operator bool() const { return locked_; }
private:
    Display* display_;
    bool locked_;
};
