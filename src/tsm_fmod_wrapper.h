// tsm_fmod_wrapper.h
#pragma once

#include <fmod.hpp>
#include <fmod_errors.h>
#include <string>
#include <vector>
#include <spdlog/spdlog.h>

namespace TSM 
{

class FModWrapper 
{
public:
    static FModWrapper& GetInstance() 
    {
        static FModWrapper instance;
        return instance;
    }

    bool Initialize();
    void Update();
    void Shutdown();

    FMOD::System* GetSystem() { return m_system; }

private:
    FModWrapper() : m_system(nullptr) {}
    ~FModWrapper() {}

    FModWrapper(const FModWrapper&) = delete;
    FModWrapper& operator=(const FModWrapper&) = delete;

    FMOD::System* m_system;
};

} // namespace TSM
