#pragma once

#include "model.hpp"

#include <future>
#include <map>
#include <string>
#include <vector>

namespace wd {

struct SatelliteRecord {
    int norad{};
    std::string name;
    std::string internationalId;
    std::string objectType;
    std::string opsCode;
    std::string ownerCode;
    std::string launchDate;
    std::string launchSite;
    double periodMinutes{};
    double inclinationDeg{};
    double apogeeKm{};
    double perigeeKm{};
    double radarCrossSection{};
    std::vector<std::string> categories;
};

struct CloseApproach {
    std::string designation;
    std::string date;
    double distanceAu{};
    double velocityKms{};
    double absoluteMagnitude{};
};

struct FeedStatus {
    std::string label;
    std::string source;
    std::string state;
    std::size_t records{};
};

struct Snapshot {
    std::vector<SatelliteRecord> satellites;
    std::vector<CloseApproach> approaches;
    std::map<std::string, std::size_t> categoryCounts;
    std::vector<FeedStatus> feeds;
    std::string refreshedAt;
};

class DataHub {
public:
    DataHub();
    ~DataHub();
    DataHub(const DataHub&) = delete;
    DataHub& operator=(const DataHub&) = delete;

    void refresh();
    bool poll();
    bool loading() const;
    const Snapshot& snapshot() const { return snapshot_; }
    const std::string& error() const { return error_; }

    std::vector<SearchResult> search(const std::string& query) const;
    ObjectCard inspect(const SearchResult& result) const;
    ObjectCard inspectSatellite(int norad) const;

private:
    Snapshot snapshot_;
    std::future<Snapshot> pending_;
    std::string error_;
};

} // namespace wd
