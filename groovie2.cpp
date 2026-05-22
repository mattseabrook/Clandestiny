#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "config.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <objbase.h>
#include <wincodec.h>
#else
#include <csetjmp>
#include <jpeglib.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr uint16_t kRoQQuad = 0x1000;
constexpr uint16_t kRoQInfo = 0x1001;
constexpr uint16_t kRoQCodebook = 0x1002;
constexpr uint16_t kRoQVQ = 0x1011;
constexpr uint16_t kRoQJpeg = 0x1012;
constexpr uint16_t kRoQHang = 0x1013;
constexpr uint16_t kRoQSoundMono = 0x1020;
constexpr uint16_t kRoQSoundStereo = 0x1021;
constexpr uint16_t kRoQPacket = 0x1030;
constexpr uint16_t kRoQSignature = 0x1084;

struct Pixel {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;
};

struct BlockHeader {
    uint16_t type = 0;
    uint32_t size = 0;
    uint16_t argument = 0;
};

struct DecodeOptions {
    bool raw = false;
    bool forceAlt = false;
    bool forceNorm = false;
    bool list = false;
    bool all = false;
    bool rootExplicit = false;
    bool writeFiles = true;
    bool encodeMp4 = true;
    bool collectFrames = false;
    bool log = true;
    fs::path catalogRoot = ".";
    fs::path outRoot = ".";
};

struct DecodedMovie {
    std::string sequenceName;
    std::string leafName;
    int width = 0;
    int height = 0;
    int fps = 0;
    uint16_t alpha = 0;
    bool interlaced = false;
    bool altMotionDecoder = false;
    uint32_t audioChunks = 0;
    uint32_t badCodebookRefs = 0;
    std::vector<std::vector<Pixel>> frames;
    std::vector<uint8_t> pcm;
};

std::string toLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool iendsWith(std::string_view text, std::string_view suffix) {
    if (suffix.size() > text.size()) {
        return false;
    }
    return iequals(text.substr(text.size() - suffix.size()), suffix);
}

std::string trimNullTerminated(const uint8_t *data, size_t size) {
    size_t len = 0;
    while (len < size && data[len] != 0) {
        ++len;
    }
    while (len > 0 && std::isspace(static_cast<unsigned char>(data[len - 1]))) {
        --len;
    }
    return std::string(reinterpret_cast<const char *>(data), len);
}

std::vector<uint8_t> readWholeFile(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Could not open " + path.string());
    }
    file.seekg(0, std::ios::end);
    const auto size = file.tellg();
    if (size < 0) {
        throw std::runtime_error("Could not size " + path.string());
    }
    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!data.empty()) {
        file.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    if (!file && !data.empty()) {
        throw std::runtime_error("Could not read " + path.string());
    }
    return data;
}

void writeWholeFile(const fs::path &path, const std::vector<uint8_t> &data) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Could not create " + path.string());
    }
    if (!data.empty()) {
        file.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    if (!file) {
        throw std::runtime_error("Could not write " + path.string());
    }
}

uint16_t readU16LE(const uint8_t *data) {
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

uint32_t readU32LE(const uint8_t *data) {
    return static_cast<uint32_t>(data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24));
}

void appendU32BE(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

void appendU16LE(std::vector<uint8_t> &out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

void appendU32LE(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

uint32_t crc32(const uint8_t *data, size_t size) {
    static std::array<uint32_t, 256> table{};
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < table.size(); ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        ready = true;
    }

    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        c = table[(c ^ data[i]) & 0xff] ^ (c >> 8);
    }
    return c ^ 0xffffffffu;
}

uint32_t adler32(const uint8_t *data, size_t size) {
    constexpr uint32_t kMod = 65521;
    uint32_t a = 1;
    uint32_t b = 0;
    for (size_t i = 0; i < size; ++i) {
        a = (a + data[i]) % kMod;
        b = (b + a) % kMod;
    }
    return (b << 16) | a;
}

void appendPngChunk(std::vector<uint8_t> &png, const char type[4], const std::vector<uint8_t> &payload) {
    appendU32BE(png, static_cast<uint32_t>(payload.size()));
    const size_t typeOffset = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), payload.begin(), payload.end());
    appendU32BE(png, crc32(png.data() + typeOffset, 4 + payload.size()));
}

void writePngRGBA(const fs::path &path, int width, int height, const std::vector<Pixel> &pixels) {
    if (width <= 0 || height <= 0 || pixels.size() != static_cast<size_t>(width * height)) {
        throw std::runtime_error("PNG writer received an invalid frame");
    }

    std::vector<uint8_t> scanlines;
    scanlines.reserve(static_cast<size_t>(height) * (1 + static_cast<size_t>(width) * 4));
    for (int y = 0; y < height; ++y) {
        scanlines.push_back(0); // filter type: none
        for (int x = 0; x < width; ++x) {
            const Pixel &p = pixels[static_cast<size_t>(y) * width + x];
            scanlines.push_back(p.r);
            scanlines.push_back(p.g);
            scanlines.push_back(p.b);
            scanlines.push_back(p.a);
        }
    }

    std::vector<uint8_t> zlib;
    zlib.reserve(scanlines.size() + scanlines.size() / 65535 * 5 + 16);
    zlib.push_back(0x78);
    zlib.push_back(0x01);
    size_t pos = 0;
    while (pos < scanlines.size()) {
        const uint16_t blockSize = static_cast<uint16_t>(std::min<size_t>(65535, scanlines.size() - pos));
        const bool final = pos + blockSize == scanlines.size();
        zlib.push_back(final ? 0x01 : 0x00);
        zlib.push_back(static_cast<uint8_t>(blockSize & 0xff));
        zlib.push_back(static_cast<uint8_t>(blockSize >> 8));
        const uint16_t nlen = static_cast<uint16_t>(~blockSize);
        zlib.push_back(static_cast<uint8_t>(nlen & 0xff));
        zlib.push_back(static_cast<uint8_t>(nlen >> 8));
        zlib.insert(zlib.end(), scanlines.begin() + static_cast<std::ptrdiff_t>(pos),
                    scanlines.begin() + static_cast<std::ptrdiff_t>(pos + blockSize));
        pos += blockSize;
    }
    appendU32BE(zlib, adler32(scanlines.data(), scanlines.size()));

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<uint8_t> ihdr;
    appendU32BE(ihdr, static_cast<uint32_t>(width));
    appendU32BE(ihdr, static_cast<uint32_t>(height));
    ihdr.push_back(8); // bit depth
    ihdr.push_back(6); // RGBA
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    appendPngChunk(png, "IHDR", ihdr);
    appendPngChunk(png, "IDAT", zlib);
    appendPngChunk(png, "IEND", {});

    writeWholeFile(path, png);
}

std::vector<uint8_t> makeRawRGBA(int width, int height, const std::vector<Pixel> &pixels) {
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(width) * height * 4);
    for (const Pixel &p : pixels) {
        raw.push_back(p.r);
        raw.push_back(p.g);
        raw.push_back(p.b);
        raw.push_back(p.a);
    }
    return raw;
}

std::vector<uint8_t> makeWavPCM16Stereo(const std::vector<uint8_t> &pcm) {
    constexpr uint16_t channels = 2;
    constexpr uint32_t sampleRate = 22050;
    constexpr uint16_t bitsPerSample = 16;
    constexpr uint16_t blockAlign = channels * bitsPerSample / 8;
    constexpr uint32_t byteRate = sampleRate * blockAlign;

    if (pcm.size() > 0xffffffffu - 36u) {
        throw std::runtime_error("WAV data too large");
    }

    std::vector<uint8_t> wav;
    wav.reserve(44 + pcm.size());
    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    appendU32LE(wav, static_cast<uint32_t>(36 + pcm.size()));
    wav.insert(wav.end(), {'W', 'A', 'V', 'E'});
    wav.insert(wav.end(), {'f', 'm', 't', ' '});
    appendU32LE(wav, 16);
    appendU16LE(wav, 1);
    appendU16LE(wav, channels);
    appendU32LE(wav, sampleRate);
    appendU32LE(wav, byteRate);
    appendU16LE(wav, blockAlign);
    appendU16LE(wav, bitsPerSample);
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    appendU32LE(wav, static_cast<uint32_t>(pcm.size()));
    wav.insert(wav.end(), pcm.begin(), pcm.end());

    return wav;
}

void writeWavPCM16Stereo(const fs::path &path, const std::vector<uint8_t> &pcm) {
    const std::vector<uint8_t> wav = makeWavPCM16Stereo(pcm);
    writeWholeFile(path, wav);
}

[[maybe_unused]] std::string quoteProcessArg(const fs::path &path) {
    std::string text = path.string();
    std::string quoted = "\"";
    for (char ch : text) {
        if (ch == '"') {
            quoted += "\\\"";
        } else {
            quoted += ch;
        }
    }
    quoted += "\"";
    return quoted;
}

[[maybe_unused]] int runExternalCommand(const std::string &command) {
#ifdef _WIN32
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::string mutableCommand = command;
    if (!CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup,
                        &process)) {
        return -1;
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
#else
    return std::system(command.c_str());
#endif
}

[[maybe_unused]] std::optional<fs::path> findFfmpegOnPath() {
    const char *pathEnv = std::getenv("PATH");
    if (!pathEnv || *pathEnv == 0) {
        return std::nullopt;
    }
#ifdef _WIN32
    constexpr char separator = ';';
    const std::vector<std::string> names = {"ffmpeg.exe", "ffmpeg.cmd", "ffmpeg.bat", "ffmpeg"};
#else
    constexpr char separator = ':';
    const std::vector<std::string> names = {"ffmpeg"};
#endif
    std::stringstream ss(pathEnv);
    std::string dir;
    while (std::getline(ss, dir, separator)) {
        fs::path base = dir.empty() ? fs::path(".") : fs::path(dir);
        for (const std::string &name : names) {
            fs::path candidate = base / name;
            if (fs::is_regular_file(candidate)) {
                return candidate;
            }
        }
    }
    return std::nullopt;
}

fs::path findChildInsensitive(const fs::path &dir, std::string_view child) {
    fs::path direct = dir / std::string(child);
    if (fs::exists(direct)) {
        return direct;
    }
    for (const fs::directory_entry &entry : fs::directory_iterator(dir)) {
        if (iequals(entry.path().filename().string(), child)) {
            return entry.path();
        }
    }
    return direct;
}

std::vector<std::string> splitVirtualPath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/') {
        path.erase(path.begin());
    }
    std::vector<std::string> parts;
    std::stringstream ss(path);
    std::string item;
    while (std::getline(ss, item, '/')) {
        if (!item.empty()) {
            parts.push_back(item);
        }
    }
    return parts;
}

fs::path resolveInsensitivePath(const fs::path &root, std::string virtualPath) {
    fs::path cur = root;
    for (const std::string &part : splitVirtualPath(std::move(virtualPath))) {
        if (part == ".") {
            continue;
        }
        if (part == "..") {
            cur = cur.parent_path();
            continue;
        }
        if (!fs::is_directory(cur)) {
            return cur / part;
        }
        cur = findChildInsensitive(cur, part);
    }
    return cur;
}

class DataSource {
public:
    virtual ~DataSource() = default;
    virtual bool readFile(const std::string &virtualPath, std::vector<uint8_t> &out) = 0;
    virtual bool readFileRange(const std::string &virtualPath, uint64_t offset, uint64_t size,
                               std::vector<uint8_t> &out) {
        std::vector<uint8_t> whole;
        if (!readFile(virtualPath, whole) || offset > whole.size() || size > whole.size() - offset) {
            return false;
        }
        out.assign(whole.begin() + static_cast<std::ptrdiff_t>(offset),
                   whole.begin() + static_cast<std::ptrdiff_t>(offset + size));
        return true;
    }
    virtual std::string description() const = 0;
};

class FolderSource final : public DataSource {
public:
    explicit FolderSource(fs::path root) : _root(std::move(root)) {}

    bool readFile(const std::string &virtualPath, std::vector<uint8_t> &out) override {
        const fs::path path = resolveInsensitivePath(_root, virtualPath);
        if (!fs::is_regular_file(path)) {
            return false;
        }
        out = readWholeFile(path);
        return true;
    }

