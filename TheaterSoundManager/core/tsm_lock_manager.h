// tsm_lock_manager.h 
#pragma once

#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <atomic>
#include <thread>
#include <functional>
#include <cassert>
#include <stdexcept>
#include <algorithm>

class TSMLockManager {
public:
    enum class ResourceType {
        CHANNEL,
        TRACK,
        FADE,
        ANNOUNCEMENT,
        PLAYBACK
    };

    struct LockInfo {
        std::thread::id ownerThread;        
        std::chrono::steady_clock::time_point acquisitionTime;  
        std::string resourceName;            
        ResourceType type;                  
    };

    void releaseAllLocks() {
        std::unique_lock<std::shared_mutex> guard(managerMutex);
        activeLocks.clear();
        cv.notify_all();
    }

    static TSMLockManager& getInstance() {
        static TSMLockManager instance;
        return instance;
    }

    bool acquireLock(ResourceType type,
                     const std::string& resourceName,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)) 
    {
        if (resourceName.empty()) {
            return false;
        }

        std::unique_lock<std::shared_mutex> guard(managerMutex);
        
        auto threadId = std::this_thread::get_id();
        auto startTime = std::chrono::steady_clock::now();
        auto key = getResourceKey(type, resourceName);

        auto it = activeLocks.find(key);
        if (it != activeLocks.end()) {
            if (it->second.ownerThread == threadId) {
                return true;
            }

            while (it != activeLocks.end() && it->second.ownerThread != threadId) {
                if (cv.wait_for(guard, timeout) == std::cv_status::timeout) {
                    return false;
                }
                if (std::chrono::steady_clock::now() - startTime > timeout) {
                    return false;
                }
                it = activeLocks.find(key);
            }
        }

        LockInfo lockInfo{
            threadId,
            std::chrono::steady_clock::now(),
            resourceName,
            type
        };
        
        activeLocks[key] = lockInfo;
        return true;
    }

    void releaseLock(ResourceType type, const std::string& resourceName) {
        std::unique_lock<std::shared_mutex> guard(managerMutex);
        auto threadId = std::this_thread::get_id();
        auto key = getResourceKey(type, resourceName);

        auto it = activeLocks.find(key);
        if (it != activeLocks.end() && it->second.ownerThread == threadId) {
            activeLocks.erase(it);
            cv.notify_all();
        }
    }

    class ScopedLock {
    public:
        ScopedLock(TSMLockManager& manager, ResourceType type, const std::string& resourceName)
            : manager_(manager)
            , type_(type)
            , resourceName_(resourceName)
            , locked_(false)
        {
            if (resourceName_.empty()) {
                throw std::runtime_error("Le nom de la ressource ne peut pas être vide");
            }
            locked_ = manager_.acquireLock(type_, resourceName_);
            if (!locked_) {
                throw std::runtime_error("Impossible d'acquérir le verrou pour la ressource : " + resourceName_);
            }
        }

        ~ScopedLock() {
            if (locked_) {
                manager_.releaseLock(type_, resourceName_);
            }
        }

        ScopedLock(const ScopedLock&) = delete;
        ScopedLock& operator=(const ScopedLock&) = delete;
        ScopedLock(ScopedLock&&) = delete;
        ScopedLock& operator=(ScopedLock&&) = delete;

    private:
        TSMLockManager& manager_;
        ResourceType type_;
        std::string resourceName_;
        bool locked_;
    };

private:
    TSMLockManager() = default;
    ~TSMLockManager() = default;
    
    TSMLockManager(const TSMLockManager&) = delete;
    TSMLockManager& operator=(const TSMLockManager&) = delete;

    std::string getResourceKey(ResourceType type, const std::string& resourceName) const {
        return std::to_string(static_cast<int>(type)) + ":" + resourceName;
    }

    mutable std::shared_mutex managerMutex;
    std::condition_variable_any cv;
    std::unordered_map<std::string, LockInfo> activeLocks;
};

#define TSM_LOCK(type, name) \
    TSMLockManager::ScopedLock scopedLock##__LINE__( \
        TSMLockManager::getInstance(), \
        TSMLockManager::ResourceType::type, \
        name \
    )

#define TSM_LOCK_NAMED(type, name, lockname) \
    TSMLockManager::ScopedLock lockname( \
        TSMLockManager::getInstance(), \
        TSMLockManager::ResourceType::type, \
        name \
    )