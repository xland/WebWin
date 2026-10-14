#include <memory>

#include "Config.h"

namespace {
    std::unique_ptr<Config> config;
}

Config::Config()
{
}

Config::~Config()
{
}

void Config::init()
{
    if (!config)
    {
        Config* rawPtr = new Config();
        config = std::unique_ptr<Config>(rawPtr);
    }
}

Config* Config::get()
{
    return config.get();
}

