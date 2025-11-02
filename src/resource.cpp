#include "resource.h"

void resource::AddObject(RealObj obj)
{
    std::shared_ptr<SharedObj> sharedObj;

    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = objects.find(obj.symbol + obj.accountId);
        if (it != objects.end()) {
            sharedObj = it->second;
        } else {
            // 不存在，新增 SharedObj
            sharedObj = std::make_shared<SharedObj>();
            objects[obj.symbol + obj.accountId] = sharedObj;
        }
    }

    // 在鎖外使用 SharedObj 的線程安全方法
    sharedObj->AddDetail(obj.id, std::make_shared<RealObj>(obj));
}

void resource::RemoveObject(RealObj obj)
{
    std::shared_ptr<SharedObj> sharedObj;

    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = objects.find(obj.symbol + obj.accountId);
        if (it != objects.end()) {
            sharedObj = it->second;
        } else {
            return;  // 對象不存在
        }
    }

    // 在鎖外移除 detail
    sharedObj->RemoveDetail(obj.id);

    // 如果 SharedObj 的 details 已空，則移除該 SharedObj
    if (sharedObj->IsEmpty()) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = objects.find(obj.symbol + obj.accountId);
        if (it != objects.end() && it->second->IsEmpty()) {
            objects.erase(it);
        }
    }
}

std::shared_ptr<SharedObj> resource::GetObject(const std::string &key)
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = objects.find(key);
    if (it != objects.end()) {
        return it->second;
    }
    return nullptr;
}
