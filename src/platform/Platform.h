#pragma once
//
// Platform.h -- thin wrappers over OS services the editor and player need:
// native file dialogs, message boxes, opening files/folders in the shell.
// All strings are UTF-8. Dialogs are modal and return "" when cancelled.
//
#include <string>
#include <vector>

struct GLFWwindow;

namespace Platform {

// Dialogs are parented to this window (call once after creating it).
void setMainWindow(GLFWwindow* window);

struct FileFilter {
    std::string name;     // "Images"
    std::string patterns; // "*.png;*.jpg;*.jpeg"
};

std::string openFileDialog(const std::string& title, const std::vector<FileFilter>& filters,
                           const std::string& startFolder = "");
// defaultExtension without the dot ("json"); appended when the user types a bare name.
std::string saveFileDialog(const std::string& title, const std::vector<FileFilter>& filters,
                           const std::string& startFolder = "", const std::string& defaultName = "",
                           const std::string& defaultExtension = "");
std::string pickFolderDialog(const std::string& title, const std::string& startFolder = "");

enum class MessageType { Info, Warning, Error };
void messageBox(const std::string& title, const std::string& text, MessageType type = MessageType::Info);
// Yes/No question; true for Yes.
bool askYesNo(const std::string& title, const std::string& text);

// Shows the file selected in Explorer (or opens the folder).
void revealInExplorer(const std::string& path);
// Opens a file/folder/URL with its default application.
void openWithShell(const std::string& pathOrUrl);

// Clipboard (text).
void setClipboard(const std::string& text);

// Starts another program (arguments are quoted as needed). False if it could not start.
bool launch(const std::string& exe, const std::vector<std::string>& arguments, const std::string& workingFolder = "");

// The user's Documents folder ("" if unknown).
std::string documentsFolder();
// Deletes a file or folder by moving it to the Recycle Bin (recoverable).
bool moveToRecycleBin(const std::string& path);

} // namespace Platform
