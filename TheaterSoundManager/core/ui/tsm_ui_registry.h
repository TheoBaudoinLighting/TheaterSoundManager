// tsm_ui_registry.h

#pragma once

#include <unordered_map>
#include <string>
#include <memory>

#include "ui/tsm_ui_template.h"

namespace TSM {

class UI_Registry 
{
public:
    static UI_Registry& Instance() 
    {
        static UI_Registry instance;
        return instance;
    }

    template<typename T>
    void registerTemplate(const std::string& name) 
    {
        static_assert(std::is_base_of<UI_Template, T>::value, "Type must inherit from UI_Template");
        if (templates_.find(name) == templates_.end()) 
        {
            templates_[name] = std::make_unique<T>();
        }
    }

    void renderAll() 
    {
        for (const auto& [name, ui] : templates_) 
        {
            ui->Render();
        }
    }

    private:
        UI_Registry() = default;
        std::unordered_map<std::string, std::unique_ptr<UI_Template>> templates_;
    };

    #define REGISTER_UI_TEMPLATE(Type, Name) \
        static struct Register##Type { \
            Register##Type() { \
                UI_Registry::Instance().registerTemplate<Type>(Name); \
            } \
        } register##Type;

}