    bool readFileRange(const std::string &virtualPath, uint64_t offset, uint64_t size,
                       std::vector<uint8_t> &out) override {
        const fs::path path = resolveInsensitivePath(_root, virtualPath);
        if (!fs::is_regular_file(path)) {
            return false;
        }
        const uint64_t fileSize = fs::file_size(path);
        if (offset > fileSize || size > fileSize - offset) {
            return false;
        }
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return false;
        }
        out.resize(static_cast<size_t>(size));
        file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (size != 0) {
            file.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
        }
        return static_cast<uint64_t>(file.gcount()) == size || size == 0;
    }

    std::string description() const override {
        return _root.string();
    }

private:
    fs::path _root;
};

struct ResourceEntry {
    uint32_t disks = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
    uint16_t gjd = 0;
    std::string name;
};

class Catalog {
public:
    explicit Catalog(std::unique_ptr<DataSource> source) : _source(std::move(source)) {}

    void load() {
        std::vector<uint8_t> gjdData;
        std::vector<uint8_t> dirData;

        struct Layout {
            const char *grooviePrefix;
            const char *mediaPrefix;
        };
        const Layout layouts[] = {
            {"GROOVIE/", "MEDIA/"},
            {"", "../MEDIA/"},
            {"", "MEDIA/"},
            {"", ""},
        };

        bool found = false;
        for (const Layout &layout : layouts) {
            if (_source->readFile(std::string(layout.grooviePrefix) + "GJD.GJD", gjdData) &&
                _source->readFile(std::string(layout.grooviePrefix) + "DIR.RL", dirData)) {
                _mediaPrefix = layout.mediaPrefix;
                found = true;
                break;
            }
        }
        if (!found) {
            throw std::runtime_error("Could not find GJD.GJD and DIR.RL in " + _source->description());
        }

        parseGjdIndex(gjdData);
        parseDir(dirData);
        if (_gjds.empty() || _entries.empty()) {
            throw std::runtime_error("Groovie-2 catalog is empty");
        }
    }

    const std::vector<ResourceEntry> &entries() const {
        return _entries;
    }

    std::optional<ResourceEntry> find(std::string_view name) const {
        for (const ResourceEntry &entry : _entries) {
            if (iequals(entry.name, name)) {
                return entry;
            }
        }
        return std::nullopt;
    }

    bool readResource(const ResourceEntry &entry, std::vector<uint8_t> &out) {
        if (entry.gjd >= _gjds.size() || _gjds[entry.gjd].empty()) {
            return false;
        }
        const std::string mediaPath = _mediaPrefix + _gjds[entry.gjd];
        return _source->readFileRange(mediaPath, entry.offset, entry.size, out);
    }

    std::string sourceDescription() const {
        return _source->description();
    }

private:
    void parseGjdIndex(const std::vector<uint8_t> &data) {
        std::string text(reinterpret_cast<const char *>(data.data()), data.size());
        std::stringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            std::stringstream ls(line);
            std::string name;
            int index = -1;
            ls >> name >> index;
            if (name.empty() || index < 0) {
                continue;
            }
            if (static_cast<size_t>(index) >= _gjds.size()) {
                _gjds.resize(static_cast<size_t>(index) + 1);
            }
            _gjds[static_cast<size_t>(index)] = name;
        }
    }

    void parseDir(const std::vector<uint8_t> &data) {
        for (size_t pos = 0; pos + 32 <= data.size(); pos += 32) {
            ResourceEntry entry;
            entry.disks = readU32LE(data.data() + pos);
            entry.offset = readU32LE(data.data() + pos + 4);
            entry.size = readU32LE(data.data() + pos + 8);
            entry.gjd = readU16LE(data.data() + pos + 12);
            entry.name = trimNullTerminated(data.data() + pos + 14, 18);
            if (!entry.name.empty()) {
                _entries.push_back(entry);
            }
        }
    }

    std::unique_ptr<DataSource> _source;
    std::vector<std::string> _gjds;
    std::vector<ResourceEntry> _entries;
    std::string _mediaPrefix = "MEDIA/";
};

class Reader {
public:
    explicit Reader(const std::vector<uint8_t> &data) : _data(data) {}

    bool eof() const {
        return _pos >= _data.size();
    }

    size_t pos() const {
        return _pos;
    }

    size_t size() const {
        return _data.size();
    }

    void seek(size_t pos) {
        if (pos > _data.size()) {
            throw std::runtime_error("ROQ seek past end");
        }
        _pos = pos;
    }

    void skip(size_t bytes) {
        seek(std::min(_data.size(), _pos + bytes));
    }

    uint8_t readU8() {
        require(1);
        return _data[_pos++];
    }

    uint16_t readU16() {
        require(2);
        uint16_t value = readU16LE(_data.data() + _pos);
        _pos += 2;
        return value;
    }

    uint32_t readU32() {
        require(4);
        uint32_t value = readU32LE(_data.data() + _pos);
        _pos += 4;
        return value;
    }

    std::vector<uint8_t> readBytes(size_t size) {
        require(size);
        std::vector<uint8_t> out(_data.begin() + static_cast<std::ptrdiff_t>(_pos),
                                 _data.begin() + static_cast<std::ptrdiff_t>(_pos + size));
        _pos += size;
        return out;
    }

    BlockHeader readHeader() {
        BlockHeader header;
        header.type = readU16();
        header.size = readU32();
        header.argument = readU16();
        return header;
    }

private:
    void require(size_t bytes) const {
        if (bytes > _data.size() - _pos) {
            throw std::runtime_error("Unexpected end of ROQ stream");
        }
    }

    const std::vector<uint8_t> &_data;
    size_t _pos = 0;
};

class RoqDecoder {
public:
    explicit RoqDecoder(DecodeOptions options) : _options(std::move(options)) {
        setupYCbCrTables();
    }

    bool decode(const std::vector<uint8_t> &data, const std::string &sequenceName) {
        _sequenceName = sequenceName;
        _leafName = fs::path(sequenceName).filename().string();
        _result = {};
        _result.sequenceName = _sequenceName;
        _result.leafName = _leafName;
        _altMotionDecoder = chooseAltDecoder(_leafName);
        _outDir = _options.outRoot / sanitizeFolderName(_leafName);
        if (_options.writeFiles) {
            fs::create_directories(_outDir);
            cleanupGeneratedOutputs();
        }
        _rawConcatPath.clear();
        if (_options.raw && _options.writeFiles) {
            _rawConcatPath = _outDir / "frames.rgba.tmp";
        }

        Reader reader(data);
        const BlockHeader signature = reader.readHeader();
        if (signature.type != kRoQSignature) {
            throw std::runtime_error(sequenceName + " is not a ROQ/ROL/RNR stream");
        }
        if (signature.size == 0 && signature.argument == 0) {
            _fps = 30;
        } else {
            _fps = signature.argument == 0 ? 30 : signature.argument;
        }

        if (_options.log) {
            std::cout << sequenceName << "\n";
            std::cout << "  decoder: " << (_altMotionDecoder ? "alternate" : "regular") << "\n";
            std::cout << "  playback: " << _fps << " fps\n";
        }

        while (!reader.eof()) {
            if (reader.size() - reader.pos() < 8) {
                break;
            }
            const BlockHeader header = reader.readHeader();
            const size_t endPos = std::min(reader.size(), reader.pos() + static_cast<size_t>(header.size));
            bool alignToEnd = true;

            switch (header.type) {
            case kRoQQuad:
                reader.skip(header.size);
                break;
            case kRoQInfo:
                handleInfo(reader, header);
                break;
            case kRoQCodebook:
                handleCodebook(reader, header);
                break;
            case kRoQVQ:
                handleVQ(reader, header);
                break;
            case kRoQJpeg:
                handleJpeg(reader, header);
                alignToEnd = false;
                break;
            case kRoQHang:
                if (_options.log && (header.size != 0 || header.argument != 0)) {
                    std::cout << "  warning: malformed HANG chunk\n";
                }
                writeFrame();
                break;
            case kRoQSoundMono:
                handleSoundMono(reader, header);
                break;
            case kRoQSoundStereo:
                handleSoundStereo(reader, header);
                break;
            case kRoQPacket:
                alignToEnd = false;
                break;
            case kRoQSignature:
                throw std::runtime_error("Unexpected nested ROQ signature chunk");
            default:
                if (_options.log) {
                    std::cout << "  warning: skipping unknown chunk 0x" << std::hex << std::setw(4)
                              << std::setfill('0') << header.type << std::dec << std::setfill(' ') << "\n";
                }
                reader.skip(header.size);
                break;
            }

            if (alignToEnd && reader.pos() != endPos) {
                reader.seek(endPos);
            }
        }

        writeWav();
        const bool mp4Ok = _options.encodeMp4 ? encodeMp4() : true;
        if (_options.writeFiles) {
            writeManifest();
        }
        populateResult();
        if (_options.log) {
            std::cout << "  frames: " << _frameCount << "\n";
            if (_width > 0 && _height > 0) {
                std::cout << "  format: " << _width << "x" << _height << " RGBA8888";
                if (_interlaced) {
                    std::cout << " (interlaced source height)";
                }
                std::cout << "\n";
            }
            if (_audioChunks > 0) {
                std::cout << "  audio chunks: " << _audioChunks << "\n";
                if (_wavWritten) {
                    std::cout << "  wav: " << (_outDir / "audio.wav").string() << "\n";
                }
            }
            if (_mp4Written) {
                std::cout << "  mp4: " << _mp4Path.string() << "\n";
            }
        }
        return mp4Ok;
    }

    DecodedMovie takeResult() {
        populateResult();
        return std::move(_result);
    }

private:
    void populateResult() {
        _result.sequenceName = _sequenceName;
        _result.leafName = _leafName;
        _result.width = _width;
        _result.height = _height;
        _result.fps = _fps;
        _result.alpha = _alpha;
        _result.interlaced = _interlaced;
        _result.altMotionDecoder = _altMotionDecoder;
        _result.audioChunks = _audioChunks;
        _result.badCodebookRefs = _badCodebookRefs;
        _result.pcm = _pcm;
    }

