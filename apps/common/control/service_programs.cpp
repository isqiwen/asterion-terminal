#include "service_programs.hpp"
#include "application_environment.hpp"
#include <asterion/system/environment.hpp>
#include <asterion/system/process/artifact.hpp>
#include <stdexcept>

namespace asterion::terminal {

ServicePrograms local_service_programs(node::proto::ServiceKind kind)
{
    const auto library = current_platform().os == "macos" ? ".dylib" : ".so";
    const auto resolve = [](const char* variable, const std::string& name) {
        return environment_path(variable).value_or(application_directory() / name);
    };
    ServicePrograms programs;
    switch (kind) {
        case node::proto::MARKET:
            programs.executable = resolve("ASTERION_MARKET_EXECUTABLE", "asterion_market");
            programs.provider = resolve("ASTERION_CTP_LIBRARY", std::string("ctp-md") + library);
            programs.catalog =
                resolve("ASTERION_CTP_CATALOG_LIBRARY", std::string("ctp-trader") + library);
            break;
        case node::proto::DATA:
            programs.executable = resolve("ASTERION_DATA_SERVICE_EXECUTABLE", "asterion_data");
            break;
        case node::proto::RESEARCH:
            programs.executable = resolve("ASTERION_RESEARCH_EXECUTABLE", "asterion_research");
            break;
        default:
            throw std::invalid_argument("unknown service kind");
    }
    return programs;
}

} // namespace asterion::terminal
