#include <iostream>
#include <map>
#include <unordered_map>
#include <string>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <chrono>
#include <random>
#include <vector>
#include <memory>
#include <functional>
#include <queue>
#include <condition_variable>
#include <set>

using namespace std;
using namespace chrono;

// ==================== 数据结构定义 ====================

// 合约行情
struct MarketData {
    string contract_id;
    double last_price;
    int64_t timestamp;
};

// 仓位信息
struct Position {
    string contract_id;
    string user_id;
    string main_account_id;
    double quantity;        // 持仓数量
    double avg_price;       // 持仓均价
    double margin_rate;     // 保证金率
    
    double calc_unrealized_pnl(double current_price) const {
        return (current_price - avg_price) * quantity;
    }
    
    double calc_margin(double current_price) const {
        return current_price * abs(quantity) * margin_rate;
    }
};

// 资金信息
struct Account {
    string main_account_id;
    string user_id;
    double balance;         // 账户余额
    double available;       // 可用资金
};

// 强平结果
struct LiquidationEvent {
    string user_id;
    string main_account_id;
    string contract_id;
    double price;
    string reason;
    int64_t timestamp;
};

// 全局配置
const int NUM_USERS = 10000;
const int NUM_MAIN_ACCOUNTS = 5;      // 每个用户多个主账户
const int NUM_CONTRACTS = 10000;
const int NUM_POSITIONS = 10000;
const int MARKET_UPDATE_INTERVAL_MS = 100;  // 行情更新间隔
const int POLLING_INTERVAL_MS = 50;         // 轮询间隔
const int TEST_DURATION_SECONDS = 30;
const double LIQUIDATION_RATIO = 0.8;       // 维持保证金率80%

// 性能统计
struct Statistics {
    atomic<long long> market_updates{0};
    atomic<long long> risk_checks{0};
    atomic<long long> liquidations{0};
    atomic<long long> polling_cycles{0};
    atomic<long long> event_triggers{0};
    atomic<bool> should_stop{false};
    
    void reset() {
        market_updates = 0;
        risk_checks = 0;
        liquidations = 0;
        polling_cycles = 0;
        event_triggers = 0;
        should_stop = false;
    }
};

// ==================== 行情资源对象 ====================
class MarketDataManager {
private:
    unordered_map<string, shared_ptr<MarketData>> market_data;
    shared_mutex mutex_;
    
public:
    void init_data() {
        unique_lock<shared_mutex> lock(mutex_);
        for (int i = 0; i < NUM_CONTRACTS; ++i) {
            string contract_id = "CONTRACT_" + to_string(i);
            auto data = make_shared<MarketData>();
            data->contract_id = contract_id;
            data->last_price = 10000.0 + (i % 100) * 100.0;  // 10000-19900
            data->timestamp = duration_cast<milliseconds>(
                system_clock::now().time_since_epoch()
            ).count();
            market_data[contract_id] = data;
        }
    }
    
    // 获取所有行情（轮询使用）
    unordered_map<string, shared_ptr<MarketData>> get_all_market_data() {
        shared_lock<shared_mutex> lock(mutex_);
        return market_data;
    }
    
    // 获取单个行情
    shared_ptr<MarketData> get_market_data(const string& contract_id) {
        shared_lock<shared_mutex> lock(mutex_);
        auto it = market_data.find(contract_id);
        return it != market_data.end() ? it->second : nullptr;
    }
    
    // 更新行情（模拟行情变动）
    void update_market_data(const string& contract_id, double price_change) {
        unique_lock<shared_mutex> lock(mutex_);
        auto it = market_data.find(contract_id);
        if (it != market_data.end()) {
            it->second->last_price += price_change;
            it->second->timestamp = duration_cast<milliseconds>(
                system_clock::now().time_since_epoch()
            ).count();
        }
    }
};

