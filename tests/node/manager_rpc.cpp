#include <asterion/manager.pb.h>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/foundation/serialization.hpp>
#include <google/protobuf/util/json_util.h>
#include <fstream>
#include <iostream>
using namespace asterion;
using namespace std::chrono_literals;

int main(int argc, char** argv)
{
    if (argc == 5) {
        node::proto::ServicePrograms programs;
        std::ifstream input(argv[2], std::ios::binary);
        if (!programs.ParseFromIstream(&input))
            return 1;
        auto change = [&](std::string* value) {
            if (value->starts_with(argv[3]))
                value->replace(0, std::string(argv[3]).size(), argv[4]);
        };
        for (auto& item : *programs.mutable_services()) {
            change(item.mutable_executable());
            change(item.mutable_catalog());
            change(item.mutable_provider());
        }
        for (auto& item : *programs.mutable_bundled_plugins())
            change(&item);
        std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
        return programs.SerializeToOstream(&output) ? 0 : 1;
    }
    for (std::string line; std::getline(std::cin, line);) {
        try {
            manager::proto::Request request;
            if (!google::protobuf::util::JsonStringToMessage(line, &request).ok())
                return 2;
            request.set_version(1);
            request.set_correlation_id("fixture");
            auto channel = ipc::Channel::connect(argv[1], 10s);
            channel.send(request.SerializeAsString(), 5s);
            manager::proto::Response response;
            if (!response.ParseFromString(channel.receive(180s)))
                return 3;
            if (response.has_error())
                std::cout << Json{{"error", response.error().message()}}.dump();
            else if (response.has_presentation())
                std::cout << response.presentation();
            else
                std::cout << "{}";
            std::cout << std::endl;
        } catch (const std::exception& error) {
            std::cout << Json{{"error", error.what()}}.dump() << std::endl;
        }
    }
}
