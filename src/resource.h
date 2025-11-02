#if !defined(RESOURCE_H)
#define RESOURCE_H

#include <map>
#include <string>
#include <memory>
#include <mutex>
#include <shared_mutex>

struct RealObj
{
    std::string id;
    
    std::string symbol;
    std::string accountId;
    int direct;
    
    double price;
    double number;
};

struct CalObj
{
    std::string symbol;
    std::string accountId;
    int direct;

    int total_count;
    double avg_price;
    double total_number;
};

struct SharedObj
{
    std::map<std::string, std::shared_ptr<RealObj>> details;
    std::map<std::string, std::shared_ptr<CalObj>> totals;
    mutable std::shared_mutex mutex;  // 保護內部數據的鎖

    // 線程安全的寫入方法
    void AddDetail(const std::string& id, std::shared_ptr<RealObj> obj) {
        std::unique_lock<std::shared_mutex> lock(mutex);
        details[id] = obj;
    }

    void RemoveDetail(const std::string& id) {
        std::unique_lock<std::shared_mutex> lock(mutex);
        details.erase(id);
    }

    void AddTotal(const std::string& key, std::shared_ptr<CalObj> obj) {
        std::unique_lock<std::shared_mutex> lock(mutex);
        totals[key] = obj;
    }

    void RemoveTotal(const std::string& key) {
        std::unique_lock<std::shared_mutex> lock(mutex);
        totals.erase(key);
    }

    // 線程安全的讀取方法 - 獲取單個元素
    std::shared_ptr<RealObj> GetDetail(const std::string& id) const {
        std::shared_lock<std::shared_mutex> lock(mutex);
        auto it = details.find(id);
        return (it != details.end()) ? it->second : nullptr;
    }

    std::shared_ptr<CalObj> GetTotal(const std::string& key) const {
        std::shared_lock<std::shared_mutex> lock(mutex);
        auto it = totals.find(key);
        return (it != totals.end()) ? it->second : nullptr;
    }

    // 線程安全的遍歷方法 - 使用回調函數
    template<typename Func>
    void ForEachDetail(Func func) const {
        std::shared_lock<std::shared_mutex> lock(mutex);
        for (const auto& [key, value] : details) {
            func(key, value);
        }
    }

    template<typename Func>
    void ForEachTotal(Func func) const {
        std::shared_lock<std::shared_mutex> lock(mutex);
        for (const auto& [key, value] : totals) {
            func(key, value);
        }
    }

    // 獲取副本（深拷貝版本，適合需要長時間處理的場景）
    std::map<std::string, std::shared_ptr<RealObj>> GetDetailsCopy() const {
        std::shared_lock<std::shared_mutex> lock(mutex);
        return details;  // 返回 map 的副本
    }

    std::map<std::string, std::shared_ptr<CalObj>> GetTotalsCopy() const {
        std::shared_lock<std::shared_mutex> lock(mutex);
        return totals;  // 返回 map 的副本
    }

    // 狀態查詢
    bool IsEmpty() const {
        std::shared_lock<std::shared_mutex> lock(mutex);
        return details.empty() && totals.empty();
    }

    size_t GetDetailCount() const {
        std::shared_lock<std::shared_mutex> lock(mutex);
        return details.size();
    }

    size_t GetTotalCount() const {
        std::shared_lock<std::shared_mutex> lock(mutex);
        return totals.size();
    }
};

class resource
{
private:
    std::map<std::string, std::shared_ptr<SharedObj>> objects;
    std::shared_mutex mutex_;

public:
    resource() {};
    ~resource() {};

    void AddObject(RealObj obj);
    void RemoveObject(RealObj obj);
    std::shared_ptr<SharedObj> GetObject(const std::string& key);
};

#endif // RESOURCE_H