    static std::string sanitizeFolderName(std::string name) {
        for (char &ch : name) {
            if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' ||
                ch == '>' || ch == '|' || static_cast<unsigned char>(ch) < 32) {
                ch = '_';
            }
        }
        if (name.empty()) {
            name = "roq";
        }
        return name;
    }

    static bool isGeneratedFrameName(const std::string &name) {
        const size_t dot = name.find('.');
        if (dot != 5 || name.size() <= dot + 1) {
            return false;
        }
        for (size_t i = 0; i < 5; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
                return false;
            }
        }
        const std::string ext = toLower(name.substr(dot));
        return ext == ".png" || ext == ".raw" || ext == ".jpg" || ext == ".jpeg";
    }

    void cleanupGeneratedOutputs() {
        if (!fs::is_directory(_outDir)) {
            return;
        }
        const std::string mp4Name = _leafName + ".mp4";
        for (const fs::directory_entry &entry : fs::directory_iterator(_outDir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::string name = entry.path().filename().string();
            if (isGeneratedFrameName(name) || name == "audio.wav" || name == "frames.txt" ||
                name == "frames.rgba.tmp" || name == mp4Name) {
                fs::remove(entry.path());
            }
        }
    }

    bool chooseAltDecoder(const std::string &name) const {
        if (_options.forceAlt) {
            return true;
        }
        if (_options.forceNorm) {
            return false;
        }
        const std::string lower = toLower(name);
        return lower.rfind("act", 0) == 0 || lower.rfind("door", 0) == 0 || lower == "trailer.rnr";
    }

    void setupYCbCrTables() {
        constexpr double yCbBScale = 57.204;
        constexpr double yCrRScale = 45.364;
        constexpr double yCbGScale = -11.51248;
        constexpr double yCrGScale = -23.35248;
        constexpr double shift = 32.0;
        for (int i = 0; i < 256; ++i) {
            const int x = 2 * i - 255;
            _yCbB[i] = static_cast<int>(yCbBScale * x + shift);
            _yCrR[i] = static_cast<int>(yCrRScale * x + shift);
            _yCbG[i] = static_cast<int>(yCbGScale * x);
            _yCrG[i] = static_cast<int>(yCrGScale * x + shift);
            _yTable[i] = (i << 6) | (i >> 2);
        }
    }

    static uint8_t clampByte(int value) {
        return static_cast<uint8_t>(std::clamp(value, 0, 255));
    }

    Pixel yCbCrToRgb(uint8_t y, uint8_t a, uint8_t cb, uint8_t cr) const {
        const int yy = _yTable[y];
        Pixel color;
        color.r = clampByte((yy + _yCrR[cr]) / 64);
        color.g = clampByte((yy + _yCbG[cb] + _yCrG[cr]) / 64);
        color.b = clampByte((yy + _yCbB[cb]) / 64);
        color.a = a;
        return color;
    }

    void allocateFrame(int width, int height) {
        _width = width;
        _height = height;
        const Pixel black{0, 0, 0, 255};
        _cur.assign(static_cast<size_t>(width) * height, black);
        _prev.assign(static_cast<size_t>(width) * height, black);
        _lastFrame.assign(static_cast<size_t>(width) * height, black);
    }

    void handleInfo(Reader &reader, const BlockHeader &header) {
        if (header.size != 8 || (header.argument != 0 && header.argument != 1)) {
            throw std::runtime_error("Invalid ROQ info chunk");
        }
        _infoSeen = true;
        _firstFrame = true;
        _alpha = header.argument;
        const uint16_t width = reader.readU16();
        const uint16_t height = reader.readU16();
        const uint16_t unknown1 = reader.readU16();
        const uint16_t unknown2 = reader.readU16();
        if (_options.log && (unknown1 != 8 || unknown2 != 4)) {
            std::cout << "  warning: info unknown fields are " << unknown1 << ", " << unknown2 << "\n";
        }
        _interlaced = height <= width / 3;
        if (_cur.empty() || width != _width || height != _height) {
            allocateFrame(width, height);
        }
        if (_options.log) {
            std::cout << "  info: " << _width << "x" << _height << ", alpha " << _alpha
                      << ", interlaced " << (_interlaced ? "yes" : "no") << "\n";
        }
    }

    void handleCodebook(Reader &reader, const BlockHeader &header) {
        int newNum2 = header.argument >> 8;
        if (newNum2 == 0) {
            newNum2 = 256;
        }
        if (newNum2 > _num2Tiles) {
            _num2Tiles = newNum2;
            _codebook2.resize(static_cast<size_t>(_num2Tiles) * 4);
        }

        int newNum4 = header.argument & 0xff;
        if (newNum4 == 0 && header.size > static_cast<uint32_t>(newNum2 * (6 + _alpha * 4))) {
            newNum4 = 256;
        }
        if (newNum4 > _num4Tiles) {
            _num4Tiles = newNum4;
            _codebook4.resize(static_cast<size_t>(_num4Tiles) * 4);
        }

        for (int i = 0; i < newNum2; ++i) {
            std::array<uint8_t, 4> y{};
            std::array<uint8_t, 4> a{};
            for (int j = 0; j < 4; ++j) {
                y[j] = reader.readU8();
                a[j] = _alpha ? reader.readU8() : 255;
            }
            const uint8_t cb = reader.readU8();
            const uint8_t cr = reader.readU8();
            for (int j = 0; j < 4; ++j) {
                _codebook2[static_cast<size_t>(i) * 4 + j] = yCbCrToRgb(y[j], a[j], cb, cr);
            }
        }

        for (int i = 0; i < newNum4 * 4; ++i) {
            _codebook4[static_cast<size_t>(i)] = reader.readU8();
        }
    }

    uint16_t getCodingType(Reader &reader) {
        if (_codingTypeCount == 0) {
            _codingType = reader.readU16();
            _codingTypeCount = 8;
        } else {
            _codingType <<= 2;
        }
        --_codingTypeCount;
        return _codingType >> 14;
    }

    bool inBounds(int x, int y) const {
        return x >= 0 && y >= 0 && x < _width && y < _height;
    }

    Pixel &curPixel(int x, int y) {
        return _cur[static_cast<size_t>(y) * _width + x];
    }

    const Pixel &prevPixel(int x, int y) const {
        return _prev[static_cast<size_t>(y) * _width + x];
    }

    void copyBlock(int size, int destX, int destY, int deltaX, int deltaY) {
        int offX = deltaX - _motionOffX;
        int offY = deltaY - _motionOffY;
        if (_interlaced) {
            offX *= 2;
        }
        if (_altMotionDecoder) {
            offX *= 2;
            offY *= 2;
        }

        const int srcX = destX + offX;
        const int srcY = destY + offY;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const int dx = destX + x;
                const int dy = destY + y;
                const int sx = srcX + x;
                const int sy = srcY + y;
                if (inBounds(dx, dy) && inBounds(sx, sy)) {
                    curPixel(dx, dy) = prevPixel(sx, sy);
                }
            }
        }
    }

    void paint2(uint8_t index, int destX, int destY) {
        if (index >= _num2Tiles) {
            ++_badCodebookRefs;
            return;
        }
        size_t codeIndex = static_cast<size_t>(index) * 4;
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 2; ++x) {
                if (inBounds(destX + x, destY + y)) {
                    curPixel(destX + x, destY + y) = _codebook2[codeIndex];
                }
                ++codeIndex;
            }
        }
    }

    void paint4(uint8_t index, int destX, int destY) {
        if (index >= _num4Tiles) {
            ++_badCodebookRefs;
            return;
        }
        size_t codeIndex = static_cast<size_t>(index) * 4;
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 2; ++x) {
                paint2(_codebook4[codeIndex++], destX + x * 2, destY + y * 2);
            }
        }
    }

    void paint8(uint8_t index, int destX, int destY) {
        if (index >= _num4Tiles) {
            ++_badCodebookRefs;
            return;
        }
        size_t code4 = static_cast<size_t>(index) * 4;
        for (int y4 = 0; y4 < 2; ++y4) {
            for (int x4 = 0; x4 < 2; ++x4) {
                size_t code2 = static_cast<size_t>(_codebook4[code4++]) * 4;
                for (int y2 = 0; y2 < 2; ++y2) {
                    for (int x2 = 0; x2 < 2; ++x2) {
                        const Pixel color = _codebook2[code2++];
                        const int x = destX + x4 * 4 + x2 * 2;
                        const int y = destY + y4 * 4 + y2 * 2;
                        if (inBounds(x, y)) {
                            curPixel(x, y) = color;
                        }
                        if (inBounds(x + 1, y)) {
                            curPixel(x + 1, y) = color;
                        }
                        if (inBounds(x, y + 1)) {
                            curPixel(x, y + 1) = color;
                        }
                        if (inBounds(x + 1, y + 1)) {
                            curPixel(x + 1, y + 1) = color;
                        }
                    }
                }
            }
        }
    }

    void processTile4(Reader &reader, int baseX, int baseY) {
        const uint16_t codingType = getCodingType(reader);
        switch (codingType) {
        case 0:
            break;
        case 1: {
            const uint8_t argument = reader.readU8();
            copyBlock(4, baseX, baseY, 8 - (argument >> 4), 8 - (argument & 0x0f));
            break;
        }
        case 2:
            paint4(reader.readU8(), baseX, baseY);
            break;
        case 3:
            paint2(reader.readU8(), baseX + 0, baseY + 0);
            paint2(reader.readU8(), baseX + 2, baseY + 0);
            paint2(reader.readU8(), baseX + 0, baseY + 2);
            paint2(reader.readU8(), baseX + 2, baseY + 2);
            break;
        default:
            break;
        }
    }

    void processTile8(Reader &reader, int baseX, int baseY) {
        const uint16_t codingType = getCodingType(reader);
        switch (codingType) {
        case 0:
            break;
        case 1: {
            const uint8_t argument = reader.readU8();
            copyBlock(8, baseX, baseY, 8 - (argument >> 4), 8 - (argument & 0x0f));
            break;
        }
        case 2:
            paint8(reader.readU8(), baseX, baseY);
            break;
        case 3:
            processTile4(reader, baseX + 0, baseY + 0);
            processTile4(reader, baseX + 4, baseY + 0);
            processTile4(reader, baseX + 0, baseY + 4);
            processTile4(reader, baseX + 4, baseY + 4);
            break;
        default:
            break;
        }
    }

    void handleVQ(Reader &reader, const BlockHeader &header) {
        if (!_infoSeen) {
            throw std::runtime_error("ROQ vector frame appeared before info chunk");
        }
        _motionOffX = static_cast<int8_t>(header.argument >> 8);
        _motionOffY = static_cast<int8_t>(header.argument & 0xff);
        _codingTypeCount = 0;

        for (int y = 0; y < _height; y += 16) {
            for (int x = 0; x < _width; x += 16) {
                processTile8(reader, x + 0, y + 0);
                processTile8(reader, x + 8, y + 0);
                processTile8(reader, x + 0, y + 8);
                processTile8(reader, x + 8, y + 8);
            }
        }
        finalizeFrame();
        writeFrame();
    }

    void appendSampleLE(uint16_t sample) {
        appendU16LE(_pcm, sample);
    }

    void handleSoundMono(Reader &reader, const BlockHeader &header) {
        uint16_t sample = header.argument ^ 0x8000u;
        for (uint32_t i = 0; i < header.size; ++i) {
            uint16_t in = reader.readU8();
            if ((in & 0x80u) != 0) {
                in &= 0x7fu;
                sample = static_cast<uint16_t>(sample - in * in);
            } else {
                sample = static_cast<uint16_t>(sample + in * in);
            }
            appendSampleLE(sample);
            appendSampleLE(sample);
        }
        ++_audioChunks;
    }

    void handleSoundStereo(Reader &reader, const BlockHeader &header) {
        uint16_t leftSample = (header.argument & 0xff00u) ^ 0x8000u;
        uint16_t rightSample = static_cast<uint16_t>((header.argument & 0x00ffu) << 8) ^ 0x8000u;
        bool left = true;
        for (uint32_t i = 0; i < header.size; ++i) {
            uint16_t in = reader.readU8();
            uint16_t &sample = left ? leftSample : rightSample;
            if ((in & 0x80u) != 0) {
                in &= 0x7fu;
                sample = static_cast<uint16_t>(sample - in * in);
            } else {
                sample = static_cast<uint16_t>(sample + in * in);
            }
            appendSampleLE(sample);
            left = !left;
        }
        ++_audioChunks;
    }

#ifdef _WIN32
    template <typename T>
    static void releaseCom(T *&ptr) {
        if (ptr) {
            ptr->Release();
            ptr = nullptr;
        }
    }

    bool decodeJpegWic(const std::vector<uint8_t> &jpeg, std::vector<Pixel> &pixels, int &width, int &height,
                       std::string &error) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool shouldUninit = SUCCEEDED(hr);
        if (hr == RPC_E_CHANGED_MODE) {
            hr = S_OK;
        }
        if (FAILED(hr)) {
            error = "CoInitializeEx failed";
            return false;
        }

        IWICImagingFactory *factory = nullptr;
        IWICStream *stream = nullptr;
        IWICBitmapDecoder *decoder = nullptr;
        IWICBitmapFrameDecode *frame = nullptr;
        IWICFormatConverter *converter = nullptr;

        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) {
            hr = factory->CreateStream(&stream);
        }
        if (SUCCEEDED(hr)) {
            hr = stream->InitializeFromMemory(const_cast<BYTE *>(reinterpret_cast<const BYTE *>(jpeg.data())),
                                              static_cast<DWORD>(jpeg.size()));
        }
        if (SUCCEEDED(hr)) {
            hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
        }
        if (SUCCEEDED(hr)) {
            hr = decoder->GetFrame(0, &frame);
        }
        UINT w = 0;
        UINT h = 0;
        if (SUCCEEDED(hr)) {
            hr = frame->GetSize(&w, &h);
        }
        if (SUCCEEDED(hr)) {
            hr = factory->CreateFormatConverter(&converter);
        }
        if (SUCCEEDED(hr)) {
            hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom);
        }
        std::vector<uint8_t> rgba;
        if (SUCCEEDED(hr)) {
            rgba.resize(static_cast<size_t>(w) * h * 4);
            hr = converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(rgba.size()), rgba.data());
        }
        if (SUCCEEDED(hr)) {
            width = static_cast<int>(w);
            height = static_cast<int>(h);
            pixels.resize(static_cast<size_t>(width) * height);
            for (size_t i = 0; i < pixels.size(); ++i) {
                pixels[i] = Pixel{rgba[i * 4 + 0], rgba[i * 4 + 1], rgba[i * 4 + 2], rgba[i * 4 + 3]};
            }
        } else {
            std::ostringstream ss;
            ss << "WIC JPEG decode failed: 0x" << std::hex << static_cast<unsigned long>(hr);
            error = ss.str();
        }

        releaseCom(converter);
        releaseCom(frame);
        releaseCom(decoder);
        releaseCom(stream);
        releaseCom(factory);
        if (shouldUninit) {
            CoUninitialize();
        }
        return SUCCEEDED(hr);
    }
