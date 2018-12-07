/* Author: Sounak Gupta, July/Aug 2018 */
#pragma once

#include <vector>
#include <tuple>
#include <memory>
#include <string>
#include <cassert>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <mutex>
#include <thread>
#include <set>
#include <iostream>

#include "thread_buffer.hpp"
#include <nlohmann/json.hpp>

namespace metrics {

template <typename T, typename... V>
constexpr auto array_of(V&&... v) -> std::array<T, sizeof...(V)> {
    return {{std::forward<T>(v)...}};
}

static auto g_histogram_bucket_specs = array_of<uint64_t>(
    300, 450, 750, 1000, 3000, 5000, 7000, 9000, 11000, 13000, 15000,
    17000, 19000, 21000, 32000, 45000, 75000, 110000, 160000, 240000,
    360000, 540000, 800000, 1200000, 1800000, 2700000, 4000000);

#define HIST_BKT_SIZE (g_histogram_bucket_specs.size() + 1)

class MetricsGroup;
extern MetricsGroup mgroup;

enum _publish_as { publish_as_counter, publish_as_gauge, publish_as_histogram, };

class _counter {
public:
    _counter() = default;
    void increment (int64_t value = 1)  { m_value += value; }
    void decrement (int64_t value = 1)  { m_value -= value; }
    int64_t get() const { return m_value; }
    int64_t merge (const _counter& other) {
        this->m_value += other.m_value;
        return this->m_value;
    }
private:
    int64_t m_value = 0;
};

class _gauge {
public:
    _gauge() : m_value(0) {}
    _gauge(const std::atomic<int64_t> &oval) : m_value(oval.load(std::memory_order_relaxed)) {}
    _gauge(const _gauge &other) : m_value(other.get()) {}
    _gauge &operator=(const _gauge &other) {
        m_value.store(other.get(), std::memory_order_relaxed);
        return *this;
    }
    void update (int64_t value) { m_value.store(value, std::memory_order_relaxed); }
    int64_t get() const { return m_value.load(std::memory_order_relaxed); }

private:
    std::atomic<int64_t> m_value;
};

class _histogram {
public:
    _histogram() {
        memset(&m_freqs, 0, sizeof(int64_t) * m_freqs.size());
        m_sum = 0;
    }

    void observe(int64_t value) {
        auto lower = std::lower_bound(g_histogram_bucket_specs.begin(), g_histogram_bucket_specs.end(), value);
        auto bkt_idx = lower - g_histogram_bucket_specs.begin();
        m_freqs[bkt_idx]++;
        m_sum += value;
    }
    void merge(const _histogram& other) {
        for (auto i = 0U; i < HIST_BKT_SIZE; i++) {
            this->m_freqs[i] += other.m_freqs[i];
        }
        this->m_sum += other.m_sum;
    }
    auto& getFreqs() const { return m_freqs; }
    int64_t getSum() const { return m_sum; }

private:
    std::array<int64_t, HIST_BKT_SIZE> m_freqs;
    int64_t m_sum = 0;
};

typedef std::tuple<size_t, size_t, size_t> metrics_count_tuple;

class SafeMetrics {
private:
    _counter   *m_counters   = nullptr;
    _histogram *m_histograms = nullptr;

    uint32_t m_ncntrs;
    uint32_t m_nhists;

public:
    SafeMetrics() : SafeMetrics(0U, 0U) {}

    SafeMetrics(uint32_t ncntrs, uint32_t nhists) :
            m_ncntrs(ncntrs), m_nhists(nhists) {
        m_counters   = new _counter[ncntrs];
        m_histograms = new _histogram[nhists];

        memset(m_counters, 0, (sizeof(_counter) * ncntrs));
        memset(m_histograms, 0, (sizeof(_histogram) * nhists));
    }

    ~SafeMetrics() {
        delete [] m_counters;
        delete [] m_histograms;
    }

    _counter& getCounter (uint64_t index) { return m_counters[index]; }
    _histogram& getHistogram (uint64_t index) { return m_histograms[index]; }

    auto getNumMetrics() const { return std::make_tuple(m_ncntrs, m_nhists); }
};

class _metrics_buf {
public:
    _metrics_buf(uint32_t ncntrs, uint32_t nhists) :
            m_safe_metrics(ncntrs, nhists) {}

