#pragma once
#include <asterion/protocol/observation.hpp>
#include <filesystem>
#include <functional>
#include <stop_token>

namespace asterion::data {

class Observations
{
public:
    using Fetch = std::function<
        market::proto::ObservationCapture(const resource::proto::Object&, std::stop_token)>;
    Observations(std::filesystem::path directory, resource::proto::Owner owner);
    void validate_admission(const proto::ObservationAdmission&) const;
    proto::ObservationOperation admit(const proto::ObservationAdmission&) const;
    proto::ObservationStatus read(const std::string& id, bool payload = true) const;

    struct Prepared
    {
        proto::ObservationOperation operation;
        market::proto::ObservationCapture capture;
    };

    Prepared prepare(const std::string& id, const Fetch&, std::stop_token stop = {}) const;
    proto::ObservationOperation publish(const Prepared&) const;
    std::vector<proto::ObservationOperation> operations(std::stop_token stop = {}) const;

private:
    bool authority() const;
    std::filesystem::path root_;
    resource::proto::Owner owner_;
    std::filesystem::path operation_file(const std::string&) const;
    std::filesystem::path object_file(const std::string&) const;
    market::proto::ObservationCapture
    capture_file(const std::filesystem::path&, const resource::proto::Object&) const;
};

} // namespace asterion::data
