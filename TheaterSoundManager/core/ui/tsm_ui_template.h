// tsm_ui_template.h

#pragma once

#include "imgui.h"
#include <string>

namespace TSM {

class UI_Template {
public:
    UI_Template() = default;
    virtual ~UI_Template() = default;

    virtual void Render() = 0;

protected:
    bool m_isOpen = true;
};

}