#else
    struct JpegErrorManager {
        jpeg_error_mgr pub{};
        jmp_buf jump{};
        char message[JMSG_LENGTH_MAX]{};
    };

    static void jpegErrorExit(j_common_ptr cinfo) {
        auto *err = reinterpret_cast<JpegErrorManager *>(cinfo->err);
        (*cinfo->err->format_message)(cinfo, err->message);
        longjmp(err->jump, 1);
    }

    bool decodeJpegLibjpeg(const std::vector<uint8_t> &jpeg, std::vector<Pixel> &pixels, int &width,
                           int &height, std::string &error) {
        if (jpeg.empty()) {
            error = "empty JPEG chunk";
            return false;
        }

        jpeg_decompress_struct cinfo{};
        JpegErrorManager jerr{};
        cinfo.err = jpeg_std_error(&jerr.pub);
        jerr.pub.error_exit = jpegErrorExit;

        if (setjmp(jerr.jump) != 0) {
            error = jerr.message[0] ? jerr.message : "libjpeg decode failed";
            jpeg_destroy_decompress(&cinfo);
            return false;
        }

        jpeg_create_decompress(&cinfo);
        jpeg_mem_src(&cinfo, const_cast<unsigned char *>(jpeg.data()), static_cast<unsigned long>(jpeg.size()));
        jpeg_read_header(&cinfo, TRUE);
        cinfo.out_color_space = JCS_RGB;
        jpeg_start_decompress(&cinfo);

        width = static_cast<int>(cinfo.output_width);
        height = static_cast<int>(cinfo.output_height);
        const int components = static_cast<int>(cinfo.output_components);
        if (width <= 0 || height <= 0 || components < 3) {
            error = "libjpeg produced an invalid RGB frame";
            jpeg_destroy_decompress(&cinfo);
            return false;
        }

        pixels.resize(static_cast<size_t>(width) * height);
        std::vector<uint8_t> row(static_cast<size_t>(width) * components);
        while (cinfo.output_scanline < cinfo.output_height) {
            const size_t y = cinfo.output_scanline;
            JSAMPROW rowPtr = row.data();
            jpeg_read_scanlines(&cinfo, &rowPtr, 1);
            for (int x = 0; x < width; ++x) {
                const size_t src = static_cast<size_t>(x) * components;
                pixels[y * static_cast<size_t>(width) + x] = Pixel{row[src], row[src + 1], row[src + 2], 255};
            }
        }

        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return true;
    }
#endif

    void handleJpeg(Reader &reader, const BlockHeader &header) {
        const std::vector<uint8_t> jpeg = reader.readBytes(header.size);
        std::vector<Pixel> pixels;
        int jpegWidth = 0;
        int jpegHeight = 0;
        std::string error;
#ifdef _WIN32
        if (!decodeJpegWic(jpeg, pixels, jpegWidth, jpegHeight, error)) {
            throw std::runtime_error(error);
        }
#else
        if (!decodeJpegLibjpeg(jpeg, pixels, jpegWidth, jpegHeight, error)) {
            throw std::runtime_error(error);
        }
#endif
        if (_cur.empty() || jpegWidth != _width || jpegHeight != _height) {
            allocateFrame(jpegWidth, jpegHeight);
        }
        _cur = std::move(pixels);
        for (Pixel &pixel : _cur) {
            pixel.a = 255;
        }
        finalizeFrame();
        writeFrame();
    }

    void finalizeFrame() {
        if (_firstFrame) {
            _prev = _cur;
            _firstFrame = false;
        }
        _lastFrame = _cur;
        std::swap(_cur, _prev);
    }

    void writeFrame() {
        if (_lastFrame.empty()) {
            return;
        }
        if (_options.collectFrames) {
            _result.frames.push_back(_lastFrame);
        }
        if (!_options.writeFiles) {
            ++_frameCount;
            return;
        }
        fs::create_directories(_outDir);
        std::ostringstream name;
        name << std::setw(5) << std::setfill('0') << _frameCount << (_options.raw ? ".raw" : ".png");
        const fs::path path = _outDir / name.str();
        if (_options.raw) {
            std::vector<uint8_t> raw = makeRawRGBA(_width, _height, _lastFrame);
            writeWholeFile(path, raw);
            std::ofstream concat(_rawConcatPath, std::ios::binary | std::ios::app);
            if (!concat) {
                throw std::runtime_error("Could not append raw video stream");
            }
            concat.write(reinterpret_cast<const char *>(raw.data()), static_cast<std::streamsize>(raw.size()));
        } else {
            writePngRGBA(path, _width, _height, _lastFrame);
        }
        ++_frameCount;
    }

    void writeWav() {
        if (_pcm.empty()) {
            return;
        }
        if (!_options.writeFiles) {
            return;
        }
        _wavPath = _outDir / "audio.wav";
        writeWavPCM16Stereo(_wavPath, _pcm);
        _wavWritten = true;
    }

    static std::string quoteArg(const fs::path &path) {
        std::string text = path.string();
        std::string quoted = "\"";
        for (char ch : text) {
            if (ch == '"') {
                quoted += "\\\"";
            } else {
                quoted += ch;
            }
        }
        quoted += "\"";
        return quoted;
    }

    static int runCommand(const std::string &command) {
#ifdef _WIN32
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        std::string mutableCommand = command;
        if (!CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup,
                            &process)) {
            return -1;
        }

        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return static_cast<int>(exitCode);
#else
        return std::system(command.c_str());
#endif
    }

    static std::optional<fs::path> findFfmpegOnPath() {
        const char *pathEnv = std::getenv("PATH");
        if (!pathEnv || *pathEnv == 0) {
            return std::nullopt;
        }
#ifdef _WIN32
        constexpr char separator = ';';
        const std::vector<std::string> names = {"ffmpeg.exe", "ffmpeg.cmd", "ffmpeg.bat", "ffmpeg"};
#else
        constexpr char separator = ':';
        const std::vector<std::string> names = {"ffmpeg"};
#endif
        std::stringstream ss(pathEnv);
        std::string dir;
        while (std::getline(ss, dir, separator)) {
            fs::path base = dir.empty() ? fs::path(".") : fs::path(dir);
            for (const std::string &name : names) {
                fs::path candidate = base / name;
                if (fs::is_regular_file(candidate)) {
                    return candidate;
                }
            }
        }
        return std::nullopt;
    }

    bool encodeMp4() {
        if (_frameCount == 0) {
            return true;
        }
        if (_alpha != 0) {
            std::cerr << "lossless MP4 skipped: alpha channel present and MP4 output would lose data\n";
            cleanupRawConcat();
            return false;
        }

        const std::optional<fs::path> ffmpeg = findFfmpegOnPath();
        if (!ffmpeg) {
            std::cerr << "ffmpeg not found on %PATH%\n";
            cleanupRawConcat();
            return false;
        }

        _mp4Path = _outDir / (_leafName + ".mp4");
        std::ostringstream cmd;
        cmd << quoteArg(*ffmpeg) << " -y -hide_banner -loglevel error ";
        if (_options.raw) {
            cmd << "-f rawvideo -pixel_format rgba -video_size " << _width << "x" << _height
                << " -framerate " << _fps << " -i " << quoteArg(_rawConcatPath) << " ";
        } else {
            cmd << "-framerate " << _fps << " -start_number 0 -i " << quoteArg(_outDir / "%05d.png") << " ";
        }
        if (_wavWritten) {
            cmd << "-i " << quoteArg(_wavPath) << " ";
        }
        cmd << "-c:v libx264rgb -crf 0 -preset veryslow -pix_fmt rgb24 ";
        if (_wavWritten) {
            cmd << "-c:a alac ";
        }
        cmd << quoteArg(_mp4Path);

        const int rc = runCommand(cmd.str());
        cleanupRawConcat();
        if (rc != 0) {
            std::cerr << "ffmpeg failed while writing " << _mp4Path.string() << "\n";
            return false;
        }
        _mp4Written = true;
        return true;
    }

    void cleanupRawConcat() {
        if (!_rawConcatPath.empty()) {
            fs::remove(_rawConcatPath);
        }
    }

    void writeManifest() {
        fs::create_directories(_outDir);
        std::ofstream info(_outDir / "frames.txt", std::ios::binary);
        info << "source: " << _sequenceName << "\n";
        info << "frames: " << _frameCount << "\n";
        info << "width: " << _width << "\n";
        info << "height: " << _height << "\n";
        info << "fps: " << _fps << "\n";
        info << "alpha: " << _alpha << "\n";
        info << "interlaced: " << (_interlaced ? "yes" : "no") << "\n";
        info << "motion_decoder: " << (_altMotionDecoder ? "alternate" : "regular") << "\n";
        info << "frame_format: RGBA8888, row-major, top-to-bottom";
        if (!_options.raw) {
            info << " before PNG encoding";
        }
        info << "\n";
        if (_badCodebookRefs > 0) {
            info << "warnings: " << _badCodebookRefs << " invalid codebook reference(s)\n";
        }
        if (_wavWritten) {
            info << "audio: audio.wav\n";
            info << "audio_format: PCM signed 16-bit little-endian, stereo, 22050 Hz\n";
        }
        if (_mp4Written) {
            info << "mp4: " << _mp4Path.filename().string() << "\n";
            info << "mp4_video: libx264rgb lossless RGB, " << _fps << " fps\n";
            if (_wavWritten) {
                info << "mp4_audio: ALAC lossless\n";
            }
        }
    }

    DecodeOptions _options;
    DecodedMovie _result;
    std::string _sequenceName;
    std::string _leafName;
    fs::path _outDir;

    bool _infoSeen = false;
    bool _firstFrame = true;
    bool _altMotionDecoder = false;
    uint16_t _alpha = 0;
    int _width = 0;
    int _height = 0;
    int _fps = 0;
    bool _interlaced = false;

    std::vector<Pixel> _cur;
    std::vector<Pixel> _prev;
    std::vector<Pixel> _lastFrame;
    std::vector<Pixel> _codebook2;
    std::vector<uint8_t> _codebook4;
    std::vector<uint8_t> _pcm;
    int _num2Tiles = 0;
    int _num4Tiles = 0;
    int _motionOffX = 0;
    int _motionOffY = 0;
    uint16_t _codingType = 0;
    int _codingTypeCount = 0;

    std::array<int, 256> _yCrR{};
    std::array<int, 256> _yCrG{};
    std::array<int, 256> _yCbG{};
    std::array<int, 256> _yCbB{};
    std::array<int, 256> _yTable{};

    uint32_t _frameCount = 0;
    uint32_t _audioChunks = 0;
    uint32_t _badCodebookRefs = 0;
    bool _wavWritten = false;
    bool _mp4Written = false;
    fs::path _wavPath;
    fs::path _mp4Path;
    fs::path _rawConcatPath;
};

void printUsage() {
    std::cout << "Usage: groovie2 [options] [data-root] <sequence ...>\n"
              << "       groovie2 [options] <file.roq|file.rnr|file.rol>\n\n"
              << "Inputs:\n"
              << "  direct .roq/.rnr/.rol file, or resource names resolved through GJD.GJD/DIR.RL\n"
              << "  data root defaults to the current directory and may be the game root or GROOVIE dir\n\n"
              << "Options:\n"
              << "  -root <dir>    directory containing GROOVIE/MEDIA, or the GROOVIE directory itself\n"
              << "  -list          list ROQ/RNR resources in the catalog\n"
              << "  -all           decode every ROQ/RNR resource available from this data root\n"
              << "  -raw           write 00000.raw RGBA8888 frames instead of PNG\n"
              << "  -out <dir>     output root directory (default: current directory)\n"
              << "  -alt           force alternate motion decoder\n"
              << "  -norm          force regular motion decoder\n\n"
              << "Audio chunks are written as audio.wav beside the frames. If ffmpeg is on PATH,\n"
              << "a lossless MP4 with libx264rgb video and ALAC audio is written there too.\n";
}