    urcu::urcu_ptr<SafeMetrics> getSafe() { return m_safe_metrics.get(); }
    void rotate() {
        uint32_t ncntrs, nhists;
        std::tie(ncntrs, nhists) = m_safe_metrics.get_node()->get()->getNumMetrics();
        m_safe_metrics.make_and_exchange(ncntrs, nhists);
    }

private:
    urcu::urcu_data<SafeMetrics> m_safe_metrics;
};

class ReportCounter {
public:
    ReportCounter(const std::string& name, const std::string& desc, const std::string& sub_type,
            _publish_as ptype = publish_as_counter) :
            m_name(name),
            m_desc(desc),
            m_sub_type(sub_type) {
        if (name != "none") {
            if (sub_type != "") {
                if (ptype == publish_as_counter) {
                } else if (ptype == publish_as_gauge) {
                }
                //m_prometheus_counter =
                //    monitor::MetricsMonitor::Instance().RegisterCounter(name, desc, {{"type", sub_type}});
            } else {
                //m_prometheus_counter = monitor::MetricsMonitor::Instance().RegisterCounter(name, desc);
            }
        }
    }

    int64_t get() const { return m_counter.get(); }
    int64_t merge(const _counter& other) { return m_counter.merge(other); }
    std::string name() const { return m_name; }
    std::string desc() const { return m_desc; }
    std::string subType() const { return m_sub_type; }
    void publish() {
        //m_prometheus_counter->Update((double) m_counter.get());
    }
private:
    std::string m_name;
    std::string m_desc;
    std::string m_sub_type;
    _counter m_counter;
    //monitor::Counter* m_prometheus_counter = nullptr;
    //monitor::Gauge* m_prometheus_gauge; // In case counter to be represented as gauge
};

class ReportGauge {
    friend class MetricsGroup;
public:
    ReportGauge(const std::string& name, const std::string& desc, const std::string& sub_type) :
            m_name(name),
            m_desc(desc),
            m_sub_type(sub_type) {

        if (name != "none") {
            if (sub_type != "") {
                //m_prometheus_gauge =
                //    monitor::MetricsMonitor::Instance().RegisterGauge(name, desc, {{"type", sub_type}});
            } else {
                //m_prometheus_gauge = monitor::MetricsMonitor::Instance().RegisterGauge(name, desc);
            }
        }
    }

    uint64_t get() const { return m_gauge.get(); };
    std::string name() const { return m_name; }
    std::string desc() const { return m_desc; }
    std::string subType() const { return m_sub_type; }
    void publish() {
        //m_prometheus_gauge->Set((double) m_gauge.get());
    }
private:
    const std::string m_name, m_desc, m_sub_type;
    _gauge m_gauge;
    //monitor::Gauge *m_prometheus_gauge;
};

class ReportHistogram {
public:
    ReportHistogram(const std::string& name, const std::string& desc, const std::string& sub_type) :
            m_name(name),
            m_desc(desc),
            m_sub_type(sub_type) {
 
        if (name != "none") {
            if (sub_type != "") {
                //m_prometheus_hist =
                //    monitor::MetricsMonitor::Instance().RegisterHistogram(name, desc, {{"type", sub_type}});
            } else {
                //m_prometheus_hist = monitor::MetricsMonitor::Instance().RegisterHistogram(name, desc);
            }
        }
    }
    double percentile( float pcntl ) const {
        std::array<int64_t, HIST_BKT_SIZE> cum_freq;
        int64_t fcount = 0;
        for (auto i = 0U; i < HIST_BKT_SIZE; i++) {
            fcount += (m_histogram.getFreqs())[i];
            cum_freq[i] = fcount;
        }

        int64_t pnum = fcount * pcntl/100;
        auto i = (std::lower_bound(cum_freq.begin(), cum_freq.end(), pnum)) - cum_freq.begin();
        if ( (m_histogram.getFreqs())[i] == 0 ) return 0;
        auto Yl = i == 0 ? 0 : g_histogram_bucket_specs[i-1];
        auto ith_cum_freq = (i == 0) ? 0 : cum_freq[i-1];
        double Yp = Yl + (((pnum - ith_cum_freq) * i)/(m_histogram.getFreqs())[i]);
        return Yp;

        /* Formula:
            Yp = lower bound of i-th bucket + ((pn - cumfreq[i-1]) * i ) / freq[i]
            where
                pn = (cnt * percentile)/100
                i  = matched index of pnum in cum_freq
         */
    }
    int64_t count() const {
        int64_t cnt = 0;
        for (auto i = 0U; i < HIST_BKT_SIZE; i++) {
            cnt += (m_histogram.getFreqs())[i];
        }
        return cnt;
    }
    double average() const {
        auto cnt = count();
        return (cnt ? m_histogram.getSum()/cnt : 0);
    }
    void merge(const _histogram& other) { m_histogram.merge(other); }
    std::string name() const { return m_name; }
    std::string desc() const { return m_desc; }
    std::string subType() const { return m_sub_type; }
    void publish() {
        //std::vector<double> vec(std::begin(m_histogram.getFreqs()), std::end(m_histogram.getFreqs()));
        //m_prometheus_hist->Update(vec, m_histogram.m_sum);
    }
    _histogram& getReportHistogram() { return m_histogram; }

private:
    const std::string m_name, m_desc, m_sub_type;
    _histogram m_histogram;
   //monitor::Histogram *m_prometheus_hist;
};

typedef std::shared_ptr<MetricsGroup> MetricsGroupPtr;
typedef fds::ThreadBuffer<_metrics_buf, uint32_t, uint32_t> MetricsThreadBuffer;

class MetricsResult;
class MetricsFarm;
class MetricsGroup {
    friend class MetricsFarm;

public:
private:
    [[nodiscard]] auto lock() { return std::lock_guard<decltype(m_mutex)>(m_mutex); }

public:
    static MetricsGroupPtr make_group() { return std::make_shared<MetricsGroup>(); }

