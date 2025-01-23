// tsm_fmod_wrapper.cpp

#include "tsm_fmod_wrapper.h"

namespace TSM 
{

bool FModWrapper::Initialize()
{
    FMOD_RESULT result;

    result = FMOD::System_Create(&m_system);
    if (result != FMOD_OK)
    {
        spdlog::error("Failed to create FMOD system: {}", FMOD_ErrorString(result));
        return false;
    }

    result = m_system->init(512, FMOD_INIT_NORMAL, nullptr);
    if (result != FMOD_OK)
    {
        spdlog::error("Failed to initialize FMOD system: {}", FMOD_ErrorString(result));
        return false;
    }

    spdlog::info("FMOD initialized successfully.");
    return true;
}

void FModWrapper::Update() 
{
    if (m_system)
    {
        FMOD_RESULT result = m_system->update();
        if (result != FMOD_OK)
        {
            spdlog::error("FMOD update failed: {}", FMOD_ErrorString(result));
        }
    }
}

void FModWrapper::Shutdown() 
{
    if (m_system)
    {
        m_system->close();
        m_system->release();
        m_system = nullptr;
    }

    spdlog::info("FMOD shutdown successfully.");
}

} // namespace tsm