std::unique_ptr<DataSource> makeDataSource(const fs::path &root) {
    if (!fs::is_directory(root)) {
        throw std::runtime_error("Catalog root is not a directory: " + root.string());
    }
    return std::make_unique<FolderSource>(root);
}

bool isDirectRoqFile(const fs::path &path) {
    if (!fs::is_regular_file(path)) {
        return false;
    }
    const std::string ext = path.extension().string();
    return iendsWith(ext, ".roq") || iendsWith(ext, ".rnr") || iendsWith(ext, ".rol");
}

bool isVisualResourceName(const std::string &name) {
    return iendsWith(name, ".roq") || iendsWith(name, ".rnr");
}

std::string formatCatalogListEntry(const ResourceEntry &entry) {
    std::ostringstream ss;
    ss << entry.name << "  size=" << entry.size << "  gjd=" << entry.gjd << "  disks=0x" << std::hex
       << entry.disks << std::dec;
    return ss.str();
}

#ifdef _WIN32

std::string sanitizeTempName(std::string name) {
    for (char &ch : name) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' ||
            ch == '>' || ch == '|' || static_cast<unsigned char>(ch) < 32) {
            ch = '_';
        }
    }
    if (name.empty()) {
        name = "roq";
    }
    return name;
}

int runExternalCommandCaptured(const std::string &command, const std::function<void(const std::string &)> &log) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
        return -1;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;

    PROCESS_INFORMATION process{};
    std::string mutableCommand = command;
    if (!CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return -1;
    }
    CloseHandle(writePipe);

    std::array<char, 4096> buffer{};
    DWORD bytesRead = 0;
    while (ReadFile(readPipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) && bytesRead > 0) {
        log(std::string(buffer.data(), buffer.data() + bytesRead));
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(readPipe);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
}

bool encodeDecodedMovieMp4(const DecodedMovie &movie, const fs::path &outPath, std::string &error,
                           const std::function<void(const std::string &)> &log) {
    if (movie.frames.empty()) {
        error = "No frames are loaded";
        return false;
    }
    if (movie.alpha != 0) {
        error = "lossless MP4 skipped: alpha channel present and MP4 output would lose data";
        log(error + "\n");
        return false;
    }

    const std::optional<fs::path> ffmpeg = findFfmpegOnPath();
    if (!ffmpeg) {
        error = "ffmpeg not found on %PATH%";
        log(error + "\n");
        return false;
    }

    const std::string tempStem = "groovie2_" + std::to_string(
#ifdef _WIN32
                                     static_cast<unsigned long>(GetCurrentProcessId())
#else
                                     static_cast<unsigned long>(std::rand())
#endif
                                 ) +
                                 "_" + sanitizeTempName(movie.leafName);
    const fs::path rawPath = fs::temp_directory_path() / (tempStem + ".rgba.tmp");
    const fs::path wavPath = fs::temp_directory_path() / (tempStem + ".wav.tmp");

    try {
        log("Preparing raw RGBA video stream...\n");
        std::ofstream raw(rawPath, std::ios::binary);
        if (!raw) {
            throw std::runtime_error("Could not create raw video temp file");
        }
        for (const std::vector<Pixel> &frame : movie.frames) {
            const std::vector<uint8_t> bytes = makeRawRGBA(movie.width, movie.height, frame);
            raw.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        raw.close();

        const bool hasAudio = !movie.pcm.empty();
        if (hasAudio) {
            log("Writing temporary WAV audio stream...\n");
            writeWholeFile(wavPath, makeWavPCM16Stereo(movie.pcm));
        }

        std::ostringstream cmd;
        cmd << quoteProcessArg(*ffmpeg) << " -y -hide_banner -stats -loglevel info "
            << "-f rawvideo -pixel_format rgba -video_size " << movie.width << "x" << movie.height
            << " -framerate " << (movie.fps > 0 ? movie.fps : 30) << " -i " << quoteProcessArg(rawPath) << " ";
        if (hasAudio) {
            cmd << "-i " << quoteProcessArg(wavPath) << " ";
        }
        cmd << "-c:v libx264rgb -crf 0 -preset veryslow -pix_fmt rgb24 ";
        if (hasAudio) {
            cmd << "-c:a alac ";
        }
        cmd << quoteProcessArg(outPath);

        log("Running ffmpeg:\n" + cmd.str() + "\n\n");
        const int rc = runExternalCommandCaptured(cmd.str(), log);
        fs::remove(rawPath);
        fs::remove(wavPath);
        if (rc != 0) {
            error = "ffmpeg failed while writing " + outPath.string();
            log("\n" + error + "\n");
            return false;
        }
        log("\nMP4 written: " + outPath.string() + "\n");
        return true;
    } catch (const std::exception &e) {
        fs::remove(rawPath);
        fs::remove(wavPath);
        error = e.what();
        log(std::string("\n") + error + "\n");
        return false;
    }
}

#endif

int run(int argc, char **argv) {
    if (argc < 2) {
        printUsage();
        return 2;
    }

    DecodeOptions options;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help" || arg == "/?") {
            printUsage();
            return 0;
        }
        if (arg == "-raw" || arg == "--raw") {
            options.raw = true;
        } else if (arg == "-list" || arg == "--list") {
            options.list = true;
        } else if (arg == "-all" || arg == "--all") {
            options.all = true;
        } else if (arg == "-alt" || arg == "--alt") {
            options.forceAlt = true;
        } else if (arg == "-norm" || arg == "--norm") {
            options.forceNorm = true;
        } else if (arg == "-out" || arg == "--out") {
            if (++i >= argc) {
                throw std::runtime_error("-out requires a directory");
            }
            options.outRoot = argv[i];
        } else if (arg == "-root" || arg == "--root") {
            if (++i >= argc) {
                throw std::runtime_error("-root requires a directory");
            }
            options.catalogRoot = argv[i];
            options.rootExplicit = true;
        } else if (!arg.empty() && arg.front() == '-') {
            throw std::runtime_error("Unknown option " + arg);
        } else {
            positional.push_back(arg);
        }
    }

    if (options.forceAlt && options.forceNorm) {
        throw std::runtime_error("-alt and -norm cannot both be used");
    }
    if (positional.empty() && !options.list && !options.all) {
        throw std::runtime_error("Missing input");
    }

    fs::path catalogRoot = options.catalogRoot;
    if (!positional.empty() && isDirectRoqFile(positional.front())) {
        const fs::path input = positional.front();
        positional.erase(positional.begin());

        if (options.list || options.all || !positional.empty()) {
            throw std::runtime_error("Direct ROQ input does not use -list, -all, or extra sequence names");
        }
        const std::vector<uint8_t> data = readWholeFile(input);
        RoqDecoder decoder(options);
        return decoder.decode(data, input.filename().string()) ? 0 : 1;
    }

    if (!options.rootExplicit && !positional.empty() && fs::is_directory(positional.front())) {
        catalogRoot = positional.front();
        positional.erase(positional.begin());
    }

    Catalog catalog(makeDataSource(catalogRoot));
    catalog.load();

    if (options.list) {
        for (const ResourceEntry &entry : catalog.entries()) {
            if (isVisualResourceName(entry.name)) {
                std::cout << formatCatalogListEntry(entry) << "\n";
            }
        }
        return 0;
    }

    std::vector<ResourceEntry> work;
    if (options.all) {
        for (const ResourceEntry &entry : catalog.entries()) {
            if (isVisualResourceName(entry.name)) {
                work.push_back(entry);
            }
        }
    } else {
        if (positional.empty()) {
            throw std::runtime_error("Give one or more sequence names, or use -list / -all");
        }
        for (const std::string &name : positional) {
            std::optional<ResourceEntry> entry = catalog.find(name);
            if (!entry) {
                throw std::runtime_error("Resource not found in catalog: " + name);
            }
            work.push_back(*entry);
        }
    }

    int failures = 0;
    for (const ResourceEntry &entry : work) {
        std::vector<uint8_t> data;
        if (!catalog.readResource(entry, data)) {
            std::cout << entry.name << "\n";
            std::cout << "  not present on " << catalog.sourceDescription() << "\n";
            ++failures;
            continue;
        }
        try {
            RoqDecoder decoder(options);
            if (!decoder.decode(data, entry.name)) {
                ++failures;
            }
        } catch (const std::exception &e) {
            std::cout << entry.name << "\n";
            std::cout << "  error: " << e.what() << "\n";
            ++failures;
        }
    }

    return failures == 0 ? 0 : 1;
}

#ifdef _WIN32

constexpr int kIdAssetList = 1001;
constexpr int kIdPlay = 1002;
constexpr int kIdFitMode = 1003;
constexpr int kIdEncodeMp4 = 1004;
constexpr int kIdFrameLabel = 1005;
constexpr int kIdStatus = 1006;
constexpr int kIdPrefsDisc1Edit = 1101;
constexpr int kIdPrefsDisc2Edit = 1102;
constexpr int kIdPrefsSave = 1103;
constexpr int kIdPrefsCancel = 1104;
constexpr int kIdLogEdit = 1201;
constexpr int kIdLogCopy = 1202;
constexpr int kIdLogClose = 1203;
constexpr int kIdMenuReload = 2001;
constexpr int kIdMenuPrefs = 2002;
constexpr int kIdMenuExit = 2003;
constexpr UINT_PTR kPlaybackTimer = 3001;
constexpr UINT kMsgMovieLoaded = WM_APP + 1;
constexpr UINT kMsgEncodeLog = WM_APP + 2;
constexpr UINT kMsgEncodeDone = WM_APP + 3;

struct MovieLoadResult {
    uint32_t serial = 0;
    bool ok = false;
    std::string entryName;
    std::string message;
    std::shared_ptr<DecodedMovie> movie;
};

struct EncodeDoneResult {
    bool ok = false;
    fs::path outPath;
    std::string message;
};

struct GuiState {
    HINSTANCE instance = nullptr;
    HWND hwnd = nullptr;
    HWND assetList = nullptr;
    HWND preview = nullptr;
    HWND frameLabel = nullptr;
    HWND status = nullptr;
    HWND btnPlay = nullptr;
    HWND btnFitMode = nullptr;
    HWND btnEncodeMp4 = nullptr;
    HWND preferences = nullptr;
    HWND logWindow = nullptr;
    HWND logEdit = nullptr;
    HFONT font = nullptr;
    AppConfig config;
    std::unique_ptr<Catalog> catalog;
    std::vector<ResourceEntry> visualEntries;
    std::vector<fs::path> catalogRoots;
    fs::path catalogRoot;
    std::shared_ptr<DecodedMovie> current;
    int currentFrame = 0;
    int previewBgraFrame = -1;
    std::vector<uint8_t> previewBgra;
    std::vector<uint8_t> playWav;
    bool fitToWindow = true;
    bool loading = false;
    bool encoding = false;
    bool playing = false;
    bool populatingList = false;
    uint64_t playbackStartTick = 0;
    uint64_t lastPlaybackLabelTick = 0;
    uint32_t decodeSerial = 0;
    int sortColumn = 1;
    bool sortAscending = false;
    std::string ffmpegLog;
};

GuiState *gGui = nullptr;

LRESULT CALLBACK LogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

std::wstring utf8ToWide(const std::string &text) {
    if (text.empty()) {
        return {};
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (len <= 0) {
        return std::wstring(text.begin(), text.end());
    }
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), len);
    return out;
}

