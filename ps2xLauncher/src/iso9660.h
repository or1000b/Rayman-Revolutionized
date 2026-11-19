// ISO 9660 reading for the launcher's "Install from ISO": the user's own PAL disc image of Rayman Revolution is
// checked (SYSTEM.CNF boots SLES_500.44 in PAL) and its files are copied into "Rayman revolution files".
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace launcher
{
    struct IsoFile
    {
        std::string path; // "SOUND/MUSIC.BIN" (no ";1" version)
        uint32_t lba = 0;
        uint32_t size = 0;
    };

    // Every file of the image (directories walked recursively). False with err set when it is not a readable
    // ISO 9660 image with 2048-byte sectors.
    bool isoListFiles(const std::string &isoPath, std::vector<IsoFile> &files, std::string &err);

    // True when the image is Rayman Revolution, PAL (SYSTEM.CNF: BOOT2 ...SLES_500.44, VMODE = PAL). err says why not.
    bool isoIsRaymanRevolutionPal(const std::string &isoPath, const std::vector<IsoFile> &files, std::string &err);

    // Copies every file under destDir (folders made as needed; files that are not on the disc, like the memory card
    // folder mc0, are left alone). bytesDone counts up to the sum of the sizes; currentFile names the file in progress.
    struct IsoProgress
    {
        std::atomic<uint64_t> bytesDone{0};
        std::atomic<uint64_t> bytesTotal{0};
        std::atomic<int> fileIndex{0};
    };
    bool isoExtractAll(const std::string &isoPath, const std::vector<IsoFile> &files, const std::string &destDir,
                       IsoProgress &progress, std::string &err);
}
