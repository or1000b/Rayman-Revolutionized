#include "iso9660.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace launcher
{
    namespace
    {
        constexpr uint32_t kSector = 2048u;

        std::filesystem::path fsPath(const std::string &utf8)
        {
            return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
        }

        uint32_t le32(const uint8_t *p)
        {
            return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }

        bool readAt(std::ifstream &f, uint64_t offset, uint32_t bytes, std::vector<uint8_t> &out)
        {
            out.resize(bytes);
            f.clear();
            f.seekg(static_cast<std::streamoff>(offset));
            f.read(reinterpret_cast<char *>(out.data()), bytes);
            return static_cast<uint32_t>(f.gcount()) == bytes;
        }

        // One directory's records (the extent at lba, size bytes); subdirectories are walked too (depth-limited).
        bool walk(std::ifstream &f, uint32_t lba, uint32_t size, const std::string &prefix, int depth, std::vector<IsoFile> &files)
        {
            if (depth > 16 || size > 16u * 1024u * 1024u)
                return false;
            std::vector<uint8_t> dir;
            if (!readAt(f, uint64_t(lba) * kSector, size, dir))
                return false;
            uint32_t pos = 0;
            while (pos < size)
            {
                const uint8_t len = dir[pos];
                if (len == 0) // records do not cross a sector: the rest of this one is padding
                {
                    pos = (pos / kSector + 1u) * kSector;
                    continue;
                }
                if (len < 34u || pos + len > size)
                    break;
                const uint8_t *r = &dir[pos];
                const uint32_t extent = le32(r + 2);
                const uint32_t length = le32(r + 10);
                const bool isDir = (r[25] & 2u) != 0u;
                const uint8_t nameLen = r[32];
                std::string name(reinterpret_cast<const char *>(r + 33), nameLen);
                pos += len;
                if (nameLen == 1 && (name[0] == '\0' || name[0] == '\1'))
                    continue; // "." and ".."
                if (const size_t semi = name.find(';'); semi != std::string::npos)
                    name.resize(semi);
                if (!name.empty() && name.back() == '.')
                    name.pop_back();
                const std::string path = prefix.empty() ? name : prefix + "/" + name;
                if (isDir)
                {
                    if (!walk(f, extent, length, path, depth + 1, files))
                        return false;
                }
                else
                {
                    files.push_back(IsoFile{path, extent, length});
                }
            }
            return true;
        }
    }

    bool isoListFiles(const std::string &isoPath, std::vector<IsoFile> &files, std::string &err)
    {
        files.clear();
        std::ifstream f(fsPath(isoPath), std::ios::binary);
        if (!f)
        {
            err = "The file could not be opened.";
            return false;
        }
        std::vector<uint8_t> pvd;
        if (!readAt(f, 16ull * kSector, kSector, pvd) || pvd[0] != 1 || std::memcmp(&pvd[1], "CD001", 5) != 0)
        {
            err = "This is not a disc image (ISO 9660 with 2048-byte sectors).";
            return false;
        }
        const uint8_t *root = &pvd[156];
        if (!walk(f, le32(root + 2), le32(root + 10), "", 0, files) || files.empty())
        {
            err = "The disc image's folders could not be read.";
            return false;
        }
        return true;
    }

    bool isoIsRaymanRevolutionPal(const std::string &isoPath, const std::vector<IsoFile> &files, std::string &err)
    {
        const IsoFile *cnf = nullptr;
        for (const IsoFile &file : files)
            if (file.path == "SYSTEM.CNF")
                cnf = &file;
        if (!cnf || cnf->size == 0u || cnf->size > 4096u)
        {
            err = "This is not a PlayStation 2 disc (no SYSTEM.CNF).";
            return false;
        }
        std::ifstream f(fsPath(isoPath), std::ios::binary);
        std::vector<uint8_t> data;
        if (!f || !readAt(f, uint64_t(cnf->lba) * kSector, cnf->size, data))
        {
            err = "SYSTEM.CNF could not be read.";
            return false;
        }
        const std::string text(data.begin(), data.end());
        if (text.find("SLES_500.44") == std::string::npos)
        {
            err = "This is not Rayman Revolution for Europe (PAL, SLES-50044).";
            return false;
        }
        if (text.find("PAL") == std::string::npos)
        {
            err = "This disc is not the PAL version.";
            return false;
        }
        return true;
    }

    bool isoExtractAll(const std::string &isoPath, const std::vector<IsoFile> &files, const std::string &destDir,
                       IsoProgress &progress, std::string &err)
    {
        uint64_t total = 0;
        for (const IsoFile &file : files)
            total += file.size;
        progress.bytesTotal = total;
        progress.bytesDone = 0;
        std::ifstream in(fsPath(isoPath), std::ios::binary);
        if (!in)
        {
            err = "The disc image could not be opened.";
            return false;
        }
        std::vector<char> buffer(4u << 20);
        int index = 0;
        for (const IsoFile &file : files)
        {
            progress.fileIndex = index++;
            const std::filesystem::path out = fsPath(destDir) / fsPath(file.path);
            std::error_code ec;
            std::filesystem::create_directories(out.parent_path(), ec);
            std::ofstream o(out, std::ios::binary | std::ios::trunc);
            if (!o)
            {
                err = "Could not write " + file.path + " (is the game running, or the folder read-only?).";
                return false;
            }
            in.clear();
            in.seekg(static_cast<std::streamoff>(uint64_t(file.lba) * kSector));
            uint64_t left = file.size;
            while (left > 0)
            {
                const size_t chunk = static_cast<size_t>(std::min<uint64_t>(left, buffer.size()));
                in.read(buffer.data(), static_cast<std::streamsize>(chunk));
                if (static_cast<size_t>(in.gcount()) != chunk)
                {
                    err = "The disc image ended early (" + file.path + "): is it complete?";
                    return false;
                }
                o.write(buffer.data(), static_cast<std::streamsize>(chunk));
                if (!o)
                {
                    err = "Writing " + file.path + " failed (disk full?).";
                    return false;
                }
                left -= chunk;
                progress.bytesDone += chunk;
            }
        }
        return true;
    }
}
