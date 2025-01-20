// tsm_file_utils.h

#include <string>
#include <vector>
#include <filesystem>
#include <windows.h>

std::vector<std::string> OpenFileDialog(const char* filter = "Audio Files\0*.mp3;*.wav;*.ogg;*.flac\0All Files\0*.*\0") {
    std::vector<std::string> filenames;

    OPENFILENAMEA ofn;       
    CHAR szFile[8192] = { 0 }; 
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL; 
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    ofn.lpstrDefExt = "mp3";

    if (GetOpenFileNameA(&ofn)) {
        char* p = szFile;
        std::string directory = p;
        p += directory.length() + 1;

        if (*p == '\0') {
            filenames.push_back(directory);
        }
        else {
            while (*p) {
                std::string filename = p;
                p += filename.length() + 1;
                filenames.push_back(directory + "\\" + filename);
            }
        }
    }

    return filenames;
}

