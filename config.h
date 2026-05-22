#pragma once

#include <filesystem>

struct AppConfig {
    std::filesystem::path clandDisc1Root;
    std::filesystem::path clandDisc2Root;
};

std::filesystem::path configFilePath();
AppConfig loadConfig();
void saveConfig(const AppConfig &config);
