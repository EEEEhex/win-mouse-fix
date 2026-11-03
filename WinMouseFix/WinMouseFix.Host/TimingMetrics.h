#pragma once
#include "ProductEngine.h"
#include <map>
#include <string>
#include <vector>
namespace wmf::host {
// Used only by the asynchronous log consumer, never by the input producer.
class TimingMetrics {
public:
    explicit TimingMetrics(std::uint64_t frequency) : frequency_(frequency) {}
    void Observe(const ProductTrace&);
    std::string Json() const;
private:
    struct Group {
        std::uint64_t seen{},boundaries{},updated{};
        std::vector<double> earliest,latest,queue,processing;
    };
    std::uint64_t frequency_{},serial_{},evicted_{},invalid_{};
    std::map<std::string,Group> groups_;
};
}