std::string wideToUtf8(const std::wstring &text) {
    if (text.empty()) {
        return {};
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        return std::string(text.begin(), text.end());
    }
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

void setWindowText(HWND hwnd, const std::string &text) {
    SetWindowTextA(hwnd, text.c_str());
}

void setControlFont(HWND hwnd, HFONT font) {
    SendMessageA(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void setStatus(GuiState &state, const std::string &text) {
    if (state.status) {
        setWindowText(state.status, text);
        UpdateWindow(state.status);
    }
}

bool hasCurrentMovie(const GuiState &state) {
    return state.current && !state.current->frames.empty();
}

void setActionsEnabled(GuiState &state) {
    const bool hasFrame = hasCurrentMovie(state);
    EnableWindow(state.btnPlay, hasFrame && !state.loading);
    EnableWindow(state.btnFitMode, hasFrame);
    EnableWindow(state.btnEncodeMp4, hasFrame && !state.loading && !state.encoding);
    setWindowText(state.btnPlay, state.playing ? "Stop" : "Play");
    setWindowText(state.btnFitMode, state.fitToWindow ? "Fit" : "Original");
}

void updateFrameLabel(GuiState &state) {
    std::ostringstream ss;
    if (state.loading) {
        ss << "Loading ROQ/RNR...";
    } else if (!hasCurrentMovie(state)) {
        ss << "No ROQ selected";
    } else {
        ss << "Frame " << (state.currentFrame + 1) << " / " << state.current->frames.size() << "    "
           << state.current->width << "x" << state.current->height << "    " << state.current->fps << " fps";
        if (state.current->audioChunks > 0) {
            ss << "    audio chunks " << state.current->audioChunks;
        }
    }
    setWindowText(state.frameLabel, ss.str());
    setActionsEnabled(state);
}

void stopPlayback(GuiState &state, bool stopAudio = true) {
    if (state.playing) {
        KillTimer(state.hwnd, kPlaybackTimer);
        state.playing = false;
    }
    if (stopAudio) {
        PlaySoundA(nullptr, nullptr, SND_PURGE);
        state.playWav.clear();
    }
    updateFrameLabel(state);
}

void clearCurrentMovie(GuiState &state) {
    stopPlayback(state);
    state.current.reset();
    state.currentFrame = 0;
    state.previewBgraFrame = -1;
    state.previewBgra.clear();
    state.playWav.clear();
    updateFrameLabel(state);
    if (state.preview) {
        InvalidateRect(state.preview, nullptr, TRUE);
    }
}

std::vector<fs::path> defaultCatalogRoots(const GuiState &state) {
    std::vector<fs::path> roots;
    auto addRoot = [&](const fs::path &path) {
        if (path.empty()) {
            return;
        }
        for (const fs::path &existing : roots) {
            if (existing == path) {
                return;
            }
        }
        roots.push_back(path);
    };

    addRoot(state.config.clandDisc1Root);
    addRoot(state.config.clandDisc2Root);

    const fs::path cwd = fs::current_path();
    const fs::path bundledDisc1 = cwd / "Clandestiny" / "disc1";
    const fs::path bundledDisc2 = cwd / "Clandestiny" / "disc2";
    if (fs::is_directory(bundledDisc1)) {
        addRoot(bundledDisc1);
    }
    if (fs::is_directory(bundledDisc2)) {
        addRoot(bundledDisc2);
    }
    if (roots.empty()) {
        addRoot(cwd);
    }
    return roots;
}

std::string formatHex(uint32_t value) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::uppercase << value;
    return ss.str();
}

void sortVisualEntries(GuiState &state) {
    std::stable_sort(state.visualEntries.begin(), state.visualEntries.end(), [&](const ResourceEntry &a,
                                                                                 const ResourceEntry &b) {
        int cmp = 0;
        switch (state.sortColumn) {
        case 0:
            cmp = toLower(a.name).compare(toLower(b.name));
            break;
        case 1:
            cmp = (a.size < b.size) ? -1 : (a.size > b.size ? 1 : 0);
            break;
        case 2:
            cmp = (a.gjd < b.gjd) ? -1 : (a.gjd > b.gjd ? 1 : 0);
            break;
        case 3:
            cmp = (a.disks < b.disks) ? -1 : (a.disks > b.disks ? 1 : 0);
            break;
        case 4:
            cmp = (a.offset < b.offset) ? -1 : (a.offset > b.offset ? 1 : 0);
            break;
        default:
            cmp = 0;
            break;
        }
        if (cmp == 0) {
            cmp = toLower(a.name).compare(toLower(b.name));
        }
        return state.sortAscending ? cmp < 0 : cmp > 0;
    });
}

void addListColumn(HWND list, int index, const char *title, int width, int format = LVCFMT_LEFT) {
    LVCOLUMNA column{};
    column.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
    column.fmt = format;
    column.cx = width;
    column.pszText = const_cast<char *>(title);
    column.iSubItem = index;
    ListView_InsertColumn(list, index, &column);
}

void setupAssetListColumns(HWND list) {
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    addListColumn(list, 0, "Name", 140);
    addListColumn(list, 1, "Size", 90, LVCFMT_RIGHT);
    addListColumn(list, 2, "GJD", 52, LVCFMT_RIGHT);
    addListColumn(list, 3, "Disks", 64, LVCFMT_RIGHT);
    addListColumn(list, 4, "Offset", 92, LVCFMT_RIGHT);
}

void populateAssetList(GuiState &state, const std::string &selectName = {}) {
    state.populatingList = true;
    ListView_DeleteAllItems(state.assetList);
    for (size_t i = 0; i < state.visualEntries.size(); ++i) {
        const ResourceEntry &entry = state.visualEntries[i];
        std::string name = entry.name;
        LVITEMA item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.pszText = name.data();
        item.lParam = static_cast<LPARAM>(i);
        ListView_InsertItem(state.assetList, &item);

        std::string size = std::to_string(entry.size);
        std::string gjd = std::to_string(entry.gjd);
        std::string disks = formatHex(entry.disks);
        std::string offset = std::to_string(entry.offset);
        ListView_SetItemText(state.assetList, static_cast<int>(i), 1, size.data());
        ListView_SetItemText(state.assetList, static_cast<int>(i), 2, gjd.data());
        ListView_SetItemText(state.assetList, static_cast<int>(i), 3, disks.data());
        ListView_SetItemText(state.assetList, static_cast<int>(i), 4, offset.data());

        if (!selectName.empty() && iequals(entry.name, selectName)) {
            ListView_SetItemState(state.assetList, static_cast<int>(i), LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(state.assetList, static_cast<int>(i), FALSE);
        }
    }
    state.populatingList = false;
}

int selectedAssetIndex(const GuiState &state) {
    return ListView_GetNextItem(state.assetList, -1, LVNI_SELECTED);
}

bool readResourceFromRoots(const std::vector<fs::path> &roots, const ResourceEntry &entry, std::vector<uint8_t> &out,
                           std::string &sourceRoot, std::string &error) {
    std::vector<size_t> order;
    auto addIndex = [&](size_t index) {
        if (index >= roots.size()) {
            return;
        }
        if (std::find(order.begin(), order.end(), index) == order.end()) {
            order.push_back(index);
        }
    };

    if ((entry.disks & 0x1u) != 0) {
        addIndex(0);
    }
    if ((entry.disks & 0x2u) != 0) {
        addIndex(1);
    }
    for (size_t i = 0; i < roots.size(); ++i) {
        addIndex(i);
    }

    std::ostringstream failures;
    for (size_t index : order) {
        const fs::path &root = roots[index];
        try {
            Catalog catalog(makeDataSource(root));
            catalog.load();
            if (catalog.readResource(entry, out)) {
                sourceRoot = root.string();
                return true;
            }
            failures << root.string() << ": resource bytes not present; ";
        } catch (const std::exception &e) {
            failures << root.string() << ": " << e.what() << "; ";
        }
    }

    error = failures.str();
    if (error.empty()) {
        error = "no Clandestiny disc roots are configured";
    }
    return false;
}

void layoutMainWindow(GuiState &state, int width, int height) {
    const int margin = 8;
    const int statusHeight = 24;
    const int controlsHeight = 34;
    const int leftWidth = 430;
    const int gap = 8;
    const int rightX = margin + leftWidth + gap;
    const int rightWidth = std::max(1, width - rightX - margin);
    const int bodyHeight = std::max(1, height - statusHeight - margin * 3);
    const int buttonY = margin;
    const int encodeW = 122;
    const int fitW = 88;
    const int playW = 74;
    const int buttonGap = 8;
    const int buttonsW = playW + fitW + encodeW + buttonGap * 2;

    MoveWindow(state.assetList, margin, margin, leftWidth, bodyHeight, TRUE);
    MoveWindow(state.frameLabel, rightX, margin + 5, std::max(1, rightWidth - buttonsW - 12), 24, TRUE);
    int x = rightX + rightWidth - buttonsW;
    MoveWindow(state.btnPlay, x, buttonY, playW, 28, TRUE);
    x += playW + buttonGap;
    MoveWindow(state.btnFitMode, x, buttonY, fitW, 28, TRUE);
    x += fitW + buttonGap;
    MoveWindow(state.btnEncodeMp4, x, buttonY, encodeW, 28, TRUE);
    MoveWindow(state.preview, rightX, margin + controlsHeight, rightWidth,
               std::max(1, bodyHeight - controlsHeight), TRUE);
    MoveWindow(state.status, margin, height - statusHeight - margin, width - margin * 2, statusHeight, TRUE);
}

void paintPreview(HWND hwnd, GuiState &state) {
    PAINTSTRUCT ps{};
    HDC screenDc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int clientW = std::max(1, static_cast<int>(rc.right - rc.left));
    const int clientH = std::max(1, static_cast<int>(rc.bottom - rc.top));

    HDC dc = CreateCompatibleDC(screenDc);
    HBITMAP backBuffer = CreateCompatibleBitmap(screenDc, clientW, clientH);
    HGDIOBJ oldBitmap = SelectObject(dc, backBuffer);

    HBRUSH brush = CreateSolidBrush(RGB(32, 32, 32));
    FillRect(dc, &rc, brush);
    DeleteObject(brush);

    if (!hasCurrentMovie(state)) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(230, 230, 230));
        RECT textRc = rc;
        DrawTextA(dc, "Select a ROQ/RNR resource from the list", -1, &textRc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        BitBlt(screenDc, 0, 0, clientW, clientH, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBitmap);
        DeleteObject(backBuffer);
        DeleteDC(dc);
        EndPaint(hwnd, &ps);
        return;
    }

    const DecodedMovie &movie = *state.current;
    const std::vector<Pixel> &frame = movie.frames[static_cast<size_t>(state.currentFrame)];
    if (state.previewBgraFrame != state.currentFrame || state.previewBgra.size() != frame.size() * 4) {
        state.previewBgra.clear();
        state.previewBgra.reserve(frame.size() * 4);
        for (const Pixel &p : frame) {
            state.previewBgra.push_back(p.b);
            state.previewBgra.push_back(p.g);
            state.previewBgra.push_back(p.r);
            state.previewBgra.push_back(p.a);
        }
        state.previewBgraFrame = state.currentFrame;
    }

    double scale = 1.0;
    if (state.fitToWindow) {
        const double sx = static_cast<double>(clientW) / std::max(1, movie.width);
        const double sy = static_cast<double>(clientH) / std::max(1, movie.height);
        scale = std::max(0.1, std::min(sx, sy));
    }
    const int drawW = std::max(1, static_cast<int>(movie.width * scale));
    const int drawH = std::max(1, static_cast<int>(movie.height * scale));
    const int x = rc.left + (clientW - drawW) / 2;
    const int y = rc.top + (clientH - drawH) / 2;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = movie.width;
    bmi.bmiHeader.biHeight = -movie.height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, x, y, drawW, drawH, 0, 0, movie.width, movie.height, state.previewBgra.data(), &bmi,
                  DIB_RGB_COLORS, SRCCOPY);
    BitBlt(screenDc, 0, 0, clientW, clientH, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap);
    DeleteObject(backBuffer);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK PreviewProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (gGui) {
            paintPreview(hwnd, *gGui);
            return 0;
        }
        break;
    default:
        break;
    }
    return DefWindowProcA(hwnd, message, wParam, lParam);
}

