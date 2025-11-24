#ifndef slic3r_UserManager_hpp_
#define slic3r_UserManager_hpp_

#include <map>
#include <mutex>
#include <vector>
#include <string>
#include <memory>
#include <chrono>
#include <boost/thread.hpp>
#include "nlohmann/json.hpp"
#include "slic3r/Utils/json_diff.hpp"


using namespace nlohmann;

namespace Slic3r {

class INetworkAgent;

class UserManager
{
private:
    INetworkAgent* m_agent { nullptr };

public:
    UserManager(INetworkAgent* agent = nullptr);
    ~UserManager();

    void set_agent(INetworkAgent* agent);
    int parse_json(std::string payload);
};
} // namespace Slic3r

#endif //  slic3r_UserManager_hpp_