// ==================== 仓位对象 ====================
class PositionManager {
private:
    unordered_map<string, shared_ptr<Position>> positions;  // position_id -> Position
    unordered_map<string, vector<string>> contract_positions;  // contract_id -> position_ids
    unordered_map<string, vector<string>> account_positions;   // main_account_id -> position_ids
    shared_mutex mutex_;
    
public:
    void init_data() {
        unique_lock<shared_mutex> lock(mutex_);
        
        for (int i = 0; i < NUM_POSITIONS; ++i) {
            string position_id = "POS_" + to_string(i);
            auto pos = make_shared<Position>();
            
            pos->user_id = "USER_" + to_string(i % NUM_USERS);
            pos->main_account_id = "ACCOUNT_" + to_string(i % NUM_MAIN_ACCOUNTS);
            pos->contract_id = "CONTRACT_" + to_string(i % NUM_CONTRACTS);
            pos->quantity = (i % 2 == 0 ? 1.0 : -1.0) * (10 + i % 50);
            pos->avg_price = 10000.0 + (i % 100) * 100.0;
            pos->margin_rate = 0.1;  // 10%保证金率
            
            positions[position_id] = pos;
            contract_positions[pos->contract_id].push_back(position_id);
            account_positions[pos->main_account_id].push_back(position_id);
        }
    }
    
    // 获取所有仓位（轮询使用）
    vector<shared_ptr<Position>> get_all_positions() {
        shared_lock<shared_mutex> lock(mutex_);
        vector<shared_ptr<Position>> result;
        result.reserve(positions.size());
        for (const auto& [id, pos] : positions) {
            result.push_back(pos);
        }
        return result;
    }
    
    // 根据合约获取仓位（事件驱动使用）
    vector<shared_ptr<Position>> get_positions_by_contract(const string& contract_id) {
        shared_lock<shared_mutex> lock(mutex_);
        vector<shared_ptr<Position>> result;
        auto it = contract_positions.find(contract_id);
        if (it != contract_positions.end()) {
            for (const auto& pos_id : it->second) {
                result.push_back(positions[pos_id]);
            }
        }
        return result;
    }
    
    // 根据主账户获取仓位
    vector<shared_ptr<Position>> get_positions_by_account(const string& main_account_id) {
        shared_lock<shared_mutex> lock(mutex_);
        vector<shared_ptr<Position>> result;
        auto it = account_positions.find(main_account_id);
        if (it != account_positions.end()) {
            for (const auto& pos_id : it->second) {
                result.push_back(positions[pos_id]);
            }
        }
        return result;
    }
};

// ==================== 资金对象 ====================
class AccountManager {
private:
    unordered_map<string, shared_ptr<Account>> accounts;
    shared_mutex mutex_;
    
public:
    void init_data() {
        unique_lock<shared_mutex> lock(mutex_);
        for (int i = 0; i < NUM_MAIN_ACCOUNTS; ++i) {
            string account_id = "ACCOUNT_" + to_string(i);
            auto acc = make_shared<Account>();
            acc->main_account_id = account_id;
            acc->user_id = "USER_" + to_string(i % NUM_USERS);
            acc->balance = 100000.0 + (i % 100) * 1000.0;
            acc->available = acc->balance * 0.8;
            accounts[account_id] = acc;
        }
    }
    
    shared_ptr<Account> get_account(const string& main_account_id) {
        shared_lock<shared_mutex> lock(mutex_);
        auto it = accounts.find(main_account_id);
        return it != accounts.end() ? it->second : nullptr;
    }
};

// ==================== 风控引擎 ====================
class RiskEngine {
private:
    MarketDataManager& market_mgr;
    PositionManager& position_mgr;
    AccountManager& account_mgr;
    mutex liquidation_mutex;
    vector<LiquidationEvent> liquidations;
    
public:
    RiskEngine(MarketDataManager& m, PositionManager& p, AccountManager& a)
        : market_mgr(m), position_mgr(p), account_mgr(a) {}
    
    // 检查单个主账户的风险
    bool check_account_risk(const string& main_account_id, Statistics& stats) {
        stats.risk_checks++;
        
        auto account = account_mgr.get_account(main_account_id);
        if (!account) return false;
        
        auto positions = position_mgr.get_positions_by_account(main_account_id);
        if (positions.empty()) return false;
        
        double total_margin = 0.0;
        double total_unrealized_pnl = 0.0;
        
        for (const auto& pos : positions) {
            auto market = market_mgr.get_market_data(pos->contract_id);
            if (market) {
                double current_price = market->last_price;
                total_margin += pos->calc_margin(current_price);
                total_unrealized_pnl += pos->calc_unrealized_pnl(current_price);
            }
        }
        
        double equity = account->balance + total_unrealized_pnl;
        double margin_ratio = equity / total_margin;
        
        // 触发强平
        if (margin_ratio < LIQUIDATION_RATIO) {
            LiquidationEvent event;
            event.main_account_id = main_account_id;
            event.user_id = account->user_id;
            event.price = 0.0;
            event.reason = "Margin ratio below " + to_string(LIQUIDATION_RATIO);
            event.timestamp = duration_cast<milliseconds>(
                system_clock::now().time_since_epoch()
            ).count();
            
            lock_guard<mutex> lock(liquidation_mutex);
            liquidations.push_back(event);
            stats.liquidations++;
            return true;
        }
        
        return false;
    }
    
