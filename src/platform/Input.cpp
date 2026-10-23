#include "platform/Input.h"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <cctype>

namespace rift {

void Input::newFrame() {
    prevKeys_ = keys_;
    prevButtons_ = buttons_;
    pressed_.clear();
    released_.clear();
    mouseDelta_ = glm::vec2(0.0f);
    scroll_ = 0.0f;
    text_.clear();
}

void Input::setKey(int key, bool down) {
    if (key < 0 || key >= kMaxKeys || keys_[key] == down) return;
    keys_[key] = down;
    (down ? pressed_ : released_).push_back(key);
}

void Input::setMouseButton(int button, bool down) {
    if (button < 0 || button >= kMaxButtons) return;
    buttons_[button] = down;
}

void Input::setMousePosition(const glm::vec2& position) {
    if (hasMouse_) mouseDelta_ += position - mouse_;
    mouse_ = position;
    hasMouse_ = true;
}

void Input::addScroll(float dy) { scroll_ += dy; }

void Input::addText(unsigned int c) {
    // UTF-8 encode
    if (c < 0x80) text_ += static_cast<char>(c);
    else if (c < 0x800) { text_ += static_cast<char>(0xC0 | (c >> 6)); text_ += static_cast<char>(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { text_ += static_cast<char>(0xE0 | (c >> 12)); text_ += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); text_ += static_cast<char>(0x80 | (c & 0x3F)); }
    else { text_ += static_cast<char>(0xF0 | (c >> 18)); text_ += static_cast<char>(0x80 | ((c >> 12) & 0x3F)); text_ += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); text_ += static_cast<char>(0x80 | (c & 0x3F)); }
}

void Input::releaseAll() {
    for (int k = 0; k < kMaxKeys; ++k) if (keys_[k]) setKey(k, false);
    buttons_.fill(false);
}

bool Input::keyDown(int key) const { return key >= 0 && key < kMaxKeys && keys_[key]; }
bool Input::keyPressed(int key) const { return key >= 0 && key < kMaxKeys && keys_[key] && !prevKeys_[key]; }
bool Input::keyReleased(int key) const { return key >= 0 && key < kMaxKeys && !keys_[key] && prevKeys_[key]; }
bool Input::mouseDown(int b) const { return b >= 0 && b < kMaxButtons && buttons_[b]; }
bool Input::mousePressed(int b) const { return b >= 0 && b < kMaxButtons && buttons_[b] && !prevButtons_[b]; }
bool Input::mouseReleased(int b) const { return b >= 0 && b < kMaxButtons && !buttons_[b] && prevButtons_[b]; }

bool Input::ctrl() const { return keyDown(GLFW_KEY_LEFT_CONTROL) || keyDown(GLFW_KEY_RIGHT_CONTROL); }
bool Input::shift() const { return keyDown(GLFW_KEY_LEFT_SHIFT) || keyDown(GLFW_KEY_RIGHT_SHIFT); }
bool Input::alt() const { return keyDown(GLFW_KEY_LEFT_ALT) || keyDown(GLFW_KEY_RIGHT_ALT); }

const std::vector<std::pair<std::string, int>>& keyCodeTable() {
    static const std::vector<std::pair<std::string, int>> table = [] {
        std::vector<std::pair<std::string, int>> t;
        for (char c = 'A'; c <= 'Z'; ++c) t.emplace_back(std::string(1, c), GLFW_KEY_A + (c - 'A'));
        const char* digits[] = { "Zero", "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine" };
        for (int i = 0; i < 10; ++i) t.emplace_back(digits[i], GLFW_KEY_0 + i);
        for (int i = 1; i <= 12; ++i) t.emplace_back("F" + std::to_string(i), GLFW_KEY_F1 + (i - 1));
        for (int i = 0; i < 10; ++i) t.emplace_back(std::string("Keypad") + digits[i], GLFW_KEY_KP_0 + i);
        const std::pair<const char*, int> named[] = {
            { "Space", GLFW_KEY_SPACE }, { "Return", GLFW_KEY_ENTER }, { "Tab", GLFW_KEY_TAB }, { "Escape", GLFW_KEY_ESCAPE },
            { "Backspace", GLFW_KEY_BACKSPACE }, { "Delete", GLFW_KEY_DELETE }, { "Insert", GLFW_KEY_INSERT },
            { "Home", GLFW_KEY_HOME }, { "End", GLFW_KEY_END }, { "PageUp", GLFW_KEY_PAGE_UP }, { "PageDown", GLFW_KEY_PAGE_DOWN },
            { "Up", GLFW_KEY_UP }, { "Down", GLFW_KEY_DOWN }, { "Left", GLFW_KEY_LEFT }, { "Right", GLFW_KEY_RIGHT },
            { "LeftShift", GLFW_KEY_LEFT_SHIFT }, { "RightShift", GLFW_KEY_RIGHT_SHIFT },
            { "LeftControl", GLFW_KEY_LEFT_CONTROL }, { "RightControl", GLFW_KEY_RIGHT_CONTROL },
            { "LeftAlt", GLFW_KEY_LEFT_ALT }, { "RightAlt", GLFW_KEY_RIGHT_ALT },
            { "CapsLock", GLFW_KEY_CAPS_LOCK }, { "Minus", GLFW_KEY_MINUS }, { "Equals", GLFW_KEY_EQUAL },
            { "LeftBracket", GLFW_KEY_LEFT_BRACKET }, { "RightBracket", GLFW_KEY_RIGHT_BRACKET },
            { "Semicolon", GLFW_KEY_SEMICOLON }, { "Quote", GLFW_KEY_APOSTROPHE }, { "Comma", GLFW_KEY_COMMA },
            { "Period", GLFW_KEY_PERIOD }, { "Slash", GLFW_KEY_SLASH }, { "BackSlash", GLFW_KEY_BACKSLASH }, { "Backquote", GLFW_KEY_GRAVE_ACCENT },
        };
        for (const auto& [name, code] : named) t.emplace_back(name, code);
        return t;
    }();
    return table;
}

std::string keyName(int key) {
    for (const auto& [name, code] : keyCodeTable()) if (code == key) return name;
    return "Key" + std::to_string(key);
}

int keyFromName(const std::string& name) {
    auto lower = [](std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
    const std::string wanted = lower(name);
    for (const auto& [n, code] : keyCodeTable()) if (lower(n) == wanted) return code;
    return -1;
}

} // namespace rift