    MetricsGroup() = default;
    uint64_t registerCounter(const std::string& name, const std::string& desc, const std::string& sub_type,
            _publish_as ptype = publish_as_counter) {
        auto locked = lock();
        m_counters.emplace_back(name, desc, sub_type, ptype);
        return m_counters.size()-1;
    }
    uint64_t registerGauge(const std::string& name, const std::string& desc, const std::string& sub_type) {
        auto locked = lock();
        m_gauges.emplace_back(name, desc, sub_type);
        return m_gauges.size()-1;
    }
    uint64_t registerHistogram(const std::string& name, const std::string& desc, const std::string& sub_type) {
        auto locked = lock();
        m_histograms.emplace_back(name, desc, sub_type);
        return m_histograms.size()-1;
    }

    _counter& getCounter(uint64_t index) { return (*m_buffer)->getSafe()->getCounter(index); }
    _gauge& getGauge(uint64_t index) { return m_gauges[index].m_gauge; }
    _histogram& getHistogram(uint64_t index) { return (*m_buffer)->getSafe()->getHistogram(index); }

    std::vector<ReportCounter>      m_counters;
    std::vector<ReportGauge>        m_gauges;
    std::vector<ReportHistogram>    m_histograms;

private:
    void on_register() {
        m_buffer = std::make_unique< MetricsThreadBuffer >(m_counters.size(), m_histograms.size());
    }
    std::unique_ptr<MetricsResult> gather() { return std::make_unique<MetricsResult>(this, *m_buffer); }

private:
    std::mutex m_mutex;
    std::unique_ptr< MetricsThreadBuffer > m_buffer;
};

class MetricsResult {
public:
    MetricsResult(MetricsGroup* mgroup, MetricsThreadBuffer& all_buf) {
        m_mgroup = mgroup;
        all_buf.access_all_threads([mgroup](_metrics_buf *m) {
            /* get current metrics instance */
            auto metrics = m->getSafe();
            uint32_t num_cntrs, num_hists;
            std::tie(num_cntrs, num_hists) = metrics->getNumMetrics();

            for (auto i = 0U; i < num_cntrs; i++) {
                mgroup->m_counters[i].merge(metrics->getCounter(i));
            }
            for (auto i = 0U; i < num_hists; i++) {
                mgroup->m_histograms[i].merge(metrics->getHistogram(i));
            }
            /* replace new metrics instance */
            m->rotate();
        });
    }

    ~MetricsResult() { urcu::urcu_ctl::declare_quiscent_state(); }