    size_t get_liquidation_count() const {
        return liquidations.size();
    }
};

// ==================== 方案1: 轮询方案 ====================
class PollingRiskMonitor {
private:
    RiskEngine& risk_engine;
    PositionManager& position_mgr;
    Statistics& stats;
    
public:
    PollingRiskMonitor(RiskEngine& r, PositionManager& p, Statistics& s)
        : risk_engine(r), position_mgr(p), stats(s) {}
    
    void run() {
        // 收集所有需要检查的主账户
        set<string> all_accounts;
        auto positions = position_mgr.get_all_positions();
        for (const auto& pos : positions) {
            all_accounts.insert(pos->main_account_id);
        }
        
        while (!stats.should_stop.load()) {
            stats.polling_cycles++;
            
            // 轮询检查所有主账户
            for (const auto& account_id : all_accounts) {
                risk_engine.check_account_risk(account_id, stats);
            }
            
            this_thread::sleep_for(milliseconds(POLLING_INTERVAL_MS));
        }
    }
};

// ==================== 方案2: 事件驱动方案 ====================
class EventDrivenRiskMonitor {
private:
    RiskEngine& risk_engine;
    PositionManager& position_mgr;
    Statistics& stats;
    
    struct RiskCheckTask {
        set<string> affected_accounts;
    };
    
    queue<RiskCheckTask> task_queue;
    mutex queue_mutex;
    condition_variable cv;
    
public:
    EventDrivenRiskMonitor(RiskEngine& r, PositionManager& p, Statistics& s)
        : risk_engine(r), position_mgr(p), stats(s) {}
    
    // 行情变化时触发
    void on_market_data_update(const string& contract_id) {
        stats.event_triggers++;
        
        // 找出受影响的主账户
        auto positions = position_mgr.get_positions_by_contract(contract_id);
        set<string> affected_accounts;
        for (const auto& pos : positions) {
            affected_accounts.insert(pos->main_account_id);
        }
        
        if (!affected_accounts.empty()) {
            RiskCheckTask task;
            task.affected_accounts = affected_accounts;
            
            lock_guard<mutex> lock(queue_mutex);
            task_queue.push(task);
            cv.notify_one();
        }
    }
    
    // 风控检查线程
    void run_worker() {
        while (!stats.should_stop.load()) {
            unique_lock<mutex> lock(queue_mutex);
            cv.wait_for(lock, milliseconds(100), [this] {
                return !task_queue.empty() || stats.should_stop.load();
            });
            
            if (stats.should_stop.load()) break;
            
            if (!task_queue.empty()) {
                RiskCheckTask task = task_queue.front();
                task_queue.pop();
                lock.unlock();
                
                // 检查受影响的账户
                for (const auto& account_id : task.affected_accounts) {
                    risk_engine.check_account_risk(account_id, stats);
                }
            }
        }
    }
};

// ==================== 测试框架 ====================
template<typename MonitorType>
class Benchmark {
private:
    MarketDataManager market_mgr;
    PositionManager position_mgr;
    AccountManager account_mgr;
    RiskEngine risk_engine;
    MonitorType monitor;
    Statistics stats;
    
    // 行情更新线程
    void market_update_thread() {
        random_device rd;
        mt19937 gen(rd());
        uniform_int_distribution<> contract_dis(0, NUM_CONTRACTS - 1);
        uniform_real_distribution<> price_dis(-50.0, 50.0);
        
        while (!stats.should_stop.load()) {
            int contract_idx = contract_dis(gen);
            string contract_id = "CONTRACT_" + to_string(contract_idx);
            double price_change = price_dis(gen);
            
            market_mgr.update_market_data(contract_id, price_change);
            stats.market_updates++;
            
            // 如果是事件驱动，通知监控器
            if constexpr (is_same_v<MonitorType, EventDrivenRiskMonitor>) {
                monitor.on_market_data_update(contract_id);
            }
            
            this_thread::sleep_for(milliseconds(MARKET_UPDATE_INTERVAL_MS));
        }
    }
    
