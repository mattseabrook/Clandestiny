#include "config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

class JsonReader {
public:
    explicit JsonReader(std::string text) : _text(std::move(text)) {}

    std::map<std::string, std::string> readObject() {
        std::map<std::string, std::string> values;
        skipSpace();
        consume('{');
        skipSpace();
        if (peek() == '}') {
            ++_pos;
            return values;
        }
        while (true) {
            const std::string key = readString();
            skipSpace();
            consume(':');
            skipSpace();
            values[key] = readString();
            skipSpace();
            const char next = peek();
            if (next == ',') {
                ++_pos;
                skipSpace();
                continue;
            }
            if (next == '}') {
                ++_pos;
                break;
            }
            throw std::runtime_error("Expected ',' or '}' in config.json");
        }
        return values;
    }

private:
    char peek() const {
        return _pos < _text.size() ? _text[_pos] : '\0';
    }

    void skipSpace() {
        while (_pos < _text.size() && std::isspace(static_cast<unsigned char>(_text[_pos]))) {
            ++_pos;
        }
    }

    void consume(char expected) {
        if (peek() != expected) {
            std::string message = "Expected '";
            message += expected;
            message += "' in config.json";
            throw std::runtime_error(message);
        }
        ++_pos;
    }

    std::string readString() {
        consume('"');
        std::string out;
        while (_pos < _text.size()) {
            const char ch = _text[_pos++];
            if (ch == '"') {
                return out;
            }
            if (ch != '\\') {
                out += ch;
                continue;
            }
            if (_pos >= _text.size()) {
                throw std::runtime_error("Invalid escape in config.json");
            }
            const char esc = _text[_pos++];
            switch (esc) {
            case '"':
            case '\\':
            case '/':
                out += esc;
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            default:
                throw std::runtime_error("Unsupported escape in config.json");
            }
        }
        throw std::runtime_error("Unterminated string in config.json");
    }

    std::string _text;
    size_t _pos = 0;
};

std::string readTextFile(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

std::string escapeJson(std::string text) {
    std::string out;
    for (char ch : text) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += ch;
            break;
        }
    }
    return out;
}

fs::path siblingDiscRoot(const fs::path &path, const char *discName) {
    if (path.empty()) {
        return {};
    }
    fs::path parent = path.parent_path();
    if (parent.empty()) {
        return {};
    }
    return parent / discName;
}

} // namespace

fs::path configFilePath() {
    return fs::current_path() / "config.json";
}

AppConfig loadConfig() {
    AppConfig config;
    const fs::path path = configFilePath();
    if (!fs::is_regular_file(path)) {
        return config;
    }

    const std::string text = readTextFile(path);
    if (text.empty()) {
        return config;
    }

    JsonReader reader(text);
    const std::map<std::string, std::string> values = reader.readObject();
    auto disc1 = values.find("clandestiny_disc1_root");
    if (disc1 == values.end()) {
        disc1 = values.find("clandestinyDisc1Root");
    }
    if (disc1 != values.end()) {
        config.clandDisc1Root = disc1->second;
    }

    auto disc2 = values.find("clandestiny_disc2_root");
    if (disc2 == values.end()) {
        disc2 = values.find("clandestinyDisc2Root");
    }
    if (disc2 != values.end()) {
        config.clandDisc2Root = disc2->second;
    }

    auto legacy = values.find("clandestiny_root");
    if (legacy == values.end()) {
        legacy = values.find("clandestinyDiscRoot");
    }
    if (legacy != values.end() && config.clandDisc1Root.empty() && config.clandDisc2Root.empty()) {
        const fs::path root = legacy->second;
        std::string leaf = root.filename().string();
        std::transform(leaf.begin(), leaf.end(), leaf.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (leaf == "disc2") {
            config.clandDisc2Root = root;
            const fs::path disc1Path = siblingDiscRoot(root, "disc1");
            if (fs::is_directory(disc1Path)) {
                config.clandDisc1Root = disc1Path;
            }
        } else {
            config.clandDisc1Root = root;
            const fs::path disc2Path = siblingDiscRoot(root, "disc2");
            if (fs::is_directory(disc2Path)) {
                config.clandDisc2Root = disc2Path;
            }
        }
    }
    return config;
}

void saveConfig(const AppConfig &config) {
    std::ofstream file(configFilePath(), std::ios::binary);
    if (!file) {
        throw std::runtime_error("Could not write config.json");
    }
    file << "{\n";
    file << "  \"clandestiny_disc1_root\": \"" << escapeJson(config.clandDisc1Root.string()) << "\",\n";
    file << "  \"clandestiny_disc2_root\": \"" << escapeJson(config.clandDisc2Root.string()) << "\"\n";
    file << "}\n";
}