void loadCatalogForGui(GuiState &state) {
    ++state.decodeSerial;
    state.loading = false;
    clearCurrentMovie(state);
    ListView_DeleteAllItems(state.assetList);
    state.visualEntries.clear();
    state.catalog.reset();

    std::vector<fs::path> roots = defaultCatalogRoots(state);
    fs::path root;
    try {
        std::unique_ptr<Catalog> catalog;
        std::string loadErrors;
        for (const fs::path &candidate : roots) {
            try {
                auto candidateCatalog = std::make_unique<Catalog>(makeDataSource(candidate));
                candidateCatalog->load();
                catalog = std::move(candidateCatalog);
                root = candidate;
                break;
            } catch (const std::exception &e) {
                loadErrors += candidate.string() + ": " + e.what() + "; ";
            }
        }
        if (!catalog) {
            throw std::runtime_error(loadErrors.empty() ? "no Clandestiny disc roots are configured" : loadErrors);
        }
        for (const ResourceEntry &entry : catalog->entries()) {
            if (!isVisualResourceName(entry.name)) {
                continue;
            }
            state.visualEntries.push_back(entry);
        }
        state.catalogRoots = roots;
        state.catalogRoot = root;
        sortVisualEntries(state);
        populateAssetList(state);
        state.catalog = std::move(catalog);
        std::ostringstream ss;
        ss << state.visualEntries.size() << " visual resources loaded from " << root.string();
        if (roots.size() > 1) {
            ss << " with " << roots.size() << " configured disc roots";
        }
        setStatus(state, ss.str());
    } catch (const std::exception &e) {
        std::ostringstream ss;
        ss << "Could not load catalog: " << e.what()
           << "  |  Edit > Preferences writes " << configFilePath().string();
        setStatus(state, ss.str());
    }
}

void loadSelectedMovie(GuiState &state) {
    if (state.populatingList) {
        return;
    }
    const int sel = selectedAssetIndex(state);
    if (sel < 0 || sel >= static_cast<int>(state.visualEntries.size()) || !state.catalog) {
        return;
    }
    const ResourceEntry entry = state.visualEntries[static_cast<size_t>(sel)];
    const std::vector<fs::path> roots = state.catalogRoots.empty() ? defaultCatalogRoots(state) : state.catalogRoots;
    const uint32_t serial = ++state.decodeSerial;
    state.loading = true;
    clearCurrentMovie(state);
    state.loading = true;
    updateFrameLabel(state);
    setStatus(state, "Decoding " + entry.name + " in the background...");

    HWND hwnd = state.hwnd;
    std::thread([hwnd, roots, entry, serial]() {
        auto *result = new MovieLoadResult;
        result->serial = serial;
        result->entryName = entry.name;
        try {
            std::vector<uint8_t> data;
            std::string sourceRoot;
            std::string readError;
            if (!readResourceFromRoots(roots, entry, data, sourceRoot, readError)) {
                throw std::runtime_error("resource is not present in configured disc roots: " + readError);
            }
            DecodeOptions options;
            options.writeFiles = false;
            options.encodeMp4 = false;
            options.collectFrames = true;
            options.log = false;
            RoqDecoder decoder(options);
            decoder.decode(data, entry.name);
            result->movie = std::make_shared<DecodedMovie>(decoder.takeResult());
            result->ok = true;
            std::ostringstream ss;
            ss << entry.name << ": " << result->movie->frames.size() << " frames, " << result->movie->width << "x"
               << result->movie->height;
            if (result->movie->audioChunks > 0) {
                ss << ", " << result->movie->audioChunks << " audio chunks";
            }
            ss << " from " << sourceRoot;
            result->message = ss.str();
        } catch (const std::exception &e) {
            result->message = e.what();
        }
        if (!PostMessageA(hwnd, kMsgMovieLoaded, 0, reinterpret_cast<LPARAM>(result))) {
            delete result;
        }
    }).detach();
}

void tickPlayback(GuiState &state) {
    if (!state.playing || !hasCurrentMovie(state)) {
        stopPlayback(state);
        return;
    }
    const DecodedMovie &movie = *state.current;
    const uint64_t elapsed = GetTickCount64() - state.playbackStartTick;
    const int fps = std::max(1, movie.fps > 0 ? movie.fps : 30);
    const size_t targetFrame = static_cast<size_t>((elapsed * static_cast<uint64_t>(fps)) / 1000);
    if (targetFrame >= movie.frames.size()) {
        state.currentFrame = static_cast<int>(movie.frames.size() - 1);
        InvalidateRect(state.preview, nullptr, TRUE);
        stopPlayback(state);
        return;
    }
    const int nextFrame = static_cast<int>(targetFrame);
    if (nextFrame != state.currentFrame) {
        state.currentFrame = nextFrame;
        const uint64_t now = GetTickCount64();
        if (now - state.lastPlaybackLabelTick >= 250) {
            state.lastPlaybackLabelTick = now;
            updateFrameLabel(state);
        }
        InvalidateRect(state.preview, nullptr, FALSE);
    }
}

