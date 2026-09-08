#pragma once
#include <string>
#include <vector>

namespace afterlight {
// Host-independent options. Patterns are semicolon-separated file globs, e.g. "*.png;*.jpg".
struct FileDialogFilter {
    std::string name;
    std::string pattern;
};
struct OpenFileDialogOptions {
    std::string title;
    std::string initialDirectory;
    std::vector<FileDialogFilter> filters;
};
} // namespace afterlight