    void publish() {
        for (auto i = 0U; i < m_mgroup->m_counters.size(); i++) {
            m_mgroup->m_counters[i].publish();
        }
        for (auto i = 0U; i < m_mgroup->m_gauges.size(); i++) {
            m_mgroup->m_gauges[i].publish();
        }
        for (auto i = 0U; i < m_mgroup->m_histograms.size(); i++) {
            m_mgroup->m_histograms[i].publish();
        }
    }

    std::string getJSON() const {
        nlohmann::json json;
        nlohmann::json counter_entries;
        for (auto &c : m_mgroup->m_counters) {
            std::string desc = c.name() + c.desc();
            if (!c.subType().empty()) desc = desc + " - " + c.subType();
            counter_entries[desc] = c.get();
        }
        json["Counters"] = counter_entries;

        nlohmann::json gauge_entries;
        for (auto &g : m_mgroup->m_gauges) {
            std::string desc = g.name() + g.desc();
            if (!g.subType().empty()) desc = desc + " - " + g.subType();
            gauge_entries[desc] = g.get();
        }
        json["Gauges"] = gauge_entries;

        nlohmann::json hist_entries;
        for (auto &h : m_mgroup->m_histograms) {
            std::stringstream ss;
            ss << h.average() << " / " << h.percentile(50) << " / " << h.percentile(95)
                                << " / " << h.percentile(99);
            std::string desc = h.name() + h.desc();
            if (!h.subType().empty()) desc = desc + " - " + h.subType();
            hist_entries[desc] = ss.str();
        }
        json["Histograms percentiles (usecs) avg/50/95/99"] = hist_entries;

        return json.dump();
    }

private:
    MetricsGroup* m_mgroup;
    friend class MetricsFarm;
};

std::once_flag is_farm_present;
class MetricsFarm {
private:
    static MetricsFarm *m_instance;
    std::set<MetricsGroupPtr> m_mgroups;
    std::mutex m_lock;
    MetricsFarm() = default;

private:
    [[nodiscard]] auto lock() { return std::lock_guard<decltype(m_lock)>(m_lock); }

public:
    static MetricsFarm *getInstance() {
        std::call_once(is_farm_present, [](){ m_instance = new MetricsFarm(); });
        return m_instance;
    }
    void registerMetricsGroup(MetricsGroupPtr& mgroup ) {
        assert(mgroup != nullptr);
        auto locked = lock();
        mgroup->on_register();
        m_mgroups.insert(mgroup);
    }
    void deregisterMetricsGroup(MetricsGroupPtr& mgroup ) {
        assert(mgroup != nullptr);
        auto locked = lock();
        m_mgroups.erase(mgroup);
    }

    std::string gather() {
        nlohmann::json json;
        nlohmann::json counter_entries, gauge_entries, hist_entries;

        auto locked = lock();

        /* For each registered mgroup */
        for (auto mgroup : m_mgroups) {
            auto result = mgroup->gather();
            /* For each registered counter inside the mgroup */
            for (auto const &c : result->m_mgroup->m_counters) {
                std::string desc = c.name() + c.desc();
                if (!c.subType().empty()) desc = desc + " - " + c.subType();
                counter_entries[desc] = c.get();
            }
            /* For each registered gauge inside the mgroup */
            for (auto const &g : result->m_mgroup->m_gauges) {
                std::string desc = g.name() + g.desc();
                if (!g.subType().empty()) desc = desc + " - " + g.subType();
                gauge_entries[desc] = g.get();
            }
            /* For each registered histogram inside the mgroup */
            for (auto const &h : result->m_mgroup->m_histograms) {
                std::stringstream ss;
                ss << h.average()   << " / " << h.percentile(50)
                                    << " / " << h.percentile(95)
                                    << " / " << h.percentile(99);
                std::string desc = h.name() + h.desc();
                if (!h.subType().empty()) desc = desc + " - " + h.subType();
                hist_entries[desc] = ss.str();
            }
        }
        json["Counters"] = counter_entries;
        json["Gauges"] = gauge_entries;
        json["Histograms percentiles (usecs) avg/50/95/99"] = hist_entries;

        return json.dump();
    }
    MetricsFarm( MetricsFarm const& )       = delete;
    void operator=( MetricsFarm const& )    = delete;
};
MetricsFarm *MetricsFarm::m_instance = nullptr;
}
