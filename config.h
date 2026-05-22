#pragma once

#include <filesystem>

struct AppConfig {
    std::filesystem::path clandDiscRoot;
};

std::filesystem::path configFilePath();
AppConfig loadConfig();
void saveConfig(const AppConfig &config);