    void timer_thread() {
        this_thread::sleep_for(seconds(TEST_DURATION_SECONDS));
        stats.should_stop.store(true);
    }
    
public:
    Benchmark()
        : risk_engine(market_mgr, position_mgr, account_mgr),
          monitor(risk_engine, position_mgr, stats) {}
    
    void run() {
        cout << "初始化数据..." << endl;
        market_mgr.init_data();
        position_mgr.init_data();
        account_mgr.init_data();
        
        stats.reset();
        
        auto start_time = high_resolution_clock::now();
        
        thread timer(&Benchmark::timer_thread, this);
        thread market_thread(&Benchmark::market_update_thread, this);
        
        vector<thread> monitor_threads;
        
        if constexpr (is_same_v<MonitorType, PollingRiskMonitor>) {
            // 轮询方案：启动轮询线程
            monitor_threads.emplace_back([this]() { monitor.run(); });
        } else {
            // 事件驱动方案：启动多个工作线程
            for (int i = 0; i < 4; ++i) {
                monitor_threads.emplace_back([this]() { monitor.run_worker(); });
            }
        }
        
        timer.join();
        market_thread.join();
        for (auto& t : monitor_threads) t.join();
        
        auto end_time = high_resolution_clock::now();
        auto duration = duration_cast<milliseconds>(end_time - start_time).count();
        
        print_results(duration);
    }
    
    void print_results(long long duration) {
        cout << "执行时间: " << duration << " ms" << endl;
        cout << "行情更新次数: " << stats.market_updates.load() << endl;
        cout << "风控检查次数: " << stats.risk_checks.load() << endl;
        cout << "强平触发次数: " << stats.liquidations.load() << endl;
        
        if constexpr (is_same_v<MonitorType, PollingRiskMonitor>) {
            cout << "轮询周期数: " << stats.polling_cycles.load() << endl;
        } else {
            cout << "事件触发次数: " << stats.event_triggers.load() << endl;
        }
        
        cout << "行情更新频率: " << (stats.market_updates.load() * 1000.0 / duration) << " 次/秒" << endl;
        cout << "风控检查频率: " << (stats.risk_checks.load() * 1000.0 / duration) << " 次/秒" << endl;
        cout << "平均每次行情更新触发的检查: " 
             << (double)stats.risk_checks.load() / stats.market_updates.load() << " 次" << endl;
    }
};

// ==================== 主程序 ====================
int main() {
    cout << "========================================" << endl;
    cout << "轮询 vs 事件驱动 - 风控系统性能对比" << endl;
    cout << "========================================" << endl;
    cout << "场景配置:" << endl;
    cout << "  用户数量: " << NUM_USERS << endl;
    cout << "  主账户数量: " << NUM_MAIN_ACCOUNTS << endl;
    cout << "  合约数量: " << NUM_CONTRACTS << endl;
    cout << "  仓位数量: " << NUM_POSITIONS << endl;
    cout << "  行情更新间隔: " << MARKET_UPDATE_INTERVAL_MS << " ms" << endl;
    cout << "  轮询间隔: " << POLLING_INTERVAL_MS << " ms" << endl;
    cout << "  测试时长: " << TEST_DURATION_SECONDS << " 秒" << endl;
    cout << "========================================" << endl << endl;
    
    // 方案1: 轮询方案
    cout << "【方案1: 轮询方案】" << endl;
    {
        Benchmark<PollingRiskMonitor> benchmark;
        benchmark.run();
    }
    cout << endl;
    
    // 方案2: 事件驱动方案
    cout << "【方案2: 事件驱动方案】" << endl;
    {
        Benchmark<EventDrivenRiskMonitor> benchmark;
        benchmark.run();
    }
    cout << endl;
    
    cout << "========================================" << endl;
    cout << "测试完成" << endl;
    cout << "========================================" << endl;
    
    return 0;
}