void togglePlayback(GuiState &state) {
    if (!hasCurrentMovie(state)) {
        return;
    }
    if (state.playing) {
        stopPlayback(state);
        return;
    }
    state.currentFrame = 0;
    state.playing = true;
    state.playbackStartTick = GetTickCount64();
    state.lastPlaybackLabelTick = 0;
    if (!state.current->pcm.empty()) {
        state.playWav = makeWavPCM16Stereo(state.current->pcm);
        PlaySoundA(reinterpret_cast<LPCSTR>(state.playWav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }
    SetTimer(state.hwnd, kPlaybackTimer, 10, nullptr);
    updateFrameLabel(state);
    InvalidateRect(state.preview, nullptr, FALSE);
}

void appendFfmpegLog(GuiState &state, const std::string &chunk) {
    std::string normalized;
    normalized.reserve(chunk.size() * 2);
    for (char ch : chunk) {
        if (ch == '\n') {
            normalized += "\r\n";
        } else if (ch == '\r') {
            normalized += "\r\n";
        } else {
            normalized += ch;
        }
    }
    state.ffmpegLog += normalized;
    if (state.logEdit && IsWindow(state.logEdit)) {
        const int end = GetWindowTextLengthA(state.logEdit);
        SendMessageA(state.logEdit, EM_SETSEL, end, end);
        SendMessageA(state.logEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(normalized.c_str()));
    }
}

void showLogWindow(GuiState &state) {
    if (!state.logWindow || !IsWindow(state.logWindow)) {
        state.logWindow = CreateWindowExA(WS_EX_TOOLWINDOW, "GroovieLogWindow", "FFmpeg Encode Log",
                                          WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 760, 420, state.hwnd,
                                          nullptr, state.instance, &state);
    }
    ShowWindow(state.logWindow, SW_SHOW);
    SetForegroundWindow(state.logWindow);
}

void encodeCurrentMovie(GuiState &state) {
    if (!hasCurrentMovie(state) || state.encoding) {
        return;
    }
    stopPlayback(state);
    const std::shared_ptr<DecodedMovie> movie = state.current;
    const fs::path outPath = fs::current_path() / (movie->leafName + ".mp4");
    state.encoding = true;
    state.ffmpegLog.clear();
    showLogWindow(state);
    if (state.logEdit) {
        SetWindowTextA(state.logEdit, "");
    }
    appendFfmpegLog(state, "Encoding " + outPath.string() + "\n");
    setStatus(state, "Encoding " + outPath.string() + "...");
    setActionsEnabled(state);

    HWND hwnd = state.hwnd;
    std::thread([hwnd, movie, outPath]() {
        auto log = [hwnd](const std::string &text) {
            auto *chunk = new std::string(text);
            if (!PostMessageA(hwnd, kMsgEncodeLog, 0, reinterpret_cast<LPARAM>(chunk))) {
                delete chunk;
            }
        };
        std::string error;
        const bool ok = encodeDecodedMovieMp4(*movie, outPath, error, log);
        auto *done = new EncodeDoneResult;
        done->ok = ok;
        done->outPath = outPath;
        done->message = ok ? "Encoded " + outPath.string() : error;
        if (!PostMessageA(hwnd, kMsgEncodeDone, 0, reinterpret_cast<LPARAM>(done))) {
            delete done;
        }
    }).detach();
}

HWND makeChild(HWND parent, const char *klass, const char *text, DWORD style, int id) {
    HWND hwnd = CreateWindowExA(0, klass, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                reinterpret_cast<HINSTANCE>(GetWindowLongPtrA(parent, GWLP_HINSTANCE)), nullptr);
    if (gGui && gGui->font) {
        setControlFont(hwnd, gGui->font);
    }
    return hwnd;
}

HMENU makeMainMenu() {
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    HMENU edit = CreatePopupMenu();
    AppendMenuA(file, MF_STRING, kIdMenuReload, "Reload List");
    AppendMenuA(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(file, MF_STRING, kIdMenuExit, "Exit");
    AppendMenuA(edit, MF_STRING, kIdMenuPrefs, "Preferences...");
    AppendMenuA(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), "File");
    AppendMenuA(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), "Edit");
    return menu;
}

void copyLogToClipboard(HWND owner, const std::string &text) {
    if (!OpenClipboard(owner)) {
        return;
    }
    EmptyClipboard();
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (memory) {
        void *ptr = GlobalLock(memory);
        if (ptr) {
            std::memcpy(ptr, text.c_str(), text.size() + 1);
            GlobalUnlock(memory);
            SetClipboardData(CF_TEXT, memory);
            memory = nullptr;
        }
    }
    if (memory) {
        GlobalFree(memory);
    }
    CloseClipboard();
}

void layoutLogWindow(GuiState &state, int width, int height) {
    const int margin = 8;
    const int buttonH = 28;
    const int editH = std::max(1, height - buttonH - margin * 3);
    MoveWindow(state.logEdit, margin, margin, std::max(1, width - margin * 2), editH, TRUE);
    MoveWindow(GetDlgItem(state.logWindow, kIdLogCopy), width - 188, editH + margin * 2, 84, buttonH, TRUE);
    MoveWindow(GetDlgItem(state.logWindow, kIdLogClose), width - 96, editH + margin * 2, 88, buttonH, TRUE);
}

LRESULT CALLBACK LogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    GuiState *state = reinterpret_cast<GuiState *>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE: {
        auto *create = reinterpret_cast<CREATESTRUCTA *>(lParam);
        state = reinterpret_cast<GuiState *>(create->lpCreateParams);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->logWindow = hwnd;
        state->logEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                                                   ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                         0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(kIdLogEdit), state->instance,
                                         nullptr);
        HWND copy = CreateWindowExA(0, "BUTTON", "Copy Log", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10,
                                    hwnd, reinterpret_cast<HMENU>(kIdLogCopy), state->instance, nullptr);
        HWND close = CreateWindowExA(0, "BUTTON", "Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, hwnd,
                                     reinterpret_cast<HMENU>(kIdLogClose), state->instance, nullptr);
        setControlFont(state->logEdit, state->font);
        setControlFont(copy, state->font);
        setControlFont(close, state->font);
        if (!state->ffmpegLog.empty()) {
            SetWindowTextA(state->logEdit, state->ffmpegLog.c_str());
        }
        return 0;
    }
    case WM_SIZE:
        if (state) {
            layoutLogWindow(*state, LOWORD(lParam), HIWORD(lParam));
        }
        return 0;
    case WM_COMMAND:
        if (!state) {
            break;
        }
        switch (LOWORD(wParam)) {
        case kIdLogCopy:
            copyLogToClipboard(hwnd, state->ffmpegLog);
            return 0;
        case kIdLogClose:
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        if (state) {
            state->logWindow = nullptr;
            state->logEdit = nullptr;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcA(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK PrefsProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    GuiState *state = reinterpret_cast<GuiState *>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE: {
        auto *create = reinterpret_cast<CREATESTRUCTA *>(lParam);
        state = reinterpret_cast<GuiState *>(create->lpCreateParams);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->preferences = hwnd;
        CreateWindowExA(0, "STATIC", "Clandestiny Disc 1 root:", WS_CHILD | WS_VISIBLE, 12, 14, 220, 22, hwnd,
                        nullptr, state->instance, nullptr);
        HWND disc1 = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", state->config.clandDisc1Root.string().c_str(),
                                     WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 12, 38, 670, 24, hwnd,
                                     reinterpret_cast<HMENU>(kIdPrefsDisc1Edit), state->instance, nullptr);
        CreateWindowExA(0, "STATIC", "Clandestiny Disc 2 root:", WS_CHILD | WS_VISIBLE, 12, 72, 220, 22, hwnd,
                        nullptr, state->instance, nullptr);
        HWND disc2 = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", state->config.clandDisc2Root.string().c_str(),
                                     WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 12, 96, 670, 24, hwnd,
                                     reinterpret_cast<HMENU>(kIdPrefsDisc2Edit), state->instance, nullptr);
        HWND save = CreateWindowExA(0, "BUTTON", "Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 506, 132, 82,
                                    28, hwnd, reinterpret_cast<HMENU>(kIdPrefsSave), state->instance, nullptr);
        HWND cancel = CreateWindowExA(0, "BUTTON", "Cancel", WS_CHILD | WS_VISIBLE, 600, 132, 82, 28, hwnd,
                                      reinterpret_cast<HMENU>(kIdPrefsCancel), state->instance, nullptr);
        setControlFont(disc1, state->font);
        setControlFont(disc2, state->font);
        setControlFont(save, state->font);
        setControlFont(cancel, state->font);
        SetFocus(disc1);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kIdPrefsSave: {
            char disc1[4096]{};
            char disc2[4096]{};
            GetWindowTextA(GetDlgItem(hwnd, kIdPrefsDisc1Edit), disc1, static_cast<int>(sizeof(disc1)));
            GetWindowTextA(GetDlgItem(hwnd, kIdPrefsDisc2Edit), disc2, static_cast<int>(sizeof(disc2)));
            try {
                state->config.clandDisc1Root = disc1;
                state->config.clandDisc2Root = disc2;
                saveConfig(state->config);
                DestroyWindow(hwnd);
                loadCatalogForGui(*state);
            } catch (const std::exception &e) {
                MessageBoxA(hwnd, e.what(), "Preferences", MB_ICONERROR | MB_OK);
            }
            return 0;
        }
        case kIdPrefsCancel:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (state) {
            state->preferences = nullptr;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcA(hwnd, message, wParam, lParam);
}

void showPreferences(GuiState &state) {
    if (state.preferences && IsWindow(state.preferences)) {
        SetForegroundWindow(state.preferences);
        return;
    }
    HWND hwnd = CreateWindowExA(WS_EX_DLGMODALFRAME, "GrooviePrefsWindow", "Preferences",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 720, 205,
                                state.hwnd, nullptr, state.instance, &state);
    ShowWindow(hwnd, SW_SHOW);
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    GuiState *state = reinterpret_cast<GuiState *>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE: {
        auto *create = reinterpret_cast<CREATESTRUCTA *>(lParam);
        state = reinterpret_cast<GuiState *>(create->lpCreateParams);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->hwnd = hwnd;
        state->font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        state->assetList = makeChild(hwnd, WC_LISTVIEWA, "", WS_BORDER | LVS_REPORT | LVS_SINGLESEL |
                                                               LVS_SHOWSELALWAYS,
                                     kIdAssetList);
        setupAssetListColumns(state->assetList);
        state->frameLabel = makeChild(hwnd, "STATIC", "No ROQ selected", 0, kIdFrameLabel);
        state->btnPlay = makeChild(hwnd, "BUTTON", "Play", BS_PUSHBUTTON, kIdPlay);
        state->btnFitMode = makeChild(hwnd, "BUTTON", "Fit", BS_PUSHBUTTON, kIdFitMode);
        state->btnEncodeMp4 = makeChild(hwnd, "BUTTON", "Encode MP4", BS_PUSHBUTTON, kIdEncodeMp4);
        state->preview = CreateWindowExA(WS_EX_CLIENTEDGE, "GrooviePreviewWindow", "", WS_CHILD | WS_VISIBLE, 0,
                                         0, 10, 10, hwnd, nullptr, state->instance, nullptr);
        state->status = makeChild(hwnd, "STATIC", "", WS_BORDER, kIdStatus);
        try {
            state->config = loadConfig();
        } catch (const std::exception &e) {
            setStatus(*state, std::string("config.json: ") + e.what());
        }
        updateFrameLabel(*state);
        loadCatalogForGui(*state);
        return 0;
    }
    case WM_SIZE:
        if (state) {
            layoutMainWindow(*state, LOWORD(lParam), HIWORD(lParam));
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto *info = reinterpret_cast<MINMAXINFO *>(lParam);
        info->ptMinTrackSize.x = 1280;
        info->ptMinTrackSize.y = 720;
        return 0;
    }
    case WM_TIMER:
        if (state && wParam == kPlaybackTimer) {
            tickPlayback(*state);
            return 0;
        }
        break;
    case WM_NOTIFY:
        if (!state) {
            break;
        }
        if (reinterpret_cast<NMHDR *>(lParam)->idFrom == kIdAssetList) {
            NMHDR *hdr = reinterpret_cast<NMHDR *>(lParam);
            if (hdr->code == LVN_COLUMNCLICK) {
                auto *info = reinterpret_cast<NMLISTVIEW *>(lParam);
                std::string selectedName;
                const int selected = selectedAssetIndex(*state);
                if (selected >= 0 && selected < static_cast<int>(state->visualEntries.size())) {
                    selectedName = state->visualEntries[static_cast<size_t>(selected)].name;
                }
                if (state->sortColumn == info->iSubItem) {
                    state->sortAscending = !state->sortAscending;
                } else {
                    state->sortColumn = info->iSubItem;
                    state->sortAscending = true;
                }
                sortVisualEntries(*state);
                populateAssetList(*state, selectedName);
                return 0;
            }
            if (hdr->code == LVN_ITEMCHANGED && !state->populatingList) {
                auto *info = reinterpret_cast<NMLISTVIEW *>(lParam);
                const bool becameSelected = (info->uChanged & LVIF_STATE) != 0 &&
                                            (info->uNewState & LVIS_SELECTED) != 0 &&
                                            (info->uOldState & LVIS_SELECTED) == 0;
                if (becameSelected) {
                    loadSelectedMovie(*state);
                }
                return 0;
            }
        }
        break;
    case WM_COMMAND:
        if (!state) {
            break;
        }
        switch (LOWORD(wParam)) {
        case kIdPlay:
            togglePlayback(*state);
            return 0;
        case kIdFitMode:
            state->fitToWindow = !state->fitToWindow;
            setActionsEnabled(*state);
            InvalidateRect(state->preview, nullptr, TRUE);
            return 0;
        case kIdEncodeMp4:
            encodeCurrentMovie(*state);
            return 0;
        case kIdMenuReload:
            loadCatalogForGui(*state);
            return 0;
        case kIdMenuPrefs:
            showPreferences(*state);
            return 0;
        case kIdMenuExit:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        break;
    case kMsgMovieLoaded: {
        if (!state) {
            delete reinterpret_cast<MovieLoadResult *>(lParam);
            return 0;
        }
        std::unique_ptr<MovieLoadResult> result(reinterpret_cast<MovieLoadResult *>(lParam));
        if (result->serial != state->decodeSerial) {
            return 0;
        }
        state->loading = false;
        if (result->ok) {
            state->current = result->movie;
            state->currentFrame = 0;
            state->previewBgraFrame = -1;
            state->previewBgra.clear();
            state->playWav.clear();
            setStatus(*state, result->message);
        } else {
            state->current.reset();
            state->currentFrame = 0;
            state->previewBgraFrame = -1;
            state->previewBgra.clear();
            setStatus(*state, result->entryName + ": " + result->message);
            MessageBoxA(state->hwnd, result->message.c_str(), result->entryName.c_str(), MB_ICONERROR | MB_OK);
        }
        updateFrameLabel(*state);
        InvalidateRect(state->preview, nullptr, TRUE);
        return 0;
    }
    case kMsgEncodeLog: {
        if (!state) {
            delete reinterpret_cast<std::string *>(lParam);
            return 0;
        }
        std::unique_ptr<std::string> chunk(reinterpret_cast<std::string *>(lParam));
        appendFfmpegLog(*state, *chunk);
        return 0;
    }
    case kMsgEncodeDone: {
        if (!state) {
            delete reinterpret_cast<EncodeDoneResult *>(lParam);
            return 0;
        }
        std::unique_ptr<EncodeDoneResult> result(reinterpret_cast<EncodeDoneResult *>(lParam));
        state->encoding = false;
        setStatus(*state, result->message);
        setActionsEnabled(*state);
        if (result->ok) {
            MessageBoxA(state->hwnd, result->outPath.string().c_str(), "Encode MP4 complete", MB_OK);
        } else {
            MessageBoxA(state->hwnd, result->message.c_str(), "Encode MP4", MB_ICONERROR | MB_OK);
        }
        return 0;
    }
    case WM_DESTROY:
        if (state) {
            ++state->decodeSerial;
            stopPlayback(*state);
        } else {
            PlaySoundA(nullptr, nullptr, SND_PURGE);
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcA(hwnd, message, wParam, lParam);
}

void registerGuiClasses(HINSTANCE instance) {
    WNDCLASSA mainClass{};
    mainClass.lpfnWndProc = MainProc;
    mainClass.hInstance = instance;
    mainClass.hIcon = LoadIconA(nullptr, IDI_APPLICATION);
    mainClass.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    mainClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    mainClass.lpszClassName = "GroovieMainWindow";
    RegisterClassA(&mainClass);

    WNDCLASSA previewClass{};
    previewClass.lpfnWndProc = PreviewProc;
    previewClass.hInstance = instance;
    previewClass.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    previewClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    previewClass.lpszClassName = "GrooviePreviewWindow";
    RegisterClassA(&previewClass);

    WNDCLASSA prefsClass{};
    prefsClass.lpfnWndProc = PrefsProc;
    prefsClass.hInstance = instance;
    prefsClass.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    prefsClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    prefsClass.lpszClassName = "GrooviePrefsWindow";
    RegisterClassA(&prefsClass);

    WNDCLASSA logClass{};
    logClass.lpfnWndProc = LogProc;
    logClass.hInstance = instance;
    logClass.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    logClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    logClass.lpszClassName = "GroovieLogWindow";
    RegisterClassA(&logClass);
}

int runGui(HINSTANCE instance, int showCommand) {
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&controls);
    registerGuiClasses(instance);
    GuiState state;
    state.instance = instance;
    gGui = &state;

    HWND hwnd = CreateWindowExA(0, "GroovieMainWindow", "Groovie-2 Asset Viewer", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, makeMainMenu(), instance, &state);
    if (!hwnd) {
        MessageBoxA(nullptr, "Could not create Groovie-2 window", "groovie2", MB_ICONERROR | MB_OK);
        return 1;
    }
    ShowWindow(hwnd, showCommand);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    gGui = nullptr;
    return static_cast<int>(msg.wParam);
}

std::vector<std::string> windowsCommandLineArgs() {
    int argc = 0;
    LPWSTR *argvWide = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    if (!argvWide) {
        return args;
    }
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.push_back(wideToUtf8(argvWide[i]));
    }
    LocalFree(argvWide);
    return args;
}

bool prepareCliConsole() {
    bool allocated = false;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        allocated = AllocConsole() != 0;
    }

    FILE *stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    freopen_s(&stream, "CONIN$", "r", stdin);
    std::cout.clear();
    std::cerr.clear();
    std::cin.clear();
    return allocated;
}

int runWindowsCli(const std::vector<std::string> &args) {
    const bool allocatedConsole = prepareCliConsole();
    std::vector<char *> argv;
    argv.reserve(args.size());
    for (const std::string &arg : args) {
        argv.push_back(const_cast<char *>(arg.c_str()));
    }

    int rc = 1;
    try {
        rc = run(static_cast<int>(argv.size()), argv.data());
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n\n";
        printUsage();
        rc = 1;
    }

    if (allocatedConsole) {
        std::cout << "\nPress Enter to close.";
        std::cin.get();
        FreeConsole();
    }
    return rc;
}

#endif

} // namespace

#ifdef _WIN32

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    const std::vector<std::string> args = windowsCommandLineArgs();
    if (args.size() > 1) {
        return runWindowsCli(args);
    }
    return runGui(instance, showCommand);
}

#else

int main(int argc, char **argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n\n";
        printUsage();
        return 1;
    }
}

#endif
